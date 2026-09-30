#ifndef CHUNKDB_H
#define CHUNKDB_H

#include "chunk.h"
#include <llama.h>
#include <cstdint>
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

    void chunkify(const std::vector<float>& embedding, const std::string& input);
    std::vector<llama_token> tokenize(const std::string& input, bool add_special);
    float cosineSimilarity(const std::vector<float>& v1, const std::vector<float>& v2);

public:
    ChunkDB();
    ~ChunkDB();

    bool init();
    std::vector<float> embed(const std::string& input, bool add_special = true, bool store = true);

    std::vector<Chunk*> getSimilarChunks(const std::vector<float>& embedding, uint8_t k = 3);
};

#endif
