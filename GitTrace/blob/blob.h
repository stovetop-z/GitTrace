#ifndef BLOB_H
#define BLOB_H

#include <string>
#include <vector>

struct Blob
{
    std::string sha, filepath, author_name, author_email;
    int64_t time_mod;
    std::vector<unsigned char> raw_data;
};
#endif