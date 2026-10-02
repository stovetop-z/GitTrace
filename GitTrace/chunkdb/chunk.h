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

    Chunk(uint32_t id, const std::vector<float>& embedding, const std::string& input,
          const std::string& commit_sha, const std::string& filepath,
          uint32_t start_line, uint32_t end_line)
    {
        this->id = id;
        this->embedding = embedding;
        this->input = input;
        this->commit_sha = commit_sha;
        this->filepath = filepath;
        this->start_line = start_line;
        this->end_line = end_line;
    }
};

struct ChunkMatch
{
    const Chunk* chunk;
    float similarity;
};
#endif
