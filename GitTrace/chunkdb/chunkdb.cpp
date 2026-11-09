#include "chunkdb.h"
#include "retrieval.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <tuple>

ChunkDB::ChunkDB() : curr_seq_id(0) {}

ChunkDB::~ChunkDB()
{
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
}

bool ChunkDB::init()
{
    if (ctx != nullptr && model != nullptr && vcb != nullptr) return true;
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

    if (load_model(-1, false)) {
        backend_name = "automatic device selection";
        return true;
    }

    std::cerr << "Warning: GPU model initialization failed; retrying with CPU only.\n";
    if (cpu_devices[0] != nullptr && load_model(0, true)) {
        backend_name = "CPU fallback";
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
                                  uint32_t syntax_ordinal,
                                  const std::vector<GitOccurrence>& occurrences)
{
    if ((ctx == nullptr || model == nullptr || vcb == nullptr) && !init()) return {};
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
                 commit_time, syntax_type, lineage_depth, syntax_ordinal, occurrences);
    }
    return embedding;
}

void ChunkDB::chunkify(const std::vector<float>& embedding, const std::string& input,
                       const std::string& commit_sha, const std::string& filepath,
                       uint32_t start_line, uint32_t end_line, int64_t commit_time,
                       const std::string& syntax_type, uint32_t lineage_depth,
                       uint32_t syntax_ordinal,
                       const std::vector<GitOccurrence>& occurrences)
{
    chunks.emplace_back(curr_seq_id++, embedding, input, commit_sha, filepath, start_line, end_line,
                        commit_time, syntax_type, lineage_depth, syntax_ordinal, occurrences);
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

void ChunkDB::alignHistoricalChunks()
{
    for (Chunk& chunk : chunks) {
        std::stable_sort(chunk.occurrences.begin(), chunk.occurrences.end(),
            [](const GitOccurrence& lhs, const GitOccurrence& rhs) {
                if (lhs.commit_time != rhs.commit_time) return lhs.commit_time > rhs.commit_time;
                if (lhs.commit_sha != rhs.commit_sha) return lhs.commit_sha < rhs.commit_sha;
                return lhs.tree_sha < rhs.tree_sha;
            });
        if (!chunk.occurrences.empty()) {
            chunk.commit_sha = chunk.occurrences.front().commit_sha;
            chunk.commit_time = chunk.occurrences.front().commit_time;
        }
    }
    inferLineages();
    std::stable_sort(chunks.begin(), chunks.end(), [](const Chunk& lhs, const Chunk& rhs) {
        return std::tie(lhs.filepath, lhs.commit_sha, lhs.start_line, lhs.end_line,
                        lhs.syntax_type, lhs.syntax_ordinal, lhs.input) <
               std::tie(rhs.filepath, rhs.commit_sha, rhs.start_line, rhs.end_line,
                        rhs.syntax_type, rhs.syntax_ordinal, rhs.input);
    });
    for (std::size_t i = 0; i < chunks.size(); ++i) chunks[i].id = static_cast<uint32_t>(i);
    curr_seq_id = static_cast<uint32_t>(std::min<std::size_t>(chunks.size(),
        std::numeric_limits<uint32_t>::max()));
}

void ChunkDB::replaceChunks(std::vector<Chunk> loaded_chunks)
{
    chunks = std::move(loaded_chunks);
    curr_seq_id = 0;
    for (const Chunk& chunk : chunks) {
        if (chunk.id >= curr_seq_id && chunk.id != std::numeric_limits<uint32_t>::max())
            curr_seq_id = chunk.id + 1;
    }
}

void ChunkDB::pruneChunksWithoutOccurrences()
{
    chunks.erase(std::remove_if(chunks.begin(), chunks.end(), [](const Chunk& chunk) {
        return chunk.occurrences.empty();
    }), chunks.end());
    curr_seq_id = 0;
    for (const Chunk& chunk : chunks) {
        if (chunk.id >= curr_seq_id && chunk.id != std::numeric_limits<uint32_t>::max())
            curr_seq_id = chunk.id + 1;
    }
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

std::vector<ChunkMatch> ChunkDB::search(const std::string& query, std::size_t k,
                                        bool use_retrieval_instruction,
                                        RetrievalMode mode,
                                        bool expand_query_terms)
{
    static constexpr char retrieval_instruction[] =
        "Given a question about a software repository, retrieve code snippets that are appropriate responses to the question.";
    std::vector<float> query_embedding;
    if (mode != RetrievalMode::Keyword) {
        const std::string embedding_input = use_retrieval_instruction
            ? "<instruct>" + std::string(retrieval_instruction) + "\n<query>" + query
            : query;
        query_embedding = embed(embedding_input, true, false);
        if (query_embedding.empty()) return {};
    }
    if (k == 0) return {};

    std::vector<ChunkMatch> matches;
    matches.reserve(chunks.size());
    for (const Chunk& chunk : chunks) {
        const float decay = mode == RetrievalMode::Keyword
            ? 1.0f : std::exp(-0.12f * static_cast<float>(chunk.lineage_depth));
        const float raw_cosine_similarity = mode == RetrievalMode::Keyword
            ? 0.0f : cosineSimilarity(query_embedding, chunk.embedding);
        const float adjusted_similarity = raw_cosine_similarity * decay;
        matches.push_back({&chunk, raw_cosine_similarity, adjusted_similarity, 0.0f});
    }

    // Build a small in-memory BM25 ranking over chunk text and paths, then fuse
    // it with vector ranks. Reciprocal rank fusion avoids comparing raw cosine
    // scores directly with BM25 scores.
    const std::size_t count = matches.size();
    std::vector<std::size_t> vector_rank(count, 0);
    if (mode != RetrievalMode::Keyword) {
        std::vector<std::size_t> vector_order(count);
        std::iota(vector_order.begin(), vector_order.end(), 0);
        std::stable_sort(vector_order.begin(), vector_order.end(), [&](std::size_t lhs, std::size_t rhs) {
            return matches[lhs].similarity > matches[rhs].similarity;
        });
        for (std::size_t i = 0; i < count; ++i) vector_rank[vector_order[i]] = i + 1;
    }

    std::vector<std::string> documents;
    documents.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        documents.push_back(chunks[i].input + " " + chunks[i].filepath);
    }
    const std::vector<double> lexical_score =
        gittrace::retrieval::bm25Scores(query, documents, expand_query_terms);

    std::vector<std::size_t> lexical_rank(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (lexical_score[i] > 0.0) lexical_rank[i] = i + 1;
    }
    std::vector<std::size_t> lexical_order;
    lexical_order.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (lexical_score[i] > 0.0) lexical_order.push_back(i);
    }
    std::stable_sort(lexical_order.begin(), lexical_order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return lexical_score[lhs] > lexical_score[rhs];
    });
    std::fill(lexical_rank.begin(), lexical_rank.end(), 0);
    for (std::size_t i = 0; i < lexical_order.size(); ++i) lexical_rank[lexical_order[i]] = i + 1;

    if (mode == RetrievalMode::Keyword) {
        for (std::size_t i = 0; i < count; ++i) {
            matches[i].retrieval_score = static_cast<float>(lexical_score[i]);
        }
    } else if (mode == RetrievalMode::Vector) {
        for (ChunkMatch& match : matches) match.retrieval_score = match.similarity;
    } else {
        constexpr double rrf_constant = 60.0;
        for (std::size_t i = 0; i < count; ++i) {
            double fused = 1.0 / (rrf_constant + static_cast<double>(vector_rank[i]));
            if (lexical_rank[i] != 0) {
                fused += 1.0 / (rrf_constant + static_cast<double>(lexical_rank[i]));
            }
            matches[i].retrieval_score = static_cast<float>(fused);
        }
    }

    if (mode == RetrievalMode::Keyword) {
        matches.erase(std::remove_if(matches.begin(), matches.end(), [](const ChunkMatch& match) {
            return match.retrieval_score <= 0.0f;
        }), matches.end());
    }

    std::stable_sort(matches.begin(), matches.end(), [](const ChunkMatch& lhs, const ChunkMatch& rhs) {
        return lhs.retrieval_score > rhs.retrieval_score;
    });
    if (matches.size() > k) matches.resize(k);
    return matches;
}
