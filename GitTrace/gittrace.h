#ifndef GITTRACE_H
#define GITTRACE_H

#include <git2.h>
#include "chunkdb/chunkdb.h"
#include "blob/blob.h"
#include <string>
#include <cstddef>

class TreeSitter;

class GitTrace
{
private:
    git_repository* repo;
    ChunkDB chunkdb;
    char* repo_path;
    int error;
    bool initialized;
    bool activate_chunkdb;

    void chunkify(const TreeSitter& tree_sitter);

public:
    std::vector<Blob> blobs;

    GitTrace();
    ~GitTrace();

    bool init(const char* path, bool activate_chunkdb);
    void beginGitTrace();
    std::vector<ChunkMatch> searchChunks(const std::string& query, std::size_t k = 5);

    std::string lastError();
};
#endif
