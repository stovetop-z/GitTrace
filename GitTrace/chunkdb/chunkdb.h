#ifndef CHUNKDB_H
#define CHUNKDB_H

#include "chunk.h"
#include <llama.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class ChunkDB
{
private:
    std::vector<Chunk> chunks;
    llama_model_params mparams;
    llama_context_params cparams;
    const llama_vocab* vcb = nullptr;
    llama_context* ctx = nullptr;
    llama_model* model = nullptr;

    uint32_t curr_seq_id;

    const char* model_path = "GitTrace/chunkdb/embedder/bge-code-v1-q8_0.gguf";

    void chunkify(const std::vector<float>& embedding, const std::string& input,
                  const std::string& commit_sha, const std::string& filepath,
                  uint32_t start_line, uint32_t end_line);
    std::vector<llama_token> tokenize(const std::string& input, bool add_special);
    float cosineSimilarity(const std::vector<float>& v1, const std::vector<float>& v2);

public:
    ChunkDB();
    ~ChunkDB();

    bool init();
    std::vector<float> embed(const std::string& input, bool add_special = true, bool store = true,
                             const std::string& commit_sha = {}, const std::string& filepath = {},
                             uint32_t start_line = 0, uint32_t end_line = 0);

    std::vector<Chunk*> getSimilarChunks(const std::vector<float>& embedding, uint8_t k = 3);
    std::vector<ChunkMatch> search(const std::string& query, std::size_t k = 5);
};

#endif
