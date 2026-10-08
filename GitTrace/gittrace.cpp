#include "gittrace.h"
#include "utils/treesitter.h"
#include <cstring>
#include <unordered_set>
#include <iostream>
#include <set>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

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
    if(activate_chunkdb && !chunkdb.init()) return false;

    initialized = true;

    std::cout << "Succefully opening repo!\n";
    return true;
}

void GitTrace::beginGitTrace()
{
    if (!repo) {
        std::cerr << "Cannot read commits before initializing GitTrace\n";
        return;
    }

    git_revwalk* walker = nullptr;
    if (git_revwalk_new(&walker, repo) < 0) {
        std::cerr << "Could not create commit walker\n";
        return;
    }
    git_revwalk_push_head(walker);
    std::set<std::string> extensions;

    git_oid oid;
    std::unordered_set<std::string> seen_blobs;
    while(git_revwalk_next(&oid, walker) == 0)
    {
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
            struct Context
            {
                git_repository* repo;
                std::string curr_sha, a_name, a_email;
                int64_t timestamp;
                std::vector<Blob>* res;
                std::unordered_set<std::string>* processed_blobs; 
                std::set<std::string>* extensions;
            };

            Context context {repo, sha, auth->name, auth->email, static_cast<int64_t>(commit_time), &blobs, &seen_blobs, &extensions};

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
                int ignored = 0;
                if (git_ignore_path_is_ignored(&ignored, wcontext->repo, filepath.c_str()) == 0 && ignored) {
                    return 0;
                }

                // Skip this file if we have already processed this exact byte-for-byte state
                if (wcontext->processed_blobs->find(blob_hash) != wcontext->processed_blobs->end()) 
                {
                    return 0; 
                }
                wcontext->processed_blobs->insert(blob_hash);

                git_blob* blob = nullptr;
                if(git_blob_lookup(&blob, wcontext->repo, entry_oid) == 0)
                {
                    if(!git_blob_is_binary(blob))
                    {
                        Blob b;
                        b.sha = wcontext->curr_sha;
                        b.filepath = filepath;
                        b.time_mod = wcontext->timestamp;
                        b.author_name = wcontext->a_name;
                        b.author_email = wcontext->a_email;

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

                        wcontext->res->push_back(std::move(b));
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

    std::vector<std::string> extension_list(extensions.begin(), extensions.end());
    TreeSitter tree_sitter("GitTrace/rsc");
    const bool prepared = tree_sitter.prepare(extension_list);
    size_t parsed = 0;
    size_t failed = 0;
    for(const auto& blob : blobs) 
    {
        if(tree_sitter.parse(blob.filepath, reinterpret_cast<const char*>(blob.raw_data.data()), blob.raw_data.size())) 
        {
            parsed++;
        } 
        else 
        {
            failed++;
        }
    }

    std::cout << "Tree-sitter parsed " << parsed << " unique file versions";
    if(failed) std::cout << "; " << failed << " unsupported or invalid";
    std::cout << "\n";
    if(!prepared) std::cerr << "One or more grammars were unavailable; see messages above\n";

    if (activate_chunkdb) chunkify(tree_sitter);
}

std::vector<ChunkMatch> GitTrace::searchChunks(const std::string& query, std::size_t k)
{
    if (!initialized || !activate_chunkdb) {
        std::cerr << "Cannot search chunks: embedding database is not active\n";
        return {};
    }
    return chunkdb.search(query, k);
}

void GitTrace::chunkify(const TreeSitter& tree_sitter)
{
    std::cout << "Creating chunks to send through embedder...\n";
    constexpr size_t MAX_FALLBACK_BYTES = 1500;
    size_t total_chunks = 0;
    for (const Blob& blob : blobs) {
        if (blob.raw_data.empty()) continue;
        const char* source = reinterpret_cast<const char*>(blob.raw_data.data());
        const size_t length = blob.raw_data.size();
        const std::vector<SyntaxSpan> spans = tree_sitter.extractSpans(blob.filepath, source, length);

        if (spans.empty()) {
            for (size_t offset = 0; offset < length; offset += MAX_FALLBACK_BYTES) {
                const size_t chunk_length = std::min(MAX_FALLBACK_BYTES, length - offset);
                uint32_t start_line = 1;
                for (size_t i = 0; i < offset; ++i) if (source[i] == '\n') ++start_line;
                uint32_t end_line = start_line;
                for (size_t i = offset; i < offset + chunk_length; ++i) if (source[i] == '\n') ++end_line;
                if (chunk_length && source[offset + chunk_length - 1] == '\n' && end_line > start_line) --end_line;
                const std::string content(source + offset, chunk_length);
                const auto embedding = chunkdb.embed(content, true, true, blob.sha, blob.filepath,
                                                     start_line, std::max(start_line, end_line),
                                                     blob.time_mod, "fallback", 0, 0);
                if (!embedding.empty()) ++total_chunks;
            }
            continue;
        }

        uint32_t ordinal = 0;
        for (const SyntaxSpan& span : spans) {
            if (span.start_byte >= span.end_byte || span.end_byte > length) continue;
            const std::string content(source + span.start_byte, span.end_byte - span.start_byte);
            const auto embedding = chunkdb.embed(content, true, true, blob.sha, blob.filepath,
                                                 span.start_row, span.end_row, blob.time_mod,
                                                 span.type, 0, ordinal++);
            if (!embedding.empty()) ++total_chunks;
        }
    }
    chunkdb.alignHistoricalChunks();
    std::cout << "Indexed " << total_chunks << " semantic chunks into ChunkDB.\n";
}
