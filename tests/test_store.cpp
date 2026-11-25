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
