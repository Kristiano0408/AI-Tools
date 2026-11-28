#include "knowledge_store.h"
#include "../utils/perf_metrics.h"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace knowledge_store {

namespace {

// Translate an sqlite3 failure into a message that can never be empty: an
// empty error_ is what let a rolled-back or failed batch read as "it probably
// worked" (DB-01).
std::string failure_text(const std::string& recorded, const char* fallback) {
    return recorded.empty() ? std::string(fallback) : recorded;
}

#ifdef AI_TOOLS_DEBUG
// Compact probe key for a SQL statement: first keyword + a short prefix of the
// rest, so per-statement timings group meaningfully in the perf table.
std::string sql_probe_key(const std::string& tag, const std::string& sql) {
    std::string key = tag + " ";
    size_t i = 0;
    while (i < sql.size() && sql[i] == ' ') i++;
    size_t kw_end = i;
    while (kw_end < sql.size() && sql[kw_end] != ' ' && sql[kw_end] != '(') kw_end++;
    key.append(sql, i, kw_end - i);
    key.push_back(' ');
    size_t start = kw_end;
    while (start < sql.size() && sql[start] == ' ') start++;
    key.append(sql, start, std::min(size_t(48), sql.size() - start));
    return key;
}
#endif

} // namespace

KnowledgeStore::KnowledgeStore() : db_(nullptr) {}

KnowledgeStore::~KnowledgeStore() {
    close();
}

bool KnowledgeStore::open(const std::string& db_path) {
    PERF_SCOPE("knowledge_store.open");
    PERF_COUNTER("knowledge_store.open.calls");
    if (db_) return true;
    
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        error_ = sqlite3_errmsg(db_);
        return false;
    }
    
    // Enable WAL mode for better concurrency.
    //
    // DB-01: these PRAGMAs ARE the consistency contract every later
    // transaction relies on (WAL, the busy timeout that lets concurrent
    // writers wait instead of failing, and the pragma that makes the schema's
    // FOREIGN KEY declarations actually enforced). Silently ignoring a failure
    // here used to let the store open claiming protections it does not have,
    // so each result is checked and a failure aborts the open.
    // statement_ok() because a PRAGMA that answers (journal_mode=WAL returns a
    // result row) succeeds with SQLITE_ROW, not SQLITE_OK.
    bool configured = statement_ok(execute("PRAGMA journal_mode=WAL;")) &&
                      statement_ok(execute("PRAGMA busy_timeout=5000;")) &&
                      statement_ok(execute("PRAGMA foreign_keys=ON;"));
    if (!configured) {
        if (error_.empty()) error_ = "database configuration failed";
        close();
        return false;
    }
    
    return init_schema();
}

void KnowledgeStore::close() {
    if (db_) {
        for (auto& [sql, stmt] : stmt_cache_) {
            sqlite3_finalize(stmt);
        }
        stmt_cache_.clear();
        // A never-committed transaction cannot outlive its connection, and a
        // fault must never leak from one opened database into the next.
        tx_names_.clear();
        clear_sql_fault();
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ---- nested transactions (BEGIN at depth 0, SAVEPOINT beyond that) ----
//
// DB-01: this is what makes composition safe. index_repository() opens one
// transaction per file re-indexing; a scan also runs a deleted-file
// reconciliation. Without a depth counter the nested shape either fails with
// 'cannot start a transaction within a transaction' — turning a valid
// operation into a scan failure — or, worse, the inner scope issues COMMIT and
// commits the OUTER scope's half-written work. A savepoint gives each scope its
// own rollback boundary inside the enclosing transaction, so a failing inner
// unit is cancelled without discarding or committing the outer one.
std::string KnowledgeStore::next_savepoint_name() {
    static int counter = 0;
    return "ai_tools_tx_" + std::to_string(++counter);
}

int KnowledgeStore::tx_begin(const char* name) {
    if (!db_) {
        error_ = "cannot start a transaction: database is closed";
        return SQLITE_ERROR;
    }
    // The savepoint label is independent of the optional diagnostic name; the
    // caller's label only documents which unit opened the scope.
    std::string label = tx_names_.empty() ? std::string() : next_savepoint_name();
    int rc = label.empty()
                 ? execute("BEGIN;")
                 : execute("SAVEPOINT " + label + ";");
    if (rc != SQLITE_OK) {
        error_ = failure_text(error_, "cannot start transaction");
        return rc;
    }
    tx_names_.push_back(label);
    (void)name;
    return SQLITE_OK;
}

int KnowledgeStore::tx_commit() {
    if (tx_names_.empty()) {
        if (error_.empty()) error_ = "cannot commit: no transaction is open";
        return SQLITE_ERROR;
    }
    const std::string label = tx_names_.back();
    int rc = label.empty() ? execute("COMMIT;")
                           : execute("RELEASE SAVEPOINT " + label + ";");
    if (rc != SQLITE_OK) {
        error_ = failure_text(error_, label.empty() ? "commit failed"
                                                    : "savepoint release failed");
        // The scope's writes must not survive a failed commit: cancel this
        // level (or, for the BEGIN level, the whole transaction) before the
        // Transaction reports the failure.
        if (!label.empty()) {
            int undone = execute("ROLLBACK TO SAVEPOINT " + label + ";");
            if (undone != SQLITE_OK) error_ = failure_text(error_, "rollback failed");
        } else {
            int undone = execute("ROLLBACK;");
            if (undone == SQLITE_OK) tx_names_.clear();
        }
        return rc;
    }
    tx_names_.pop_back();
    return SQLITE_OK;
}

int KnowledgeStore::tx_rollback() {
    if (tx_names_.empty()) {
        if (!error_.empty()) error_ = failure_text(error_, "rollback with no transaction open");
        return SQLITE_OK; // nothing to undo: not a failure
    }
    const std::string label = tx_names_.back();
    // A savepoint rollback is scoped to its own unit: the enclosing transaction
    // (and its savepoint) stays open, so an inner write failure does not
    // destroy work the outer scope already committed-to-be.
    int rc = label.empty() ? execute("ROLLBACK;")
                           : execute("ROLLBACK TO SAVEPOINT " + label + ";");
    if (rc != SQLITE_OK) {
        error_ = failure_text(error_, "rollback failed");
        tx_names_.clear(); // the transaction is unusable: drop the whole scope record
        return rc;
    }
    tx_names_.pop_back();
    return SQLITE_OK;
}

// ---- deterministic fault injection (test hook; zero cost when unset) ----
//
// The regression test for DB-01 has to break a transaction at a chosen
// statement — for example the INSERT that writes the new symbols — and prove
// the whole batch was cancelled. Forcing that from the outside (dropping a
// table, holding an exclusive lock) either targets the wrong statement or is
// slow and flaky. This hook injects the failure at the exact statement:
// execute() reports the configured error for the targeted SQL instead of
// running it, which is what a test of the ERROR PATH needs (the write never
// happens, exactly as if SQLite had failed on it). Inert whenever unset, so
// production behaviour and cost are unchanged.
void KnowledgeStore::set_sql_fault(const std::string& sql_substring, int rc, int max_hits) {
    if (sql_substring.empty() || max_hits <= 0 || rc == SQLITE_OK) return;
    fault_substring_ = sql_substring;
    fault_rc_ = rc;
    fault_hits_ = max_hits;
}

void KnowledgeStore::clear_sql_fault() {
    fault_substring_.clear();
    fault_rc_ = SQLITE_OK;
    fault_hits_ = 0;
}

// True when this statement is the configured fault target and a hit is left.
// Consumes the hit budget so a limited fault breaks exactly one statement —
// the statement the test aims at — and the rest of the run is normal.
bool KnowledgeStore::consume_sql_fault(const std::string& sql) {
    if (fault_hits_ <= 0 || fault_substring_.empty()) return false;
    if (sql.find(fault_substring_) == std::string::npos) return false;
    fault_hits_--;
    PERF_COUNTER("knowledge_store.sql_fault.injected");
    return true;
}

// ---- Transaction (DB-01) ----

KnowledgeStore::Transaction::Transaction(KnowledgeStore& store, const char* name)
    : store_(store) {
    int rc = store_.tx_begin(name);
    if (rc == SQLITE_OK) {
        open_ = true;
        return;
    }
    begin_failed_ = true;
    error_ = store_.get_error();
    if (error_.empty()) error_ = "cannot start transaction";
    std::cerr << "ai-tools: cannot start transaction: " << error_ << "\n";
}

KnowledgeStore::Transaction::~Transaction() {
    // The safety net this class exists for: a scope left without commit —
    // whether by an early return, a failure path, or a plain fallthrough —
    // cancels its writes instead of leaving them open on the connection, where
    // the next unrelated write would silently join them.
    if (open_) rollback();
}

int KnowledgeStore::Transaction::exec(const std::string& sql, const std::vector<std::string>& params) {
    if (!open_) return SQLITE_ERROR; // the BEGIN failed: this scope is inert
    // A poisoned scope keeps running its statements — sqlite3 does not stop a
    // transaction by itself — because only commit() is refused. Statements may
    // also legitimately fail when the transaction is already poisoned, so the
    // FIRST failure is what gets recorded as this scope's cause (the one that
    // made the batch uncommittable), not the last incidental one.
    int rc = store_.execute(sql, params);
    if (!KnowledgeStore::statement_ok(rc)) {
        poisoned_ = true;
        if (error_.empty()) {
            error_ = store_.get_error();
            if (error_.empty()) {
                std::string head = sql.substr(0, sql.find(' '));
                error_ = "statement failed: " + (head.empty() ? "sql" : head);
            }
        }
    }
    return rc;
}

bool KnowledgeStore::Transaction::commit() {
    if (!open_) return false;
    // The core guarantee of DB-01: a batch whose statements were not all
    // verified used to be committed anyway, so an 'indexed' claim could
    // describe knowledge that was never written. A poisoned scope cannot
    // commit; its writes are cancelled instead, and the caller reports the
    // cause it recorded.
    if (poisoned_) {
        rollback();
        if (error_.empty()) error_ = "transaction cancelled: a statement failed";
        return false;
    }
    int rc = store_.tx_commit();
    if (rc != SQLITE_OK) {
        error_ = failure_text(store_.get_error(), "commit failed");
        return false;
    }
    open_ = false;
    return true;
}

bool KnowledgeStore::Transaction::rollback() {
    if (!open_) return false;
    int rc = store_.tx_rollback();
    if (rc != SQLITE_OK) {
        error_ = failure_text(store_.get_error(), "rollback failed");
        std::cerr << "ai-tools: transaction rollback failed: " << error_ << "\n";
        open_ = false;
        return false;
    }
    open_ = false;
    if (poisoned_ && error_.empty()) error_ = "transaction rolled back after a failed statement";
    return true;
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

#ifdef AI_TOOLS_DEBUG
    PERF_SCOPE(sql_probe_key("sql.execute", sql));
    PERF_COUNTER("sql.execute.total");
#endif
    // Fault injection first: prepare_cached() caches statements and reuses
    // them, so a fault must not depend on prepare/step state. The statement is
    // described by the error it reports, exactly like a real failure.
    if (consume_sql_fault(sql)) {
        error_ = sqlite3_errstr(fault_rc_);
        return fault_rc_;
    }

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
    last_query_failed_ = false;

    if (!db_) {
        last_query_failed_ = true;
        error_ = "query on a closed database";
        return results;
    }

#ifdef AI_TOOLS_DEBUG
    PERF_SCOPE(sql_probe_key("sql.query", sql));
    PERF_COUNTER("sql.query.total");
#endif
    // Fault injection applies to reads too: a regression test can force a read
    // to fail and verify the fail-safe direction the callers depend on (an
    // unreadable query must degrade to 'not provably fresh', never to 'data
    // is fine').
    if (consume_sql_fault(sql)) {
        error_ = sqlite3_errstr(fault_rc_);
        last_query_failed_ = true;
        return results;
    }

    sqlite3_stmt* stmt = prepare_cached(sql);
    if (!stmt) {
        last_query_failed_ = true;
        return results;
    }

    for (size_t i = 0; i < params.size(); i++) {
        int rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            error_ = sqlite3_errmsg(db_);
            last_query_failed_ = true;
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
        last_query_failed_ = true;
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
    // DB-01: opening a store means bringing it to a known schema. A failure
    // halfway through used to be survivable: the statements were unchecked, so
    // an open() could report success on a half-migrated or half-created
    // database and every later query would fail on the missing piece. The
    // whole bootstrap now runs in one verified RAII scope: if any schema
    // statement fails, the scope is cancelled and open() returns false — the
    // calling code treats the store as unavailable instead of operating on a
    // database with a broken layout.
    Transaction schema_tx(*this, "init_schema");
    if (schema_tx.begin_failed()) return false;

    // Migrate legacy databases where repo_id was an INTEGER AUTOINCREMENT key.
    {
        auto cols = query("PRAGMA table_info(repositories)");
        bool legacy = false;
        for (const auto& c : cols) {
            // columns: cid, name, type, notnull, dflt_value, pk
            if (c.size() >= 6 && c[1] == "repo_id" && c[2] == "INTEGER") legacy = true;
        }
        if (legacy) {
            // The migration moves data, so it MUST be all-or-nothing: the old
            // BEGIN/COMMIT wrapper ignored every failure between them, and a
            // mid-way failure (e.g. the copy INSERT hitting a constraint on a
            // duplicate root_path) left repositories dropped while
            // repositories_new was still named *_new. As a SAVEPOINT scope it
            // cancels itself on any failure, and a poisoned migration cancels
            // the whole init_schema scope with it (open() fails).
            Transaction migration(*this, "migrate_legacy_repo_id");
            migration.exec("DROP TABLE IF EXISTS repositories_new");
            migration.exec("CREATE TABLE repositories_new ("
                           "repo_id TEXT PRIMARY KEY, "
                           "root_path TEXT UNIQUE NOT NULL, "
                           "name TEXT NOT NULL, "
                           "created_at TEXT NOT NULL, "
                           "updated_at TEXT NOT NULL)");
            migration.exec("INSERT OR IGNORE INTO repositories_new SELECT CAST(repo_id AS TEXT), root_path, name, created_at, updated_at FROM repositories");
            migration.exec("DROP TABLE repositories");
            migration.exec("ALTER TABLE repositories_new RENAME TO repositories");

            // indexing_state had the same INTEGER primary key in legacy schemas
            migration.exec("DROP TABLE IF EXISTS indexing_state_new");
            migration.exec("CREATE TABLE indexing_state_new ("
                           "repo_id TEXT PRIMARY KEY, "
                           "last_full_index_at TEXT, "
                           "last_incremental_at TEXT, "
                           "status TEXT DEFAULT 'idle', "
                           "files_indexed INTEGER DEFAULT 0, "
                           "files_total INTEGER DEFAULT 0, "
                           "full_index_covered INTEGER, "
                           "coverage_computed_at TEXT, "
                           "last_error TEXT)");
            migration.exec("INSERT OR IGNORE INTO indexing_state_new SELECT CAST(repo_id AS TEXT), last_full_index_at, last_incremental_at, status, files_indexed, files_total, NULL, NULL, NULL FROM indexing_state");
            migration.exec("DROP TABLE indexing_state");
            migration.exec("ALTER TABLE indexing_state_new RENAME TO indexing_state");

            if (!migration.commit()) {
                std::cerr << "ai-tools: legacy schema migration failed, store will not open: "
                          << migration.error() << "\n";
                return false; // both scopes cancel: migration's, then schema_tx's
            }
        }
    }

    // Create repositories table (repo_id is a stable identity string)
    schema_tx.exec("CREATE TABLE IF NOT EXISTS repositories ("
                   "repo_id TEXT PRIMARY KEY, "
                   "root_path TEXT UNIQUE NOT NULL, "
                   "name TEXT NOT NULL, "
                   "created_at TEXT NOT NULL, "
                   "updated_at TEXT NOT NULL)");

    // Create files table
    schema_tx.exec("CREATE TABLE IF NOT EXISTS files ("
                   "file_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "repo_id INTEGER NOT NULL, "
                   "path TEXT NOT NULL, "
                   "content_hash TEXT NOT NULL, "
                   "language TEXT, "
                   "file_type TEXT, "
                   "size_bytes INTEGER DEFAULT 0, "
                   "line_count INTEGER DEFAULT 0, "
                   "last_indexed_at TEXT, "
                   "content_verified_at TEXT, "
                   "is_binary INTEGER DEFAULT 0, "
                   "is_deleted INTEGER DEFAULT 0, "
                   "stale INTEGER DEFAULT 0, "
                   "UNIQUE(repo_id, path))");

    // Migration: older databases may lack the stale flag or the content
    // verification stamp (IDX-04). content_verified_at is the moment the
    // index last PROVED that the bytes on disk match content_hash — written
    // even for files a scan skipped as unchanged — while last_indexed_at
    // keeps meaning "when the derived knowledge was written".
    {
        auto file_cols = query("PRAGMA table_info(files)");
        bool has_stale = false, has_verified = false;
        for (const auto& c : file_cols) {
            if (c.size() >= 2 && c[1] == "stale") has_stale = true;
            if (c.size() >= 2 && c[1] == "content_verified_at") has_verified = true;
        }
        if (!has_stale) schema_tx.exec("ALTER TABLE files ADD COLUMN stale INTEGER DEFAULT 0");
        if (!has_verified) {
            schema_tx.exec("ALTER TABLE files ADD COLUMN content_verified_at TEXT");
            // Conservative backfill for legacy rows: treat the last write of
            // the derived knowledge as its last content verification. The
            // effect is exactly today's behaviour (a file written before its
            // indexing looks fresh); the first completed scan re-stamps every
            // visited record and removes the IDX-04 false positive for good.
            // DB-01: the ADD COLUMN and its backfill commit or cancel together
            // — a table with the column and unbackfilled rows would make
            // every legacy record look unverified (permanently stale coverage).
            schema_tx.exec("UPDATE files SET content_verified_at = last_indexed_at "
                           "WHERE content_verified_at IS NULL");
        }
    }
    
    // Create symbols table
    //
    // qualified_name (SUM-01): the deterministic identity of a symbol,
    // derived at indexing time from scope + name ("cache::reset_cache",
    // "Config.init", name alone at top level). symbols.symbol_id is NOT a
    // stable identity — index_snapshot() deletes and re-inserts every symbol
    // row on each re-index — so anything that must survive a re-index
    // (symbol summaries) keys on qualified_name plus the hash of the body,
    // never on symbol_id.
    // body_hash (SUM-01): SHA-256 of the symbol's line range within the
    // snapshot that produced content_hash_at_index, making "this derived
    // knowledge still describes these bytes" decidable per symbol.
    schema_tx.exec("CREATE TABLE IF NOT EXISTS symbols ("
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
                   "content_hash_at_index TEXT, "
                   "qualified_name TEXT, "
                   "body_hash TEXT)");

    // Migration: older databases lack the per-symbol identity columns
    // (SUM-01). qualified_name gets a conservative SQL backfill from the
    // stored scope/name pair (mirrors the IDX-04 content_verified_at
    // backfill shape: the ADD COLUMN and its backfill commit or cancel
    // together inside the schema transaction). body_hash is NOT backfilled:
    // recomputing a body hash needs the exact indexed snapshot bytes, and a
    // hash guessed from the current disk content could certify the wrong
    // version. A missing body_hash means "not yet verified" — the read path
    // only serves a symbol summary for such a row when the file's own
    // content hash still matches disk (then the stored line range IS the
    // indexed body), and the next re-index fills the column.
    {
        auto symbol_cols = query("PRAGMA table_info(symbols)");
        bool has_qualified = false, has_body_hash = false;
        for (const auto& c : symbol_cols) {
            if (c.size() >= 2 && c[1] == "qualified_name") has_qualified = true;
            if (c.size() >= 2 && c[1] == "body_hash") has_body_hash = true;
        }
        if (!has_qualified) {
            schema_tx.exec("ALTER TABLE symbols ADD COLUMN qualified_name TEXT");
            // The separator must match what each language parser uses to spell
            // its scopes ('::' for C++/CUDA, '.' for Python), or the backfilled
            // identity would not match the identity the next re-index writes.
            schema_tx.exec(
                "UPDATE symbols SET qualified_name = CASE "
                "WHEN scope IS NULL OR scope = '' THEN name "
                "ELSE scope || COALESCE((SELECT CASE WHEN f.language = 'python' THEN '.' ELSE '::' END "
                "FROM files f WHERE f.file_id = symbols.file_id), '::') || name END "
                "WHERE qualified_name IS NULL");
        }
        if (!has_body_hash) schema_tx.exec("ALTER TABLE symbols ADD COLUMN body_hash TEXT");
    }

    // Create dependencies table
    schema_tx.exec("CREATE TABLE IF NOT EXISTS dependencies ("
                   "dep_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "repo_id INTEGER NOT NULL, "
                   "source_path TEXT NOT NULL, "
                   "target_path TEXT NOT NULL, "
                   "source_symbol TEXT, "
                   "target_symbol TEXT, "
                   "dep_type TEXT, "
                   "dep_direction TEXT)");

    // Create summaries table
    schema_tx.exec("CREATE TABLE IF NOT EXISTS summaries ("
                   "summary_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "file_id INTEGER NOT NULL, "
                   "content_hash TEXT NOT NULL, "
                   "summary_text TEXT NOT NULL, "
                   "created_at TEXT NOT NULL, "
                   "invalidated_at TEXT, "
                   "is_valid INTEGER DEFAULT 1)");

    // Create symbol_summaries table (SUM-01)
    //
    // Per-symbol semantic summaries: the granularity the project exists for
    // (answer a question about one function without re-reading its file).
    // The file-level `summaries` table above stays as the aggregate level.
    //
    // Identity: (file_id, symbol_key, symbol_type) — symbol_key is the
    // symbols.qualified_name of the symbol at write time. symbol_id is
    // unusable as a key because every re-index deletes and recreates symbol
    // rows; qualified_name is recomputed identically from the same snapshot,
    // so a summary survives a re-index of unchanged content.
    // Validity: (body_hash) — SHA-256 of the symbol's line range within the
    // indexed snapshot. A summary is served only while its body_hash matches
    // a live symbol row's body_hash, exactly as a file summary is served only
    // while its content_hash matches files.content_hash.
    // Provenance (SUM-04 groundwork): who (model/prompt version) produced the
    // text and whether anything verified it. verification_state never holds
    // 'stale' — staleness is expressed by is_valid, so the two channels
    // cannot disagree.
    schema_tx.exec("CREATE TABLE IF NOT EXISTS symbol_summaries ("
                   "symbol_summary_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "file_id INTEGER NOT NULL, "
                   "symbol_key TEXT NOT NULL, "
                   "symbol_type TEXT NOT NULL, "
                   "body_hash TEXT NOT NULL, "
                   "start_line INTEGER, "
                   "end_line INTEGER, "
                   "summary_text TEXT NOT NULL, "
                   "model_version TEXT, "
                   "prompt_version TEXT, "
                   "verification_state TEXT DEFAULT 'unverified', "
                   "created_at TEXT NOT NULL, "
                   "invalidated_at TEXT, "
                   "is_valid INTEGER DEFAULT 1, "
                   "UNIQUE(file_id, symbol_key, symbol_type, body_hash))");

    // Create indexing_state table
    //
    // status describes the *last scan kind*, never coverage:
    //   idle | incremental | partial | full | failed
    // A partial index (single files indexed on demand) must never be taken as
    // proof of a complete index, so a completed full scan records the coverage
    // snapshot it certified: full_index_covered is how many live file records
    // that scan committed to covering, coverage_computed_at when it did, and
    // last_error keeps the failure reason of a scan that did not complete.
    schema_tx.exec("CREATE TABLE IF NOT EXISTS indexing_state ("
                   "repo_id TEXT PRIMARY KEY, "
                   "last_full_index_at TEXT, "
                   "last_incremental_at TEXT, "
                   "status TEXT DEFAULT 'idle', "
                   "files_indexed INTEGER DEFAULT 0, "
                   "files_total INTEGER DEFAULT 0, "
                   "full_index_covered INTEGER, "
                   "coverage_computed_at TEXT, "
                   "last_error TEXT)");

    // Migration: older databases lack the coverage columns of the index state
    {
        auto state_cols = query("PRAGMA table_info(indexing_state)");
        bool has_covered = false, has_computed = false, has_error = false;
        for (const auto& c : state_cols) {
            if (c.size() >= 2 && c[1] == "full_index_covered") has_covered = true;
            if (c.size() >= 2 && c[1] == "coverage_computed_at") has_computed = true;
            if (c.size() >= 2 && c[1] == "last_error") has_error = true;
        }
        if (!has_covered) schema_tx.exec("ALTER TABLE indexing_state ADD COLUMN full_index_covered INTEGER");
        if (!has_computed) schema_tx.exec("ALTER TABLE indexing_state ADD COLUMN coverage_computed_at TEXT");
        if (!has_error) schema_tx.exec("ALTER TABLE indexing_state ADD COLUMN last_error TEXT");
    }

    // Create indexes
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_files_repo_path ON files(repo_id, path)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_files_content_hash ON files(content_hash)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file_id)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name)");
    // SUM-01: per-file symbol lookups by the stable identity (qualified name)
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_symbols_qualified ON symbols(file_id, qualified_name)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_deps_repo ON dependencies(repo_id)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_deps_source ON dependencies(source_path)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_deps_target ON dependencies(repo_id, target_path)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_deps_type ON dependencies(repo_id, dep_type)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_deps_source_symbol ON dependencies(repo_id, source_symbol)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_deps_target_symbol ON dependencies(repo_id, target_symbol)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_summaries_file ON summaries(file_id)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_summaries_hash ON summaries(content_hash)");
    // SUM-01: hot lookups are 'all valid summaries of one file' (list mode)
    // and 'the summary matching this live symbol row' (single read) — both
    // lead with file_id, so the covering index mirrors that access shape.
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_symbol_summaries_file ON symbol_summaries(file_id)");
    schema_tx.exec("CREATE INDEX IF NOT EXISTS idx_symbol_summaries_lookup ON symbol_summaries(file_id, symbol_key, symbol_type, body_hash)");

    // Success is a COMMITTED success: an unverified 'return true' at the end of
    // a half-failed bootstrap is the exact failure mode DB-01 exists to remove.
    if (!schema_tx.commit()) {
        std::cerr << "ai-tools: schema initialization failed, store will not open: "
                  << schema_tx.error() << "\n";
        return false;
    }
    return true;
}

}
