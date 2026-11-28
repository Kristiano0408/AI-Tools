#pragma once

#include <sqlite3.h>
#include <concepts>
#include <functional>
#include <unordered_dense.h>
#include <string>
#include <vector>

namespace knowledge_store {

template <typename T>
concept StringConvertibleParam = std::convertible_to<T, std::string> || std::integral<T>;

template <typename... Args>
concept StringConvertible = (StringConvertibleParam<Args> && ...);

class KnowledgeStore {
public:
    KnowledgeStore();
    ~KnowledgeStore();
    
    bool open(const std::string& db_path);
    void close();

    // True when an execute()/Transaction::exec() result means the statement
    // ran to completion (SQLITE_OK) or produced a row (SQLITE_ROW: e.g. a
    // PRAGMA that returns a value). Every checked write uses this instead of
    // comparing against SQLITE_OK directly.
    static bool statement_ok(int rc) { return rc == SQLITE_OK || rc == SQLITE_ROW; }

    // Execute SQL with parameters.
    //
    // DB-01: [[nodiscard]]. A write whose result is discarded is exactly the
    // shape of the bug this issue describes — a failing statement inside a
    // BEGIN/COMMIT batch that still gets committed, leaving the index
    // claiming success on knowledge that was never written. -Wunused-result
    // (-Wall -Wextra are on in CMakeLists.txt) makes every unchecked call a
    // compile warning, and Transaction::exec() is the way to batch writes so
    // one failure fails the whole scope.
    [[nodiscard]] int execute(const std::string& sql, const std::vector<std::string>& params = {});
    [[nodiscard]] int execute(const std::string& sql, const std::string& p1);
    [[nodiscard]] int execute(const std::string& sql, const std::string& p1, const std::string& p2);
    [[nodiscard]] int execute(const std::string& sql, const std::string& p1, const std::string& p2, const std::string& p3);
    [[nodiscard]] int execute(const std::string& sql, const std::string& p1, const std::string& p2, const std::string& p3, const std::string& p4);
    
    // Variadic execute for any number of string-convertible parameters
    template <typename... Args>
        requires StringConvertible<Args...>
    [[nodiscard]] int execute(const std::string& sql, Args... params) {
        std::vector<std::string> param_vec;
        (..., param_vec.push_back(to_param(std::forward<Args>(params))));
        return execute(sql, param_vec);
    }
    
    // Query SQL and return results
    std::vector<std::vector<std::string>> query(const std::string& sql);
    std::vector<std::vector<std::string>> query(const std::string& sql, const std::vector<std::string>& params);
    std::vector<std::vector<std::string>> query(const std::string& sql, const std::string& p1);
    std::vector<std::vector<std::string>> query(const std::string& sql, const std::string& p1, const std::string& p2);

    // Variadic query for any number of string-convertible parameters
    template <typename... Args>
        requires StringConvertible<Args...>
    std::vector<std::vector<std::string>> query(const std::string& sql, Args... params) {
        std::vector<std::string> param_vec;
        (..., param_vec.push_back(to_param(std::forward<Args>(params))));
        return query(sql, param_vec);
    }
    
    // Get last error message
    std::string get_error() const;

    // True when the most recent query() ended in a failure (bad SQL, step
    // error, bind error, injected fault) rather than an empty-but-successful
    // result. An empty result set is a legitimate answer to a question; a
    // failed read is not. Reconciliation and coverage code must tell these
    // apart (DB-01): treating a failed read as 'nobody depends on this file'
    // is how a tombstone silently orphans a dependent's knowledge. Always
    // false before the first query, and reset by every query.
    bool last_query_failed() const { return last_query_failed_; }

    // Check if database is open
    bool is_open() const;

    // ---- deterministic fault injection (regression-test hook) ----
    //
    // Make execute()/query() report a chosen SQLite error for every SQL
    // containing sql_substring, at most max_hits times. The statement itself is
    // not executed: only the value REPORTED to the caller changes, exactly like
    // a real failure — which is what a test of the ERROR PATH needs: DB-01 is
    // verified by making the INSERT of the new symbols fail and observing the
    // whole transaction being cancelled. Inert when unset (zero overhead).
    void set_sql_fault(const std::string& sql_substring, int rc, int max_hits = 1);
    void clear_sql_fault();

    // ---- RAII transaction (DB-01) ----
    //
    // All-or-nothing scope over a group of writes. The design goal is that
    // ignoring a write result is NOT EXPRESSED inside the scope: exec() is the
    // way to run a statement there, and the first failing statement makes
    // commit() impossible — so a batch can never be committed with one of its
    // statements failed, and a scope that is left without a commit is rolled
    // back by the destructor instead of leaking into the connection.
    //
    // Nesting is safe: the outermost scope is BEGIN/COMMIT, each inner scope is
    // a SAVEPOINT, so a failing unit is cancelled without committing or
    // discarding the enclosing scope's work. The error of commit() is recorded
    // for the caller to propagate (UpdateReport::error, FullScanResult::error,
    // InvalidationResult::error): the store is not a signalling-by-exception
    // module, so the existing return-code channels remain the error path.
    class Transaction {
    public:
        // Begins the scope immediately; explicit failure (begin_failed() +
        // error()) instead of throwing from a constructor.
        explicit Transaction(KnowledgeStore& store, const char* name = nullptr);
        ~Transaction();

        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        // Runs one statement through the store and remembers the first failure
        // as this scope's cause. All parameters are bound as text, exactly as
        // KnowledgeStore::execute() binds them. statement_ok() is the success
        // test (PRAGMAs answer with SQLITE_ROW).
        int exec(const std::string& sql, const std::vector<std::string>& params);

        template <typename... Args>
            requires StringConvertible<Args...>
        int exec(const std::string& sql, Args... params) {
            std::vector<std::string> param_vec;
            (..., param_vec.push_back(to_param(std::forward<Args>(params))));
            return exec(sql, param_vec);
        }

        // No-parameter statement (a PRAGMA, or a literal-only UPDATE).
        int exec(const std::string& sql) { return exec(sql, std::vector<std::string>{}); }

        // True when a statement in this scope failed: commit() would be a lie.
        bool poisoned() const { return poisoned_; }

        // Commits the scope. Refuses to commit a scope where any statement
        // failed, cancelling the scope's writes instead; on a failure error()
        // holds the cause (the failing statement's message, or the commit
        // failure's message). After a failed commit the scope is rolled back,
        // so the caller's 'nothing was written' claim stays true.
        bool commit();

        // Cancel this scope's writes explicitly. Also happens in the
        // destructor when commit() was never called.
        bool rollback();

        // The first failure recorded by exec(), else the commit/rollback
        // failure, else the store's last error. Never describes a success.
        std::string error() const {
            return error_.empty() ? store_.get_error() : error_;
        }

        // False when BEGIN/SAVEPOINT itself failed: the scope is inert, exec()
        // and commit() do nothing and error() explains why.
        bool begin_failed() const { return begin_failed_; }

    private:
        KnowledgeStore& store_;
        bool open_ = false;
        bool poisoned_ = false;
        bool begin_failed_ = false;
        std::string error_;
    };

private:
    sqlite3* db_ = nullptr;
    std::string error_;
    ankerl::unordered_dense::map<std::string, sqlite3_stmt*> stmt_cache_;

    // One entry per open transaction level: the BEGIN level has an empty name
    // (the outermost transaction has one identity, COMMIT/ROLLBACK address it
    // by name), each nested level holds the name of its savepoint, so
    // COMMIT/ROLLBACK TO always target the scope they belong to.
    std::vector<std::string> tx_names_;

    // Fault-injection state (see set_sql_fault). rc == SQLITE_OK means unset.
    std::string fault_substring_;
    int fault_rc_ = SQLITE_OK;
    int fault_hits_ = 0;

    // Set true by a query() that failed, false by one that succeeded (even
    // with zero rows). See last_query_failed().
    bool last_query_failed_ = false;

    // Initialize schema
    bool init_schema();

    // Transaction plumbing used by Transaction: BEGIN at the outermost level,
    // SAVEPOINT for every nested one. tx_rollback() cancels the innermost open
    // scope only; the enclosing level stays open.
    int tx_begin(const char* name);
    int tx_commit();
    int tx_rollback();
    static std::string next_savepoint_name();

    // True and consuming when this SQL is the configured fault target.
    bool consume_sql_fault(const std::string& sql);

    // Return a cached prepared statement for the SQL, resetting it and clearing
    // bindings. Statements are reset (never left mid-step) after each use, so
    // transactions (BEGIN/COMMIT) remain safe. Returns nullptr on failure.
    sqlite3_stmt* prepare_cached(const std::string& sql);

    static std::string to_param(const std::string& s) { return s; }
    static std::string to_param(const char* s) { return std::string(s); }
    template <typename T>
        requires std::integral<T>
    static std::string to_param(T v) { return std::to_string(v); }
};

}
