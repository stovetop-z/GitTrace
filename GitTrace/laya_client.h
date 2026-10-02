#ifndef GITTRACE_LAYA_CLIENT_H
#define GITTRACE_LAYA_CLIENT_H

#include "chunkdb/chunk.h"

#include <string>
#include <vector>

struct LayaMatch
{
    ChunkMatch match;
    std::string relevance;
    float confidence;
    bool accepted;
};

class LayaClient
{
private:
    std::string endpoint;

public:
    explicit LayaClient(std::string endpoint);

    bool rerank(const std::string& query,
                const std::vector<ChunkMatch>& candidates,
                std::vector<LayaMatch>& results,
                std::string& error) const;
};

#endif
