#include "GitTrace/gittrace.h"
#include "GitTrace/laya_client.h"

#include <cstdint>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

#ifdef _WIN32
#include <io.h>
#define gittrace_dup _dup
#define gittrace_dup2 _dup2
#define gittrace_close _close
#define gittrace_fileno _fileno
#else
#include <unistd.h>
#define gittrace_dup dup
#define gittrace_dup2 dup2
#define gittrace_close close
#define gittrace_fileno fileno
#endif

namespace
{
int saved_stdout_fd = -1;

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
              << "      --laya-url URL   Laya reranker URL (default: http://127.0.0.1:8787/rerank)\n"
              << "      --no-laya        Show vector results without Laya reranking\n"
              << "      --no-interactive  Finish after indexing without starting the query prompt\n"
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
    const std::string short_sha = chunk.commit_sha.size() > 10
        ? chunk.commit_sha.substr(0, 10) : chunk.commit_sha;
    std::cout << '\n' << rank << ". " << chunk.filepath
              << ":" << chunk.start_line << "-" << chunk.end_line
              << "  similarity=" << std::fixed << std::setprecision(4)
              << match.similarity;
    if (laya_match) {
        std::cout << "  relevance=" << laya_match->relevance
                  << " (confidence=" << std::setprecision(3) << laya_match->confidence << ')';
    }
    std::cout << "  commit=" << short_sha << '\n';
    std::cout << chunk.input;
    if (chunk.input.empty() || chunk.input.back() != '\n') std::cout << '\n';
}

void run_query_prompt(GitTrace& trace, std::size_t top_k,
                      bool use_laya, const std::string& laya_url)
{
    LayaClient laya(laya_url);
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

        const std::size_t candidate_k = use_laya
            ? (top_k >= 16 ? 32 : top_k * 2)
            : top_k;
        const std::vector<ChunkMatch> matches = trace.searchChunks(query, candidate_k);
        if (matches.empty()) {
            std::cout << "No matching chunks found.\n";
            continue;
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
                    continue;
                }
                std::size_t rank = 0;
                for (const LayaMatch& match : reranked) {
                    if (!match.accepted) continue;
                    print_match(match.match, ++rank, &match);
                    if (rank == top_k) break;
                }
                continue;
            }
            std::cerr << "Warning: could not reach the reranker at " << laya_url
                      << " (" << error << "). Showing vector results without reranking.\n";
        }

        for (std::size_t i = 0; i < std::min(top_k, matches.size()); ++i) {
            print_match(matches[i], i + 1);
        }
    }
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
    std::size_t top_k = 5;
    const char* configured_laya_url = std::getenv("GITTRACE_LAYA_URL");
    std::string laya_url = configured_laya_url && *configured_laya_url
        ? configured_laya_url : "http://127.0.0.1:8787/rerank";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
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
        if (arg == "--no-laya") {
            use_laya = false;
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

    const std::filesystem::path normalized_path = std::filesystem::absolute(repository_path);
    std::cout << "Loading...\n" << std::flush;
    if (!redirect_startup_output()) {
        std::cerr << "Could not open dev.log for writing.\n";
        return 1;
    }
    std::cout << "Opening repository: " << normalized_path.string() << '\n';

    GitTrace trace;
    if (!trace.init(normalized_path.string().c_str(), embeddings_enabled)) {
        std::cerr << "Failed to initialize GitTrace. Check the repository path";
        if (embeddings_enabled) std::cerr << " and embedding model configuration";
        std::cerr << ".\n";
        restore_console_output();
        std::cout << "Initialization failed; see dev.log.\n";
        return 1;
    }

    std::cout << "Reading Git history and parsing supported files...\n";
    trace.beginGitTrace();

    if (list_blobs) {
        std::cout << "\nUnique historical file versions:\n";
        for (const Blob& blob : trace.blobs) print_blob(blob);
    }
    std::cout << "GitTrace finished. Collected " << trace.blobs.size()
              << " unique text blob version(s).";
    if (!embeddings_enabled) std::cout << " Embeddings were skipped.";
    std::cout << '\n';
    restore_console_output();
    if (embeddings_enabled && interactive_enabled) {
        run_query_prompt(trace, top_k, use_laya, laya_url);
    } else {
        std::cout << "Loading complete. See dev.log for details.\n";
    }
    return 0;
}
