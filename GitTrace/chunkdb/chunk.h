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

    Chunk(uint32_t id, const std::vector<float>& embedding, const std::string& input)
    {
        this->id = id;
        this->embedding = embedding;
        this->input = input;
    }
};
#endif
