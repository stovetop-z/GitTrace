#include "GitTrace/chunkdb/chunkdb.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
    bool checkEmbedding(const std::string& label, const std::vector<float>& embedding)
    {
        if (embedding.empty())
        {
            std::cerr << "FAIL: " << label << " produced no embedding\n";
            return false;
        }

        std::cout << "PASS: " << label << " produced an embedding with "
                  << embedding.size() << " values\n";
        return true;
    }
}

int main()
{
    ChunkDB db;

    std::cout << "Initializing ChunkDB...\n";
    if (!db.init())
    {
        std::cerr << "FAIL: ChunkDB could not initialize. Check that the model "
                     "file is available at the configured path.\n";
        return 1;
    }
    std::cout << "PASS: ChunkDB initialized\n";

    // Store a few sample chunks, then embed a related query without storing it.
    const std::vector<std::string> samples = {
        "A binary search finds an item in a sorted array.",
        "A hash table maps keys to values using a hash function.",
        "A linked list stores nodes connected by pointers."
    };

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto embedding = db.embed(samples[i]);
        if (!checkEmbedding("sample " + std::to_string(i + 1), embedding))
        {
            return 1;
        }
    }

    const std::string query = "Search a sorted array efficiently.";
    const auto queryEmbedding = db.embed(query, true, false);
    if (!checkEmbedding("query", queryEmbedding))
    {
        return 1;
    }

    const auto matches = db.getSimilarChunks(queryEmbedding, 3);
    std::cout << "Similarity search returned " << matches.size() << " result(s):\n";
    for (const Chunk* chunk : matches)
    {
        if (chunk == nullptr)
        {
            std::cerr << "FAIL: similarity search returned a null chunk\n";
            return 1;
        }
        std::cout << "  [chunk " << chunk->id << "] " << chunk->input << '\n';
    }

    std::cout << "ChunkDB smoke test finished.\n";
    return 0;
}
