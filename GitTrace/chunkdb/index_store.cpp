#include "index_store.h"

#include <sqlite3.h>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <unordered_map>

namespace gittrace {
namespace index {
namespace {

class Database {
public:
    sqlite3* handle = nullptr;
    ~Database() { if (handle) sqlite3_close(handle); }
};

bool exec(sqlite3* db, const char* sql, std::string& error)
{
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) == SQLITE_OK) return true;
    error = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    return false;
}

bool prepare(sqlite3* db, const char* sql, sqlite3_stmt** statement, std::string& error)
{
    if (sqlite3_prepare_v2(db, sql, -1, statement, nullptr) == SQLITE_OK) return true;
    error = sqlite3_errmsg(db);
    return false;
}

bool current_schema(sqlite3* db, std::string& error)
{
    sqlite3_stmt* statement = nullptr;
    if (!prepare(db, "PRAGMA user_version", &statement, error)) return false;
    const int result = sqlite3_step(statement);
    const int version = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    if (version != 1) {
        error = "unsupported SQLite index schema version " + std::to_string(version);
        return false;
    }
    return true;
}

bool bind_text(sqlite3_stmt* statement, int index, const std::string& value)
{
    return sqlite3_bind_text(statement, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

const char* column_text(sqlite3_stmt* statement, int index)
{
    const auto* value = sqlite3_column_text(statement, index);
    return value ? reinterpret_cast<const char*>(value) : "";
}

bool create_schema(sqlite3* db, std::string& error)
{
    return exec(db,
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS manifest (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS chunks ("
        " id INTEGER PRIMARY KEY, input TEXT NOT NULL, commit_sha TEXT NOT NULL,"
        " filepath TEXT NOT NULL, start_line INTEGER NOT NULL, end_line INTEGER NOT NULL,"
        " commit_time INTEGER NOT NULL, syntax_type TEXT NOT NULL, lineage_depth INTEGER NOT NULL,"
        " syntax_ordinal INTEGER NOT NULL, embedding_dim INTEGER NOT NULL, embedding BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS occurrences ("
        " chunk_id INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE, occurrence_index INTEGER NOT NULL,"
        " commit_sha TEXT NOT NULL, tree_sha TEXT NOT NULL, filepath TEXT NOT NULL, blob_oid TEXT NOT NULL,"
        " commit_time INTEGER NOT NULL, author_name TEXT NOT NULL, author_email TEXT NOT NULL,"
        " PRIMARY KEY(chunk_id, occurrence_index));"
        "CREATE INDEX IF NOT EXISTS idx_chunks_path ON chunks(filepath);"
        "CREATE INDEX IF NOT EXISTS idx_occurrences_commit ON occurrences(commit_sha);"
        "PRAGMA user_version=1;", error);
}

} // namespace

WriterLock::~WriterLock()
{
    if (handle_) {
        sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(handle_);
    }
}

bool WriterLock::acquire(const std::string& index_path, int timeout_ms, std::string& error)
{
    const std::filesystem::path lock_path = index_path + ".writer-lock.sqlite";
    std::error_code ec;
    if (!lock_path.parent_path().empty())
        std::filesystem::create_directories(lock_path.parent_path(), ec);
    if (ec) { error = ec.message(); return false; }
    if (sqlite3_open_v2(lock_path.string().c_str(), &handle_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = handle_ ? sqlite3_errmsg(handle_) : "could not open writer lock";
        if (handle_) { sqlite3_close(handle_); handle_ = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(handle_, timeout_ms);
    if (!exec(handle_, "BEGIN IMMEDIATE", error)) {
        sqlite3_close(handle_);
        handle_ = nullptr;
        return false;
    }
    return true;
}

bool load(const std::string& path, const std::string& manifest,
          std::vector<Chunk>& chunks, std::string& error,
          bool allow_head_change, std::string* stored_manifest)
{
    if (!std::filesystem::exists(path)) return false;
    Database database;
    if (sqlite3_open_v2(path.c_str(), &database.handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = database.handle ? sqlite3_errmsg(database.handle) : "could not open SQLite index";
        return false;
    }
    if (!current_schema(database.handle, error)) return false;
    sqlite3_stmt* statement = nullptr;
    if (!prepare(database.handle, "SELECT value FROM manifest WHERE key='fingerprint'",
                 &statement, error)) return false;
    const int manifest_result = sqlite3_step(statement);
    const std::string stored = manifest_result == SQLITE_ROW ? column_text(statement, 0) : "";
    if (stored_manifest && manifest_result == SQLITE_ROW) *stored_manifest = stored;
    const bool compatible = manifest_result == SQLITE_ROW &&
        (manifest == stored || (allow_head_change &&
         stored.compare(0, manifest.size(), manifest) == 0 &&
         stored.size() > manifest.size() && stored[manifest.size()] == ';'));
    sqlite3_finalize(statement);
    if (!compatible) {
        error = manifest_result == SQLITE_ROW ? "index configuration fingerprint changed" :
                                                "index manifest is missing";
        return false;
    }

    if (!prepare(database.handle,
        "SELECT id,input,commit_sha,filepath,start_line,end_line,commit_time,syntax_type,"
        "lineage_depth,syntax_ordinal,embedding_dim,embedding FROM chunks ORDER BY id",
        &statement, error)) return false;
    std::unordered_map<uint32_t, std::size_t> chunk_indexes;
    std::vector<Chunk> loaded;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const int dimension = sqlite3_column_int(statement, 10);
        const int embedding_size = sqlite3_column_bytes(statement, 11);
        const void* embedding_data = sqlite3_column_blob(statement, 11);
        if (dimension < 0 || embedding_size != dimension * static_cast<int>(sizeof(float)) ||
            (embedding_size > 0 && !embedding_data)) {
            error = "index contains an invalid embedding payload";
            sqlite3_finalize(statement);
            return false;
        }
        std::vector<float> embedding(static_cast<std::size_t>(dimension));
        if (embedding_size) std::memcpy(embedding.data(), embedding_data, static_cast<std::size_t>(embedding_size));
        const uint32_t id = static_cast<uint32_t>(sqlite3_column_int64(statement, 0));
        Chunk chunk(id, embedding, column_text(statement, 1), column_text(statement, 2),
                    column_text(statement, 3),
                    static_cast<uint32_t>(sqlite3_column_int(statement, 4)),
                    static_cast<uint32_t>(sqlite3_column_int(statement, 5)),
                    sqlite3_column_int64(statement, 6), column_text(statement, 7),
                    static_cast<uint32_t>(sqlite3_column_int(statement, 8)),
                    static_cast<uint32_t>(sqlite3_column_int(statement, 9)));
        chunk_indexes.emplace(id, loaded.size());
        loaded.push_back(std::move(chunk));
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) { error = sqlite3_errmsg(database.handle); return false; }

    if (!prepare(database.handle,
        "SELECT chunk_id,commit_sha,tree_sha,filepath,blob_oid,commit_time,author_name,author_email "
        "FROM occurrences ORDER BY chunk_id,occurrence_index", &statement, error)) return false;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto found = chunk_indexes.find(static_cast<uint32_t>(sqlite3_column_int64(statement, 0)));
        if (found == chunk_indexes.end()) continue;
        GitOccurrence occurrence;
        occurrence.commit_sha = column_text(statement, 1);
        occurrence.tree_sha = column_text(statement, 2);
        occurrence.filepath = column_text(statement, 3);
        occurrence.blob_oid = column_text(statement, 4);
        occurrence.commit_time = sqlite3_column_int64(statement, 5);
        occurrence.author_name = column_text(statement, 6);
        occurrence.author_email = column_text(statement, 7);
        loaded[found->second].occurrences.push_back(std::move(occurrence));
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) { error = sqlite3_errmsg(database.handle); return false; }
    for (const Chunk& chunk : loaded) {
        bool representative_found = false;
        for (const GitOccurrence& occurrence : chunk.occurrences) {
            if (occurrence.filepath != chunk.filepath || occurrence.commit_sha.empty() ||
                occurrence.blob_oid.empty() || occurrence.tree_sha.empty()) {
                error = "index contains invalid occurrence metadata";
                return false;
            }
            if (occurrence.commit_sha == chunk.commit_sha) representative_found = true;
        }
        if (!representative_found) {
            error = "index chunk is missing its representative Git occurrence";
            return false;
        }
    }
    chunks = std::move(loaded);
    return true;
}

Status inspect(const std::string& path, const std::string& manifest)
{
    Status status;
    status.exists = std::filesystem::exists(path);
    if (!status.exists) { status.detail = "index file not found"; return status; }
    Database database;
    if (sqlite3_open_v2(path.c_str(), &database.handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        status.detail = database.handle ? sqlite3_errmsg(database.handle) : "could not open index";
        return status;
    }
    sqlite3_stmt* statement = nullptr;
    std::string error;
    if (!current_schema(database.handle, error)) { status.detail = error; return status; }
    if (!prepare(database.handle, "SELECT value FROM manifest WHERE key='fingerprint'",
                 &statement, error)) { status.detail = error; return status; }
    const int result = sqlite3_step(statement);
    status.compatible = result == SQLITE_ROW && manifest == column_text(statement, 0);
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) { status.detail = "index manifest is missing"; return status; }
    if (!prepare(database.handle, "SELECT count(*) FROM chunks", &statement, error)) {
        status.detail = error; return status;
    }
    if (sqlite3_step(statement) == SQLITE_ROW)
        status.chunks = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    status.detail = status.compatible ? "compatible with current repository and index settings"
                                      : "stale: repository or index settings changed";
    return status;
}

static bool write_snapshot(const std::string& path, const std::string& manifest,
                           const std::vector<Chunk>& chunks, std::string& error)
{
    std::error_code filesystem_error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), filesystem_error);
    if (filesystem_error) { error = filesystem_error.message(); return false; }
    Database database;
    if (sqlite3_open_v2(path.c_str(), &database.handle,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = database.handle ? sqlite3_errmsg(database.handle) : "could not create SQLite index";
        return false;
    }
    if (!create_schema(database.handle, error) || !exec(database.handle, "BEGIN IMMEDIATE", error)) return false;

    sqlite3_stmt* statement = nullptr;
    if (!prepare(database.handle, "INSERT OR REPLACE INTO manifest(key,value) VALUES('fingerprint',?)",
                 &statement, error)) { exec(database.handle, "ROLLBACK", error); return false; }
    bind_text(statement, 1, manifest);
    bool ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok) { error = sqlite3_errmsg(database.handle); exec(database.handle, "ROLLBACK", error); return false; }

    if (!exec(database.handle, "DELETE FROM occurrences; DELETE FROM chunks;", error)) {
        exec(database.handle, "ROLLBACK", error); return false;
    }
    if (!prepare(database.handle,
        "INSERT INTO chunks(id,input,commit_sha,filepath,start_line,end_line,commit_time,syntax_type,"
        "lineage_depth,syntax_ordinal,embedding_dim,embedding) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        &statement, error)) { exec(database.handle, "ROLLBACK", error); return false; }
    for (const Chunk& chunk : chunks) {
        sqlite3_reset(statement); sqlite3_clear_bindings(statement);
        bind_text(statement, 2, chunk.input); bind_text(statement, 3, chunk.commit_sha);
        bind_text(statement, 4, chunk.filepath);
        sqlite3_bind_int64(statement, 1, chunk.id);
        sqlite3_bind_int64(statement, 5, chunk.start_line); sqlite3_bind_int64(statement, 6, chunk.end_line);
        sqlite3_bind_int64(statement, 7, chunk.commit_time); bind_text(statement, 8, chunk.syntax_type);
        sqlite3_bind_int64(statement, 9, chunk.lineage_depth); sqlite3_bind_int64(statement, 10, chunk.syntax_ordinal);
        sqlite3_bind_int(statement, 11, static_cast<int>(chunk.embedding.size()));
        sqlite3_bind_blob(statement, 12, chunk.embedding.data(),
                          static_cast<int>(chunk.embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_DONE) { ok = false; break; }
    }
    sqlite3_finalize(statement);
    if (!ok) { error = sqlite3_errmsg(database.handle); exec(database.handle, "ROLLBACK", error); return false; }

    if (!prepare(database.handle,
        "INSERT INTO occurrences(chunk_id,occurrence_index,commit_sha,tree_sha,filepath,blob_oid,"
        "commit_time,author_name,author_email) VALUES(?,?,?,?,?,?,?,?,?)", &statement, error)) {
        exec(database.handle, "ROLLBACK", error); return false;
    }
    for (const Chunk& chunk : chunks) {
        for (std::size_t i = 0; i < chunk.occurrences.size(); ++i) {
            const GitOccurrence& occurrence = chunk.occurrences[i];
            sqlite3_reset(statement); sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, chunk.id);
            sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(i));
            bind_text(statement, 3, occurrence.commit_sha); bind_text(statement, 4, occurrence.tree_sha);
            bind_text(statement, 5, occurrence.filepath); bind_text(statement, 6, occurrence.blob_oid);
            sqlite3_bind_int64(statement, 7, occurrence.commit_time);
            bind_text(statement, 8, occurrence.author_name); bind_text(statement, 9, occurrence.author_email);
            if (sqlite3_step(statement) != SQLITE_DONE) { ok = false; break; }
        }
        if (!ok) break;
    }
    sqlite3_finalize(statement);
    if (!ok) { error = sqlite3_errmsg(database.handle); exec(database.handle, "ROLLBACK", error); return false; }
    if (!exec(database.handle, "COMMIT", error)) { exec(database.handle, "ROLLBACK", error); return false; }
    return true;
}

bool save(const std::string& path, const std::string& manifest,
          const std::vector<Chunk>& chunks, std::string& error)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path target(path);
    const std::filesystem::path temporary = target.string() + ".new." + std::to_string(nonce);
    if (!write_snapshot(temporary.string(), manifest, chunks, error)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        error = "could not atomically replace index: " + ec.message();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

} // namespace index
} // namespace gittrace
