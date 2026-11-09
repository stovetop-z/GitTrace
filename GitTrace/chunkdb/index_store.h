#ifndef GITTRACE_INDEX_STORE_H
#define GITTRACE_INDEX_STORE_H

#include "chunk.h"
#include <sqlite3.h>
#include <string>
#include <vector>

namespace gittrace {
namespace index {

class WriterLock
{
public:
    WriterLock() = default;
    ~WriterLock();
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    bool acquire(const std::string& index_path, int timeout_ms, std::string& error);
private:
    sqlite3* handle_ = nullptr;
};

struct Status
{
    bool exists = false;
    bool compatible = false;
    std::size_t chunks = 0;
    std::string detail;
};

bool load(const std::string& path, const std::string& manifest,
          std::vector<Chunk>& chunks, std::string& error,
          bool allow_head_change = false, std::string* stored_manifest = nullptr);
bool save(const std::string& path, const std::string& manifest,
          const std::vector<Chunk>& chunks, std::string& error);
Status inspect(const std::string& path, const std::string& manifest);

} // namespace index
} // namespace gittrace

#endif
