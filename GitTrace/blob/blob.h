#ifndef BLOB_H
#define BLOB_H

#include <string>
#include <vector>

struct GitOccurrence
{
    std::string commit_sha;
    std::string tree_sha;
    std::string filepath;
    std::string blob_oid;
    int64_t commit_time = 0;
    std::string author_name;
    std::string author_email;
};

struct Blob
{
    // `sha` is retained as the representative commit for existing callers.
    std::string sha, blob_oid, filepath, author_name, author_email;
    int64_t time_mod;
    std::vector<unsigned char> raw_data;
    std::vector<GitOccurrence> occurrences;
};
#endif
