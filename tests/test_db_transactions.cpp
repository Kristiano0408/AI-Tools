// Tests for DB-01: SQLite errors during indexing must be propagated, the
// transaction must be cancelled, the old index must stay coherent, and no
// success state may be recorded.
//
// Fault injection: KnowledgeStore::set_sql_fault() makes execute()/query()
// report a chosen SQLite error for the SQL containing a substring, at most
// max_hits times — without running the targeted statement. That breaks the
// exact statement a transaction is made of (e.g. the INSERT of the new
// symbols) and proves the whole batch was cancelled, with nothing applied.

#include "engine/knowledge_store.h"
#include "engine/content_hasher.h"
#include "engine/cache_manager.h"
#include "engine/repository.h"
#include "engine/indexer.h"
#include "engine/summary_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  PASS: " << what << "\n";
    } else {
        std::cout << "  FAIL: " << what << "\n";
        ++failures;
    }
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

static void must_execute(knowledge_store::KnowledgeStore& store, const std::string& sql,
                         const std::vector<std::string>& params = {}) {
    if (!knowledge_store::KnowledgeStore::statement_ok(store.execute(sql, params))) {
        std::cout << "  FAIL: fixture setup write failed: " << sql << " (" << store.get_error() << ")\n";
        ++failures;
    }
}

static int row_int(knowledge_store::KnowledgeStore& store, const std::string& sql,
                   const std::vector<std::string>& params = {}) {
    auto rows = store.query(sql, params);
    if (rows.empty() || rows[0].empty()) return -1;
    try { return std::stoi(rows[0][0]); } catch (...) { return -1; }
}

struct Snapshot {
    std::string content_hash;
    long long size_bytes = -1;
    std::set<std::string> symbols;
    int deps = 0;
    int stale = 0;
};

static Snapshot index_state(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                            const std::string& path) {
    Snapshot s;
    auto rows = store.query(
        "SELECT content_hash, size_bytes, stale FROM files WHERE repo_id = ? AND path = ?",
        repo_id, path);
    if (!rows.empty() && rows[0].size() >= 3) {
        s.content_hash = rows[0][0];
        try { s.size_bytes = std::stoll(rows[0][1]); } catch (...) {}
        try { s.stale = std::stoi(rows[0][2]); } catch (...) {}
    }
    auto syms = store.query(
        "SELECT name FROM symbols WHERE file_id IN "
        "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)", repo_id, path);
    for (const auto& r : syms) if (!r.empty()) s.symbols.insert(r[0]);
    s.deps = row_int(store,
        "SELECT COUNT(*) FROM dependencies WHERE repo_id = ? AND source_path = ?", {repo_id, path});
    return s;
}

int run_db_transaction_tests() {
    failures = 0;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "ai-tools-dbtx-tests";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    setenv("HOME", tmp.c_str(), 1);
    setenv("AI_TOOLS_DATA_DIR", (tmp / ".ai-tools").c_str(), 1);

    const char* kV1 = "int alpha_only_v1() { return 1; }\nint common_fn() { return 2; }\n";
    const char* kV2 = "int bravo_only_v2() { return 3; }\nint common_fn() { return 4; }\n";

    // ---- T-08a: failing symbols INSERT cancels the whole file update ----
    std::cout << "[fault during symbols INSERT rolls back index_snapshot (T-08a)]\n";
    {
        std::string repo_root = (tmp / "fault-symbols").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/unit.cpp";
        write_file(path, kV1);
        auto first = indexer::index_snapshot(store, path, repo.repo_id, kV1);
        check(first.indexed, "baseline index_snapshot succeeds");
        Snapshot before = index_state(store, repo.repo_id, path);
        check(before.symbols.count("alpha_only_v1") == 1, "baseline symbols written");

        // A second version exists on disk; indexing it must fail at the exact
        // INSERT of the new symbols.
        write_file(path, kV2);
        store.set_sql_fault("INSERT INTO symbols", SQLITE_CONSTRAINT);
        auto failed = indexer::index_snapshot(store, path, repo.repo_id, kV2);
        store.clear_sql_fault();

        check(failed.indexed == false, "index_snapshot reports failure (indexed == false)");
        check(!failed.error.empty(), "the failure carries a cause (report.error non-empty)");
        check(failed.dependents.empty() && !failed.propagated,
              "a rolled-back update does not claim dependents were invalidated");

        Snapshot after = index_state(store, repo.repo_id, path);
        check(after.content_hash == before.content_hash,
              "OLD files.content_hash intact after the rollback (no half write)");
        check(after.symbols == before.symbols,
              "OLD symbols intact: the DELETE+INSERT pair cancelled together");
        check(after.content_hash != content_hasher::compute_string_hash(kV2),
              "the superseded version was never stored");
        check(!index_state(store, repo.repo_id, path).symbols.count("bravo_only_v2"),
              "no new-version symbol leaked into the old index");

        // The index did not register a success: needs_indexing still flags it,
        // and a clean re-index (fault removed) lands the new version fully.
        check(indexer::needs_indexing(store, path, repo.repo_id),
              "the file is still marked as needing re-indexing (no false success state)");
        auto retry = indexer::index_snapshot(store, path, repo.repo_id, kV2);
        check(retry.indexed, "after clearing the fault the same update succeeds");
        Snapshot healed = index_state(store, repo.repo_id, path);
        check(healed.content_hash == content_hasher::compute_string_hash(kV2) &&
                  healed.symbols.count("bravo_only_v2") == 1 &&
                  healed.symbols.count("alpha_only_v1") == 0,
              "full recovery: consistent new version, old symbols gone");

        // The indexing_state must not show this file as freshly indexed.
        auto state = store.query(
            "SELECT status FROM indexing_state WHERE repo_id = ?", repo.repo_id);
        check(!state.empty() && state[0][0] == "partial",
              "single-file indexing state stays 'partial' (IDX-01 shape preserved)");
    }

    // ---- T-08b: COMMIT failure cancels everything ----
    std::cout << "[fault at COMMIT rolls back the whole batch (T-08b)]\n";
    {
        std::string repo_root = (tmp / "fault-commit").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/unit.cpp";
        write_file(path, kV1);
        auto first = indexer::index_snapshot(store, path, repo.repo_id, kV1);
        (void)first;
        Snapshot before = index_state(store, repo.repo_id, path);

        write_file(path, kV2);
        // Target the commit issued by index_snapshot; the scope label differs
        // from other commit shapes, so the literal COMMIT statement is the hit.
        store.set_sql_fault("COMMIT", SQLITE_BUSY);
        auto failed = indexer::index_snapshot(store, path, repo.repo_id, kV2);
        store.clear_sql_fault();

        check(!failed.indexed, "commit failure -> indexed == false");
        check(!failed.error.empty(), "commit failure carries the cause");
        Snapshot after = index_state(store, repo.repo_id, path);
        check(after.content_hash == before.content_hash && after.symbols == before.symbols,
              "commit rollback restored the previous snapshot (nothing partially applied)");
        check(!failed.propagated && failed.dependents.empty(),
              "failed commit does not claim an invalidation that never happened");
    }

    // ---- T-08c: failing first statement (UPDATE files) leaves the record untouched ----
    std::cout << "[fault on the first write cancels before any visible change (T-08c)]\n";
    {
        std::string repo_root = (tmp / "fault-first-write").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/unit.cpp";
        write_file(path, kV1);
        indexer::index_snapshot(store, path, repo.repo_id, kV1);
        Snapshot before = index_state(store, repo.repo_id, path);

        write_file(path, kV2);
        store.set_sql_fault("UPDATE files SET content_hash", SQLITE_IOERR);
        auto failed = indexer::index_snapshot(store, path, repo.repo_id, kV2);
        store.clear_sql_fault();

        check(!failed.indexed && !failed.error.empty(), "first write failure reported");
        Snapshot after = index_state(store, repo.repo_id, path);
        check(after.content_hash == before.content_hash &&
                  after.size_bytes == before.size_bytes &&
                  after.symbols == before.symbols,
              "record completely unchanged when the first statement of the batch failed");
    }

    // ---- T-08d: CacheManager::invalidate reports failure and applies nothing ----
    std::cout << "[cache invalidation is all-or-nothing and honest (T-08d)]\n";
    {
        std::string repo_root = (tmp / "fault-invalidate").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        write_file(repo_root + "/base.h", "#pragma once\nint base_fn();\n");
        write_file(repo_root + "/leaf.cpp", "#include \"base.h\"\nint leaf() { return base_fn(); }\n");
        indexer::index_file(store, repo_root + "/base.h", repo.repo_id);
        indexer::index_file(store, repo_root + "/leaf.cpp", repo.repo_id);

        store.set_sql_fault("UPDATE files SET stale", SQLITE_IOERR);
        cache_manager::CacheManager cache(store);
        auto res = cache.invalidate(repo_root + "/base.h", repo.repo_id);
        store.clear_sql_fault();

        check(res.success == false, "invalidate() reports success == false on a storage failure");
        check(!res.error.empty(), "invalidate() carries the failure cause");
        check(res.dependents.empty(),
              "a failed invalidation does not report dependents as invalidated");

        // Nothing was applied: neither the file's own reap nor the dependent's
        // made it into the database — both cancelled together.
        int leaf_stale = row_int(store,
            "SELECT stale FROM files WHERE repo_id = ? AND path = ?",
            {repo.repo_id, repo_root + "/leaf.cpp"});
        int base_stale = row_int(store,
            "SELECT stale FROM files WHERE repo_id = ? AND path = ?",
            {repo.repo_id, repo_root + "/base.h"});
        check(leaf_stale == 0 && base_stale == 0,
              "no stale marks survived the rolled-back invalidation");
        int leaf_syms = row_int(store,
            "SELECT COUNT(*) FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            {repo.repo_id, repo_root + "/leaf.cpp"});
        check(leaf_syms > 0, "dependent's cached symbols intact after rollback");

        // The same invalidation without the fault works end to end.
        auto ok = cache.invalidate(repo_root + "/base.h", repo.repo_id);
        check(ok.success && !ok.dependents.empty(), "invalidation succeeds once the fault is removed");
    }

    // ---- T-08e: full scan with a fault mid-reindex fails honestly and heals ----
    std::cout << "[scan failure is recorded, blocks coverage, and heals on retry (T-08e)]\n";
    {
        std::string repo_root = (tmp / "fault-scan").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        write_file(repo_root + "/one.cpp", "int one_symbol() { return 1; }\n");
        write_file(repo_root + "/two.cpp", "int two_symbol() { return 2; }\n");
        auto baseline = indexer::index_repository(store, repo.repo_id);
        check(baseline.ok && baseline.files_refreshed == 2, "baseline full scan indexes both files");
        check(indexer::repo_coverage(store, repo.repo_id, 0).coverage ==
                  indexer::IndexCoverage::full_fresh,
              "baseline coverage certified full_fresh");

        // Change both files; make the first symbols INSERT of the scan fail
        // (max_hits=1 breaks exactly the first re-indexed file; which one is
        // walk order, deliberately not asserted). The other file may or may
        // not have been re-indexed yet — no claim about that — but the scan
        // must stop and refuse to certify coverage.
        write_file(repo_root + "/one.cpp", "int one_symbol_v2() { return 1; }\n");
        write_file(repo_root + "/two.cpp", "int two_symbol_v2() { return 2; }\n");
        store.set_sql_fault("INSERT INTO symbols", SQLITE_CONSTRAINT, 1);
        auto scan = indexer::index_repository(store, repo.repo_id);
        store.clear_sql_fault();

        check(!scan.ok, "scan with a failing file re-index returns ok == false");
        check(!scan.error.empty(), "FullScanResult.error names the failing file/cause");
        auto st = store.query(
            "SELECT status, full_index_covered, last_error FROM indexing_state WHERE repo_id = ?",
            repo.repo_id);
        check(!st.empty() && st[0].size() >= 3 && st[0][0] == "failed",
              "indexing_state records status 'failed'");
        check(st.empty() || st[0][1].empty(),
              "coverage snapshot dropped: a failed scan certifies nothing");
        check(!st.empty() && !st[0][2].empty(),
              "indexing_state.last_error records the scan cause");
        auto cov = indexer::repo_coverage(store, repo.repo_id, 0);
        check(cov.coverage != indexer::IndexCoverage::full_fresh,
              "coverage is not full_fresh after a failed scan (no false completeness)");
        check(!cov.last_error.empty(), "coverage report carries the last scan error");

        // Healing: a clean re-scan re-indexes everything left stale and
        // certifies coverage again.
        auto healed = indexer::index_repository(store, repo.repo_id);
        auto cov2 = indexer::repo_coverage(store, repo.repo_id, -1);
        check(healed.ok && cov2.coverage == indexer::IndexCoverage::full_fresh,
              "rescan after the fault heals to full_fresh");
        auto sym1 = store.query(
            "SELECT name FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo.repo_id, repo_root + "/one.cpp");
        auto sym2 = store.query(
            "SELECT name FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo.repo_id, repo_root + "/two.cpp");
        bool v1 = false, v2 = false;
        for (const auto& r : sym1) if (!r.empty()) { if (r[0] == "one_symbol_v2") v1 = true; }
        for (const auto& r : sym2) if (!r.empty()) { if (r[0] == "two_symbol_v2") v2 = true; }
        check(v1 && v2,
              "healing scan re-indexes every changed file (the failed one included) to v2 symbols");
    }

    // ---- T-08f: nested scopes compose (savepoint), inner failure does not commit/discarding outer work ----
    std::cout << "[nested transactions are safe (T-08f)]\n";
    {
        knowledge_store::KnowledgeStore store;
        check(store.open((tmp / "nested.db").string()), "store opens");
        must_execute(store, "CREATE TABLE n (a INTEGER)");

        knowledge_store::KnowledgeStore::Transaction outer(store, "outer");
        outer.exec("INSERT INTO n VALUES (1)");
        {
            knowledge_store::KnowledgeStore::Transaction inner(store, "inner");
            inner.exec("INSERT INTO n VALUES (2)");
            inner.exec("INSERT INTO nonexistent_inner_table VALUES (3)"); // poisons inner
            check(inner.poisoned(), "inner scope records the failure");
            check(inner.commit() == false, "a poisoned inner scope refuses to commit");
            check(!inner.error().empty(), "inner scope reports its cause");
        }
        // The BEGIN-level scope survived: COMMIT inside a nested scope must
        // not commit or cancel the outer's work.
        check(outer.commit(), "outer scope still commits after a failed inner scope");
        auto rows = store.query("SELECT a FROM n ORDER BY a");
        check(rows.size() == 1 && rows[0][0] == "1",
              "outer write kept, inner write cancelled (savepoint semantics, no leaked commit)");

        // A scope left without commit (early return shape) is rolled back by
        // the destructor, not left open on the connection.
        {
            knowledge_store::KnowledgeStore::Transaction dropped(store, "dropped");
            dropped.exec("INSERT INTO n VALUES (9)");
        }
        auto rows2 = store.query("SELECT a FROM n ORDER BY a");
        check(rows2.size() == 1,
              "an uncommitted scope is cancelled by its destructor (no partial leak)");
        must_execute(store, "INSERT INTO n VALUES (3)"); // connection still usable
        check(row_int(store, "SELECT COUNT(*) FROM n") == 2, "connection healthy after scopes closed");
    }

    // ---- Summary: the invalidation+insert pair is atomic ----
    std::cout << "[summary write is transactional and honest (DB-01)]\n";
    {
        std::string repo_root = (tmp / "fault-summary").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/unit.cpp";
        write_file(path, kV1);
        indexer::index_snapshot(store, path, repo.repo_id, kV1);
        check(summary_store::store_file_summary(store, repo.repo_id, path, "first summary"),
              "baseline summary stores");

        store.set_sql_fault("INSERT INTO summaries", SQLITE_CONSTRAINT);
        bool stored = summary_store::store_file_summary(store, repo.repo_id, path, "second summary");
        store.clear_sql_fault();
        check(!stored, "a failing summary store reports false");
        check(summary_store::get_file_summary(store, repo.repo_id, path) == "first summary",
              "the previous summary is still valid: invalidation+insert cancelled together");

        check(summary_store::store_file_summary(store, repo.repo_id, path, "third summary"),
              "summary store succeeds after the fault is removed");
        check(summary_store::get_file_summary(store, repo.repo_id, path) == "third summary",
              "recovery: the new summary replaced the old one");
    }

    // ---- Fault injection hook itself is inert/safe ----
    std::cout << "[fault hook is inert when unset and scoped when set]\n";
    {
        knowledge_store::KnowledgeStore store;
        check(store.open((tmp / "hook.db").string()), "store opens");
        must_execute(store, "CREATE TABLE h (a INTEGER)");
        must_execute(store, "INSERT INTO h VALUES (1)");
        // max_hits=1: only the first matching statement fails.
        store.set_sql_fault("INSERT INTO h", SQLITE_CONSTRAINT, 1);
        int rc1 = store.execute("INSERT INTO h VALUES (2)");
        check(rc1 != SQLITE_OK, "first targeted statement fails as configured");
        int rc2 = store.execute("INSERT INTO h VALUES (3)");
        check(rc2 == SQLITE_OK, "after the hit budget is spent the store is normal again");
        check(row_int(store, "SELECT COUNT(*) FROM h") == 2,
              "the injected statement was not executed, the next one was");
        store.clear_sql_fault();
        check(store.execute("INSERT INTO h VALUES (4)") == SQLITE_OK, "clear_sql_fault fully restores");
    }

    fs::remove_all(tmp);
    std::cout << (failures == 0 ? "\n[db transaction tests] ALL PASSED\n"
                                : "\n[db transaction tests] " + std::to_string(failures) + " FAILURES\n");
    return failures;
}
