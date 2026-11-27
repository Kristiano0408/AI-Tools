// Tests for knowledge_store, content_hasher, cache_manager, repository identity,
// and incremental indexing behaviour.

#include "engine/knowledge_store.h"
#include "engine/content_hasher.h"
#include "engine/cache_manager.h"
#include "engine/repository.h"
#include "engine/indexer.h"
#include "engine/dependency_graph.h"
#include "engine/summary_store.h"
#include "engine/symbol_index.h"

#include <algorithm>
#include <chrono>
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

// True when any entry of a dependency-graph result mentions a substring.
static bool mentions(const std::vector<std::string>& entries, const std::string& needle) {
    for (const auto& e : entries)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

int run_store_tests() {
    failures = 0;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "ai-tools-store-tests";
    // ---- IDX-04: coverage freshness is based on verified content ----
    std::cout << "[coverage freshness (IDX-04)]\n";
    {
        std::string repo_root = (tmp / "coverage-freshness").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "IDX-04 store opens");

        const std::string path = repo_root + "/stable.cpp";
        write_file(path, "int stable_symbol() { return 7; }\n");
        auto initial = indexer::index_repository(store, repo.repo_id);
        check(initial.ok && initial.files_refreshed == 1,
              "initial full scan indexes the source");

        // Simulate the runtime report: the file's mtime is newer than its
        // indexing stamp, but the bytes and stored hash are unchanged.
        std::error_code ec;
        auto old_write = fs::last_write_time(path, ec);
        fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(10), ec);
        auto touched_but_same = indexer::repo_coverage(store, repo.repo_id, -1);
        check(!ec && touched_but_same.coverage == indexer::IndexCoverage::full_fresh,
              "mtime-only touch with identical hash remains full_fresh");

        auto verified = indexer::index_repository(store, repo.repo_id);
        auto after_verify = indexer::repo_coverage(store, repo.repo_id, -1);
        check(verified.ok && verified.files_verified == 1 && verified.files_refreshed == 0,
              "full scan verifies and stamps an unchanged record");
        check(after_verify.coverage == indexer::IndexCoverage::full_fresh,
              "one full scan restores full_fresh without rewriting file content");

        // An old or missing verification timestamp cannot mask a true content
        // change: the hash tie-breaker must fail closed.
        store.execute("UPDATE files SET content_verified_at = '2000-01-01 00:00:00', "
                      "last_indexed_at = '2000-01-01 00:00:00' WHERE repo_id = ? AND path = ?",
                      repo.repo_id, path);
        write_file(path, "int changed_symbol() { return 8; }\n");
        fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(10), ec);
        auto changed = indexer::repo_coverage(store, repo.repo_id, -1);
        check(changed.coverage == indexer::IndexCoverage::full_stale &&
                  changed.touched_records == 1 && !changed.touched_sample.empty(),
              "different content is reported as touched and blocks full coverage");

        auto repaired = indexer::index_repository(store, repo.repo_id);
        auto repaired_cov = indexer::repo_coverage(store, repo.repo_id, -1);
        check(repaired.ok && repaired.files_refreshed == 1 &&
                  repaired_cov.coverage == indexer::IndexCoverage::full_fresh,
              "scan reindexes changed content and restores full_fresh");

        store.execute("UPDATE files SET content_verified_at = NULL WHERE repo_id = ? AND path = ?",
                      repo.repo_id, path);
        auto missing_stamp = indexer::repo_coverage(store, repo.repo_id, -1);
        check(missing_stamp.coverage == indexer::IndexCoverage::full_fresh,
              "missing verification stamp falls back to matching content hash");

        fs::last_write_time(path, old_write, ec);
    }

    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Isolate HOME + AI_TOOLS_DATA_DIR so repository tests don't touch the
    // real data directory (even when the binary has a compiled-in default)
    setenv("HOME", tmp.c_str(), 1);
    setenv("AI_TOOLS_DATA_DIR", (tmp / ".ai-tools").c_str(), 1);

    // ---- content_hasher ----
    std::cout << "[content_hasher]\n";
    std::string f1 = (tmp / "a.txt").string();
    write_file(f1, "hello world");
    std::string h1 = content_hasher::compute_file_hash(f1);
    check(h1.size() == 64, "hash is 64 hex chars");
    check(h1 == content_hasher::compute_string_hash("hello world"), "file hash matches string hash");

    // Large file (> 4096 buffer boundary) hashes fully
    std::string big_content(10000, 'x');
    std::string f2 = (tmp / "big.txt").string();
    write_file(f2, big_content);
    check(content_hasher::compute_file_hash(f2) == content_hasher::compute_string_hash(big_content),
          "large file hash covers final partial block");

    check(content_hasher::compute_file_hash((tmp / "missing.txt").string()).empty(), "missing file -> empty hash");

    // ---- knowledge_store ----
    std::cout << "[knowledge_store]\n";
    {
        knowledge_store::KnowledgeStore store;
        check(store.open((tmp / "store.db").string()), "store opens");
        check(store.is_open(), "is_open true");

        store.execute("CREATE TABLE t (a TEXT, b INTEGER)");
        store.execute("INSERT INTO t VALUES (?, ?)", "one", 1);
        store.execute("INSERT INTO t VALUES (?, ?)", "two", 2);
        auto rows = store.query("SELECT a, b FROM t ORDER BY b");
        check(rows.size() == 2, "insert + query works");
        check(rows[0][0] == "one" && rows[0][1] == "1", "params bound correctly");

        auto bad = store.query("SELECT * FROM nonexistent_table");
        check(bad.empty(), "bad query returns empty (no crash)");
        check(!store.get_error().empty(), "error message recorded");

        // variadic mixed int/string binding
        auto r2 = store.query("SELECT a FROM t WHERE b = ?", 2);
        check(r2.size() == 1 && r2[0][0] == "two", "integral param binding works");
    }

    // ---- repository identity ----
    std::cout << "[repository]\n";
    {
        std::string repo_root = (tmp / "myrepo").string();
        fs::create_directories(repo_root);

        repository::RepoInfo info = repository::get_or_create_repository(repo_root);
        check(!info.repo_id.empty(), "repo identity created");
        check(info.name == "myrepo", "repo name derived from path");
        check(fs::exists(info.db_path), "per-repo db file created");
        check(info.repo_id == repository::repo_identity(repo_root), "identity is stable");
        check(repository::repository_exists(info.repo_id), "repository_exists true");
        check(!repository::repository_exists("nonexistent-id"), "unknown repo not found");

        // Second call returns the same identity, preserving created_at
        repository::RepoInfo again = repository::get_or_create_repository(repo_root);
        check(again.repo_id == info.repo_id, "identity stable across calls");
        check(again.created_at == info.created_at, "created_at preserved on re-open");

        // Different roots get different identities (repository isolation)
        std::string other_root = (tmp / "otherrepo").string();
        fs::create_directories(other_root);
        repository::RepoInfo other = repository::get_or_create_repository(other_root);
        check(other.repo_id != info.repo_id, "different repos have different identities");
        check(other.db_path != info.db_path, "different repos have separate databases");
    }

    // ---- cache_manager + incremental indexing ----
    std::cout << "[cache_manager / incremental indexing]\n";
    {
        std::string repo_root = (tmp / "cacherepo").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "repo store opens");

        std::string header = repo_root + "/util.h";
        std::string source = repo_root + "/util.cpp";
        write_file(header, "#pragma once\nint helper();\n");
        write_file(source, "#include \"util.h\"\nint helper() { return 42; }\n");

        indexer::index_file(store, header, repo.repo_id);
        indexer::index_file(store, source, repo.repo_id);

        check(!indexer::needs_indexing(store, header, repo.repo_id), "header fresh after indexing");
        check(!indexer::needs_indexing(store, source, repo.repo_id), "source fresh after indexing");

        // Transitive dependency recorded: util.cpp -> util.h
        auto deps = dependency_graph::get_dependencies(store, repo.repo_id, source);
        bool found = false;
        for (const auto& d : deps) if (d.find("util.h") != std::string::npos) found = true;
        check(found, "include edge stored with resolved target");

        auto dependents = dependency_graph::get_dependents(store, repo.repo_id, header);
        check(!dependents.empty(), "reverse lookup (dependents) works");

        // Summary validity follows the content hash
        summary_store::store_file_summary(store, repo.repo_id, header, "utility header");
        check(summary_store::get_file_summary(store, repo.repo_id, header) == "utility header", "summary valid");

        // Modify the header -> cache invalidation + transitive staleness
        write_file(header, "#pragma once\nint helper();\nint extra();\n");
        cache_manager::CacheManager cache(store);
        cache.invalidate_transitive(header); // must run before edges are deleted
        cache.invalidate(header);

        auto stale = cache.get_stale_files();
        bool header_stale = false, source_stale = false;
        for (const auto& s : stale) {
            if (s == header) header_stale = true;
            if (s == source) source_stale = true;
        }
        check(header_stale, "changed header detected stale");
        check(source_stale, "dependent source detected stale via hash comparison");

        // Summary invalidated when file hash changes (even without explicit invalidate)
        summary_store::store_file_summary(store, repo.repo_id, source, "helper implementation");
        check(summary_store::get_file_summary(store, repo.repo_id, source) == "helper implementation", "source summary valid");
        write_file(source, "#include \"util.h\"\nint helper() { return 43; }\n");
        check(summary_store::get_file_summary(store, repo.repo_id, source).empty(),
              "summary invalid after source change");

        // Re-indexing refreshes everything
        indexer::index_file(store, header, repo.repo_id);
        indexer::index_file(store, source, repo.repo_id);
        check(!indexer::needs_indexing(store, header, repo.repo_id), "header fresh after re-index");
        check(!indexer::needs_indexing(store, source, repo.repo_id), "source fresh after re-index");

        // Symbol-level call edges persisted and queryable
        write_file(repo_root + "/app.cpp",
                   "#include \"util.h\"\nint run() { return helper(); }\n");
        indexer::index_file(store, repo_root + "/app.cpp", repo.repo_id);
        auto callees = dependency_graph::get_callees(store, repo.repo_id, "run");
        bool callee_found = false;
        for (const auto& c : callees) if (c.find("helper") != std::string::npos) callee_found = true;
        check(callee_found, "symbol callee edge persisted (run -> helper)");
        auto callers = dependency_graph::get_callers(store, repo.repo_id, "helper");
        bool caller_found = false;
        for (const auto& c : callers) if (c.find("run") != std::string::npos) caller_found = true;
        check(caller_found, "symbol caller edge queryable (helper <- run)");

        // index_repository walks and skips build dirs
        fs::create_directories(repo_root + "/build");
        write_file(repo_root + "/build/generated.cpp", "int generated() { return 0; }\n");
        indexer::index_repository(store, repo.repo_id);
        auto gen = store.query("SELECT 1 FROM files WHERE path LIKE ?", std::string("%/build/generated.cpp"));
        check(gen.empty(), "index_repository skips build/ directory");

        auto state = store.query("SELECT status, files_indexed, full_index_covered FROM indexing_state WHERE repo_id = ?", repo.repo_id);
        check(!state.empty() && state[0][0] == "full", "full indexing state recorded");
        check(!state.empty() && !state[0][2].empty(), "full scan records a coverage snapshot");

        // Incremental re-index: unchanged files are skipped, only changed ones reprocessed
        write_file(source, "#include \"util.h\"\nint helper() { return 44; }\n");
        indexer::index_repository(store, repo.repo_id);
        // files_indexed is the certified coverage (live file records), not the
        // number of reprocessed files. Re-running a full scan must certify a
        // coverage consistent with the live records and with the repository:
        // build/generated.cpp stays excluded, the 3 real sources are covered.
        auto state2 = store.query("SELECT files_indexed, full_index_covered FROM indexing_state WHERE repo_id = ?", repo.repo_id);
        check(!state2.empty() && state2[0][0] == "3" && state2[0][1] == "3",
              "full scan certifies a coverage consistent with live records");
        auto cov = indexer::repo_coverage(store, repo.repo_id, -1);
        check(cov.coverage == indexer::IndexCoverage::full_fresh &&
              cov.files_total == 3 && cov.files_indexed == 3,
              "repo reports full-fresh coverage after a completed scan");
        auto gen_state = store.query("SELECT files_total FROM indexing_state WHERE repo_id = ?", repo.repo_id);
        check(!gen_state.empty() && gen_state[0][0] == "3", "certified coverage excludes build/ files");
        check(!indexer::needs_indexing(store, source, repo.repo_id), "changed file re-indexed");
        check(!indexer::needs_indexing(store, header, repo.repo_id), "unchanged header untouched");
    }

    // ---- DEP-01/DEP-02: transitive invalidation is collected before the
    // edges are deleted and propagates through arbitrarily long chains ----
    std::cout << "[transitive invalidation (DEP-01/DEP-02)]\n";
    {
        std::string repo_root = (tmp / "chainrepo").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "chain repo store opens");

        // base.h <- middle.h <- leaf.cpp: a change to the deepest header must
        // reach the far end of the chain, not just its direct includer.
        write_file(repo_root + "/base.h", "#pragma once\nint base_fn();\n");
        write_file(repo_root + "/middle.h", "#include \"base.h\"\nint middle_fn();\n");
        write_file(repo_root + "/leaf.cpp", "#include \"middle.h\"\nint leaf() { return base_fn(); }\n");

        indexer::index_file(store, repo_root + "/base.h", repo.repo_id);
        indexer::index_file(store, repo_root + "/middle.h", repo.repo_id);
        indexer::index_file(store, repo_root + "/leaf.cpp", repo.repo_id);
        check(!indexer::needs_indexing(store, repo_root + "/leaf.cpp", repo.repo_id),
              "chain fully indexed before the edit");

        write_file(repo_root + "/base.h", "#pragma once\nint base_fn();\nint added_in_base();\n");
        cache_manager::CacheManager cache(store);
        auto res = cache.invalidate(repo_root + "/base.h", repo.repo_id);

        bool has_middle = false, has_leaf = false;
        for (const auto& d : res.dependents) {
            if (d.find("middle.h") != std::string::npos) has_middle = true;
            if (d.find("leaf.cpp") != std::string::npos) has_leaf = true;
        }
        check(has_middle, "direct dependent collected (middle.h)");
        check(has_leaf, "transitive dependent collected through the chain (leaf.cpp, DEP-02)");

        // The collection happens before any edge is deleted: the returned set
        // is exactly what invalidate() then marks stale. A dependent must be
        // recognizable as needing a re-index without ever being touched.
        check(indexer::needs_indexing(store, repo_root + "/middle.h", repo.repo_id),
              "middle.h marked stale by the invalidation (no edge deleted first)");
        check(indexer::needs_indexing(store, repo_root + "/leaf.cpp", repo.repo_id),
              "leaf.cpp marked stale transitively (DEP-01: collect-before-delete)");

        auto leaf_syms = store.query(
            "SELECT COUNT(*) FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo.repo_id, repo_root + "/leaf.cpp");
        check(!leaf_syms.empty() && leaf_syms[0][0] == "0",
              "transitive dependent's cached symbols dropped");
        auto mid_syms = store.query(
            "SELECT COUNT(*) FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo.repo_id, repo_root + "/middle.h");
        check(!mid_syms.empty() && mid_syms[0][0] == "0",
              "direct dependent's cached symbols dropped");

        // has_dependents: reverse reachability used by single-file self-heal
        check(indexer::has_dependents(store, repo.repo_id, repo_root + "/base.h"),
              "has_dependents true for a transitively included header");
        check(!indexer::has_dependents(store, repo.repo_id, repo_root + "/leaf.cpp"),
              "has_dependents false for a leaf file nothing includes");

        // index_dependent_chain: the high-level entry point re-indexes from
        // one fresh read and propagates invalidation to every dependent.
        auto chain = indexer::index_dependent_chain(store, repo_root + "/base.h", repo.repo_id);
        check(chain.size() >= 2, "index_dependent_chain reports the transitive dependents");
        check(!indexer::needs_indexing(store, repo_root + "/base.h", repo.repo_id),
              "chain entry point re-indexed base.h fresh");
        check(indexer::needs_indexing(store, repo_root + "/middle.h", repo.repo_id) &&
                  indexer::needs_indexing(store, repo_root + "/leaf.cpp", repo.repo_id),
              "dependents stay stale until a scan re-indexes them");

        // A full scan reconciles the whole chain back to fresh.
        indexer::index_repository(store, repo.repo_id);
        check(!indexer::needs_indexing(store, repo_root + "/middle.h", repo.repo_id) &&
                  !indexer::needs_indexing(store, repo_root + "/leaf.cpp", repo.repo_id),
              "full scan re-indexes the invalidated chain");
        auto leaf_syms2 = store.query(
            "SELECT COUNT(*) FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo.repo_id, repo_root + "/leaf.cpp");
        check(!leaf_syms2.empty() && leaf_syms2[0][0] != "0",
              "re-indexed dependent recovers its symbols from the fresh content");
        check(indexer::has_dependents(store, repo.repo_id, repo_root + "/base.h"),
              "include edges rebuilt after the rescan");
    }

    // ---- DEP-03: a change detected outside the server propagates through the
    // single re-indexing entry point, with no separate invalidation call ----
    std::cout << "[external change propagates through index_file (DEP-03)]\n";
    {
        std::string repo_root = (tmp / "externalrepo").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "external-change repo store opens");

        // base.h <- middle.h <- leaf.cpp, indexed file by file (no CacheManager,
        // no full scan): the propagation must come from index_file itself.
        write_file(repo_root + "/base.h", "#pragma once\nint base_fn();\n");
        write_file(repo_root + "/middle.h", "#include \"base.h\"\nint middle_fn();\n");
        write_file(repo_root + "/leaf.cpp", "#include \"middle.h\"\nint leaf() { return base_fn(); }\n");
        indexer::index_file(store, repo_root + "/base.h", repo.repo_id);
        indexer::index_file(store, repo_root + "/middle.h", repo.repo_id);
        indexer::index_file(store, repo_root + "/leaf.cpp", repo.repo_id);

        auto count_stale = [&]() -> int {
            auto rows = store.query(
                "SELECT COUNT(*) FROM files WHERE repo_id = ? AND is_deleted = 0 AND stale = 1", repo.repo_id);
            return rows.empty() || rows[0].empty() ? -1 : std::stoi(rows[0][0]);
        };
        auto count_symbols = [&](const std::string& path) -> int {
            auto rows = store.query(
                "SELECT COUNT(*) FROM symbols WHERE file_id IN "
                "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)", repo.repo_id, path);
            return rows.empty() || rows[0].empty() ? -1 : std::stoi(rows[0][0]);
        };

        check(count_stale() == 0, "nothing is stale right after a clean per-file index");

        // The file changes on disk only (an editor, Git, another process). The
        // only thing the server does is re-index that one file.
        write_file(repo_root + "/base.h", "#pragma once\nint base_fn();\nint added_by_external_edit();\n");
        auto report = indexer::index_file(store, repo_root + "/base.h", repo.repo_id);

        check(report.indexed && report.changed, "index_file reports it superseded the stored content");
        check(report.dependents.size() >= 2, "index_file invalidated the transitive dependents");
        bool has_middle = false, has_leaf = false;
        for (const auto& d : report.dependents) {
            if (d.find("middle.h") != std::string::npos) has_middle = true;
            if (d.find("leaf.cpp") != std::string::npos) has_leaf = true;
        }
        check(has_middle && has_leaf, "the report names both the direct and the transitive dependent");
        check(!indexer::needs_indexing(store, repo_root + "/base.h", repo.repo_id),
              "the re-indexed file is fresh again");
        check(indexer::needs_indexing(store, repo_root + "/middle.h", repo.repo_id) &&
                  indexer::needs_indexing(store, repo_root + "/leaf.cpp", repo.repo_id),
              "dependents are marked stale by the single-file update");
        check(count_symbols(repo_root + "/leaf.cpp") == 0 &&
                  count_symbols(repo_root + "/middle.h") == 0,
              "dependents' cached symbols were dropped (no knowledge from the superseded header)");
        check(indexer::has_dependents(store, repo.repo_id, repo_root + "/base.h"),
              "the propagation keeps the include edges, so the chain is still traversable");
        check(count_stale() >= 2, "stale records are visible in coverage bookkeeping");

        // Idempotency: re-indexing the SAME bytes a second time supersedes
        // nothing and must not re-invalidate the (already stale) dependents,
        // and must not mark the file itself stale.
        auto report2 = indexer::index_file(store, repo_root + "/base.h", repo.repo_id);
        check(report2.indexed && !report2.changed, "re-indexing identical content is not a change");
        check(report2.dependents.empty(), "an unchanged snapshot propagates nothing");
        check(!indexer::needs_indexing(store, repo_root + "/base.h", repo.repo_id),
              "an unchanged re-index leaves the file fresh, never self-marked stale");

        // A full scan settles the whole chain back to fresh (the dependents the
        // single-file update marked stale are re-parsed against current disk).
        auto scan = indexer::index_repository(store, repo.repo_id);
        check(scan.ok, "scan after the external change succeeds");
        check(count_stale() == 0, "no stale record survives a completed scan");
        check(count_symbols(repo_root + "/leaf.cpp") > 0,
              "the dependent recovered its symbols from the fresh content");
        auto cov = indexer::repo_coverage(store, repo.repo_id, -1);
        check(cov.coverage == indexer::IndexCoverage::full_fresh && cov.stale_records == 0,
              "coverage is full_fresh after the scan settled the chain");
    }

    // ---- DEP-03: a mutual-include cycle must not make propagation spin ----
    std::cout << "[external change through a mutual include cycle (DEP-03)]\n";
    {
        std::string repo_root = (tmp / "cycleexternal").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "cycle-external repo store opens");

        write_file(repo_root + "/a.h", "#include \"b.h\"\nint a_fn();\n");
        write_file(repo_root + "/b.h", "#include \"a.h\"\nint b_fn();\n");
        indexer::index_file(store, repo_root + "/a.h", repo.repo_id);
        indexer::index_file(store, repo_root + "/b.h", repo.repo_id);

        // a.h includes b.h and b.h includes a.h: collecting a.h's dependents
        // finds b.h, whose dependents include a.h again. The visited set must
        // terminate the walk instead of looping.
        write_file(repo_root + "/a.h", "#include \"b.h\"\nint a_fn();\nint a_added();\n");
        auto report = indexer::index_file(store, repo_root + "/a.h", repo.repo_id);
        check(report.indexed, "indexing a cyclic-inclusion file terminates and writes");
        check(!report.dependents.empty(), "the cycle's dependent was collected once");
        bool only_b = true;
        for (const auto& d : report.dependents)
            if (d.find("b.h") == std::string::npos) only_b = false;
        check(only_b && report.dependents.size() == 1,
              "the cycle reports b.h exactly once (visited guard against re-entering a.h)");
    }

    // ---- DEP-03: a full scan settles a header change whose includer was
    // visited before the header (stabilization pass) ----
    std::cout << "[full scan stabilizes dependents visited out of order (DEP-03)]\n";
    {
        std::string repo_root = (tmp / "stabilizerepo").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "stabilize repo store opens");

        // leaf.cpp is walked before z_header.h (directory order), so the scan
        // would re-parse leaf.cpp from its own bytes and only LATER invalidate
        // it via the header it includes. The stabilization pass must settle it.
        write_file(repo_root + "/leaf.cpp", "#include \"z_header.h\"\nint leaf() { return zed(); }\n");
        write_file(repo_root + "/z_header.h", "#pragma once\nint zed();\n");
        indexer::index_repository(store, repo.repo_id);
        check(indexer::repo_coverage(store, repo.repo_id, -1).coverage == indexer::IndexCoverage::full_fresh,
              "baseline scan is full_fresh");

        // The header changes on disk. Nothing else is touched beforehand.
        write_file(repo_root + "/z_header.h", "#pragma once\nint zed();\nint zed_again();\n");
        auto scan = indexer::index_repository(store, repo.repo_id);
        check(scan.ok, "scan after the header change succeeds");
        check(scan.dependents_invalidated >= 1, "scan reports it invalidated the includer");
        // The includer is settled by either the main loop (if the walk visited
        // it after the header) or the stabilization pass (if it visited it
        // before). Filesystem walk order is not specified, so assert the
        // observable guarantee instead of which path settled it: every file in
        // the changed chain was re-parsed and nothing is left stale.
        check(scan.files_refreshed + scan.dependents_refreshed >= 2,
              "the changed header and its includer were both re-parsed");
        auto stale = store.query(
            "SELECT COUNT(*) FROM files WHERE repo_id = ? AND is_deleted = 0 AND stale = 1", repo.repo_id);
        check(!stale.empty() && stale[0][0] == "0", "no stale record is left after the scan");
        auto leaf_syms = store.query(
            "SELECT COUNT(*) FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo.repo_id, repo_root + "/leaf.cpp");
        check(!leaf_syms.empty() && leaf_syms[0][0] != "0", "the includer keeps its symbols after settling");
        check(indexer::repo_coverage(store, repo.repo_id, -1).coverage == indexer::IndexCoverage::full_fresh,
              "coverage is full_fresh: the scan did not certify a chain it left unsettled");
    }

    // ---- DEP-03 precision: a single-file change never invalidates a caller
    // (only include/import edges propagate) ----
    std::cout << "[single-file change ignores call edges (DEP-03)]\n";
    {
        std::string repo_root = (tmp / "callexternal").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "call-external repo store opens");

        write_file(repo_root + "/def.cpp", "int helper() { return 1; }\n");
        write_file(repo_root + "/user.cpp", "int helper();\nint run() { return helper(); }\n");
        indexer::index_file(store, repo_root + "/def.cpp", repo.repo_id);
        indexer::index_file(store, repo_root + "/user.cpp", repo.repo_id);

        write_file(repo_root + "/def.cpp", "int helper() { return 2; }\n");
        auto report = indexer::index_file(store, repo_root + "/def.cpp", repo.repo_id);
        bool user_marked = false;
        for (const auto& d : report.dependents)
            if (d.find("user.cpp") != std::string::npos) user_marked = true;
        check(!user_marked,
              "a caller is not invalidated just because it calls the re-indexed file (dep_type != 'call')");
        check(!indexer::needs_indexing(store, repo_root + "/user.cpp", repo.repo_id),
              "the caller stays fresh: only include/import dependents are invalidated");
    }

    // ---- DEP-02 (python flavour): module imports propagate invalidation --
    std::cout << "[transitive invalidation via python imports]\n";
    {
        std::string repo_root = (tmp / "pyrepo").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "python repo store opens");

        write_file(repo_root + "/utils.py", "def helper():\n    return 1\n");
        write_file(repo_root + "/service.py", "from utils import helper\n\ndef run():\n    return helper()\n");

        indexer::index_file(store, repo_root + "/utils.py", repo.repo_id);
        indexer::index_file(store, repo_root + "/service.py", repo.repo_id);

        auto deps = dependency_graph::get_dependencies(store, repo.repo_id, repo_root + "/service.py");
        check(mentions(deps, "utils.py"), "python import edge resolved to a real file");

        write_file(repo_root + "/utils.py", "def helper():\n    return 2\n\ndef extra():\n    return 3\n");
        cache_manager::CacheManager cache(store);
        auto res = cache.invalidate(repo_root + "/utils.py", repo.repo_id);
        bool importer_marked = false;
        for (const auto& d : res.dependents)
            if (d.find("service.py") != std::string::npos) importer_marked = true;
        check(importer_marked, "importer invalidated transitively (dep_type filter covers 'import')");
        check(indexer::needs_indexing(store, repo_root + "/service.py", repo.repo_id),
              "importing module marked stale when an imported module changes");
    }

    // ---- DEP-02 safety cap: a pathological include cycle is bounded and
    // reported instead of spinning or silently marking thousands stale ----
    std::cout << "[transitive invalidation safety cap]\n";
    {
        std::string repo_root = (tmp / "cycrepo").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "cycle repo store opens");

        const int kDependents = 1001;
        const std::string root = repo_root + "/root.hpp";
        for (int i = 0; i < kDependents; ++i) {
            store.execute(
                "INSERT INTO dependencies (repo_id, source_path, target_path, dep_type, dep_direction) "
                "VALUES (?, ?, ?, 'include', 'out')",
                repo.repo_id, repo_root + "/dependent_" + std::to_string(i) + ".hpp", root);
        }
        cache_manager::CacheManager cache(store);
        auto res = cache.invalidate(root, repo.repo_id);
        check(res.truncated,
              "traversal reports truncation when the dependent safety cap is reached");
        check(res.dependents.size() == 1000,
              "traversal stops at the documented cap while returning collected dependents");
    }

    // ---- DEP-02 precision: call edges never propagate invalidation ----
    std::cout << "[invalidation ignores call edges]\n";
    {
        std::string repo_root = (tmp / "callrepo").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "call repo store opens");

        write_file(repo_root + "/def.cpp", "int helper() { return 1; }\n");
        write_file(repo_root + "/user.cpp", "int helper();\nint run() { return helper(); }\n");
        indexer::index_file(store, repo_root + "/def.cpp", repo.repo_id);
        indexer::index_file(store, repo_root + "/user.cpp", repo.repo_id);

        write_file(repo_root + "/def.cpp", "int helper() { return 2; }\n");
        cache_manager::CacheManager cache(store);
        auto res = cache.invalidate(repo_root + "/def.cpp", repo.repo_id);
        bool user_marked = false;
        for (const auto& d : res.dependents)
            if (d.find("user.cpp") != std::string::npos) user_marked = true;
        check(!user_marked,
              "a caller is not invalidated just because it calls the edited file (dep_type != 'call' guard)");
    }

    // ---- DEP-02 precision: a tombstoned deleted file stays out of the
    // propagation graph (reuses the IDX-02 reconciliation) ----
    std::cout << "[deleted files stay out of invalidation propagation]\n";
    {
        std::string repo_root = (tmp / "goneinvalid").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "gone-invalid repo store opens");

        std::string header = repo_root + "/shared.h";
        std::string source = repo_root + "/user.cpp";
        write_file(header, "#pragma once\nint shared();\n");
        write_file(source, "#include \"shared.h\"\nint use() { return shared(); }\n");
        indexer::index_file(store, header, repo.repo_id);
        indexer::index_file(store, source, repo.repo_id);

        // The full scan reconciles the deleted header away (IDX-02)...
        fs::remove(header);
        auto rescan = indexer::index_repository(store, repo.repo_id);
        check(rescan.files_reconciled == 1, "deleted header tombstoned by the scan");

        // ...so a later invalidation of the same (now dead) path finds nobody.
        cache_manager::CacheManager cache(store);
        auto res = cache.invalidate(header, repo.repo_id);
        check(res.dependents.empty(),
              "invalidating a tombstoned file propagates to nobody (edges dropped with it)");
    }

    // ---- IDX-01: partial index is never mistaken for a complete one ----
    std::cout << "[index coverage (IDX-01)]\n";
    {
        std::string repo_root = (tmp / "coverage").string();
        fs::create_directories(repo_root);
        repository::RepoInfo repo = repository::get_or_create_repository(repo_root);

        knowledge_store::KnowledgeStore store;
        check(store.open(repo.db_path), "coverage store opens");

        // A repository with several files, none of them indexed yet.
        write_file(repo_root + "/a.cpp", "int a() { return 1; }\n");
        write_file(repo_root + "/b.cpp", "int b() { return 2; }\n");
        write_file(repo_root + "/c.cpp", "int c() { return 3; }\n");

        auto cov0 = indexer::repo_coverage(store, repo.repo_id, -1);
        check(cov0.coverage == indexer::IndexCoverage::not_indexed,
              "untouched repository reports not_indexed");

        // Indexing a single file must be recognizable as PARTIAL coverage.
        indexer::index_file(store, repo_root + "/a.cpp", repo.repo_id);
        auto cov1 = indexer::repo_coverage(store, repo.repo_id, -1);
        check(cov1.coverage == indexer::IndexCoverage::partial,
              "single-file indexing reports partial coverage, never full");
        check(cov1.files_indexed == 1 && cov1.files_total == 3,
              "partial coverage reports tracked vs total indexable files");
        auto st1 = store.query("SELECT status, full_index_covered FROM indexing_state WHERE repo_id = ?", repo.repo_id);
        check(!st1.empty() && st1[0][0] == "partial",
              "single-file indexing records status 'partial'");
        check(!st1.empty() && st1[0][1].empty(),
              "single-file indexing certifies no coverage snapshot");

        // A full scan turns the partial index into a certified complete one.
        indexer::index_repository(store, repo.repo_id);
        auto cov2 = indexer::repo_coverage(store, repo.repo_id, -1);
        check(cov2.coverage == indexer::IndexCoverage::full_fresh &&
              cov2.files_indexed == 3 && cov2.files_total == 3,
              "completed full scan certifies complete coverage");

        // A file added outside the server makes the certified claim stale: the
        // index is no longer provably complete (this is what a repo-level query
        // must detect instead of answering from an incomplete index).
        write_file(repo_root + "/d.cpp", "int d() { return 4; }\n");
        auto cov3 = indexer::repo_coverage(store, repo.repo_id, -1);
        check(cov3.coverage == indexer::IndexCoverage::full_stale,
              "file added outside the server invalidates the full claim");

        // Index the new file first, then delete it. A file deleted from disk is
        // reconciled by the next full scan: its
        // record becomes a tombstone and stops counting as tracked knowledge.
        indexer::index_file(store, repo_root + "/d.cpp", repo.repo_id);
        fs::remove(repo_root + "/d.cpp");
        indexer::index_repository(store, repo.repo_id);
        auto cov4 = indexer::repo_coverage(store, repo.repo_id, -1);
        check(cov4.coverage == indexer::IndexCoverage::full_fresh &&
              cov4.files_indexed == 3 && cov4.files_total == 3,
              "deleted file reconciled: tracked records match the repository again");
        // The tombstone is keyed by the stored spelling (what file_id and every
        // derived row reference), which may differ from the canonical spelling
        // produced by a filesystem walk; query it through the same lookup used
        // by coverage bookkeeping.
        auto tomb = store.query("SELECT is_deleted FROM files WHERE repo_id = ? AND path = ?",
                                repo.repo_id, fs::path(repo_root + "/d.cpp").string());
        check(!tomb.empty() && tomb[0][0] == "1", "deleted file kept as tombstone (audit trail)");
        auto live = store.query("SELECT COUNT(*) FROM files WHERE repo_id = ? AND is_deleted = 0",
                                repo.repo_id);
        check(!live.empty() && live[0][0] == "3", "tombstoned record stops counting as tracked knowledge");

        // ---- IDX-02: deleted files stop answering searches and the graph ----
        {
            std::string del_root = (tmp / "deleted").string();
            fs::create_directories(del_root + "/src");
            repository::RepoInfo del_repo = repository::get_or_create_repository(del_root);

            knowledge_store::KnowledgeStore del_store;
            check(del_store.open(del_repo.db_path), "deleted-file store opens");

            write_file(del_root + "/src/extra.h", "#pragma once\nint gone_helper();\n");
            write_file(del_root + "/src/used.h",
                       "#pragma once\nint used();\nint used_fn() { return used(); }\n");
            write_file(del_root + "/src/main.cpp",
                       "#include \"used.h\"\n#include \"extra.h\"\nint main() { return gone_helper(); }\n");

            indexer::index_repository(del_store, del_repo.repo_id);
            check(!symbol_index::search_symbols(del_store, del_repo.repo_id, "used").empty(),
                  "symbol of a live file resolvable before deletion");
            check(!symbol_index::search_symbols(del_store, del_repo.repo_id, "gone_helper").empty(),
                  "symbol of the to-be-deleted header resolvable before deletion");
            check(!dependency_graph::get_dependents(del_store, del_repo.repo_id, del_root + "/src/extra.h").empty(),
                  "dependents of the header recorded before deletion");
            check(!dependency_graph::get_dependencies(del_store, del_repo.repo_id,
                                                      del_root + "/src/main.cpp").empty(),
                  "include edges of the dependent recorded before deletion");

            // A stored summary must not survive its file either
            summary_store::store_file_summary(del_store, del_repo.repo_id, del_root + "/src/extra.h",
                                             "extra interface");
            check(!summary_store::get_file_summary(del_store, del_repo.repo_id, del_root + "/src/extra.h").empty(),
                  "summary of the header readable before deletion");

            // Delete the header through the filesystem only. Before any scan
            // reconciles it, the knowledge of the deleted file is still there,
            // and answering from it is exactly what must not happen silently:
            // coverage reports the claim as stale, and repo-level tools
            // revalidate and rescan instead of trusting the stale index.
            fs::remove(del_root + "/src/extra.h");

            auto cov_pre = indexer::repo_coverage(del_store, del_repo.repo_id, -1);
            check(cov_pre.coverage == indexer::IndexCoverage::full_stale,
                  "deleted file invalidates the certified full claim (IDX-01/IDX-02 gate)");

            indexer::FullScanResult rescan = indexer::index_repository(del_store, del_repo.repo_id);
            check(rescan.ok && rescan.files_reconciled == 1,
                  "scan reports the reconciled deleted file");
            check(rescan.dependents_invalidated > 0,
                  "dependents of the deleted file invalidated before tombstoning");

            auto cov_post = indexer::repo_coverage(del_store, del_repo.repo_id, -1);
            check(cov_post.coverage == indexer::IndexCoverage::full_fresh &&
                  cov_post.files_indexed == 2 && cov_post.tombstoned_records == 1,
                  "reconciled index covers the repository again (tombstone counted separately)");

            // The deleted file's knowledge must be gone from queries...
            check(symbol_index::search_symbols(del_store, del_repo.repo_id, "gone_helper").empty(),
                  "deleted symbol no longer answers searches");
            check(summary_store::get_file_summary(del_store, del_repo.repo_id, del_root + "/src/extra.h").empty(),
                  "summary of a deleted file is not readable");
            auto live_files = del_store.query(
                "SELECT path FROM files WHERE repo_id = ? AND is_deleted = 0 ORDER BY path", del_repo.repo_id);
            check(live_files.size() == 2, "deleted file drops out of the live file records");
            bool deleted_still_live = false;
            for (const auto& f : live_files)
                if (f[0].find("extra.h") != std::string::npos) deleted_still_live = true;
            check(!deleted_still_live, "live file records exclude the reconciled file");

            // ...and the dependent was re-parsed against the repository as it is now:
            // its graph no longer mentions the file that disappeared.
            check(!mentions(dependency_graph::get_dependencies(del_store, del_repo.repo_id,
                                                               del_root + "/src/main.cpp"),
                            "extra.h"),
                  "dependent's include edges rebuilt without the deleted file");
            check(!mentions(dependency_graph::get_all_dependencies(del_store, del_repo.repo_id,
                                                                  del_root + "/src/main.cpp"),
                            "extra.h"),
                  "no dependency edge survives to a tombstoned file");
            check(!mentions(symbol_index::get_file_symbols(del_store, del_repo.repo_id,
                                                          del_root + "/src/extra.h"),
                            "gone_helper"),
                  "tombstoned file exposes no symbols to per-file queries");
            check(dependency_graph::get_dependents(del_store, del_repo.repo_id, del_root + "/src/extra.h").empty(),
                  "no graph edge still points at the tombstoned file");
            // The live dependent was re-parsed, so its include list no longer
            // offers the deleted path; and its call edge to the vanished symbol
            // is dropped (nothing live defines that name any more).
            check(!mentions(dependency_graph::get_dependencies(del_store, del_repo.repo_id,
                                                               del_root + "/src/main.cpp"),
                            "extra.h"),
                  "live dependent's include edges rebuilt without the deleted path");
            auto dead_edges = del_store.query(
                "SELECT COUNT(*) FROM dependencies WHERE repo_id = ? AND target_path LIKE ?",
                del_repo.repo_id, std::string("%extra.h"));
            check(!dead_edges.empty() && dead_edges[0][0] == "0", "no dependency row targets a tombstoned path");
            check(!symbol_index::get_file_symbols(del_store, del_repo.repo_id,
                                                  del_root + "/src/used.h").empty(),
                  "re-indexed dependent keeps its own symbols");

            // A tombstone is resurrected when the file comes back: the record
            // becomes live knowledge again instead of staying deleted.
            write_file(del_root + "/src/extra.h", "#pragma once\nint extra();\nint extra2();\n");
            indexer::index_repository(del_store, del_repo.repo_id);
            auto cov_back = indexer::repo_coverage(del_store, del_repo.repo_id, -1);
            check(cov_back.coverage == indexer::IndexCoverage::full_fresh &&
                  cov_back.files_indexed == 3 && cov_back.tombstoned_records == 0,
                  "a restored file is re-indexed and stops being a tombstone");
            check(!symbol_index::search_symbols(del_store, del_repo.repo_id, "extra2").empty(),
                  "restored file answers searches again");
            // The restored content differs from the hash the invalidated summary
            // was stored against, so the old summary must not silently return.
            check(summary_store::get_file_summary(del_store, del_repo.repo_id, del_root + "/src/extra.h").empty(),
                  "invalidated summary stays invalid after a restore with different content");
            summary_store::store_file_summary(del_store, del_repo.repo_id, del_root + "/src/extra.h",
                                             "restored extra interface");
            check(summary_store::get_file_summary(del_store, del_repo.repo_id, del_root + "/src/extra.h") ==
                      "restored extra interface",
                  "restored file accepts a fresh valid summary");
        }

        // A certified 'full' index whose tracked records no longer match the
        // certified snapshot must not be read as complete, even without a walk.
        store.execute("UPDATE files SET stale = 1 WHERE repo_id = ? AND path = ?",
                      repo.repo_id, repo_root + "/a.cpp");
        auto cov5 = indexer::repo_coverage(store, repo.repo_id, 0);
        check(cov5.coverage == indexer::IndexCoverage::full_stale && cov5.stale_records == 1,
              "stale record downgrades a certified full claim (no filesystem needed)");

        // An index whose rows exist but were never produced by a completed scan
        // is partial even when the stored status text says otherwise.
        store.execute("UPDATE indexing_state SET status = 'full', full_index_covered = 3 "
                      "WHERE repo_id = ?", repo.repo_id);
        store.execute("DELETE FROM files WHERE repo_id = ? AND path = ?",
                      repo.repo_id, repo_root + "/c.cpp");
        auto cov6 = indexer::repo_coverage(store, repo.repo_id, 0);
        check(cov6.coverage == indexer::IndexCoverage::full_stale,
              "a stored 'full' status whose records lost coverage is not trusted (IDX-01)");

        // A failed scan must never leave a full claim behind.
        store.execute(
            "INSERT INTO indexing_state (repo_id, status, last_error) VALUES (?, 'failed', 'cannot walk') "
            "ON CONFLICT(repo_id) DO UPDATE SET status = 'failed', last_error = 'cannot walk', "
            "full_index_covered = NULL", repo.repo_id);
        auto cov7 = indexer::repo_coverage(store, repo.repo_id, 0);
        check(cov7.coverage == indexer::IndexCoverage::partial &&
              cov7.full_index_covered == -1,
              "failed scan leaves no certified coverage (status 'failed' is not 'full')");
    }

    fs::remove_all(tmp);
    std::cout << (failures == 0 ? "\n[store tests] ALL PASSED\n" : "\n[store tests] " + std::to_string(failures) + " FAILURES\n");
    return failures;
}
