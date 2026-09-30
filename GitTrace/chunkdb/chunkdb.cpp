#include "chunkdb.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

ChunkDB::ChunkDB() : curr_seq_id(0) {}

ChunkDB::~ChunkDB()
{
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
}

bool ChunkDB::init()
{
    llama_backend_init();

    mparams = llama_model_default_params();
    cparams = llama_context_default_params();
    cparams.embeddings = true;
    cparams.n_ctx = 2048;

    model = llama_model_load_from_file(model_path, mparams);
    if(model == nullptr)
    {
        std::cerr << "Error: failed to load model at " << model_path << '\n';
        return false;
    }

    vcb = llama_model_get_vocab(model);
    ctx = llama_init_from_model(model, cparams);
    if(ctx == nullptr)
    {
        std::cerr << "Error: failed to create llama context\n";
        llama_model_free(model);
        model = nullptr;
        return false;
    }
    
    return true;
}

std::vector<llama_token> ChunkDB::tokenize(const std::string& input, bool add_special)
{
    if(input.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max() - 8))
    {
        return {};
    }

    int32_t capacity = static_cast<int32_t>(input.size()) + 8;
    std::vector<llama_token> tokens(static_cast<std::size_t>(capacity));
    int32_t count = llama_tokenize(vcb, input.c_str(), static_cast<int32_t>(input.size()),
                                   tokens.data(), capacity, add_special, true);
    if(count < 0)
    {
        if(count == std::numeric_limits<int32_t>::min())
        {
            return {};
        }
        capacity = -count;
        tokens.resize(static_cast<std::size_t>(capacity));
        count = llama_tokenize(vcb, input.c_str(), static_cast<int32_t>(input.size()),
                               tokens.data(), capacity, add_special, true);
    }
    if(count < 0)
    {
        return {};
    }
    tokens.resize(static_cast<std::size_t>(count));
    return tokens;
}

std::vector<float> ChunkDB::embed(const std::string& input, bool add_special, bool store)
{
    if(ctx == nullptr || model == nullptr || vcb == nullptr)
    {
        std::cerr << "Error: ChunkDB is not initialized\n";
        return {};
    }

    const std::vector<llama_token> tokens = tokenize(input, add_special);
    if(tokens.empty())
    {
        std::cerr << "Error: failed to tokenize input\n";
        return {};
    }

    // Each embedding call handles an independent input.
    llama_memory_clear(llama_get_memory(ctx), true);
    llama_batch batch = llama_batch_get_one(
        const_cast<llama_token*>(tokens.data()), static_cast<int32_t>(tokens.size()));
    if(llama_decode(ctx, batch) != 0)
    {
        std::cerr << "Error: failed to evaluate input\n";
        return {};
    }

    float* values = llama_get_embeddings_seq(ctx, 0);
    if(values == nullptr)
    {
        values = llama_get_embeddings_ith(ctx, batch.n_tokens - 1);
    }
    if(values == nullptr)
    {
        std::cerr << "Error: model did not return an embedding\n";
        return {};
    }

    const int32_t dimension = llama_model_n_embd(model);
    std::vector<float> embedding(values, values + dimension);
    if(store)
    {
        chunkify(embedding, input);
    }
    return embedding;
}

void ChunkDB::chunkify(const std::vector<float>& embedding, const std::string& input)
{
    chunks.emplace_back(curr_seq_id++, embedding, input);
}

float ChunkDB::cosineSimilarity(const std::vector<float>& v1, const std::vector<float>& v2)
{
    if(v1.empty() || v1.size() != v2.size())
    {
        return 0.0f;
    }

    float dot = 0.0f;
    float norm1 = 0.0f;
    float norm2 = 0.0f;
    for(std::size_t i = 0; i < v1.size(); ++i)
    {
        dot += v1[i] * v2[i];
        norm1 += v1[i] * v1[i];
        norm2 += v2[i] * v2[i];
    }

    const float denominator = std::sqrt(norm1) * std::sqrt(norm2);
    return denominator == 0.0f ? 0.0f : dot / denominator;
}

std::vector<Chunk*> ChunkDB::getSimilarChunks(const std::vector<float>& embedding, uint8_t k)
{
    struct ScoredChunk
    {
        Chunk* chunk;
        float score;
    };

    std::vector<ScoredChunk> scored;
    scored.reserve(chunks.size());
    for(Chunk& chunk : chunks)
    {
        scored.push_back({&chunk, cosineSimilarity(embedding, chunk.embedding)});
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredChunk& lhs, const ScoredChunk& rhs)
    {
        return lhs.score > rhs.score;
    });

    const std::size_t count = std::min<std::size_t>(k, scored.size());
    std::vector<Chunk*> results;
    results.reserve(count);
    for(std::size_t i = 0; i < count; ++i)
    {
        results.push_back(scored[i].chunk);
    }
    return results;
}
