#include "knowledge_store.h"
#include <sstream>
#include <iomanip>

namespace knowledge_store {

KnowledgeStore::KnowledgeStore() : db_(nullptr) {}

KnowledgeStore::~KnowledgeStore() {
    close();
}

bool KnowledgeStore::open(const std::string& db_path) {
    if (db_) return true;
    
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        error_ = sqlite3_errmsg(db_);
        return false;
    }
    
    // Enable WAL mode for better concurrency
    execute("PRAGMA journal_mode=WAL;");
    execute("PRAGMA busy_timeout=5000;");
    execute("PRAGMA foreign_keys=ON;");
    
    return init_schema();
}

void KnowledgeStore::close() {
    if (db_) {
        for (auto& [sql, stmt] : stmt_cache_) {
            sqlite3_finalize(stmt);
        }
        stmt_cache_.clear();
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

sqlite3_stmt* KnowledgeStore::prepare_cached(const std::string& sql) {
    if (!db_) return nullptr;

    auto it = stmt_cache_.find(sql);
    sqlite3_stmt* stmt = nullptr;
    if (it != stmt_cache_.end()) {
        stmt = it->second;
    } else {
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            error_ = sqlite3_errmsg(db_);
            return nullptr;
        }
        stmt_cache_.emplace(sql, stmt);
    }

    // Reuse: rewind the statement and drop any stale bindings
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return stmt;
}

int KnowledgeStore::execute(const std::string& sql, const std::vector<std::string>& params) {
    if (!db_) return SQLITE_ERROR;

    sqlite3_stmt* stmt = prepare_cached(sql);
    if (!stmt) return SQLITE_ERROR;

    for (size_t i = 0; i < params.size(); i++) {
        int rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            error_ = sqlite3_errmsg(db_);
            sqlite3_reset(stmt);
            return rc;
        }
    }

    int rc = sqlite3_step(stmt);
    int result = (rc == SQLITE_ROW) ? SQLITE_ROW : SQLITE_OK;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        error_ = sqlite3_errmsg(db_);
    }

    sqlite3_reset(stmt);
    return result;
}

int KnowledgeStore::execute(const std::string& sql, const std::string& p1) {
    return execute(sql, std::vector<std::string>{p1});
}

int KnowledgeStore::execute(const std::string& sql, const std::string& p1, const std::string& p2) {
    return execute(sql, std::vector<std::string>{p1, p2});
}

int KnowledgeStore::execute(const std::string& sql, const std::string& p1, const std::string& p2, const std::string& p3) {
    return execute(sql, std::vector<std::string>{p1, p2, p3});
}

int KnowledgeStore::execute(const std::string& sql, const std::string& p1, const std::string& p2, const std::string& p3, const std::string& p4) {
    return execute(sql, std::vector<std::string>{p1, p2, p3, p4});
}

std::vector<std::vector<std::string>> KnowledgeStore::query(const std::string& sql) {
    return query(sql, std::vector<std::string>{});
}

std::vector<std::vector<std::string>> KnowledgeStore::query(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<std::vector<std::string>> results;

    if (!db_) return results;

    sqlite3_stmt* stmt = prepare_cached(sql);
    if (!stmt) return results;

    for (size_t i = 0; i < params.size(); i++) {
        int rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            error_ = sqlite3_errmsg(db_);
            sqlite3_reset(stmt);
            return results;
        }
    }

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::vector<std::string> row;
        int ncols = sqlite3_column_count(stmt);
        for (int i = 0; i < ncols; i++) {
            if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                row.push_back("");
            } else {
                row.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)));
            }
        }
        results.push_back(row);
    }

    if (rc != SQLITE_DONE) {
        error_ = sqlite3_errmsg(db_);
    }

    sqlite3_reset(stmt);
    return results;
}

std::vector<std::vector<std::string>> KnowledgeStore::query(const std::string& sql, const std::string& p1) {
    return query(sql, std::vector<std::string>{p1});
}

std::vector<std::vector<std::string>> KnowledgeStore::query(const std::string& sql, const std::string& p1, const std::string& p2) {
    return query(sql, std::vector<std::string>{p1, p2});
}

std::string KnowledgeStore::get_error() const {
    return error_;
}

bool KnowledgeStore::is_open() const {
    return db_ != nullptr;
}

bool KnowledgeStore::init_schema() {
    // Migrate legacy databases where repo_id was an INTEGER AUTOINCREMENT key.
    {
        auto cols = query("PRAGMA table_info(repositories)");
        bool legacy = false;
        for (const auto& c : cols) {
            // columns: cid, name, type, notnull, dflt_value, pk
            if (c.size() >= 6 && c[1] == "repo_id" && c[2] == "INTEGER") legacy = true;
        }
        if (legacy) {
            execute("BEGIN");
            execute("DROP TABLE IF EXISTS repositories_new");
            execute("CREATE TABLE repositories_new ("
                    "repo_id TEXT PRIMARY KEY, "
                    "root_path TEXT UNIQUE NOT NULL, "
                    "name TEXT NOT NULL, "
                    "created_at TEXT NOT NULL, "
                    "updated_at TEXT NOT NULL)");
            execute("INSERT OR IGNORE INTO repositories_new SELECT CAST(repo_id AS TEXT), root_path, name, created_at, updated_at FROM repositories");
            execute("DROP TABLE repositories");
            execute("ALTER TABLE repositories_new RENAME TO repositories");

            // indexing_state had the same INTEGER primary key in legacy schemas
            execute("DROP TABLE IF EXISTS indexing_state_new");
            execute("CREATE TABLE indexing_state_new ("
                    "repo_id TEXT PRIMARY KEY, "
                    "last_full_index_at TEXT, "
                    "last_incremental_at TEXT, "
                    "status TEXT DEFAULT 'idle', "
                    "files_indexed INTEGER DEFAULT 0, "
                    "files_total INTEGER DEFAULT 0)");
            execute("INSERT OR IGNORE INTO indexing_state_new SELECT CAST(repo_id AS TEXT), last_full_index_at, last_incremental_at, status, files_indexed, files_total FROM indexing_state");
            execute("DROP TABLE indexing_state");
            execute("ALTER TABLE indexing_state_new RENAME TO indexing_state");
            execute("COMMIT");
        }
    }

    // Create repositories table (repo_id is a stable identity string)
    execute("CREATE TABLE IF NOT EXISTS repositories ("
            "repo_id TEXT PRIMARY KEY, "
            "root_path TEXT UNIQUE NOT NULL, "
            "name TEXT NOT NULL, "
            "created_at TEXT NOT NULL, "
            "updated_at TEXT NOT NULL)");
    
    // Create files table
    execute("CREATE TABLE IF NOT EXISTS files ("
            "file_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "repo_id INTEGER NOT NULL, "
            "path TEXT NOT NULL, "
            "content_hash TEXT NOT NULL, "
            "language TEXT, "
            "file_type TEXT, "
            "size_bytes INTEGER DEFAULT 0, "
            "line_count INTEGER DEFAULT 0, "
            "last_indexed_at TEXT, "
            "is_binary INTEGER DEFAULT 0, "
            "is_deleted INTEGER DEFAULT 0, "
            "stale INTEGER DEFAULT 0, "
            "UNIQUE(repo_id, path))");

    // Migration: older databases may lack the stale flag
    {
        auto file_cols = query("PRAGMA table_info(files)");
        bool has_stale = false;
        for (const auto& c : file_cols) {
            if (c.size() >= 2 && c[1] == "stale") has_stale = true;
        }
        if (!has_stale) execute("ALTER TABLE files ADD COLUMN stale INTEGER DEFAULT 0");
    }
    
    // Create symbols table
    execute("CREATE TABLE IF NOT EXISTS symbols ("
            "symbol_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "file_id INTEGER NOT NULL, "
            "name TEXT NOT NULL, "
            "symbol_type TEXT NOT NULL, "
            "signature TEXT, "
            "start_line INTEGER NOT NULL, "
            "end_line INTEGER NOT NULL, "
            "scope TEXT, "
            "is_declaration INTEGER DEFAULT 0, "
            "is_definition INTEGER DEFAULT 0, "
            "is_exported INTEGER DEFAULT 0, "
            "content_hash_at_index TEXT)");
    
    // Create dependencies table
    execute("CREATE TABLE IF NOT EXISTS dependencies ("
            "dep_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "repo_id INTEGER NOT NULL, "
            "source_path TEXT NOT NULL, "
            "target_path TEXT NOT NULL, "
            "source_symbol TEXT, "
            "target_symbol TEXT, "
            "dep_type TEXT, "
            "dep_direction TEXT)");
    
    // Create summaries table
    execute("CREATE TABLE IF NOT EXISTS summaries ("
            "summary_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "file_id INTEGER NOT NULL, "
            "content_hash TEXT NOT NULL, "
            "summary_text TEXT NOT NULL, "
            "created_at TEXT NOT NULL, "
            "invalidated_at TEXT, "
            "is_valid INTEGER DEFAULT 1)");
    
    // Create indexing_state table
    execute("CREATE TABLE IF NOT EXISTS indexing_state ("
            "repo_id TEXT PRIMARY KEY, "
            "last_full_index_at TEXT, "
            "last_incremental_at TEXT, "
            "status TEXT DEFAULT 'idle', "
            "files_indexed INTEGER DEFAULT 0, "
            "files_total INTEGER DEFAULT 0)");
    
    // Create indexes
    execute("CREATE INDEX IF NOT EXISTS idx_files_repo_path ON files(repo_id, path)");
    execute("CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash)");
    execute("CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file_id)");
    execute("CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name)");
    execute("CREATE INDEX IF NOT EXISTS idx_deps_repo ON dependencies(repo_id)");
    execute("CREATE INDEX IF NOT EXISTS idx_deps_source ON dependencies(source_path)");
    execute("CREATE INDEX IF NOT EXISTS idx_deps_target ON dependencies(repo_id, target_path)");
    execute("CREATE INDEX IF NOT EXISTS idx_deps_type ON dependencies(repo_id, dep_type)");
    execute("CREATE INDEX IF NOT EXISTS idx_deps_source_symbol ON dependencies(repo_id, source_symbol)");
    execute("CREATE INDEX IF NOT EXISTS idx_deps_target_symbol ON dependencies(repo_id, target_symbol)");
    execute("CREATE INDEX IF NOT EXISTS idx_summaries_file ON summaries(file_id)");
    execute("CREATE INDEX IF NOT EXISTS idx_summaries_hash ON summaries(content_hash)");
    
    return true;
}

}
