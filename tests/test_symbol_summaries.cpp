// SUM-01 — per-symbol semantic summaries: identity stability, granularity,
// validity, propagation, provenance, transactional honesty, and the migration
// path for databases written before the feature existed.
//
// The acceptance property of SUM-01 is granularity: a change confined to one
// function must not discard the knowledge cached about its untouched neighbours,
// and a symbol whose body really did change must never keep serving a summary of
// the old body — while both states stay distinguishable from 'nothing stored'.

#include "engine/knowledge_store.h"
#include "engine/content_hasher.h"
#include "engine/repository.h"
#include "engine/indexer.h"
#include "engine/cache_manager.h"
#include "engine/symbol_index.h"
#include "engine/summary_store.h"
#include "utils/file_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
    auto rows = params.empty() ? store.query(sql) : store.query(sql, params);
    if (rows.empty() || rows[0].empty()) return -1;
    try { return std::stoi(rows[0][0]); } catch (...) { return -1; }
}

// The summary_store read of one symbol, as a short readable string, so an
// assertion can name the state it expected instead of only pass/fail.
static std::string read_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                                const std::string& path, const std::string& symbol,
                                const std::string& type = "") {
    summary_store::SymbolSummaryInfo info;
    std::string text = summary_store::get_symbol_summary(store, repo_id, path, symbol, type, info);
    return text.empty() ? info.status : text;
}

static bool store_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                          const std::string& path, const std::string& symbol,
                          const std::string& text, const std::string& type = "") {
    summary_store::SymbolSummaryInfo info;
    return summary_store::store_symbol_summary(store, repo_id, path, symbol, type, text,
                                               "model-test-1", "prompt-v1", info);
}

// Stored rows for one symbol identity (any validity): tells 'nothing was ever
// stored' apart from 'a summary exists and was invalidated'.
static int summary_rows(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                        const std::string& path, const std::string& symbol_key) {
    return row_int(store,
        "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
        "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND ss.symbol_key = ?",
        {repo_id, path, symbol_key});
}

// A two-function C++ file: the fixture SUM-01's granularity claims are tested against.
static const char* kTwoFn =
    "int alpha() { return 1; }\n"
    "int beta() { return 2; }\n";
static const char* kTwoFnAlphaChanged =
    "int alpha() { return 100; }\n"
    "int beta() { return 2; }\n";

int run_symbol_summary_tests() {
    failures = 0;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "ai-tools-symbol-summary-tests";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    setenv("HOME", tmp.c_str(), 1);
    setenv("AI_TOOLS_DATA_DIR", (tmp / ".ai-tools").c_str(), 1);

    // ---- T-06: two distinct summaries for two functions of the same file ----
    std::cout << "[two symbols in one file carry independent summaries (T-06)]\n";
    {
        std::string repo_root = (tmp / "granularity").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        check(indexer::index_file(store, path, repo.repo_id).indexed, "fixture indexed");

        check(store_summary(store, repo.repo_id, path, "alpha", "returns one"), "alpha summary stored");
        check(store_summary(store, repo.repo_id, path, "beta", "returns two"), "beta summary stored");

        check(read_summary(store, repo.repo_id, path, "alpha") == "returns one", "alpha summary readable");
        check(read_summary(store, repo.repo_id, path, "beta") == "returns two", "beta summary readable");

        // The index carries the identity each summary keys on.
        auto regions = symbol_index::list_file_symbol_regions(store, repo.repo_id, path);
        bool qualified_present = false, hashes_present = true;
        for (const auto& r : regions) {
            if (r.qualified_name == "alpha" && r.name == "alpha") qualified_present = true;
            if (!r.has_body_hash) hashes_present = false;
        }
        check(qualified_present, "symbols carry a qualified_name identity after indexing");
        check(hashes_present && regions.size() == 2, "every indexed symbol carries a body hash");

        // The identity is stable across a re-index: symbol_id is not usable as a
        // key (index_snapshot deletes and re-inserts), and the summary survived
        // precisely because the identity is recomputed from the snapshot.
        auto id_before = store.query(
            "SELECT symbol_id FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) ORDER BY start_line",
            repo.repo_id, path);
        check(indexer::index_file(store, path, repo.repo_id).indexed ||
                  !indexer::needs_indexing(store, path, repo.repo_id),
              "re-indexing the unchanged file is a no-op update");
        auto id_after = store.query(
            "SELECT symbol_id FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) ORDER BY start_line",
            repo.repo_id, path);
        check(id_before.size() == 2 && id_after.size() == 2, "both symbol rows present before and after");
        check(read_summary(store, repo.repo_id, path, "alpha") == "returns one" &&
                  read_summary(store, repo.repo_id, path, "beta") == "returns two",
              "both summaries survive a re-index of unchanged content (identity is not symbol_id)");
    }

    // ---- THE acceptance test: an edit confined to one symbol invalidates only it ----
    std::cout << "[edit confined to one function keeps the other summary valid]\n";
    {
        std::string repo_root = (tmp / "confined-edit").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        indexer::index_file(store, path, repo.repo_id);
        store_summary(store, repo.repo_id, path, "alpha", "returns one");
        store_summary(store, repo.repo_id, path, "beta", "returns two");

        // Re-index the version where ONLY alpha's body changed.
        write_file(path, kTwoFnAlphaChanged);
        auto report = indexer::index_file(store, path, repo.repo_id);
        check(report.indexed && report.changed, "the changed snapshot superseded the stored one");

        check(read_summary(store, repo.repo_id, path, "beta") == "returns two",
              "UNCHANGED sibling function keeps its valid summary after its file was re-indexed");
        std::string alpha = read_summary(store, repo.repo_id, path, "alpha");
        check(alpha == summary_store::symbol_status::kStaleBody,
              "CHANGED function reports stale_symbol_body (a summary exists, it describes other bytes)");
        check(summary_rows(store, repo.repo_id, path, "alpha") >= 1,
              "the superseded alpha summary is kept as an invalidated row, not erased");

        // Writing a fresh summary for the changed symbol revalidates it, and does
        // not disturb the sibling.
        check(store_summary(store, repo.repo_id, path, "alpha", "returns one hundred"),
              "changed symbol accepts a new summary");
        check(read_summary(store, repo.repo_id, path, "alpha") == "returns one hundred",
              "the new alpha summary is valid");
        check(read_summary(store, repo.repo_id, path, "beta") == "returns two",
              "sibling summary untouched by the new write");
    }

    // ---- Reverted edit: the exact previous snapshot revives its summary ----
    std::cout << "[reverted edit revives the symbol summary it replaced]\n";
    {
        std::string repo_root = (tmp / "reverted").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        indexer::index_snapshot(store, path, repo.repo_id, kTwoFn);
        store_summary(store, repo.repo_id, path, "alpha", "returns one");

        write_file(path, kTwoFnAlphaChanged);
        indexer::index_snapshot(store, path, repo.repo_id, kTwoFnAlphaChanged);
        check(read_summary(store, repo.repo_id, path, "alpha") == summary_store::symbol_status::kStaleBody,
              "while the changed version is indexed, the old summary is stale");

        // The original bytes come back (a revert), and the hash-driven revival
        // brings the summary back without rewriting it.
        write_file(path, kTwoFn);
        indexer::index_snapshot(store, path, repo.repo_id, kTwoFn);
        check(read_summary(store, repo.repo_id, path, "alpha") == "returns one",
              "the stored summary is revived when the exact snapshot it described reappears");
    }

    // ---- Idempotency: re-indexing unchanged content invalidates nothing ----
    std::cout << "[re-indexing unchanged content propagates and invalidates nothing]\n";
    {
        std::string repo_root = (tmp / "idempotent").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        indexer::index_file(store, path, repo.repo_id);
        store_summary(store, repo.repo_id, path, "alpha", "returns one");
        store_summary(store, repo.repo_id, path, "beta", "returns two");

        auto again = indexer::index_file(store, path, repo.repo_id);
        check(!again.changed && !again.propagated, "unchanged snapshot supersedes nothing");
        int valid = row_int(store,
            "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND ss.is_valid = 1",
            {repo.repo_id, path});
        check(valid == 2, "both summaries still valid after a redundant re-index");
        check(row_int(store,
                  "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
                  "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
                  {repo.repo_id, path}) == 2,
              "no duplicate rows accumulated for unchanged symbols");
        check(read_summary(store, repo.repo_id, path, "beta") == "returns two", "beta summary readable");

        // Idempotency of the write itself: the same text for the same bytes updates
        // in place instead of stacking rows.
        check(store_summary(store, repo.repo_id, path, "alpha", "returns one"),
              "re-storing identical text succeeds");
        check(summary_rows(store, repo.repo_id, path, "alpha") == 1,
              "re-storing the same summary for the same body stays one row (idempotent)");
    }

    // ---- Ambiguity: a name shared by two symbols is never resolved silently ----
    std::cout << "[ambiguous symbol names report candidates instead of picking one]\n";
    {
        std::string repo_root = (tmp / "ambiguous").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        // Both structs define handle(); the qualified names differ.
        const char* kSrc =
            "struct A { int handle() { return 1; } };\n"
            "struct B { int handle() { return 2; } };\n";
        std::string path = repo_root + "/multi.cpp";
        write_file(path, kSrc);
        check(indexer::index_file(store, path, repo.repo_id).indexed, "multi-scope fixture indexed");

        summary_store::SymbolSummaryInfo info;
        std::string text = summary_store::get_symbol_summary(store, repo.repo_id, path, "handle", "", info);
        check(text.empty() && info.status == summary_store::symbol_status::kAmbiguous,
              "a bare name matching two symbols is reported ambiguous, not guessed");
        check(info.candidates.size() == 2, "both candidates are listed");
        bool both_qualified = true;
        for (const auto& c : info.candidates)
            if (c.qualified_name.find("::handle") == std::string::npos) both_qualified = false;
        check(both_qualified, "candidates are spelled with their qualified names");

        summary_store::SymbolSummaryInfo winfo;
        check(!summary_store::store_symbol_summary(store, repo.repo_id, path, "handle", "", "x", "", "", winfo),
              "an ambiguous WRITE also refuses instead of attaching to one symbol");
        check(winfo.status == summary_store::symbol_status::kAmbiguous, "the write reports the ambiguity");

        // The qualified form resolves unambiguously (and different symbols get
        // different summaries even with the same bare name).
        check(store_summary(store, repo.repo_id, path, "A::handle", "A's handler"),
              "qualified A::handle is writable");
        check(store_summary(store, repo.repo_id, path, "B::handle", "B's handler"),
              "qualified B::handle is writable");
        check(read_summary(store, repo.repo_id, path, "A::handle") == "A's handler",
              "A::handle reads its own summary");
        check(read_summary(store, repo.repo_id, path, "B::handle") == "B's handler",
              "B::handle reads a different summary for the same bare name");

        // symbol_type can disambiguate a same-name collision in one scope:
        // two overloads of the same function name in the same scope.
        const char* kOver =
            "int over() { return 1; }\n"
            "double over() { return 2.0; }\n";
        std::string opath = repo_root + "/over.cpp";
        write_file(opath, kOver);
        indexer::index_file(store, opath, repo.repo_id);
        summary_store::SymbolSummaryInfo oinfo;
        summary_store::get_symbol_summary(store, repo.repo_id, opath, "over", "", oinfo);
        check(oinfo.status == summary_store::symbol_status::kAmbiguous &&
                  oinfo.candidates.size() == 2,
              "overloads of the same name are ambiguous without a filter");
    }

    // ---- Symbol deletion: its summary goes, a sibling's survives ----
    std::cout << "[deleted symbol loses its summary while a sibling keeps its own]\n";
    {
        std::string repo_root = (tmp / "symbol-gone").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        indexer::index_file(store, path, repo.repo_id);
        store_summary(store, repo.repo_id, path, "alpha", "returns one");
        store_summary(store, repo.repo_id, path, "beta", "returns two");

        // alpha is removed from the source entirely.
        const char* kOnlyBeta = "int beta() { return 2; }\n";
        write_file(path, kOnlyBeta);
        indexer::index_snapshot(store, path, repo.repo_id, kOnlyBeta);

        check(read_summary(store, repo.repo_id, path, "alpha") == summary_store::symbol_status::kNotFound,
              "a symbol no longer in the file is reported not_indexed (its stale row is not resurrected)");
        check(row_int(store,
                  "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
                  "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND ss.symbol_key = 'alpha' "
                  "AND ss.is_valid = 1",
                  {repo.repo_id, path}) == 0,
              "the removed symbol keeps no valid summary row");
        check(read_summary(store, repo.repo_id, path, "beta") == "returns two",
              "the surviving symbol keeps its valid summary");
    }

    // ---- Propagation (DEP-03 applies to symbol summaries too) ----
    std::cout << "[a header change invalidates its includer's symbol summaries]\n";
    {
        std::string repo_root = (tmp / "propagation").string();
        fs::create_directories(repo_root + "/src");
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string header = repo_root + "/src/base.h";
        std::string leaf = repo_root + "/src/leaf.cpp";
        write_file(header, "#pragma once\nint base_fn();\n");
        write_file(leaf, "#include \"base.h\"\nint leaf() { return base_fn(); }\n");
        indexer::index_file(store, header, repo.repo_id);
        indexer::index_file(store, leaf, repo.repo_id);
        check(store_summary(store, repo.repo_id, leaf, "leaf", "returns base_fn"),
              "includer's symbol summary stored");

        // Change the header through the ONE entry point: the dependent's derived
        // knowledge must go with it, including the per-symbol summary.
        write_file(header, "#pragma once\nint base_fn();\ninline int extra() { return 0; }\n");
        auto report = indexer::index_file(store, header, repo.repo_id);
        check(report.propagated && !report.dependents.empty(), "the change propagated to the includer");
        check(read_summary(store, repo.repo_id, leaf, "leaf") == summary_store::symbol_status::kStaleFile ||
              read_summary(store, repo.repo_id, leaf, "leaf") == summary_store::symbol_status::kNotFound,
              "the includer's symbol summary is no longer readable (its file is stale)");

        // Re-indexing the (unchanged) includer revives the summary whose symbol
        // body is actually unchanged — the stale flag, not the bytes, had blocked it.
        indexer::index_dependent_chain(store, leaf, repo.repo_id);
        check(!store.last_query_failed(), "chain reconciled");
        check(read_summary(store, repo.repo_id, leaf, "leaf") == "returns base_fn",
              "a dependent whose own symbol is unchanged regains its summary after re-indexing");

        // Explicit cache invalidation reaps them too (it is the shared reap).
        check(store_summary(store, repo.repo_id, header, "extra", "inline helper"),
              "header's own symbol summary stored");
        store_summary(store, repo.repo_id, leaf, "leaf", "returns base_fn v2");
        cache_manager::CacheManager cache(store);
        auto inv = cache.invalidate(header, repo.repo_id);
        check(inv.success && !inv.dependents.empty(), "invalidate() reports the dependents it reaped");
        check(row_int(store,
                  "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
                  "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND ss.is_valid = 1",
                  {repo.repo_id, leaf}) == 0,
              "the reaped dependent's symbol summaries were marked invalid by the shared reap");
        check(row_int(store,
                  "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
                  "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND ss.is_valid = 1",
                  {repo.repo_id, header}) == 0,
              "the invalidated file's own symbol summary was reaped too");
        std::string state = read_summary(store, repo.repo_id, leaf, "leaf");
        // The shared reap deletes the dependent's symbol rows (its re-index
        // rebuilds them), so the honest answer is 'this symbol is not in the index
        // any more': a stale summary cannot be attached to a symbol the index no
        // longer holds. The invalidated row itself is asserted above; the point is
        // that nothing readable survives the reap.
        check(state == summary_store::symbol_status::kNotFound,
              "a reaped dependent reports its symbol as not_indexed rather than serving a stale summary");
    }

    // ---- Tombstoned file: no symbol summaries readable (IDX-02) ----
    std::cout << "[a deleted file's symbol summaries are not readable (IDX-02)]\n";
    {
        std::string repo_root = (tmp / "deleted-file").string();
        fs::create_directories(repo_root + "/src");
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/src/gone.cpp";
        write_file(path, kTwoFn);
        check(indexer::index_repository(store, repo.repo_id).ok, "baseline full scan");
        store_summary(store, repo.repo_id, path, "alpha", "returns one");
        check(!read_summary(store, repo.repo_id, path, "alpha").empty(), "summary readable before deletion");

        fs::remove(path);
        auto scan = indexer::index_repository(store, repo.repo_id);
        check(scan.ok && scan.files_reconciled == 1, "the file is reconciled as deleted");
        check(read_summary(store, repo.repo_id, path, "alpha") != "returns one",
              "a tombstoned file's symbol summary is not served");
        check(summary_rows(store, repo.repo_id, path, "alpha") == 0 ||
              row_int(store, "SELECT COUNT(*) FROM symbol_summaries ss JOIN files f ON f.file_id = ss.file_id "
                             "WHERE f.repo_id = ? AND f.path = ? AND ss.is_valid = 1",
                      {repo.repo_id, path}) == 0,
              "reconciliation marked the file's symbol summaries invalid");
        auto inv = summary_store::list_symbol_summaries(store, repo.repo_id, path);
        check(inv.empty(), "a tombstoned file has no symbol inventory");
    }

    // ---- Legacy rows: a hash is never presumed (no body_hash stored yet) ----
    std::cout << "[symbol rows without a stored body_hash are decided by hash, never assumed]\n";
    {
        std::string repo_root = (tmp / "legacy-rows").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        indexer::index_file(store, path, repo.repo_id);
        store_summary(store, repo.repo_id, path, "alpha", "returns one");

        // Simulate a pre-SUM-01 index: strip the stored body hash. The file is
        // still byte-for-byte what the index claims, so the summary must still be
        // served — derived from the verified bytes, not assumed from a timestamp.
        must_execute(store,
            "UPDATE symbols SET body_hash = NULL WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            {repo.repo_id, path});
        check(read_summary(store, repo.repo_id, path, "alpha") == "returns one",
              "a summary survives a symbol row without a stored hash while the file hash still matches");

        // Now the file changes WITHOUT being re-indexed: nothing may be served,
        // because the row's hash can no longer be certified against disk.
        write_file(path, kTwoFnAlphaChanged);
        auto regions = symbol_index::list_file_symbol_regions(store, repo.repo_id, path);
        bool any_hash = false;
        for (const auto& r : regions) if (r.has_body_hash) any_hash = true;
        check(!any_hash, "the stripped rows really have no stored hashes");
        check(read_summary(store, repo.repo_id, path, "alpha") == summary_store::symbol_status::kStaleFile,
              "with the file hash no longer matching, an unhashed legacy row is refused (never guessed)");

        // The next re-index fills the column and the knowledge is answerable again.
        indexer::index_file(store, path, repo.repo_id);
        check(read_summary(store, repo.repo_id, path, "alpha") == summary_store::symbol_status::kStaleBody,
              "after re-indexing, the old summary is reported stale rather than unanswerable");
        check(store_summary(store, repo.repo_id, path, "alpha", "returns one hundred"),
              "re-indexed legacy symbol accepts a fresh summary");
        auto regions2 = symbol_index::list_file_symbol_regions(store, repo.repo_id, path);
        bool all_hashed = !regions2.empty();
        for (const auto& r : regions2) if (!r.has_body_hash) all_hashed = false;
        check(all_hashed, "the re-index filled in body_hash for every symbol row");
    }

    // ---- Inventory: pick a cached symbol instead of reading the whole file ----
    std::cout << "[per-file inventory reports each symbol's summary state]\n";
    {
        std::string repo_root = (tmp / "inventory").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        indexer::index_file(store, path, repo.repo_id);
        store_summary(store, repo.repo_id, path, "alpha", "returns one");

        auto rows = summary_store::list_symbol_summaries(store, repo.repo_id, path);
        check(rows.size() == 2, "inventory lists both live symbols");
        int valid = 0, none = 0;
        for (const auto& r : rows) {
            if (r.has_valid_summary) { ++valid; check(r.summary_text == "returns one", "valid row carries its text"); }
            else ++none;
        }
        check(valid == 1 && none == 1,
              "inventory distinguishes 'valid' from 'none' per symbol (never both the same way)");

        check(store_summary(store, repo.repo_id, path, "beta", "returns two"), "beta summarized");
        auto r2 = summary_store::list_symbol_summaries(store, repo.repo_id, path);
        int valid2 = 0;
        for (const auto& r : r2) if (r.has_valid_summary) ++valid2;
        check(valid2 == 2, "inventory follows the newly stored summary");
        for (const auto& r : r2) {
            if (r.has_valid_summary) {
                check(!r.created_at.empty(), "inventory reports provenance (created_at)");
                check(r.verification_state == "unverified", "fresh summaries start unverified");
            }
        }

        write_file(path, kTwoFnAlphaChanged);
        indexer::index_file(store, path, repo.repo_id);
        auto r3 = summary_store::list_symbol_summaries(store, repo.repo_id, path);
        int stale = 0, valid3 = 0;
        for (const auto& r : r3) {
            if (r.has_stale_summary) ++stale;
            if (r.has_valid_summary) ++valid3;
        }
        check(stale == 1 && valid3 == 1,
              "after a confined edit the inventory shows exactly one stale and one valid symbol");

        // Repository-wide inventory (used behind the coverage gate) only surfaces
        // summaries that still match a live symbol row.
        auto repo_rows = summary_store::list_repo_symbol_summaries(store, repo.repo_id);
        bool alpha_valid_row = false;
        for (const auto& r : repo_rows) {
            if (r.region.qualified_name == "alpha" && r.has_valid_summary) alpha_valid_row = true;
            if (r.region.qualified_name == "beta") check(!r.summary_text.empty(), "repo inventory serves beta's text");
        }
        check(!alpha_valid_row, "repo inventory does not present the changed alpha summary as valid");
        check(!repo_rows.empty(), "repo inventory is populated");
    }

    // ---- Transactional honesty: a failed write leaves the previous summary valid ----
    std::cout << "[a failing symbol summary write is not reported as stored (DB-01)]\n";
    {
        std::string repo_root = (tmp / "fault-symbol-summary").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "store opens");

        std::string path = repo_root + "/pair.cpp";
        write_file(path, kTwoFn);
        indexer::index_file(store, path, repo.repo_id);
        check(store_summary(store, repo.repo_id, path, "alpha", "first alpha summary"),
              "baseline alpha summary stored");

        // Fault hits the INSERT of the new row: the invalidation of the old one
        // and the insert of the new one must cancel together.
        store.set_sql_fault("INSERT INTO symbol_summaries", SQLITE_CONSTRAINT);
        bool stored = store_summary(store, repo.repo_id, path, "alpha", "second alpha summary");
        store.clear_sql_fault();
        check(!stored, "a failing symbol summary write reports false");
        check(read_summary(store, repo.repo_id, path, "alpha") == "first alpha summary",
              "the previous summary is intact: invalidation and insert cancelled together");
        check(store_summary(store, repo.repo_id, path, "alpha", "third alpha summary"),
              "the store works again once the fault is removed");
        check(read_summary(store, repo.repo_id, path, "alpha") == "third alpha summary",
              "recovery: the new summary replaced the old one");

        // A failing file re-index must leave the stored summaries untouched: the
        // fault targets a statement index_snapshot really issues (the symbols
        // INSERT), so its whole transaction — the dependent reaps, this file's
        // summary invalidation and its revival — cancels together.
        write_file(path, kTwoFnAlphaChanged);
        std::string hash_before = store.query(
            "SELECT content_hash FROM files WHERE repo_id = ? AND path = ?", repo.repo_id, path)[0][0];
        auto valid_before = row_int(store,
            "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND ss.is_valid = 1",
            {repo.repo_id, path});
        store.set_sql_fault("INSERT INTO symbols", SQLITE_CONSTRAINT);
        auto failed = indexer::index_snapshot(store, path, repo.repo_id, kTwoFnAlphaChanged);
        store.clear_sql_fault();
        auto valid_after = row_int(store,
            "SELECT COUNT(*) FROM symbol_summaries ss WHERE ss.file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?) AND ss.is_valid = 1",
            {repo.repo_id, path});
        std::string hash_after = store.query(
            "SELECT content_hash FROM files WHERE repo_id = ? AND path = ?", repo.repo_id, path)[0][0];
        check(!failed.indexed, "the re-index that failed reports no success");
        check(hash_after == hash_before,
              "the file record still describes the pre-edit snapshot: nothing was half-applied");
        check(valid_before == valid_after,
              "a rolled-back re-index changes nothing about the stored summaries");
        check(read_summary(store, repo.repo_id, path, "alpha") == summary_store::symbol_status::kStaleFile,
              "the honest state after a rolled-back re-index is file-level staleness, not an updated summary");
        check(indexer::index_snapshot(store, path, repo.repo_id, kTwoFnAlphaChanged).indexed,
              "the same re-index succeeds once the fault is removed");
        check(read_summary(store, repo.repo_id, path, "alpha") == summary_store::symbol_status::kStaleBody,
              "and now the summary that really describes an old body reads as stale");
    }

    // ---- Migration: a pre-SUM-01 database opens and keeps working ----
    std::cout << "[a database written before SUM-01 migrates and keeps working]\n";
    {
        std::string db = (tmp / "legacy.db").string();
        fs::remove(db);
        std::string repo_root = (tmp / "legacy-root").string();
        fs::remove_all(repo_root);
        const std::string legacy_cpp = repo_root + "/legacy/root/old.cpp";

        // Build a legacy-shaped database: open once so the base schema exists,
        // then replace `symbols` with its PRE-SUM-01 shape (no identity columns)
        // and drop symbol_summaries. Reopening runs init_schema against that file,
        // which is exactly the path an existing ~/.ai-tools database takes.
        {
            knowledge_store::KnowledgeStore legacy;
            check(legacy.open(db), "store opens to lay down the base schema");
            must_execute(legacy, "DROP TABLE IF EXISTS symbol_summaries");
            must_execute(legacy, "DROP TABLE IF EXISTS symbols");
            must_execute(legacy,
                "CREATE TABLE symbols (symbol_id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "file_id INTEGER NOT NULL, name TEXT NOT NULL, symbol_type TEXT NOT NULL, "
                "signature TEXT, start_line INTEGER NOT NULL, end_line INTEGER NOT NULL, "
                "scope TEXT, is_declaration INTEGER DEFAULT 0, is_definition INTEGER DEFAULT 0, "
                "is_exported INTEGER DEFAULT 0, content_hash_at_index TEXT)");

            bool absent_qualified = false, absent_body = false;
            for (const auto& c : legacy.query("PRAGMA table_info(symbols)")) {
                if (c.size() >= 2 && c[1] == "qualified_name") absent_qualified = true;
                if (c.size() >= 2 && c[1] == "body_hash") absent_body = true;
            }
            check(!absent_qualified && !absent_body, "fixture reproduces a pre-SUM-01 symbols table");

            must_execute(legacy,
                "INSERT OR REPLACE INTO repositories (repo_id, root_path, name, created_at, updated_at) "
                "VALUES ('legacy-repo', '/legacy/root', 'legacy', datetime('now'), datetime('now'))");
            // The legacy record must describe a file that really exists, with the
            // hash the legacy index would have written for it: that is what lets
            // the migration test exercise the read rules against real bytes instead
            // of a dangling path.
            fs::create_directories(fs::path(legacy_cpp).parent_path());
            const std::string legacy_content =
                "int legacy_fn() { return 1; }\nint other() { return 2; }\n";
            write_file(legacy_cpp, legacy_content);
            must_execute(legacy,
                "INSERT OR REPLACE INTO files (repo_id, path, content_hash, language, file_type, line_count) "
                "VALUES ('legacy-repo', ?, ?, 'cpp', 'source', 2)",
                {legacy_cpp, content_hasher::compute_string_hash(legacy_content)});
            long long file_id = row_int(legacy, "SELECT file_id FROM files WHERE path = ?", {legacy_cpp});
            must_execute(legacy,
                "INSERT INTO symbols (file_id, name, symbol_type, start_line, end_line, scope) "
                "VALUES (?, 'legacy_fn', 'function', 1, 1, '')",
                {std::to_string(file_id)});
            must_execute(legacy,
                "INSERT OR REPLACE INTO files (repo_id, path, content_hash, language, file_type, line_count) "
                "VALUES ('legacy-repo', '/legacy/root/p.py', 'hash-p', 'python', 'source', 4)");
            long long pfile = row_int(legacy, "SELECT file_id FROM files WHERE path = '/legacy/root/p.py'");
            must_execute(legacy,
                "INSERT INTO symbols (file_id, name, symbol_type, start_line, end_line, scope) "
                "VALUES (?, 'save', 'function', 3, 4, 'Config')",
                {std::to_string(pfile)});
            check(row_int(legacy, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='symbol_summaries'") == 0,
                  "the legacy database has no symbol_summaries table at all");
        }

        // Reopen: init_schema must migrate the legacy shape in place.
        knowledge_store::KnowledgeStore store;
        check(store.open(db), "migrated store opens");
        check(store.get_error().empty(), "opening the migrated store reports no error");

        bool has_qualified = false, has_body = false;
        for (const auto& c : store.query("PRAGMA table_info(symbols)")) {
            if (c.size() >= 2 && c[1] == "qualified_name") has_qualified = true;
            if (c.size() >= 2 && c[1] == "body_hash") has_body = true;
        }
        check(has_qualified && has_body,
              "migration adds the identity columns to a pre-SUM-01 symbols table");
        check(row_int(store, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='symbol_summaries'") == 1,
              "symbol_summaries is created when migrating a legacy database");

        auto regions = symbol_index::list_file_symbol_regions(store, "legacy-repo", legacy_cpp);
        check(regions.size() == 1 && regions[0].qualified_name == "legacy_fn",
              "the backfill spells a top-level legacy symbol's identity from its name");
        check(!regions[0].has_body_hash,
              "a legacy row keeps no body hash: nothing is backfilled from disk");

        // The separator used by the backfill must match what the language's own
        // parser writes, or a migrated identity would not match the identity the
        // next re-index produces.
        auto python_regions = symbol_index::list_file_symbol_regions(store, "legacy-repo", "/legacy/root/p.py");
        check(python_regions.size() == 1 && python_regions[0].qualified_name == "Config.save",
              "migration spells a Python scope with '.' exactly like the Python parser does");

        // The legacy inventory lists the symbol with no summary verdict rather
        // than guessing one, and file-level summaries still work unchanged.
        auto inv = summary_store::list_symbol_summaries(store, "legacy-repo", legacy_cpp);
        check(inv.size() == 1 && !inv[0].has_valid_summary && !inv[0].has_stale_summary,
              "the legacy inventory lists the symbol with no summary verdict rather than a guess");
        // A legacy symbol IS answerable while the file hash still certifies it:
        // its body hash is completed from the verified bytes on disk.
        check(store_summary(store, "legacy-repo", legacy_cpp, "legacy_fn", "legacy helper"),
              "a migrated symbol row accepts a summary (its hash completed from verified bytes)");
        check(read_summary(store, "legacy-repo", legacy_cpp, "legacy_fn") == "legacy helper",
              "the migrated symbol summary reads back valid");
        write_file(legacy_cpp, "int legacy_fn() { return 99; }\nint other() { return 2; }\n");
        check(read_summary(store, "legacy-repo", legacy_cpp, "legacy_fn") ==
                  summary_store::symbol_status::kStaleFile,
              "once the file bytes change without a re-index, the unhashed legacy row is refused");
        check(summary_store::store_file_summary(store, "legacy-repo", legacy_cpp, "legacy file summary"),
              "file-level summary writes still work on a migrated database");
        check(summary_store::get_file_summary(store, "legacy-repo", legacy_cpp).empty(),
              "a file summary is still gated by the content hash: a changed file serves nothing");
    }

    fs::remove_all(tmp);
    return failures;
}
