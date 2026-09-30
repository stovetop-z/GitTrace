#ifndef GITTRACE_H
#define GITTRACE_H

#include <git2.h>
#include "chunkdb/chunkdb.h"
#include "blob/blob.h"
#include <string>

class GitTrace
{
private:
    git_repository* repo;
    ChunkDB chunkdb;
    char* repo_path;
    int error;
    bool initialized;

public:
    std::vector<Blob> blobs;

    GitTrace();
    ~GitTrace();

    bool init(const char* path, bool activate_chunkdb);
    void beginGitTrace();

    std::string lastError();
};
#endif