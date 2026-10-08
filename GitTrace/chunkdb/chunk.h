#ifndef CHUNK_H
#define CHUNK_H

#include <cstdint>
#include <string>
#include <vector>

struct Chunk
{
    uint32_t id;
    std::vector<float> embedding;
    std::string input;
    std::string commit_sha;
    std::string filepath;
    uint32_t start_line;
    uint32_t end_line;
    int64_t commit_time;
    std::string syntax_type;
    uint32_t lineage_depth = 0;
    uint32_t syntax_ordinal = 0;

    Chunk(uint32_t id, const std::vector<float>& embedding, const std::string& input,
          const std::string& commit_sha, const std::string& filepath,
          uint32_t start_line, uint32_t end_line, int64_t commit_time = 0,
          const std::string& syntax_type = {}, uint32_t lineage_depth = 0,
          uint32_t syntax_ordinal = 0)
    {
        this->id = id;
        this->embedding = embedding;
        this->input = input;
        this->commit_sha = commit_sha;
        this->filepath = filepath;
        this->start_line = start_line;
        this->end_line = end_line;
        this->commit_time = commit_time;
        this->syntax_type = syntax_type;
        this->lineage_depth = lineage_depth;
        this->syntax_ordinal = syntax_ordinal;
    }
};

struct ChunkMatch
{
    const Chunk* chunk;
    float similarity;
};
#endif
