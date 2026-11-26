#ifndef CHUNKDB_H
#define CHUNKDB_H

#include "chunk.h"
#include <llama.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

enum class RetrievalMode { Vector, Keyword, Hybrid };

struct ChunkEmbeddingInput
{
    std::string input;
    std::string commit_sha;
    std::string filepath;
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    int64_t commit_time = 0;
    std::string syntax_type;
    uint32_t lineage_depth = 0;
    uint32_t syntax_ordinal = 0;
    std::vector<GitOccurrence> occurrences;
};

struct ChunkBatchResult
{
    std::size_t embedded_chunks = 0;
    std::size_t model_calls = 0;
    std::size_t failed_chunks = 0;
};

class ChunkDB
{
private:
    std::vector<Chunk> chunks;
    llama_model_params mparams;
    llama_context_params cparams;
    const llama_vocab* vcb = nullptr;
    llama_context* ctx = nullptr;
    llama_model* model = nullptr;
    std::string backend_name = "not initialized";
    uint32_t active_embedding_sequences = 1;

    uint32_t curr_seq_id;

    std::string model_path = "GitTrace/chunkdb/embedder/jina-embeddings-v2-base-code-Q4_K_M.gguf";

    void chunkify(const std::vector<float>& embedding, const std::string& input,
                  const std::string& commit_sha, const std::string& filepath,
                  uint32_t start_line, uint32_t end_line, int64_t commit_time,
                  const std::string& syntax_type, uint32_t lineage_depth,
                  uint32_t syntax_ordinal, const std::vector<GitOccurrence>& occurrences);
    void inferLineages();
    std::vector<llama_token> tokenize(const std::string& input, bool add_special);
    float cosineSimilarity(const std::vector<float>& v1, const std::vector<float>& v2);

public:
    ChunkDB();
    ~ChunkDB();

    bool init();
    void setModelPath(std::string path) { model_path = std::move(path); }
    const std::string& modelPath() const { return model_path; }
    enum llama_pooling_type poolingType() const {
        return ctx ? llama_pooling_type(ctx) : LLAMA_POOLING_TYPE_UNSPECIFIED;
    }
    const std::string& backendName() const { return backend_name; }
    int32_t embeddingThreads() const { return ctx ? llama_n_threads(ctx) : 0; }
    std::vector<float> embed(const std::string& input, bool add_special = true, bool store = true,
                             const std::string& commit_sha = {}, const std::string& filepath = {},
                             uint32_t start_line = 0, uint32_t end_line = 0,
                             int64_t commit_time = 0, const std::string& syntax_type = {},
                             uint32_t lineage_depth = 0, uint32_t syntax_ordinal = 0,
                             const std::vector<GitOccurrence>& occurrences = {});
    ChunkBatchResult embedBatch(const std::vector<ChunkEmbeddingInput>& inputs);
    void alignHistoricalChunks();
    const std::vector<Chunk>& getChunks() const { return chunks; }
    std::vector<Chunk>& getMutableChunks() { return chunks; }
    void replaceChunks(std::vector<Chunk> loaded_chunks);
    void pruneChunksWithoutOccurrences();

    std::vector<Chunk*> getSimilarChunks(const std::vector<float>& embedding, uint8_t k = 3);
    std::vector<ChunkMatch> search(const std::string& query, std::size_t k = 5,
                                   bool use_retrieval_instruction = false,
                                   RetrievalMode mode = RetrievalMode::Hybrid,
                                   bool expand_query_terms = false);
};

#endif
