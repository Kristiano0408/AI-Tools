// Tests for knowledge_store, content_hasher, cache_manager, repository identity,
// and incremental indexing behaviour.

#include "engine/knowledge_store.h"
#include "engine/content_hasher.h"
#include "engine/cache_manager.h"
#include "engine/repository.h"
#include "engine/indexer.h"
#include "engine/dependency_graph.h"
#include "engine/summary_store.h"

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

        auto state = store.query("SELECT status, files_indexed FROM indexing_state WHERE repo_id = ?", repo.repo_id);
        check(!state.empty() && state[0][0] == "full", "full indexing state recorded");

        // Incremental re-index: unchanged files are skipped, only changed ones reprocessed
        write_file(source, "#include \"util.h\"\nint helper() { return 44; }\n");
        indexer::index_repository(store, repo.repo_id);
        auto state2 = store.query("SELECT files_indexed FROM indexing_state WHERE repo_id = ?", repo.repo_id);
        check(!state2.empty() && state2[0][0] == "1", "incremental re-index processes only the changed file");
        check(!indexer::needs_indexing(store, source, repo.repo_id), "changed file re-indexed");
        check(!indexer::needs_indexing(store, header, repo.repo_id), "unchanged header untouched");
    }

    fs::remove_all(tmp);
    std::cout << (failures == 0 ? "\n[store tests] ALL PASSED\n" : "\n[store tests] " + std::to_string(failures) + " FAILURES\n");
    return failures;
}
