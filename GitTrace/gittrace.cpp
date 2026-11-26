#include "gittrace.h"
#include "utils/treesitter.h"
#include "chunkdb/index_store.h"
#include <tree_sitter/api.h>
#include <cstring>
#include <unordered_map>
#include <iostream>
#include <set>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace {
std::string hash_text(const std::string& input)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : input) { hash ^= byte; hash *= 1099511628211ull; }
    std::ostringstream value;
    value << std::hex << std::setw(16) << std::setfill('0') << hash;
    return value.str();
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return input ? std::string(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()) : std::string();
}

std::string ignore_fingerprint(git_repository* repo, const std::filesystem::path& root)
{
    std::vector<std::pair<std::string, std::string>> rule_files;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        const auto path = it->path();
        const std::string name = path.filename().string();
        if (it->is_directory(ec)) {
            if (name == ".git" || name == ".gittrace") {
                it.disable_recursion_pending();
            } else if (path != root) {
                std::string relative = std::filesystem::relative(path, root, ec).generic_string();
                if (!relative.empty()) {
                    relative.push_back('/');
                    int ignored = 0;
                    if (git_ignore_path_is_ignored(&ignored, repo, relative.c_str()) == 0 && ignored)
                        it.disable_recursion_pending();
                }
            }
        } else if (!ec && name == ".gitignore") {
            const auto relative = std::filesystem::relative(path, root, ec).generic_string();
            if (!ec) rule_files.emplace_back(relative, read_file(path));
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
    rule_files.emplace_back(".gittraceignore", read_file(root / ".gittraceignore"));

    const char* gitdir = git_repository_path(repo);
    if (gitdir) rule_files.emplace_back(".git/info/exclude", read_file(std::filesystem::path(gitdir) / "info" / "exclude"));

    git_config* config = nullptr;
    if (git_repository_config(&config, repo) == 0) {
        const char* configured_excludes = nullptr;
        if (git_config_get_string(&configured_excludes, config, "core.excludesfile") == 0 && configured_excludes) {
            std::filesystem::path path(configured_excludes);
            const char* home = std::getenv("HOME");
            if (path.string().rfind("~/", 0) == 0) {
                if (home) path = std::filesystem::path(home) / path.string().substr(2);
            }
            rule_files.emplace_back("core.excludesfile=" + path.string(), read_file(path));
            if (!path.is_absolute() && home)
                rule_files.emplace_back("core.excludesfile-home=" +
                    (std::filesystem::path(home) / path).string(),
                    read_file(std::filesystem::path(home) / path));
        } else {
            const char* home = std::getenv("HOME");
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            if (xdg && *xdg)
                rule_files.emplace_back("xdg-ignore", read_file(std::filesystem::path(xdg) / "git" / "ignore"));
            else if (home)
                rule_files.emplace_back("xdg-ignore", read_file(std::filesystem::path(home) / ".config" / "git" / "ignore"));
        }
        int ignore_case = 0;
        if (git_config_get_bool(&ignore_case, config, "core.ignorecase") == 0)
            rule_files.emplace_back("core.ignorecase", ignore_case ? "true" : "false");
        git_config_free(config);
    }

    std::sort(rule_files.begin(), rule_files.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    std::string serialized;
    for (const auto& [path, contents] : rule_files) {
        serialized += path;
        serialized.push_back('\0');
        serialized += contents;
        serialized.push_back('\0');
    }
    return hash_text(serialized);
}

std::string grammar_fingerprint()
{
#if defined(__clang__)
    const std::string compiler = std::string("clang-") + __clang_version__;
#elif defined(__GNUC__)
    const std::string compiler = std::string("gcc-") + __VERSION__;
#elif defined(_MSC_VER)
    const std::string compiler = "msvc-" + std::to_string(_MSC_VER);
#else
    const std::string compiler = "unknown-compiler";
#endif
    std::vector<std::pair<std::string, std::string>> grammars;
    std::istringstream manifest(read_file("GitTrace/rsc/grammar.txt"));
    std::string line;
    auto trim_copy = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    };
    while (std::getline(manifest, line)) {
        line = trim_copy(line);
        if (line.empty() || line.front() == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::istringstream fields(line.substr(equals + 1));
        std::string url, subdir;
        std::getline(fields, url, '|');
        std::getline(fields, subdir, '|');
        url = trim_copy(url);
        subdir = trim_copy(subdir);
        const auto slash = url.find_last_of('/');
        std::string name = slash == std::string::npos ? url : url.substr(slash + 1);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".git") name.resize(name.size() - 4);
        if (!name.empty()) grammars.emplace_back(std::move(name), std::move(subdir));
    }
    std::sort(grammars.begin(), grammars.end());
    grammars.erase(std::unique(grammars.begin(), grammars.end()), grammars.end());

    std::vector<std::string> inputs;
    const std::filesystem::path grammar_root("GitTrace/rsc/grammars");
    for (const auto& [name, subdir] : grammars) {
        const auto repository_path = grammar_root / name;
        git_repository* grammar_repo = nullptr;
        std::string head;
        if (git_repository_open(&grammar_repo, repository_path.string().c_str()) == 0) {
            git_reference* ref = nullptr;
            if (git_repository_head(&ref, grammar_repo) == 0) {
                const git_oid* oid = git_reference_target(ref);
                char text[41] = {0};
                if (oid) git_oid_tostr(text, sizeof(text), oid);
                head = text;
                git_reference_free(ref);
            }
            git_repository_free(grammar_repo);
        }
        inputs.push_back(name + "/" + subdir + ":head=" + head);
        const auto source = repository_path / subdir / "src";
        for (const char* filename : {"parser.c", "scanner.c", "scanner.cc", "scanner.cpp"}) {
            const auto path = source / filename;
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec) continue;
            const auto modified = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
            if (!ec) inputs.push_back(name + "/" + subdir + "/src/" + filename + ":" +
                std::to_string(size) + ":" + std::to_string(static_cast<long long>(modified)));
        }
    }
    std::sort(inputs.begin(), inputs.end());
    std::string contents = read_file("GitTrace/rsc/grammar.txt") +
                           ";tree-sitter-abi=" + std::to_string(TREE_SITTER_LANGUAGE_VERSION) +
                           ";compiler=" + compiler;
    for (const auto& input : inputs) { contents += input; contents.push_back('\0'); }
    return hash_text(contents);
}

std::string cache_manifest(git_repository* repo, const std::filesystem::path& root,
                           const std::string& model_path, bool include_head = true)
{
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr char platform[] = "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    constexpr char platform[] = "macos-x64";
#elif defined(__linux__) && defined(__aarch64__)
    constexpr char platform[] = "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    constexpr char platform[] = "linux-x64";
#else
    constexpr char platform[] = "other";
#endif
    git_reference* head = nullptr;
    if (git_repository_head(&head, repo) < 0) return {};
    const git_oid* target = git_reference_target(head);
    char head_sha[41] = {0};
    if (target) git_oid_tostr(head_sha, sizeof(head_sha), target);
    git_reference_free(head);

    const std::filesystem::path model = std::filesystem::absolute(model_path);
    std::error_code ec;
    const auto model_size = std::filesystem::file_size(model, ec);
    const auto model_time = std::filesystem::last_write_time(model, ec).time_since_epoch().count();
    const std::string rules = ignore_fingerprint(repo, root);
    const std::string grammar = grammar_fingerprint();
    std::string manifest = "schema=3;scope=head";
    manifest += ";model=" + model.string() + ";model_size=" + std::to_string(model_size) +
           ";model_mtime=" + std::to_string(static_cast<long long>(model_time)) +
           ";rules=" + rules + ";grammars=" + grammar +
           ";platform=" + platform + ";embedding=float32-native" +
           ";chunker=1500-v1;embed-input=source-v1;pooling=model-native;query=raw" +
           ";embed-batch=seq4-token-budget-v1";
    if (include_head) manifest += ";head=" + std::string(head_sha);
    return manifest;
}

std::string cache_config(const std::string& manifest)
{
    const auto marker = manifest.rfind(";head=");
    return marker == std::string::npos ? manifest : manifest.substr(0, marker);
}
}

GitTrace::GitTrace() : repo(nullptr), repo_path(nullptr), error(0), initialized(false), activate_chunkdb(true)
{
}

GitTrace::~GitTrace()
{
    delete[] repo_path;
    git_repository_free(repo);
    git_libgit2_shutdown();
}

bool GitTrace::init(const char* path = nullptr, bool activate_chunkdb = true)
{
    if(initialized) return true;

    if((!path || strlen(path) < 1)) 
    {
        std::cerr << "No path provided.\n";
        return false;
    }
    repo_path = new char[strlen(path) + 1];
    strcpy(repo_path, path);
    this->activate_chunkdb = activate_chunkdb;

    git_libgit2_init();
    error = git_repository_open(&repo, repo_path);
    if(error < 0)
    {
        const git_error* e = git_error_last();
        std::cerr << "Error opening repo: " << repo_path << "\n";
        return false;
    }
    const char* workdir = git_repository_workdir(repo);
    const std::filesystem::path repository_root = workdir && *workdir
        ? std::filesystem::path(workdir) : std::filesystem::path(repo_path);
    const std::filesystem::path ignore_path = repository_root / ".gittraceignore";
    std::ifstream ignore_file(ignore_path, std::ios::binary);
    if (ignore_file) {
        std::string rules((std::istreambuf_iterator<char>(ignore_file)),
                          std::istreambuf_iterator<char>());
        if (!rules.empty() && rules.back() != '\n') rules.push_back('\n');
        if (!rules.empty() && git_ignore_add_rule(repo, rules.c_str()) < 0) {
            const git_error* e = git_error_last();
            std::cerr << "Could not load " << ignore_path << ": "
                      << (e ? e->message : "invalid ignore rule") << '\n';
            return false;
        }
    }
    initialized = true;

    std::cout << "Succefully opening repo!\n";
    return true;
}

bool GitTrace::beginGitTrace(bool use_index_cache, bool force_rebuild)
{
    using Clock = std::chrono::steady_clock;
    if (!repo) {
        std::cerr << "Cannot read commits before initializing GitTrace\n";
        return false;
    }

    const char* workdir = git_repository_workdir(repo);
    const std::filesystem::path repository_root = workdir && *workdir
        ? std::filesystem::path(workdir) : std::filesystem::path(repo_path);
    const std::filesystem::path index_path = index_path_override.empty()
        ? repository_root / ".gittrace" / "index.sqlite"
        : std::filesystem::absolute(index_path_override);
    gittrace::index::WriterLock writer_lock;
    if (activate_chunkdb && use_index_cache) {
        std::string lock_error;
        if (!writer_lock.acquire(index_path.string(), 30000, lock_error)) {
            std::cerr << "Could not acquire index writer lock: " << lock_error << '\n';
            return false;
        }
    }
    std::string manifest;
    std::string config_manifest;
    if (activate_chunkdb && use_index_cache) {
        const auto manifest_started = Clock::now();
        manifest = cache_manifest(repo, repository_root, chunkdb.modelPath(), true);
        config_manifest = cache_config(manifest);
        metrics.manifest_ms += std::chrono::duration<double, std::milli>(
            Clock::now() - manifest_started).count();
    }
    bool cached_seed_loaded = false;
    bool incremental_walk = false;
    std::string prior_head;
    std::unordered_map<std::string, std::vector<std::size_t>> cached_chunk_indexes;
    if (activate_chunkdb && use_index_cache && !force_rebuild && !manifest.empty()) {
        std::vector<Chunk> cached_chunks;
        std::string cache_error;
        const auto cache_started = std::chrono::steady_clock::now();
        if (gittrace::index::load(index_path.string(), manifest, cached_chunks, cache_error)) {
            chunkdb.replaceChunks(std::move(cached_chunks));
            metrics.cache_load_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cache_started).count();
            metrics.cache_hit = true;
            metrics.chunks_created = chunkdb.getChunks().size();
            std::set<std::string> cached_file_states;
            for (const Chunk& chunk : chunkdb.getChunks()) {
                for (const GitOccurrence& occurrence : chunk.occurrences) {
                    std::string identity = occurrence.blob_oid;
                    identity.push_back('\0');
                    identity += occurrence.filepath;
                    cached_file_states.insert(std::move(identity));
                }
            }
            metrics.unique_file_versions = cached_file_states.size();
            std::cout << "Loaded " << metrics.chunks_created << " chunks from "
                      << index_path.string() << "\n";
            return true;
        }
        const std::string exact_error = cache_error;
        std::string stored_manifest;
        cache_error.clear();
        if (gittrace::index::load(index_path.string(), config_manifest, cached_chunks,
                                  cache_error, true, &stored_manifest)) {
            const auto head_marker = stored_manifest.rfind(";head=");
            const std::string old_head = head_marker == std::string::npos
                ? std::string() : stored_manifest.substr(head_marker + 6);
            prior_head = old_head;
            git_reference* current_ref = nullptr;
            git_oid current_oid{};
            git_oid old_oid{};
            const bool have_current = git_repository_head(&current_ref, repo) == 0 &&
                git_reference_target(current_ref) &&
                (current_oid = *git_reference_target(current_ref), true);
            if (current_ref) git_reference_free(current_ref);
            const bool have_old = !old_head.empty() && git_oid_fromstr(&old_oid, old_head.c_str()) == 0;
            incremental_walk = have_current && have_old &&
                git_graph_descendant_of(repo, &current_oid, &old_oid) == 1;

            chunkdb.replaceChunks(std::move(cached_chunks));
            auto& chunks = chunkdb.getMutableChunks();
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                if (!chunks[i].occurrences.empty()) {
                    const GitOccurrence& occurrence = chunks[i].occurrences.front();
                    std::string identity = occurrence.blob_oid;
                    identity.push_back('\0');
                    identity += occurrence.filepath;
                    cached_chunk_indexes[identity].push_back(i);
                }
                if (!incremental_walk) chunks[i].occurrences.clear();
            }
            cached_seed_loaded = true;
            metrics.incremental_update = incremental_walk;
            std::cout << (incremental_walk
                ? "Refreshing index from the previous HEAD...\n"
                : "Rebuilding occurrence map from cached chunks...\n");
        } else if (!exact_error.empty()) {
            std::cout << "Index cache miss: " << exact_error << '\n';
        }
        metrics.cache_load_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cache_started).count();
    }

    const auto history_started = Clock::now();
    git_revwalk* walker = nullptr;
    if (git_revwalk_new(&walker, repo) < 0) {
        std::cerr << "Could not create commit walker\n";
        return false;
    }
    git_revwalk_push_head(walker);
    if (incremental_walk) {
        git_oid old_oid{};
        if (git_oid_fromstr(&old_oid, prior_head.c_str()) == 0 &&
            git_revwalk_hide(walker, &old_oid) < 0) {
            incremental_walk = false;
            for (Chunk& chunk : chunkdb.getMutableChunks()) chunk.occurrences.clear();
        }
    }
    std::set<std::string> extensions;

    git_oid oid;
    std::unordered_map<std::string, std::size_t> seen_blobs;
    std::set<std::string> cached_states_seen;
    const auto head_marker = manifest.rfind(";head=");
    const std::string current_head_sha = head_marker == std::string::npos
        ? std::string() : manifest.substr(head_marker + 6);
    std::vector<Chunk>& existing_chunks = chunkdb.getMutableChunks();
    while(git_revwalk_next(&oid, walker) == 0)
    {
        ++metrics.commits_visited;
        git_commit* commit = nullptr;
        if(git_commit_lookup(&commit, repo, &oid) < 0) continue;

        // SHA
        char sha[41] = {0};
        git_oid_tostr(sha, sizeof(sha), &oid);

        // Mod and Author info
        git_time_t commit_time = git_commit_time(commit);
        const git_signature* auth = git_commit_author(commit);

        git_tree* tree = nullptr;
        if(git_commit_tree(&tree, commit) == 0)
        {
            char tree_sha_buf[41] = {0};
            git_oid_tostr(tree_sha_buf, sizeof(tree_sha_buf), git_tree_id(tree));
            struct Context
            {
                git_repository* repo;
                std::string curr_sha, tree_sha, a_name, a_email;
                int64_t timestamp;
                std::vector<Blob>* res;
                std::unordered_map<std::string, std::size_t>* processed_blobs;
                std::set<std::string>* extensions;
                std::unordered_map<std::string, std::vector<std::size_t>>* cached_chunks;
                std::vector<Chunk>* existing_chunks;
                std::set<std::string>* cached_states_seen;
                std::string current_head_sha;
            };

            Context context {repo, sha, tree_sha_buf,
                             auth && auth->name ? auth->name : "",
                             auth && auth->email ? auth->email : "",
                             static_cast<int64_t>(commit_time), &blobs, &seen_blobs, &extensions,
                             &cached_chunk_indexes, &existing_chunks, &cached_states_seen,
                             current_head_sha};

            auto tree_recursive = [](const char* root, const git_tree_entry* entry, void* payload) -> int 
            {
                if(git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
                {
                    return 0;
                }

                Context* wcontext = static_cast<Context*>(payload);
                
                // Generate the string representation of the current blob's OID
                const git_oid* entry_oid = git_tree_entry_id(entry);
                char blob_sha[41] = {0};
                git_oid_tostr(blob_sha, sizeof(blob_sha), entry_oid);
                std::string blob_hash(blob_sha);

                const std::string filepath = std::string(root) + git_tree_entry_name(entry);
                std::string blob_identity = blob_hash;
                blob_identity.push_back('\0');
                blob_identity += filepath;
                int ignored = 0;
                if (git_ignore_path_is_ignored(&ignored, wcontext->repo, filepath.c_str()) == 0 && ignored) {
                    return 0;
                }

                GitOccurrence occurrence;
                occurrence.commit_sha = wcontext->curr_sha;
                occurrence.tree_sha = wcontext->tree_sha;
                occurrence.filepath = filepath;
                occurrence.blob_oid = blob_hash;
                occurrence.commit_time = wcontext->timestamp;
                occurrence.author_name = wcontext->a_name;
                occurrence.author_email = wcontext->a_email;

                const auto cached = wcontext->cached_chunks->find(blob_identity);
                if (cached != wcontext->cached_chunks->end()) {
                    wcontext->cached_states_seen->insert(blob_identity);
                    for (const std::size_t chunk_index : cached->second) {
                        if (chunk_index >= wcontext->existing_chunks->size()) continue;
                        Chunk& chunk = wcontext->existing_chunks->at(chunk_index);
                        if (chunk.occurrences.empty() || occurrence.commit_sha == wcontext->current_head_sha) {
                            chunk.commit_sha = occurrence.commit_sha;
                            chunk.commit_time = occurrence.commit_time;
                        }
                        chunk.occurrences.push_back(occurrence);
                    }
                    return 0;
                }

                // Reuse content but retain every path/commit occurrence.
                const auto existing = wcontext->processed_blobs->find(blob_identity);
                if (existing != wcontext->processed_blobs->end()) {
                    wcontext->res->at(existing->second).occurrences.push_back(std::move(occurrence));
                    return 0;
                }

                git_blob* blob = nullptr;
                if(git_blob_lookup(&blob, wcontext->repo, entry_oid) == 0)
                {
                    if(!git_blob_is_binary(blob))
                    {
                        Blob b;
                        b.sha = wcontext->curr_sha;
                        b.blob_oid = blob_hash;
                        b.filepath = filepath;
                        b.time_mod = wcontext->timestamp;
                        b.author_name = wcontext->a_name;
                        b.author_email = wcontext->a_email;
                        b.occurrences.push_back(std::move(occurrence));

                        const unsigned char* content = static_cast<const unsigned char*>(git_blob_rawcontent(blob));
                        git_object_size_t size = git_blob_rawsize(blob);
                        b.raw_data.assign(content, content + size);

                        const auto dot = b.filepath.find_last_of('.');
                        const auto slash = b.filepath.find_last_of("/\\");
                        if (dot != std::string::npos && dot + 1 < b.filepath.size() &&
                            (slash == std::string::npos || dot > slash) && dot > (slash == std::string::npos ? 0 : slash + 1)) {
                            std::string extension = b.filepath.substr(dot + 1);
                            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                                return static_cast<char>(std::tolower(c));
                            });
                            wcontext->extensions->insert(std::move(extension));
                        }

                        const std::size_t index = wcontext->res->size();
                        wcontext->res->push_back(std::move(b));
                        wcontext->processed_blobs->emplace(std::move(blob_identity), index);
                    }

                    git_blob_free(blob);
                }

                return 0;
            };

            git_tree_walk(tree, GIT_TREEWALK_PRE, tree_recursive, &context);
            git_tree_free(tree);
        }
        git_commit_free(commit);
    }
    git_revwalk_free(walker);
    metrics.history_walk_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - history_started).count();
    metrics.unique_file_versions = (incremental_walk ? cached_chunk_indexes.size()
                                                     : cached_states_seen.size()) + seen_blobs.size();
    if (cached_seed_loaded) chunkdb.pruneChunksWithoutOccurrences();

    std::vector<std::string> extension_list(extensions.begin(), extensions.end());
    TreeSitter tree_sitter("GitTrace/rsc");
    const auto grammar_started = Clock::now();
    const bool prepared = tree_sitter.prepare(extension_list);
    metrics.grammar_prepare_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - grammar_started).count();
    if (activate_chunkdb && use_index_cache) {
        const auto manifest_started = Clock::now();
        manifest = cache_manifest(repo, repository_root, chunkdb.modelPath(), true);
        config_manifest = cache_config(manifest);
        metrics.manifest_ms += std::chrono::duration<double, std::milli>(
            Clock::now() - manifest_started).count();
    }
    const auto parsing_started = Clock::now();
    for(const auto& blob : blobs) 
    {
        if(tree_sitter.parse(blob.filepath, reinterpret_cast<const char*>(blob.raw_data.data()), blob.raw_data.size())) 
        {
            metrics.parsed_files++;
        } 
        else 
        {
            metrics.failed_files++;
        }
    }
    metrics.parsing_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - parsing_started).count();

    std::cout << "Tree-sitter parsed " << metrics.parsed_files << " unique file versions";
    if(metrics.failed_files) std::cout << "; " << metrics.failed_files << " unsupported or invalid";
    std::cout << "\n";
    if(!prepared) std::cerr << "One or more grammars were unavailable; see messages above\n";

    if (activate_chunkdb) {
        if (!blobs.empty()) {
            const auto model_started = Clock::now();
            if (!chunkdb.init()) {
                metrics.embedding_model_load_ms = std::chrono::duration<double, std::milli>(
                    Clock::now() - model_started).count();
                std::cerr << "Could not initialize embedding model; index was not rebuilt.\n";
                return false;
            }
            metrics.embedding_model_load_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - model_started).count();
        }
        metrics.embedding_backend = chunkdb.backendName();
        metrics.embedding_threads = chunkdb.embeddingThreads();
        metrics.embedding_pooling_type = chunkdb.poolingType();
        chunkify(tree_sitter);
        if (metrics.embedding_failures > 0) {
            std::cerr << "Embedding failed for " << metrics.embedding_failures
                      << " chunk(s); refusing to save or use an incomplete index.\n";
            return false;
        }
        if (use_index_cache && !manifest.empty()) {
            std::string store_error;
            const auto save_started = Clock::now();
            if (!gittrace::index::save(index_path.string(), manifest, chunkdb.getChunks(), store_error)) {
                std::cerr << "Warning: could not save index cache: " << store_error << '\n';
            }
            metrics.cache_save_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - save_started).count();
        }
    }
    return true;
}

std::vector<ChunkMatch> GitTrace::searchChunks(const std::string& query, std::size_t k,
                                              bool use_retrieval_instruction,
                                              RetrievalMode mode,
                                              bool expand_query_terms)
{
    if (!initialized || !activate_chunkdb) {
        std::cerr << "Cannot search chunks: embedding database is not active\n";
        return {};
    }
    const auto started = std::chrono::steady_clock::now();
    auto results = chunkdb.search(query, k, use_retrieval_instruction, mode, expand_query_terms);
    metrics.embedding_backend = chunkdb.backendName();
    metrics.embedding_threads = chunkdb.embeddingThreads();
    ++metrics.search_calls;
    metrics.query_search_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return results;
}

std::string GitTrace::indexStatus() const
{
    if (!repo || !repo_path) return "Repository is not initialized";
    const char* workdir = git_repository_workdir(repo);
    const std::filesystem::path root = workdir && *workdir
        ? std::filesystem::path(workdir) : std::filesystem::path(repo_path);
    const std::filesystem::path path = index_path_override.empty()
        ? root / ".gittrace" / "index.sqlite"
        : std::filesystem::absolute(index_path_override);
    const auto status = gittrace::index::inspect(path.string(),
        cache_manifest(repo, root, chunkdb.modelPath()));
    if (!status.exists) return "No local index at " + path.string() + " (" + status.detail + ")";
    return std::string(status.compatible ? "Index ready" : "Index stale") + ": " +
           std::to_string(status.chunks) + " chunks at " + path.string() +
           " (" + status.detail + ")";
}

void GitTrace::chunkify(const TreeSitter& tree_sitter)
{
    using Clock = std::chrono::steady_clock;
    const auto chunking_started = Clock::now();
    std::cout << "Creating chunks to send through embedder...\n";
    constexpr size_t MAX_CHUNK_BYTES = 1500;
    size_t total_chunks = 0;
    const double embedding_ms_before = metrics.chunk_embedding_ms;
    constexpr std::size_t MAX_PENDING_CHUNKS = 32;
    std::vector<ChunkEmbeddingInput> pending;
    pending.reserve(MAX_PENDING_CHUNKS);
    auto report_progress = [&](bool complete) {
        if (!progress_callback) return;
        GitTraceProgress progress;
        progress.embedding_inputs = metrics.embedding_inputs;
        progress.chunks_completed = total_chunks;
        progress.embedding_model_calls = metrics.embedding_model_calls;
        progress.embedding_failures = metrics.embedding_failures;
        progress.elapsed_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - chunking_started).count();
        progress.complete = complete;
        progress_callback(progress);
    };
    auto flush_pending = [&]() {
        if (pending.empty()) return;
        const auto embedding_started = Clock::now();
        metrics.embedding_inputs += pending.size();
        const ChunkBatchResult result = chunkdb.embedBatch(pending);
        metrics.embedding_model_calls += result.model_calls;
        metrics.embedding_failures += result.failed_chunks;
        metrics.chunk_embedding_ms += std::chrono::duration<double, std::milli>(
            Clock::now() - embedding_started).count();
        total_chunks += result.embedded_chunks;
        pending.clear();
        report_progress(false);
    };
    auto queue_chunk = [&](const std::string& content, const Blob& blob,
                           uint32_t start_line, uint32_t end_line,
                           const std::string& syntax_type, uint32_t ordinal) {
        pending.push_back({content, blob.sha, blob.filepath, start_line, end_line,
                           blob.time_mod, syntax_type, 0, ordinal, blob.occurrences});
        if (pending.size() >= MAX_PENDING_CHUNKS) flush_pending();
    };
    for (const Blob& blob : blobs) {
        if (blob.raw_data.empty()) continue;
        const char* source = reinterpret_cast<const char*>(blob.raw_data.data());
        const size_t length = blob.raw_data.size();
        const std::vector<SyntaxSpan> spans = tree_sitter.extractSpans(blob.filepath, source, length);

        if (spans.empty()) {
            for (size_t offset = 0; offset < length; offset += MAX_CHUNK_BYTES) {
                const size_t chunk_length = std::min(MAX_CHUNK_BYTES, length - offset);
                uint32_t start_line = 1;
                for (size_t i = 0; i < offset; ++i) if (source[i] == '\n') ++start_line;
                uint32_t end_line = start_line;
                for (size_t i = offset; i < offset + chunk_length; ++i) if (source[i] == '\n') ++end_line;
                if (chunk_length && source[offset + chunk_length - 1] == '\n' && end_line > start_line) --end_line;
                const std::string content(source + offset, chunk_length);
                queue_chunk(content, blob, start_line,
                    std::max(start_line, end_line), "fallback", 0);
            }
            continue;
        }

        uint32_t ordinal = 0;
        for (const SyntaxSpan& span : spans) {
            if (span.start_byte >= span.end_byte || span.end_byte > length) continue;
            size_t offset = span.start_byte;
            uint32_t start_line = span.start_row;
            while (offset < span.end_byte) {
                size_t end = std::min<size_t>(span.end_byte, offset + MAX_CHUNK_BYTES);
                if (end < span.end_byte) {
                    const size_t newline = std::string_view(source + offset, end - offset).rfind('\n');
                    if (newline != std::string_view::npos) end = offset + newline + 1;
                }
                if (end <= offset) end = std::min<size_t>(span.end_byte, offset + MAX_CHUNK_BYTES);

                const std::string content(source + offset, end - offset);
                uint32_t line_count = 0;
                for (size_t i = offset; i < end; ++i) if (source[i] == '\n') ++line_count;
                uint32_t end_line = start_line + line_count;
                if (line_count > 0 && source[end - 1] == '\n') --end_line;
                end_line = std::max(start_line, std::min(end_line, span.end_row));

                queue_chunk(content, blob, start_line, end_line, span.type, ordinal++);
                offset = end;
                start_line += line_count;
            }
        }
    }
    flush_pending();
    report_progress(true);
    metrics.chunking_ms = std::max(0.0, std::chrono::duration<double, std::milli>(
        Clock::now() - chunking_started).count() -
        (metrics.chunk_embedding_ms - embedding_ms_before));
    const auto lineage_started = Clock::now();
    chunkdb.alignHistoricalChunks();
    metrics.lineage_alignment_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - lineage_started).count();
    metrics.chunks_created = chunkdb.getChunks().size();
    std::cout << "Added " << total_chunks << " semantic chunk(s); ChunkDB now has "
              << metrics.chunks_created << ".\n";
}
