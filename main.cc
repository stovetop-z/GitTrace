#include "GitTrace/gittrace.h"
#include "GitTrace/laya_client.h"

#include <cstdint>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <chrono>

#ifdef _WIN32
#include <io.h>
#define gittrace_dup _dup
#define gittrace_dup2 _dup2
#define gittrace_close _close
#define gittrace_fileno _fileno
#define gittrace_write _write
#else
#include <unistd.h>
#define gittrace_dup dup
#define gittrace_dup2 dup2
#define gittrace_close close
#define gittrace_fileno fileno
#define gittrace_write write
#endif

namespace
{
int saved_stdout_fd = -1;
std::vector<double> query_timings_ms;

struct QueryTimer
{
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    ~QueryTimer() {
        query_timings_ms.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
};

bool redirect_startup_output()
{
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);

    saved_stdout_fd = gittrace_dup(gittrace_fileno(stdout));
    if (saved_stdout_fd < 0) return false;
    if (!std::freopen("dev.log", "a", stdout)) {
        gittrace_dup2(saved_stdout_fd, gittrace_fileno(stdout));
        gittrace_close(saved_stdout_fd);
        saved_stdout_fd = -1;
        return false;
    }
    if (gittrace_dup2(gittrace_fileno(stdout), gittrace_fileno(stderr)) < 0) {
        gittrace_dup2(saved_stdout_fd, gittrace_fileno(stdout));
        gittrace_close(saved_stdout_fd);
        saved_stdout_fd = -1;
        return false;
    }
    return true;
}

void restore_console_output()
{
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    if (saved_stdout_fd >= 0) {
        gittrace_dup2(saved_stdout_fd, gittrace_fileno(stdout));
        gittrace_close(saved_stdout_fd);
        saved_stdout_fd = -1;
    }
}

void print_usage(const char* program)
{
    std::cout << "Usage: " << program << " [options] [repository-path]\n"
              << "\nOptions:\n"
              << "  -r, --repo PATH      Repository to scan (default: current directory)\n"
              << "      --no-embeddings  Skip model loading and embedding generation\n"
              << "      --list           Print each unique file version found\n"
              << "      --top-k N        Number of similar chunks per query (default: 5)\n"
              << "      --query-format F Embedding format: raw (default) or instructed\n"
              << "      --retrieval-mode M Search with vector, keyword, or hybrid (default)\n"
              << "      --query-expansion  Add weighted code-search synonyms (default: off)\n"
              << "      --query TEXT     Run one query after indexing; may be repeated\n"
              << "      --profile PATH   Write indexing counts and stage timings as JSON\n"
              << "      --rebuild-index  Ignore and replace the local cached index\n"
              << "      --index-status   Show whether the local index matches current inputs\n"
              << "      --index PATH     Set SQLite index file (default: REPO/.gittrace/index.sqlite)\n"
              << "      --embedding-model PATH  Select GGUF embedding model\n"
              << "      --laya-url URL   Laya reranker URL (default: http://127.0.0.1:8787/rerank)\n"
              << "      --no-laya        Show retrieval results without reranking\n"
              << "      --no-interactive  Finish after indexing without starting the query prompt\n"
              << "      --version        Show the build revision\n"
              << "  -h, --help           Show this help\n"
              << "\nExamples:\n"
              << "  " << program << " --repo /path/to/project --top-k 8\n"
              << "  " << program << " --repo /path/to/project --no-embeddings --list\n"
              << "  " << program << " /path/to/project\n";
}

void print_blob(const Blob& blob)
{
    const std::string short_sha = blob.sha.size() > 10 ? blob.sha.substr(0, 10) : blob.sha;
    std::cout << short_sha << "  " << blob.filepath << "  "
              << blob.author_name << " <" << blob.author_email << ">  "
              << static_cast<std::int64_t>(blob.time_mod) << '\n';
}

bool parse_top_k(const std::string& value, std::size_t& result)
{
    unsigned long long parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto conversion = std::from_chars(begin, end, parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != end || parsed == 0 ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    result = static_cast<std::size_t>(parsed);
    return true;
}

void print_match(const ChunkMatch& match, std::size_t rank,
                 const LayaMatch* laya_match = nullptr)
{
    const Chunk& chunk = *match.chunk;
    std::cout << '\n' << rank << ". " << chunk.filepath
              << ":" << chunk.start_line << "-" << chunk.end_line
              << "  score=" << std::fixed << std::setprecision(4)
              << match.retrieval_score
              << "  cosine=" << match.raw_cosine_similarity
              << "  lineage-adjusted=" << match.similarity;
    if (laya_match) {
        std::cout << "  relevance=" << laya_match->relevance
                  << " (confidence=" << std::setprecision(3) << laya_match->confidence << ')';
    }
    std::cout << "  commit=" << chunk.commit_sha << '\n';
    std::cout << chunk.input;
    if (chunk.input.empty() || chunk.input.back() != '\n') std::cout << '\n';
}

void run_query(GitTrace& trace, const std::string& query, std::size_t top_k,
               bool use_laya, const std::string& laya_url,
               bool use_retrieval_instruction, RetrievalMode retrieval_mode,
               bool expand_query_terms, LayaClient& laya)
{
    QueryTimer query_timer;
    const char* mode_name = retrieval_mode == RetrievalMode::Keyword ? "keyword"
        : retrieval_mode == RetrievalMode::Vector ? "vector" : "hybrid";
    const std::size_t candidate_k = use_laya
        ? (top_k >= 16 ? 32 : top_k * 2)
        : top_k;
    const std::vector<ChunkMatch> matches = trace.searchChunks(
        query, candidate_k, use_retrieval_instruction, retrieval_mode,
        expand_query_terms);
    if (matches.empty()) {
        std::cout << "No matching chunks found.\n";
        return;
    }

    if (use_laya) {
        std::vector<LayaMatch> reranked;
        std::string error;
        std::cout << "Reranker assessing " << matches.size() << " candidates...\n";
        if (laya.rerank(query, matches, reranked, error)) {
            std::size_t accepted = 0;
            for (const LayaMatch& match : reranked) if (match.accepted) ++accepted;
            std::cout << "Reranker accepted " << accepted << '/' << reranked.size()
                      << " candidates as relevant.\n";
            if (accepted == 0) {
                std::cout << "No chunks passed the relevance filter.\n";
                return;
            }
            std::size_t rank = 0;
            for (const LayaMatch& match : reranked) {
                if (!match.accepted) continue;
                print_match(match.match, ++rank, &match);
                if (rank == top_k) break;
            }
            return;
        }
        std::cerr << "Warning: could not reach the reranker at " << laya_url
                  << " (" << error << "). Showing " << mode_name
                  << " results without reranking.\n";
    }

    for (std::size_t i = 0; i < std::min(top_k, matches.size()); ++i) {
        print_match(matches[i], i + 1);
    }
}

void run_query_prompt(GitTrace& trace, std::size_t top_k,
                      bool use_laya, const std::string& laya_url,
                      bool use_retrieval_instruction, RetrievalMode retrieval_mode,
                      bool expand_query_terms)
{
    LayaClient laya(laya_url);
    const char* mode_name = retrieval_mode == RetrievalMode::Keyword ? "keyword"
        : retrieval_mode == RetrievalMode::Vector ? "vector" : "hybrid";
    std::cout << "Retrieval mode: " << mode_name << '\n';
    std::cout << "Query expansion: " << (expand_query_terms ? "on" : "off") << '\n';
    std::string query;
    while (true) {
        std::cout << "query> " << std::flush;
        if (!std::getline(std::cin, query)) break;
        if (query == ":q" || query == ":quit" || query == "exit") break;
        if (query == ":help") {
            std::cout << "Type a natural-language or code query. Use :q to quit.\n";
            continue;
        }
        if (query.empty()) continue;
        run_query(trace, query, top_k, use_laya, laya_url,
                  use_retrieval_instruction, retrieval_mode, expand_query_terms, laya);
    }
}

void run_one_shot_queries(GitTrace& trace, const std::vector<std::string>& queries,
                          std::size_t top_k, bool use_laya, const std::string& laya_url,
                          bool use_retrieval_instruction, RetrievalMode retrieval_mode,
                          bool expand_query_terms)
{
    LayaClient laya(laya_url);
    const char* mode_name = retrieval_mode == RetrievalMode::Keyword ? "keyword"
        : retrieval_mode == RetrievalMode::Vector ? "vector" : "hybrid";
    std::cout << "Retrieval mode: " << mode_name << '\n';
    std::cout << "Query expansion: " << (expand_query_terms ? "on" : "off") << '\n';
    for (const std::string& query : queries) {
        std::cout << "\nQuery: " << query << '\n';
        run_query(trace, query, top_k, use_laya, laya_url,
                  use_retrieval_instruction, retrieval_mode, expand_query_terms, laya);
    }
}

std::string json_escape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    const char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += hex[(c >> 4) & 0xf];
                    escaped += hex[c & 0xf];
                } else escaped += static_cast<char>(c);
        }
    }
    return escaped;
}

bool write_profile(const std::string& path, const std::string& repository,
                   double init_ms, const GitTraceMetrics& metrics,
                   const std::vector<double>& query_timings)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n"
        << "  \"repository\": \"" << json_escape(repository) << "\",\n"
        << "  \"init_ms\": " << init_ms << ",\n"
        << "  \"history_walk_ms\": " << metrics.history_walk_ms << ",\n"
        << "  \"embedding_model_load_ms\": " << metrics.embedding_model_load_ms << ",\n"
        << "  \"grammar_prepare_ms\": " << metrics.grammar_prepare_ms << ",\n"
        << "  \"parsing_ms\": " << metrics.parsing_ms << ",\n"
        << "  \"chunk_embedding_ms\": " << metrics.chunk_embedding_ms << ",\n"
        << "  \"chunking_ms\": " << metrics.chunking_ms << ",\n"
        << "  \"lineage_alignment_ms\": " << metrics.lineage_alignment_ms << ",\n"
        << "  \"query_search_ms\": " << metrics.query_search_ms << ",\n"
        << "  \"embedding_backend\": \"" << json_escape(metrics.embedding_backend) << "\",\n"
        << "  \"embedding_threads\": " << metrics.embedding_threads << ",\n"
        << "  \"cache_load_ms\": " << metrics.cache_load_ms << ",\n"
        << "  \"cache_save_ms\": " << metrics.cache_save_ms << ",\n"
        << "  \"manifest_ms\": " << metrics.manifest_ms << ",\n"
        << "  \"cache_hit\": " << (metrics.cache_hit ? "true" : "false") << ",\n"
        << "  \"incremental_update\": " << (metrics.incremental_update ? "true" : "false") << ",\n"
        << "  \"commits_visited\": " << metrics.commits_visited << ",\n"
        << "  \"unique_file_versions\": " << metrics.unique_file_versions << ",\n"
        << "  \"parsed_files\": " << metrics.parsed_files << ",\n"
        << "  \"failed_files\": " << metrics.failed_files << ",\n"
        << "  \"chunks_created\": " << metrics.chunks_created << ",\n"
        << "  \"embedding_model_path\": \"" << json_escape(metrics.embedding_model_path) << "\",\n"
        << "  \"embedding_pooling_type\": " << metrics.embedding_pooling_type << ",\n"
        << "  \"embedding_inputs\": " << metrics.embedding_inputs << ",\n"
        << "  \"embedding_model_calls\": " << metrics.embedding_model_calls << ",\n"
        << "  \"embedding_failures\": " << metrics.embedding_failures << ",\n"
        << "  \"search_calls\": " << metrics.search_calls << ",\n"
        << "  \"query_total_ms\": [";
    for (std::size_t i = 0; i < query_timings.size(); ++i) {
        if (i) out << ", ";
        out << query_timings[i];
    }
    out << "]\n}\n";
    return out.good();
}
}

int main(int argc, char* argv[])
{
    std::string repository_path = ".";
    bool repo_was_set = false;
    bool embeddings_enabled = true;
    bool list_blobs = false;
    bool interactive_enabled = true;
    bool use_laya = true;
    bool use_retrieval_instruction = false;
    bool rebuild_index = false;
    bool index_status_requested = false;
    RetrievalMode retrieval_mode = RetrievalMode::Hybrid;
    bool expand_query_terms = false;
    std::size_t top_k = 5;
    std::vector<std::string> queries;
    std::string profile_path;
    std::string index_path;
    const char* configured_model_path = std::getenv("GITTRACE_EMBEDDING_MODEL");
    std::string embedding_model_path = configured_model_path && *configured_model_path
        ? configured_model_path
        : "GitTrace/chunkdb/embedder/jina-embeddings-v2-base-code-Q4_K_M.gguf";
    const char* configured_laya_url = std::getenv("GITTRACE_LAYA_URL");
    std::string laya_url = configured_laya_url && *configured_laya_url
        ? configured_laya_url : "http://127.0.0.1:8787/rerank";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--version") {
#ifdef GITTRACE_BUILD_ID
            std::cout << "GitTrace " << GITTRACE_BUILD_ID << '\n';
#else
            std::cout << "GitTrace unknown\n";
#endif
            return 0;
        }
        if (arg == "-r" || arg == "--repo") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a repository path.\n";
                print_usage(argv[0]);
                return 2;
            }
            if (repo_was_set) {
                std::cerr << "Error: repository path was specified more than once.\n";
                return 2;
            }
            repository_path = argv[++i];
            repo_was_set = true;
            continue;
        }
        if (arg == "--no-embeddings") {
            embeddings_enabled = false;
            continue;
        }
        if (arg == "--list") {
            list_blobs = true;
            continue;
        }
        if (arg == "--no-interactive") {
            interactive_enabled = false;
            continue;
        }
        if (arg == "--rebuild-index") {
            rebuild_index = true;
            continue;
        }
        if (arg == "--index-status") {
            index_status_requested = true;
            continue;
        }
        if (arg == "--index") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::cerr << "Error: --index requires a file path.\n";
                return 2;
            }
            index_path = argv[++i];
            continue;
        }
        if (arg == "--embedding-model") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::cerr << "Error: --embedding-model requires a GGUF file path.\n";
                return 2;
            }
            embedding_model_path = argv[++i];
            continue;
        }
        if (arg == "--no-laya") {
            use_laya = false;
            continue;
        }
        if (arg == "--query-format") {
            if (i + 1 >= argc || (std::string(argv[i + 1]) != "instructed" &&
                                  std::string(argv[i + 1]) != "raw")) {
                std::cerr << "Error: --query-format must be 'instructed' or 'raw'.\n";
                return 2;
            }
            use_retrieval_instruction = std::string(argv[++i]) == "instructed";
            continue;
        }
        if (arg == "--retrieval-mode") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --retrieval-mode requires vector, keyword, or hybrid.\n";
                return 2;
            }
            const std::string mode = argv[++i];
            if (mode == "vector") retrieval_mode = RetrievalMode::Vector;
            else if (mode == "keyword") retrieval_mode = RetrievalMode::Keyword;
            else if (mode == "hybrid") retrieval_mode = RetrievalMode::Hybrid;
            else {
                std::cerr << "Error: --retrieval-mode must be vector, keyword, or hybrid.\n";
                return 2;
            }
            continue;
        }
        if (arg == "--query-expansion") {
            expand_query_terms = true;
            continue;
        }
        if (arg == "--query") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::cerr << "Error: --query requires non-empty text.\n";
                return 2;
            }
            queries.emplace_back(argv[++i]);
            continue;
        }
        if (arg == "--profile") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::cerr << "Error: --profile requires a file path.\n";
                return 2;
            }
            profile_path = argv[++i];
            continue;
        }
        if (arg == "--laya-url") {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::cerr << "Error: --laya-url requires a URL.\n";
                return 2;
            }
            laya_url = argv[++i];
            continue;
        }
        if (arg == "--top-k") {
            if (i + 1 >= argc || !parse_top_k(argv[i + 1], top_k)) {
                std::cerr << "Error: --top-k requires a positive integer.\n";
                return 2;
            }
            ++i;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option: " << arg << '\n';
            print_usage(argv[0]);
            return 2;
        }
        if (repo_was_set) {
            std::cerr << "Error: use either --repo PATH or a positional repository path, not both.\n";
            return 2;
        }
        repository_path = arg;
        repo_was_set = true;
    }

    if (!queries.empty() && !embeddings_enabled) {
        std::cerr << "Error: --query requires embeddings to be enabled.\n";
        return 2;
    }

    const std::filesystem::path normalized_path = std::filesystem::absolute(repository_path);
    std::cout << "Loading...\n" << std::flush;
    if (!redirect_startup_output()) {
        std::cerr << "Could not open dev.log for writing.\n";
        return 1;
    }
    std::cout << "Opening repository: " << normalized_path.string() << '\n';

    GitTrace trace;
    trace.setEmbeddingModel(embedding_model_path);
    const auto init_started = std::chrono::steady_clock::now();
    if (!trace.init(normalized_path.string().c_str(), embeddings_enabled)) {
        std::cerr << "Failed to initialize GitTrace. Check the repository path";
        if (embeddings_enabled) std::cerr << " and embedding model configuration";
        std::cerr << ".\n";
        restore_console_output();
        std::cout << "Initialization failed; see dev.log.\n";
        return 1;
    }
    if (index_status_requested) {
        if (!index_path.empty()) trace.setIndexPath(index_path);
        const std::string status = trace.indexStatus();
        restore_console_output();
        std::cout << status << '\n';
        return 0;
    }
    if (!index_path.empty()) trace.setIndexPath(index_path);
    const double init_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - init_started).count();

    std::cout << "Reading Git history and parsing supported files...\n";
    trace.setProgressCallback([](const GitTraceProgress& progress) {
        if (saved_stdout_fd < 0) return;
        const double elapsed_seconds = progress.elapsed_ms / 1000.0;
        const double chunks_per_second = elapsed_seconds <= 0.0 ? 0.0
            : static_cast<double>(progress.chunks_completed) / elapsed_seconds;
        std::ostringstream line;
        line << '\r' << (progress.complete ? "Embedding complete: " : "Embedding: ")
             << progress.chunks_completed << " chunks from "
             << progress.embedding_inputs << " inputs in "
             << progress.embedding_model_calls << " model calls"
             << " | failures " << progress.embedding_failures
             << " | " << std::fixed << std::setprecision(1) << chunks_per_second
             << " chunks/s | " << std::setprecision(1) << elapsed_seconds << " s";
        if (progress.complete) line << '\n';
        const std::string message = line.str();
        gittrace_write(saved_stdout_fd, message.data(),
                       static_cast<unsigned int>(message.size()));
    });
    if (!trace.beginGitTrace(!list_blobs, rebuild_index)) {
        restore_console_output();
        if (!profile_path.empty() &&
            !write_profile(profile_path, normalized_path.string(), init_ms,
                           trace.getMetrics(), query_timings_ms)) {
            std::cerr << "Could not write profile output to " << profile_path << ".\n";
        }
        std::cerr << "GitTrace could not build or load the index; see dev.log for details.\n";
        return 1;
    }
    const GitTraceMetrics& metrics = trace.getMetrics();

    if (list_blobs) {
        std::cout << "\nUnique historical file versions:\n";
        for (const Blob& blob : trace.blobs) print_blob(blob);
    }
    std::cout << "GitTrace finished. Indexed " << trace.getMetrics().unique_file_versions
              << " unique text file state(s).";
    if (trace.getMetrics().incremental_update)
        std::cout << " Updated from new commits without re-embedding known states.";
    if (trace.getMetrics().cache_hit)
        std::cout << " Reused " << trace.getMetrics().chunks_created << " cached chunks.";
    if (!embeddings_enabled) std::cout << " Embeddings were skipped.";
    std::cout << '\n';
    restore_console_output();
    if (embeddings_enabled && !queries.empty()) {
        run_one_shot_queries(trace, queries, top_k, use_laya, laya_url,
                             use_retrieval_instruction, retrieval_mode,
                             expand_query_terms);
    } else if (embeddings_enabled && interactive_enabled) {
        run_query_prompt(trace, top_k, use_laya, laya_url, use_retrieval_instruction,
                         retrieval_mode, expand_query_terms);
    } else {
        std::cout << "Loading complete. See dev.log for details.\n";
    }
    if (metrics.embedding_threads > 0) {
        std::cout << "Embedding backend: " << metrics.embedding_backend
                  << " (" << metrics.embedding_threads << " threads).\n";
    }
    if (!profile_path.empty() &&
        !write_profile(profile_path, normalized_path.string(), init_ms,
                       trace.getMetrics(), query_timings_ms)) {
        std::cerr << "Could not write profile output to " << profile_path << ".\n";
        return 1;
    }
    return 0;
}
