#ifndef GITTRACE_H
#define GITTRACE_H

#include <git2.h>
#include "chunkdb/chunkdb.h"
#include "blob/blob.h"
#include <string>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

struct GitTraceProgress
{
    uint64_t embedding_inputs = 0;
    uint64_t chunks_completed = 0;
    uint64_t embedding_model_calls = 0;
    uint64_t embedding_failures = 0;
    double elapsed_ms = 0.0;
    bool complete = false;
};

class TreeSitter;

struct GitTraceMetrics
{
    uint64_t commits_visited = 0;
    uint64_t unique_file_versions = 0;
    uint64_t parsed_files = 0;
    uint64_t failed_files = 0;
    uint64_t chunks_created = 0;
    uint64_t embedding_inputs = 0;
    uint64_t embedding_model_calls = 0;
    uint64_t embedding_failures = 0;
    std::string embedding_model_path;
    int32_t embedding_pooling_type = LLAMA_POOLING_TYPE_UNSPECIFIED;
    uint64_t search_calls = 0;
    int32_t embedding_threads = 0;
    std::string embedding_backend = "not initialized";
    bool cache_hit = false;
    bool incremental_update = false;
    double history_walk_ms = 0.0;
    double embedding_model_load_ms = 0.0;
    double grammar_prepare_ms = 0.0;
    double parsing_ms = 0.0;
    double chunk_embedding_ms = 0.0;
    double chunking_ms = 0.0;
    double lineage_alignment_ms = 0.0;
    double query_search_ms = 0.0;
    double cache_load_ms = 0.0;
    double cache_save_ms = 0.0;
    double manifest_ms = 0.0;
};

class GitTrace
{
private:
    git_repository* repo;
    ChunkDB chunkdb;
    char* repo_path;
    int error;
    bool initialized;
    bool activate_chunkdb;
    std::string index_path_override;
    GitTraceMetrics metrics;
    std::function<void(const GitTraceProgress&)> progress_callback;

    void chunkify(const TreeSitter& tree_sitter);

public:
    std::vector<Blob> blobs;

    GitTrace();
    ~GitTrace();

    bool init(const char* path, bool activate_chunkdb);
    bool beginGitTrace(bool use_index_cache = true, bool force_rebuild = false);
    std::vector<ChunkMatch> searchChunks(const std::string& query, std::size_t k = 5,
                                         bool use_retrieval_instruction = false,
                                         RetrievalMode mode = RetrievalMode::Hybrid,
                                         bool expand_query_terms = false);

    std::string lastError();
    const GitTraceMetrics& getMetrics() const { return metrics; }
    std::string indexStatus() const;
    void setIndexPath(const std::string& path) { index_path_override = path; }
    void setEmbeddingModel(const std::string& path) {
        chunkdb.setModelPath(path);
        metrics.embedding_model_path = path;
    }
    void setProgressCallback(std::function<void(const GitTraceProgress&)> callback) {
        progress_callback = std::move(callback);
    }
};
#endif
