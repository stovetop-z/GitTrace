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
    cparams = llama_context_default_params();
    cparams.embeddings = true;
    cparams.n_ctx = 2048;

    ggml_backend_dev_t cpu_devices[] = {
        ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU),
        nullptr
    };
    auto load_model = [&](int32_t gpu_layers, bool cpu_only) {
        mparams = llama_model_default_params();
        mparams.n_gpu_layers = gpu_layers;
        if (cpu_only) mparams.devices = cpu_devices;
        model = llama_model_load_from_file(model_path, mparams);
        if (model == nullptr) return false;

        vcb = llama_model_get_vocab(model);
        ctx = llama_init_from_model(model, cparams);
        if (ctx != nullptr) return true;

        llama_model_free(model);
        model = nullptr;
        vcb = nullptr;
        return false;
    };

    if (load_model(-1, false)) return true;

    std::cerr << "Warning: GPU model initialization failed; retrying with CPU only.\n";
    if (cpu_devices[0] != nullptr && load_model(0, true)) {
        std::cerr << "Loaded embedding model with CPU backend.\n";
        return true;
    }

    std::cerr << "Error: failed to initialize embedding model at " << model_path << '\n';
    return false;
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

std::vector<float> ChunkDB::embed(const std::string& input, bool add_special, bool store,
                                  const std::string& commit_sha, const std::string& filepath,
                                  uint32_t start_line, uint32_t end_line, int64_t commit_time,
                                  const std::string& syntax_type, uint32_t lineage_depth,
                                  uint32_t syntax_ordinal)
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
        chunkify(embedding, input, commit_sha, filepath, start_line, end_line,
                 commit_time, syntax_type, lineage_depth, syntax_ordinal);
    }
    return embedding;
}

void ChunkDB::chunkify(const std::vector<float>& embedding, const std::string& input,
                       const std::string& commit_sha, const std::string& filepath,
                       uint32_t start_line, uint32_t end_line, int64_t commit_time,
                       const std::string& syntax_type, uint32_t lineage_depth,
                       uint32_t syntax_ordinal)
{
    chunks.emplace_back(curr_seq_id++, embedding, input, commit_sha, filepath, start_line, end_line,
                        commit_time, syntax_type, lineage_depth, syntax_ordinal);
}

void ChunkDB::inferLineages()
{
    std::vector<std::size_t> chronological(chunks.size());
    for (std::size_t i = 0; i < chronological.size(); ++i) chronological[i] = i;
    std::stable_sort(chronological.begin(), chronological.end(), [&](std::size_t a, std::size_t b) {
        if (chunks[a].filepath != chunks[b].filepath) return chunks[a].filepath < chunks[b].filepath;
        return chunks[a].commit_time < chunks[b].commit_time;
    });

    for (std::size_t position = 0; position < chronological.size(); ++position) {
        Chunk& current = chunks[chronological[position]];
        float best_score = 0.78f;
        const Chunk* best_parent = nullptr;
        for (std::size_t prior = position; prior > 0; --prior) {
            const Chunk& candidate = chunks[chronological[prior - 1]];
            if (candidate.filepath != current.filepath) break;
            if (candidate.commit_time >= current.commit_time ||
                candidate.syntax_type != current.syntax_type) continue;
            const float semantic = cosineSimilarity(current.embedding, candidate.embedding);
            const uint32_t delta = current.syntax_ordinal > candidate.syntax_ordinal
                ? current.syntax_ordinal - candidate.syntax_ordinal
                : candidate.syntax_ordinal - current.syntax_ordinal;
            const float topology = 1.0f / (1.0f + 0.15f * static_cast<float>(delta));
            const float score = 0.85f * semantic + 0.15f * topology;
            if (score > best_score) { best_score = score; best_parent = &candidate; }
        }
        current.lineage_depth = best_parent ? best_parent->lineage_depth + 1 : 0;
    }
}

void ChunkDB::alignHistoricalChunks() { inferLineages(); }

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

std::vector<ChunkMatch> ChunkDB::search(const std::string& query, std::size_t k)
{
    const std::vector<float> query_embedding = embed(query, true, false);
    if (query_embedding.empty() || k == 0) return {};

    std::vector<ChunkMatch> matches;
    matches.reserve(chunks.size());
    for (const Chunk& chunk : chunks) {
        const float decay = std::exp(-0.12f * static_cast<float>(chunk.lineage_depth));
        matches.push_back({&chunk, cosineSimilarity(query_embedding, chunk.embedding) * decay});
    }
    std::sort(matches.begin(), matches.end(), [](const ChunkMatch& lhs, const ChunkMatch& rhs) {
        return lhs.similarity > rhs.similarity;
    });
    if (matches.size() > k) matches.resize(k);
    return matches;
}
