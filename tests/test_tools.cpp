// Tests for the MCP tool layer (tools::call_tool), the MCP resources layer
// (resources::list/read/templates_list), the MCP workspace roots state
// (workspace_roots) and the JSON-RPC server.

#include "server/tools.h"
#include "server/resources.h"
#include "server/server.h"
#include "server/workspace_roots.h"
#include "engine/knowledge_store.h"
#include "engine/repository.h"
#include "engine/indexer.h"
#include "engine/summary_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace nlohmann;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  PASS: " << what << "\n";
    } else {
        std::cout << "  FAIL: " << what << "\n";
        ++failures;
    }
}

// Unwrap a tools/call payload: {"content":[{"type":"text","text":"<json>"}],"isError":bool}
static json unwrap(const std::string& raw, bool& is_error) {
    json wrapper = json::parse(raw);
    is_error = wrapper.value("isError", false);
    if (wrapper.contains("content") && !wrapper["content"].empty()) {
        try {
            return json::parse(wrapper["content"][0]["text"].get<std::string>());
        } catch (...) {
            return wrapper["content"][0]["text"];
        }
    }
    return json();
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// Fixture writes are checked: a silently-failed setup write would make a
// later assertion meaningless (DB-01 test hygiene).
static void must_execute(knowledge_store::KnowledgeStore& store, const std::string& sql,
                         const std::vector<std::string>& params = {}) {
    if (!knowledge_store::KnowledgeStore::statement_ok(store.execute(sql, params))) {
        std::cout << "  FAIL: fixture setup write failed: " << sql << " (" << store.get_error() << ")\n";
        ++failures;
    }
}

int run_tools_tests() {
    failures = 0;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "ai-tools-tool-tests";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    setenv("HOME", tmp.c_str(), 1);
    setenv("AI_TOOLS_DATA_DIR", (tmp / ".ai-tools").c_str(), 1);

    std::string repo_root = (tmp / "sample").string();
    fs::create_directories(repo_root);
    write_file(repo_root + "/main.cpp", R"cpp(
#include "greeter.h"

int main() {
    greet("world");
    return 0;
}
)cpp");
    write_file(repo_root + "/greeter.h", "#pragma once\nvoid greet(const char* name);\n");
    write_file(repo_root + "/greeter.cpp", "#include \"greeter.h\"\n#include <cstdio>\nvoid greet(const char* name) { printf(\"hi %s\\n\", name); }\n");

    repository::RepoInfo repo = repository::get_or_create_repository(repo_root);
    check(!repo.repo_id.empty(), "sample repo registered");

    // ---- index ----
    {
        bool err;
        json r = unwrap(tools::call_tool("index", {{"repo_id", repo.repo_id}}), err);
        check(!err, "index tool succeeds");
        check(r.value("files_indexed", 0) == 3, "index tool indexed 3 files");
    }

    // ---- file_content (inspect) ----
    {
        bool err;
        json r = unwrap(tools::call_tool("file_content", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}}), err);
        check(!err, "file_content succeeds");
        check(r.value("language", "") == "cpp", "language reported");
        check(r.contains("content_hash"), "content hash reported");
        check(r.value("index_status", "") == "fresh", "index status fresh");
        check(r["content"].get<std::string>().find("greet") != std::string::npos, "full content returned");
        check(!r["symbols"].empty(), "symbols returned from index");

        // ranged read
        json ranged = unwrap(tools::call_tool("file_content", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}, {"start_line", 5}, {"end_line", 6}}), err);
        check(!err && ranged["content"].get<std::string>().find("greet") != std::string::npos, "line range read works");
        check(ranged.value("returned_lines", 0) == 2, "range returns exactly requested lines");

        // invalid range rejected
        unwrap(tools::call_tool("file_content", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}, {"start_line", 5}, {"end_line", 2}}), err);
        check(err, "invalid line range rejected");
    }

    // ---- boundary enforcement ----
    {
        bool err;
        unwrap(tools::call_tool("file_content", {{"repo_id", repo.repo_id}, {"file_path", "../../etc/passwd"}}), err);
        check(err, "path escaping repository rejected");
    }

    // ---- edit ----
    {
        bool err;
        json r = unwrap(tools::call_tool("edit", {
            {"repo_id", repo.repo_id},
            {"file_path", "main.cpp"},
            {"start_line", 5},
            {"end_line", 5},
            {"new_content", "    greet(\"ai-tools\");"}}), err);
        check(!err, "edit succeeds");
        check(r.value("cache_invalidated", false), "edit invalidates cache");

        std::string content = fs::path(repo_root + "/main.cpp");
        std::ifstream f(repo_root + "/main.cpp");
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(all.find("greet(\"ai-tools\")") != std::string::npos, "edit replaced correct lines");
        check(all.find("int main()") != std::string::npos, "edit preserved surrounding content");

        // After edit, the index should be refreshed automatically
        json inspected = unwrap(tools::call_tool("file_content", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}}), err);
        check(inspected.value("index_status", "") == "fresh", "index fresh after edit");

        // invalid edit range rejected
        unwrap(tools::call_tool("edit", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}, {"start_line", 3}, {"end_line", 1}, {"new_content", "x"}}), err);
        check(err, "invalid edit range rejected");
    }

    // ---- T-02 (DEP-01/DEP-02): editing a header through the edit tool
    // invalidates every file that includes it, end-to-end through MCP ----
    {
        bool err;
        json r = unwrap(tools::call_tool("edit", {
            {"repo_id", repo.repo_id},
            {"file_path", "greeter.h"},
            {"start_line", 2},
            {"end_line", 2},
            {"new_content", "void greet(const char* name);\nvoid greet_again(const char* name);"}}), err);
        check(!err, "edit on a header succeeds");
        check(r.value("cache_invalidated", false), "header edit reports cache invalidation");
        check(r.value("dependents_invalidated", 0) >= 2,
              "header edit reports every file that includes the header (main.cpp + greeter.cpp)");
        bool dependent_listed = false;
        for (const auto& d : r["dependents"])
            if (d.get<std::string>().find("greeter.cpp") != std::string::npos) dependent_listed = true;
        check(dependent_listed, "edit result names a dependent file of the edited header");

        // The dependents' cached knowledge is gone before any rescan: their
        // symbols were dropped by the invalidation, while the edited header
        // itself was re-indexed from the content the edit just wrote.
        json hdr = unwrap(tools::call_tool("file_content", {{"repo_id", repo.repo_id}, {"file_path", "greeter.h"}}), err);
        bool new_sym = false;
        for (const auto& sym : hdr["symbols"]) if (sym["name"] == "greet_again") new_sym = true;
        check(new_sym, "edited header re-indexed from the written snapshot (new symbol visible)");

        // A full index scan re-indexes the stale dependents and restores fresh state.
        json resc = unwrap(tools::call_tool("index", {{"repo_id", repo.repo_id}}), err);
        check(!err && resc.value("status", "") == "indexed", "rescan after header edit succeeds");
        check(resc.value("files_refreshed", 0) >= 1,
              "stale dependents are re-parsed by the rescan (DEP-01 guarantee holds integrated)");

        // An edit to a .cpp that INCLUDES the header must not invalidate the header.
        json r2 = unwrap(tools::call_tool("edit", {
            {"repo_id", repo.repo_id},
            {"file_path", "greeter.cpp"},
            {"start_line", 3},
            {"end_line", 3},
            {"new_content", "void greet(const char* name) { (void)name; }\nvoid greet_again(const char* name) { (void)name; }"}}), err);
        check(!err && r2.value("dependents_invalidated", -1) == 0,
              "editing an includer invalidates nobody (reverse-edge direction)");
    }

    // ---- DEP-03: a change made OUTSIDE the server propagates through every
    // re-indexing entry point, not only through the edit tool ----
    {
        bool err;
        // Simulate an external edit (git checkout, another editor): rewrite
        // greeter.h directly on disk, adding a new function. No tool touched it.
        write_file(repo_root + "/greeter.h",
                   "#pragma once\nvoid greet(const char* name);\nvoid greet_again(const char* name);\nvoid external_added(const char* name);\n");

        // (a) Single-file index of the externally-changed header now reports and
        //     performs the propagation (previously this path reported nothing).
        json single = unwrap(tools::call_tool("index",
            {{"repo_id", repo.repo_id}, {"file_path", "greeter.h"}}), err);
        check(!err, "single-file index of externally changed header succeeds");
        check(single.value("dependents_invalidated", 0) >= 2,
              "single-file index invalidates every file including the header (DEP-03)");
        bool listed_main = false;
        for (const auto& d : single["dependents"])
            if (d.get<std::string>().find("main.cpp") != std::string::npos) listed_main = true;
        check(listed_main, "single-file index names main.cpp as an invalidated dependent");
        // Indexing one file is partial (IDX-01) and now leaves dependents stale:
        // coverage reports that with an actionable hint, not a bare stale count.
        check(single.contains("index_coverage"), "single-file index reports coverage");
        check(single["index_coverage"].value("stale_files", 0) >= 1,
              "coverage shows dependents left stale by the single-file update");
        check(single["index_coverage"].contains("hint"),
              "stale coverage carries a hint on how to settle it (DEP-03 visibility)");

        // (b) The self-heal path (file_content re-parsing a stale header) also
        //     propagates and reports it. Re-touch the header externally first.
        write_file(repo_root + "/greeter.h",
                   "#pragma once\nvoid greet(const char* name);\nvoid greet_again(const char* name);\nvoid external_added(const char* name);\nvoid another_external();\n");
        json fc = unwrap(tools::call_tool("file_content",
            {{"repo_id", repo.repo_id}, {"file_path", "greeter.h"}}), err);
        check(!err, "file_content self-heal succeeds");
        check(fc.value("index_status", "") == "fresh", "self-healed header is fresh");
        bool saw_external_sym = false;
        for (const auto& s : fc["symbols"])
            if (s["name"].get<std::string>() == "another_external") saw_external_sym = true;
        check(saw_external_sym, "self-heal re-parsed the externally changed header");
        check(fc.value("dependents_invalidated", 0) >= 2,
              "file_content self-heal invalidates the dependents (DEP-03)");

        // (c) summary read on the changed header goes through the same path.
        write_file(repo_root + "/greeter.h",
                   "#pragma once\nvoid greet(const char* name);\nvoid greet_again(const char* name);\nvoid external_added(const char* name);\nvoid another_external();\nvoid third_external();\n");
        json sm = unwrap(tools::call_tool("summary",
            {{"repo_id", repo.repo_id}, {"file_path", "greeter.h"}}), err);
        check(!err, "summary read on externally changed header succeeds");
        check(sm.value("dependents_invalidated", 0) >= 2,
              "summary self-heal propagates to dependents (DEP-03)");

        // (d) A full scan that re-indexes an externally-changed header settles
        //     its whole dependent chain inside the scan (stabilization): the
        //     includers it invalidates are re-parsed before coverage is
        //     certified, so no stale record survives and coverage is full_fresh.
        write_file(repo_root + "/greeter.h",
                   "#pragma once\nvoid greet(const char* name);\nvoid greet_again(const char* name);\nvoid external_added(const char* name);\nvoid another_external();\nvoid third_external();\nvoid settled_final(const char* name);\n");
        json full = unwrap(tools::call_tool("index", {{"repo_id", repo.repo_id}}), err);
        check(!err && full.value("status", "") == "indexed", "full rescan after external change succeeds");
        check(full.value("dependents_invalidated", 0) >= 1,
              "full scan reports it invalidated dependents");
        // Order-agnostic settling guarantee: header + both includers re-parsed,
        // whichever of the main loop / stabilization pass did each.
        check(full.value("files_refreshed", 0) + full.value("dependents_refreshed", 0) >= 3,
              "full scan re-parsed the changed header and both of its includers");
        check(full.contains("index_coverage") &&
                  full["index_coverage"].value("stale_files", 0) == 0,
              "no stale record survives the settling scan");
        check(full.value("index_status", "") == "index_full", "coverage full_fresh after settling");

        // (e) After settling, repo-level search sees the new symbol and nothing
        //     stale (no false positives from superseded knowledge).
        json sym = unwrap(tools::call_tool("symbol_search",
            {{"repo_id", repo.repo_id}, {"name", "third_external"}}), err);
        check(!err && !sym["symbols"].empty(),
              "symbol_search finds the externally added symbol after the settling scan");
    }

    // ---- search ----
    {
        bool err;
        json r = unwrap(tools::call_tool("repository_search", {{"repo_id", repo.repo_id}, {"query", "greeter"}}), err);
        check(!err, "search succeeds");
        check(r["files"].size() >= 2, "search finds matching files");

        json scoped = unwrap(tools::call_tool("repository_search", {{"repo_id", repo.repo_id}, {"query", "main"}, {"scope", repo_root + "/subdir"}}), err);
        check(!err && scoped["files"].empty(), "scope filter excludes out-of-scope files");

        // Cross-repo search without repo_id iterates the registry
        json global = unwrap(tools::call_tool("repository_search", {{"query", "main"}}), err);
        check(!err && !global["files"].empty() && !global["symbols"].empty(), "global search (no repo_id) finds files and symbols");
    }

    // ---- symbol_search ----
    {
        bool err;
        json r = unwrap(tools::call_tool("symbol_search", {{"repo_id", repo.repo_id}, {"name", "main"}}), err);
        check(!err, "symbol_search succeeds");
        check(!r["symbols"].empty(), "symbol main found");
    }

    // ---- dependency_graph ----
    {
        bool err;
        json r = unwrap(tools::call_tool("dependency_graph", {{"repo_id", repo.repo_id}, {"file_path", repo_root + "/main.cpp"}, {"direction", "out"}}), err);
        check(!err, "dependency_graph succeeds");
        bool found = false;
        for (const auto& d : r["dependencies"]) if (d.get<std::string>().find("greeter.h") != std::string::npos) found = true;
        check(found, "include dependency resolved to real file");

        json back = unwrap(tools::call_tool("dependency_graph", {{"repo_id", repo.repo_id}, {"file_path", repo_root + "/greeter.h"}, {"direction", "in"}}), err);
        bool found_main = false;
        for (const auto& d : back["dependents"]) if (d.get<std::string>().find("main.cpp") != std::string::npos) found_main = true;
        check(found_main, "reverse dependency lookup works");

        // Symbol-level call graph
        json symdeps = unwrap(tools::call_tool("dependency_graph", {{"repo_id", repo.repo_id}, {"symbol", "greet"}, {"direction", "both"}}), err);
        check(!err, "symbol dependency query succeeds");
        bool greet_caller = false;
        for (const auto& c : symdeps["callers"]) if (c.get<std::string>().find("main") != std::string::npos) greet_caller = true;
        check(greet_caller, "symbol callers found (main -> greet)");

        json callees = unwrap(tools::call_tool("dependency_graph", {{"repo_id", repo.repo_id}, {"symbol", "main"}, {"direction", "out"}}), err);
        bool greet_callee = false;
        for (const auto& c : callees["callees"]) if (c.get<std::string>().find("greet") != std::string::npos) greet_callee = true;
        check(greet_callee, "symbol callees found (main -> greet)");
    }

    // ---- summary ----
    {
        bool err;
        json w = unwrap(tools::call_tool("summary", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}, {"write", "program entry point"}}), err);
        check(!err && w.value("stored", false), "summary write succeeds");
        json g = unwrap(tools::call_tool("summary", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}}), err);
        check(g.value("summary", "") == "program entry point", "summary read back");
        check(g.value("valid", false), "summary marked valid");
    }

    // ---- file_tree ----
    {
        bool err;
        json r = unwrap(tools::call_tool("file_tree", {{"path", repo_root}}), err);
        check(!err, "file_tree succeeds");
        check(r["tree"].size() == 3, "file_tree lists entries");
    }

    // ---- self-healing / index-status contract ----
    {
        bool err;

        std::string heal_root = (tmp / "heal").string();
        fs::create_directories(heal_root);
        write_file(heal_root + "/a.cpp", "int helper() { return 1; }\nint use() { return helper(); }\n");

        json s = unwrap(tools::call_tool("repository_search", {{"path", heal_root}, {"query", "helper"}}), err);
        check(!err, "search on new repo succeeds");
        check(s.value("index_status", "") == "auto_indexed", "new repo auto-indexed on first search");
        check(!s["symbols"].empty(), "auto-indexed search finds symbols");
        std::string heal_id = s.value("repo_id", "");

        // IDX-04: reproduce a file whose mtime is newer than its old index
        // timestamps although its bytes still match the stored hash. A
        // repository-level query must use the content hash as the freshness
        // tie-breaker and return real hits, not the old empty gate envelope.
        auto stable_path = heal_root + "/a.cpp";
        auto stable_time = fs::last_write_time(stable_path);
        auto future_time = fs::file_time_type::clock::now() + std::chrono::seconds(10);
        fs::last_write_time(stable_path, future_time);
        auto db_path = repository::lookup_repository(heal_id).db_path;
        knowledge_store::KnowledgeStore freshness_store;
        check(freshness_store.open(db_path), "IDX-04 tool freshness store opens");
        must_execute(freshness_store,
            "UPDATE files SET last_indexed_at = '2000-01-01 00:00:00', "
            "content_verified_at = '2000-01-01 00:00:00' WHERE repo_id = ? AND path = ?",
            {heal_id, fs::weakly_canonical(stable_path).string()});
        json fresh_search = unwrap(
            tools::call_tool("symbol_search", {{"repo_id", heal_id}, {"name", "helper"}}), err);
        check(!err && !fresh_search["symbols"].empty(),
              "repo-level search returns symbols for mtime-only touch (IDX-04)");
        check(fresh_search.value("index_status", "") == "index_full" &&
                  fresh_search["index_coverage"].value("complete", false),
              "mtime-only touch is reported as full coverage, not an empty stale gate");
        json repeated_search = unwrap(
            tools::call_tool("symbol_search", {{"repo_id", heal_id}, {"name", "helper"}}), err);
        check(!err && !repeated_search["symbols"].empty() &&
                  repeated_search.value("index_status", "") == "index_full",
              "consecutive repo-level query remains full and keeps returning hits");

        // The post-scan failure envelope is rare in a healthy repository, so
        // make it deterministic with a conflicting legacy DB row whose path
        // canonicalizes to the same disk file. The scan sees the real path,
        // while the extra row remains touched and blocks the certified claim.
        std::string alias_path = heal_root + "/./a.cpp";
        must_execute(freshness_store,
            "INSERT INTO files (repo_id, path, content_hash, language, file_type, size_bytes, "
            "line_count, last_indexed_at, content_verified_at, is_binary, is_deleted, stale) "
            "SELECT repo_id, ?, 'deliberately-wrong-hash', language, file_type, size_bytes, "
            "line_count, '2000-01-01 00:00:00', '2000-01-01 00:00:00', is_binary, 0, 0 "
            "FROM files WHERE repo_id = ? AND path = ?",
            {alias_path, heal_id, fs::weakly_canonical(stable_path).string()});
        json blocked = unwrap(
            tools::call_tool("symbol_search", {{"repo_id", heal_id}, {"name", "helper"}}), err);
        check(!err && blocked.value("index_status", "") == "index_stale_after_scan",
              "gate reports a distinct status when coverage remains stale after scan");
        check(blocked.contains("hint") && blocked.contains("index_coverage") &&
                  blocked["index_coverage"].value("touched_files", 0) > 0 &&
                  !blocked["index_coverage"]["touched_paths"].empty(),
              "post-scan stale envelope includes hint and touched paths, not a no-hit result");
        must_execute(freshness_store, "DELETE FROM files WHERE repo_id = ? AND path = ?",
                     {heal_id, alias_path});
        auto restored_scan = indexer::index_repository(freshness_store, heal_id);
        check(restored_scan.ok &&
                  indexer::repo_coverage(freshness_store, heal_id, -1).coverage ==
                      indexer::IndexCoverage::full_fresh,
              "removing the injected conflicting row restores a fresh tool-test index");
        fs::last_write_time(stable_path, stable_time);

        // A file changed outside the server is re-parsed inline: fresh + new symbol.
        write_file(heal_root + "/a.cpp", "int helper() { return 1; }\nint use() { return helper(); }\nint brand_new() { return 2; }\n");
        json fc = unwrap(tools::call_tool("file_content", {{"repo_id", heal_id}, {"file_path", "a.cpp"}}), err);
        check(!err, "file_content on externally-changed file succeeds");
        check(fc.value("index_status", "") == "fresh", "externally-changed file self-heals to fresh");
        bool new_sym = false;
        for (const auto& sym : fc["symbols"]) if (sym["name"] == "brand_new") new_sym = true;
        check(new_sym, "self-heal picks up newly added symbol");

        // summary on a non-indexable file must fail loudly, not claim stored.
        write_file(heal_root + "/data.bin", std::string("\x01\x02\x03\x00binary", 11));
        unwrap(tools::call_tool("summary", {{"repo_id", heal_id}, {"file_path", "data.bin"}, {"write", "x"}}), err);
        check(err, "summary on non-indexable file errors (no false stored)");

        // summary on a source file that was never indexed still stores + reads back.
        write_file(heal_root + "/b.cpp", "int b() { return 3; }\n");
        json w = unwrap(tools::call_tool("summary", {{"repo_id", heal_id}, {"file_path", "b.cpp"}, {"write", "b summary"}}), err);
        check(!err && w.value("stored", false), "summary write self-indexes an unindexed source file");
        json rb = unwrap(tools::call_tool("summary", {{"repo_id", heal_id}, {"file_path", "b.cpp"}}), err);
        check(rb.value("summary", "") == "b summary" && rb.value("valid", false), "self-indexed summary reads back valid");

        // A file deleted outside the server (IDX-02): a repo-level query must
        // reconcile on the fly, and the deleted file's knowledge must not be
        // offered any more.
        write_file(heal_root + "/c.cpp", "int helper_removed() { return 4; }\n");
        json before_del = unwrap(tools::call_tool("repository_search", {{"repo_id", heal_id}, {"query", "c.cpp"}}), err);
        check(!err && !before_del["files"].empty(), "file added outside the server becomes searchable after rescan");
        bool listed_before = false;
        for (const auto& f : before_del["files"])
            if (f["path"].get<std::string>().find("c.cpp") != std::string::npos) listed_before = true;
        check(listed_before, "added file listed by searches");

        fs::remove(heal_root + "/c.cpp");
        json after_del = unwrap(tools::call_tool("repository_search", {{"repo_id", heal_id}, {"query", "c.cpp"}}), err);
        check(!err, "search after external deletion succeeds");
        check(after_del.value("index_status", "") == "reindexed_full", "external deletion triggers a reconciling rescan");
        bool listed_after = false;
        for (const auto& f : after_del["files"])
            if (f["path"].get<std::string>().find("c.cpp") != std::string::npos) listed_after = true;
        check(!listed_after, "deleted file no longer listed by searches");

        json sym_del = unwrap(tools::call_tool("symbol_search", {{"repo_id", heal_id}, {"name", "helper_removed"}}), err);
        check(!err && sym_del["symbols"].empty(), "deleted file's symbol no longer answers symbol searches");
        check(after_del.contains("index_coverage") &&
                  after_del["index_coverage"].value("tombstoned_files", 0) == 1,
              "tool result reports the tombstoned record of the deleted file");
    }

    // ---- pagination (limit/offset) ----
    {
        bool err;
        json r = unwrap(tools::call_tool("repository_search", {{"repo_id", repo.repo_id}, {"query", ""}, {"limit", 2}, {"offset", 0}}), err);
        check(!err, "paginated search succeeds");
        check(r["files"].size() <= 2, "limit caps returned files");
        check(r.value("files_total", 0) >= 3, "files_total reports full match count");
        check(r.value("files_truncated", false), "truncation flagged");
        check(r.value("files_next_offset", -1) == 2, "next_offset provided");

        json p2 = unwrap(tools::call_tool("repository_search", {{"repo_id", repo.repo_id}, {"query", ""}, {"limit", 2}, {"offset", 2}}), err);
        check(!err && p2["files"].size() >= 1, "offset returns the next page");
        // pages must not overlap
        bool overlap = false;
        for (const auto& a : r["files"])
            for (const auto& b : p2["files"])
                if (a["path"] == b["path"]) overlap = true;
        check(!overlap, "pages do not overlap");

        // small result sets stay clean (no pagination noise)
        json small = unwrap(tools::call_tool("symbol_search", {{"repo_id", repo.repo_id}, {"name", "main"}}), err);
        check(!err && !small.contains("symbols_truncated"), "untruncated results carry no pagination fields");
    }

    // ---- qualified call-graph lookup ----
    {
        bool err;
        // A call site "obj.method()" / intra-class "method()" records the bare
        // callee name; querying the qualified symbol must still find the edge.
        write_file(repo_root + "/widget.cpp", R"cpp(
namespace ui {
class Widget {
public:
    void draw();
    void resize();
};
void Widget::draw() { resize(); }
void Widget::resize() { repaint(); }
void repaint() {}
}
)cpp");
        unwrap(tools::call_tool("index", {{"repo_id", repo.repo_id}, {"file_path", repo_root + "/widget.cpp"}}), err);
        check(!err, "index widget.cpp succeeds");

        json q = unwrap(tools::call_tool("dependency_graph", {{"repo_id", repo.repo_id}, {"symbol", "Widget::resize"}, {"direction", "in"}}), err);
        check(!err, "qualified callee callers query succeeds");
        bool found_draw = false;
        for (const auto& c : q["callers"]) if (c.get<std::string>().find("draw") != std::string::npos) found_draw = true;
        check(found_draw, "qualified lookup finds callers of bare-recorded callee (Widget::resize <- draw)");

        json fully = unwrap(tools::call_tool("dependency_graph", {{"repo_id", repo.repo_id}, {"symbol", "ui::Widget::resize"}, {"direction", "in"}}), err);
        bool found_fully = false;
        for (const auto& c : fully["callers"]) if (c.get<std::string>().find("draw") != std::string::npos) found_fully = true;
        check(found_fully, "fully-qualified lookup resolves through bare fallback");
    }

    // ---- symbol_summary (SUM-01) ----
    {
        bool err;
        write_file(repo_root + "/symsum.cpp", R"cpp(
struct Left { int handle() { return 1; } };
struct Right { int handle() { return 2; } };
int standalone() { return 3; }
)cpp");
        unwrap(tools::call_tool("index", {{"repo_id", repo.repo_id}, {"file_path", repo_root + "/symsum.cpp"}}), err);
        check(!err, "index symsum.cpp succeeds");

        // (a) The inventory answers 'what knowledge do I already have about this
        //     file's symbols?' without reading any body.
        json inv0 = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}}), err);
        check(!err && inv0.contains("symbols"), "per-file symbol inventory succeeds");
        bool saw_standalone_none = false;
        for (const auto& s : inv0["symbols"])
            if (s.value("name", "") == "standalone" && s.value("state", "") == "none") saw_standalone_none = true;
        check(saw_standalone_none, "inventory reports an unsummarized symbol as 'none'");

        // (b) An ambiguous name is answered with candidates, never by picking one.
        json amb = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "handle"}}), err);
        check(!err && amb.value("status", "") == summary_store::symbol_status::kAmbiguous,
              "ambiguous symbol reads as ambiguous, not as a silent pick");
        check(amb["candidates"].size() == 2, "ambiguous read lists both candidate symbols");
        check(amb.contains("hint"), "ambiguous read carries the disambiguation hint");

        // A write against the same ambiguous name fails honestly, with candidates.
        json ambw = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "handle"}, {"write", "ambiguous"}}), err);
        check(err && ambw.value("stored", true) == false, "ambiguous write is refused, not silently attached");
        check(ambw.contains("candidates") && ambw["candidates"].size() == 2,
              "refused write carries the candidates to choose from");

        // (c) A qualified symbol round-trips: write, then read the summary back
        //     with its provenance and its body hash.
        json wq = unwrap(tools::call_tool("symbol_summary",
            {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "Left::handle"},
             {"write", "left handler"}, {"model_version", "test-model"}, {"prompt_version", "v1"}}), err);
        check(!err && wq.value("stored", false), "qualified symbol summary write succeeds");
        check(wq.value("symbol", "") == "Left::handle", "write echoes the qualified symbol");
        check(wq.contains("body_hash") && !wq["body_hash"].get<std::string>().empty(),
              "write reports the symbol body hash it was stored against");
        check(wq.value("model_version", "") == "test-model" && wq.value("prompt_version", "") == "v1",
              "provenance echoed on write");

        json gq = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "Left::handle"}}), err);
        check(!err && gq.value("summary", "") == "left handler" && gq.value("valid", false),
              "qualified summary read back");
        check(gq.value("status", "") == summary_store::symbol_status::kOk, "read reports status ok");
        check(gq.value("model_version", "") == "test-model", "read carries the stored provenance");

        // (d) A missing summary is a stated state, not an empty answer (IDX-04
        //     lesson applied to knowledge).
        json wn = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "standalone"}}), err);
        check(!err && wn.value("status", "") == summary_store::symbol_status::kNone &&
                  !wn.contains("summary"),
              "an unsummarized symbol reads as no_summary with a recovery hint");
        check(wn.contains("hint"), "missing summary carries the write hint");

        // An unknown symbol name is distinguished from a missing summary.
        json nf = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "no_such_symbol"}}), err);
        check(!err && nf.value("status", "") == summary_store::symbol_status::kNotFound,
              "an unindexed symbol name reads as symbol_not_indexed, never as no_summary");

        // (e) After a write, the inventory marks that symbol 'valid' — so the
        //     agent can pick a symbol it already has knowledge about.
        check(unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "standalone"}, {"write", "returns three"}}), err).value("stored", false),
              "standalone summary stored");
        json inv1 = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}}), err);
        int valid_count = 0, none_count = 0;
        for (const auto& s : inv1["symbols"]) {
            if (s.value("state", "") == "valid") ++valid_count;
            if (s.value("state", "") == "none") ++none_count;
        }
        check(valid_count >= 2 && none_count >= 1, "inventory marks summarized symbols 'valid'");

        // (f) An edit confined to one symbol makes only that symbol's summary
        //     stale, and the stale answer carries the live line range needed to
        //     refresh it (it never reads like 'nothing was ever stored').
        write_file(repo_root + "/symsum.cpp", R"cpp(
struct Left { int handle() { return 1; } };
struct Right { int handle() { return 2; } };
int standalone() { return 300; }
int standalone_extra() { return 4; }
)cpp");
        json stale = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "standalone"}}), err);
        check(!err && stale.value("status", "") == summary_store::symbol_status::kStaleBody,
              "self-healed re-index reports the edited symbol's summary as stale");
        check(stale.contains("start_line") && stale.contains("end_line"),
              "the stale answer carries the symbol's live line range for recovery");
        json other = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "Left::handle"}}), err);
        check(!err && other.value("status", "") == summary_store::symbol_status::kOk,
              "the summary of an untouched symbol survives its sibling's edit");

        json fixed = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "standalone"}, {"write", "returns three hundred"}}), err);
        check(!err && fixed.value("stored", false), "the stale summary is replaced by a fresh write");
        check(unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"symbol", "standalone"}}), err).value("status", "") == summary_store::symbol_status::kOk,
              "the refreshed summary reads as valid");

        // (g) Repository-wide listing goes through the coverage gate (IDX-01) and
        //     is honest about it; a full scan certifies it.
        json partial_list = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}}), err);
        check(!err, "repo-wide symbol summary listing answers");
        check(partial_list.contains("index_coverage") || partial_list.contains("symbol_summaries"),
              "repo-wide listing reports either its coverage state or its results");
        unwrap(tools::call_tool("index", {{"repo_id", repo.repo_id}}), err);
        json list = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}}), err);
        check(!err && list.contains("symbol_summaries"), "repo-wide listing succeeds after a certified scan");
        bool listed = false;
        for (const auto& s : list["symbol_summaries"])
            if (s.value("symbol", "") == "standalone" && s.value("state", "") == "valid") listed = true;
        check(listed, "repo-wide listing carries the stored summary with its state");
        json page = unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"limit", 1}}), err);
        check(page["symbol_summaries"].size() <= 1 && page.value("symbol_summaries_truncated", false),
              "repo-wide listing honors limit and flags truncation");

        // (h) A write without a file+symbol target is refused rather than guessed.
        unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"write", "orphan"}}), err);
        check(err, "repo-wide write is rejected (write requires file_path and symbol)");
        unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "symsum.cpp"}, {"write", "orphan"}}), err);
        check(err, "per-file write is rejected (write requires a symbol)");

        // (i) Boundary enforcement applies to this tool like the others.
        unwrap(tools::call_tool("symbol_summary", {{"repo_id", repo.repo_id}, {"file_path", "/etc/passwd"}, {"symbol", "standalone"}}), err);
        check(err, "symbol_summary refuses a path outside the repository");
    }

    // ---- execute ----
    {
        bool err;
        json r = unwrap(tools::call_tool("execute", {{"repo_id", repo.repo_id}, {"command", "echo hello && echo oops >&2"}}), err);
        check(!err, "execute succeeds");
        check(r.value("exit_code", -1) == 0, "exit code captured");
        check(r["stdout"].get<std::string>().find("hello") != std::string::npos, "stdout captured");
        check(r["stderr"].get<std::string>().find("oops") != std::string::npos, "stderr captured");
        check(r.contains("duration_ms"), "duration reported");

        json failing = unwrap(tools::call_tool("execute", {{"repo_id", repo.repo_id}, {"command", "exit 7"}}), err);
        check(!err && failing.value("exit_code", 0) == 7, "nonzero exit code reported (not swallowed)");

        // execute requires repo_id (repository boundary)
        unwrap(tools::call_tool("execute", {{"command", "echo nope"}}), err);
        check(err, "execute without repo_id rejected");
    }

    // ---- unknown tool ----
    {
        bool err;
        unwrap(tools::call_tool("does_not_exist", json::object()), err);
        check(err, "unknown tool returns error");
    }

    // ---- MCP Resources interface ----
    std::cout << "\n[resources]\n";
    {
        // A stored repository summary makes it listable/readable.
        bool err;
        unwrap(tools::call_tool("summary", {{"repo_id", repo.repo_id}, {"write", "sample C++ greeter repo"}}), err);
        check(!err, "repository summary written");

        // resources/list for a single repository
        json rl = resources::list({{"repo_id", repo.repo_id}});
        check(rl["resources"].is_array() && !rl["resources"].empty(), "resources/list returns resources");
        bool has_index = false, has_summary = false, has_main = false, all_repo_scheme = true, shape_ok = true;
        std::string main_uri;
        for (const auto& r : rl["resources"]) {
            std::string uri = r["uri"].get<std::string>();
            if (uri == "repo://" + repo.repo_id + "/index") has_index = true;
            if (uri == "repo://" + repo.repo_id + "/summary") has_summary = true;
            if (uri == "repo://" + repo.repo_id + "/file/main.cpp") { has_main = true; main_uri = uri; }
            if (uri.rfind("repo://", 0) != 0) all_repo_scheme = false;
            if (!r.contains("name") || !r.contains("mimeType") || !r.contains("annotations")) shape_ok = false;
        }
        check(shape_ok, "every resource carries name/mimeType/annotations");
        check(has_index, "resources/list exposes repo://<id>/index");
        check(has_summary, "resources/list exposes repo://<id>/summary");
        check(has_main, "indexed file exposed as repo://<id>/file/<rel>");
        check(all_repo_scheme, "all repo resources use the repo:// scheme");

        // resources/read: file content
        json rr = resources::read({{"uri", "repo://" + repo.repo_id + "/file/main.cpp"}});
        check(rr["contents"].is_array() && rr["contents"].size() == 1, "resources/read returns a contents array");
        check(rr["contents"][0]["text"].get<std::string>().find("greet(\"ai-tools\")") != std::string::npos,
              "file resource text matches current content");
        check(rr["contents"][0]["mimeType"].get<std::string>() == "text/x-c++", "cpp mime type reported");
        check(rr["contents"][0]["uri"] == "repo://" + repo.repo_id + "/file/main.cpp", "contents echo the uri");

        // resources/read: repository summary
        json rs = resources::read({{"uri", "repo://" + repo.repo_id + "/summary"}});
        check(rs["contents"][0]["text"].get<std::string>() == "sample C++ greeter repo", "repo summary readable");
        check(rs["contents"][0]["mimeType"] == "text/markdown", "summary is text/markdown");

        // files with no valid summary must fail loudly
        try {
            resources::read({{"uri", "repo://" + repo.repo_id + "/summary/greeter.h"}});
            check(false, "missing file summary should raise");
        } catch (const resources::resource_error& e) {
            check(e.code == -32002, "missing file summary -> -32002");
        }

        // resources/read: repository index overview
        json ri = resources::read({{"uri", "repo://" + repo.repo_id + "/index"}});
        json overview = json::parse(ri["contents"][0]["text"].get<std::string>());
        check(overview.value("repo_id", "") == repo.repo_id, "repo index overview carries repo_id");
        check(overview.contains("files") && overview["files"].size() >= 3, "repo index overview lists files");
        bool rel_path = false;
        for (const auto& f : overview["files"]) if (f["path"] == "main.cpp") rel_path = true;
        check(rel_path, "repo index overview uses repository-relative paths");
        bool linked = false;
        for (const auto& f : overview["files"]) if (f["uri"] == main_uri) linked = true;
        check(linked, "overview file entries link back to the file resource uri");

        // file:// for a path inside a registered repository maps to the same file
        json rf = resources::read({{"uri", "file://" + repo_root + "/greeter.cpp"}});
        check(rf["contents"][0]["text"].get<std::string>().find("greet") != std::string::npos,
              "file:// inside a repository reads repository content");

        // file:// escaping every registered repository is rejected
        try {
            resources::read({{"uri", "file:///etc/passwd"}});
            check(false, "file:// outside any registered repo should raise");
        } catch (const resources::resource_error& e) {
            check(e.code == -32002, "file:// outside a repository -> -32002");
        }

        // repo-relative path traversal must not escape the repository root
        try {
            resources::read({{"uri", "repo://" + repo.repo_id + "/file/../../etc/passwd"}});
            check(false, "path traversal should raise");
        } catch (const resources::resource_error& e) {
            check(e.code == -32002, "path traversal in a repo uri -> -32002");
        }

        // unknown repository id
        try {
            resources::read({{"uri", "repo://doesnotexist/index"}});
            check(false, "unknown repo_id should raise");
        } catch (const resources::resource_error& e) {
            check(e.code == -32002, "unknown repository -> -32002");
        }

        // missing uri -> -32602 (Invalid params)
        try {
            resources::read(json{{"not_uri", 1}});
            check(false, "missing uri should raise");
        } catch (const resources::resource_error& e) {
            check(e.code == -32602, "missing uri -> -32602");
        }

        // binary files are not exposed as text resources
        std::string blob;
        blob.push_back('\x00');
        blob.push_back('\x01');
        blob.append("binary payload");
        write_file(repo_root + "/blob.dat", blob);
        try {
            resources::read({{"uri", "repo://" + repo.repo_id + "/file/blob.dat"}});
            check(false, "binary file read should raise");
        } catch (const resources::resource_error& e) {
            check(e.code == -32002, "binary file -> -32002 (not a text resource)");
        }

        // file summaries are listable once written
        unwrap(tools::call_tool("summary", {{"repo_id", repo.repo_id}, {"file_path", "greeter.h"},
                                           {"write", "greeter interface"}}), err);
        json rl2 = resources::list({{"repo_id", repo.repo_id}});
        bool has_file_summary = false;
        for (const auto& r : rl2["resources"])
            if (r["uri"] == "repo://" + repo.repo_id + "/summary/greeter.h") has_file_summary = true;
        check(has_file_summary, "file with a stored summary is listable as a resource");
        json rfs = resources::read({{"uri", "repo://" + repo.repo_id + "/summary/greeter.h"}});
        check(rfs["contents"][0]["text"] == "greeter interface", "file summary resource readable");

        // pagination: cursor/limit
        json p1 = resources::list({{"repo_id", repo.repo_id}, {"limit", 2}});
        check(p1["resources"].size() == 2, "limit caps resources/list page");
        check(p1.contains("nextCursor"), "nextCursor returned when truncated");
        json p2 = resources::list({{"repo_id", repo.repo_id}, {"limit", 2}, {"cursor", p1["nextCursor"]}});
        bool no_overlap = true;
        for (const auto& a : p1["resources"])
            for (const auto& b : p2["resources"])
                if (a["uri"] == b["uri"]) no_overlap = false;
        check(no_overlap && !p2["resources"].empty(), "cursor paginates without overlap");
        json last = resources::list({{"repo_id", repo.repo_id}, {"cursor", "0"}, {"limit", 10000}});
        check(!last.contains("nextCursor"), "unpaged list has no nextCursor");

        // cross-repository list (registry overview + every repo)
        json all = resources::list(json::object());
        bool has_registry = false, has_sample = false;
        for (const auto& r : all["resources"]) {
            std::string uri = r["uri"].get<std::string>();
            if (uri == "repo://registry/index") has_registry = true;
            if (uri == "repo://" + repo.repo_id + "/index") has_sample = true;
        }
        check(has_registry && has_sample, "resources/list without repo_id spans all repositories");
        json reg = resources::read({{"uri", "repo://registry/index"}});
        json reg_json = json::parse(reg["contents"][0]["text"].get<std::string>());
        bool sample_registered = false;
        for (const auto& r : reg_json["repositories"]) if (r["repo_id"] == repo.repo_id) sample_registered = true;
        check(sample_registered, "repo://registry/index lists registered repositories");

        // templates
        json tpl = resources::templates_list(json::object());
        check(tpl["resourceTemplates"].is_array() && tpl["resourceTemplates"].size() >= 5,
              "resources/templates/list returns templates");
        bool file_template = false;
        for (const auto& t : tpl["resourceTemplates"])
            if (t["uriTemplate"] == "repo://{repo_id}/file/{path}") file_template = true;
        check(file_template, "file resource template advertised");
    }

    // ---- MCP workspace roots (roots/list state + roots-aware resolution) ----
    std::cout << "\n[workspace roots]\n";
    {
        bool err;

        // A workspace folder the client opened inside the sample repository
        // matches that repository (folder-in-repo containment).
        workspace_roots::set_from_roots_array(json::array({
            {{"uri", "file://" + repo_root + "/subdir"}, {"name", "subdir"}},
        }));
        check(!workspace_roots::empty(), "roots stored from a roots/list result");
        bool subdir_match = false;
        for (const auto& id : workspace_roots::workspace_matching_repos())
            if (id == repo.repo_id) subdir_match = true;
        check(subdir_match, "workspace folder inside a repository matches it");

        // Repo-in-folder containment: a workspace root above the repository.
        workspace_roots::set_from_roots_array(json::array({{{"uri", "file://" + tmp.string()}}}));
        bool parent_match = false;
        for (const auto& id : workspace_roots::workspace_matching_repos())
            if (id == repo.repo_id) parent_match = true;
        check(parent_match, "workspace root containing a repository matches it");

        // Percent-encoded file:// URIs decode to canonical paths.
        std::string spaced = (tmp / "with space").string();
        fs::create_directories(spaced);
        workspace_roots::set_from_roots_array(json::array({
            {{"uri", "file://" + (tmp / "with%20space").string()}},
        }));
        check(!workspace_roots::empty() && workspace_roots::roots()[0].find(' ') != std::string::npos,
              "file:// URI percent-decoding resolves the real directory");

        // Non-file schemes are rejected.
        workspace_roots::set_from_roots_array(json::array({{{"uri", "https://example.com/x"}}}));
        check(workspace_roots::empty(), "non-file:// root URIs are rejected");

        // Unrelated workspace root: the repository must NOT resolve implicitly.
        std::string unrelated = (tmp / "unrelated").string();
        fs::create_directories(unrelated);
        workspace_roots::set_from_roots_array(json::array({{{"uri", "file://" + unrelated}}}));
        bool unrelated_leak = false;
        for (const auto& id : workspace_roots::workspace_matching_repos())
            if (id == repo.repo_id) unrelated_leak = true;
        check(!unrelated_leak, "unrelated workspace root does not match the sample repository");

        // An unscoped search now stays inside the announced workspace instead
        // of silently falling back to the whole registry.
        json scoped = unwrap(tools::call_tool("repository_search", {{"query", "main"}}), err);
        check(!err, "unscoped search with unrelated roots succeeds");
        check(scoped.value("search_scope", "") == "workspace_roots",
              "unscoped search reports search_scope=workspace_roots");
        check(scoped.contains("workspace_roots") && scoped["workspace_roots"].size() == 1 &&
                  scoped["workspace_roots"][0].get<std::string>() == unrelated,
              "search result echoes the announced workspace roots");
        check(scoped.value("repositories_searched", -1) == 0,
              "unrelated roots search covers zero repositories");
        check(!scoped.contains("files") || scoped["files"].empty(),
              "unrelated roots search returns no files from other projects");

        // Rooted search: the matching repository is used automatically.
        json rooted = unwrap(tools::call_tool("repository_search", {{"query", "greeter"}}), err);
        check(!err, "search resolves through announced workspace roots");
        check(rooted.value("search_scope", "") == "workspace_roots" ||
                  !rooted.value("repo_id", "").empty(),
              "grounded search reports its scope");

        // execute stays explicit: roots alone never pick a repository for it.
        unwrap(tools::call_tool("execute", {{"command", "echo nope"}}), err);
        check(err, "execute still requires an explicit repository when roots match none");

        workspace_roots::clear();
        check(workspace_roots::empty(), "roots cleared for the next session");
    }

    // ---- server JSON-RPC ----
    std::cout << "[server JSON-RPC]\n";
    {
        json init_req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}};
        json init_resp = json::parse(server::handle_request(init_req.dump()));
        check(init_resp["result"].contains("protocolVersion"), "initialize returns protocolVersion");
        check(init_resp["result"]["capabilities"].contains("tools"), "capabilities advertise tools");
        check(init_resp["result"]["capabilities"].contains("resources"), "capabilities advertise resources");
        check(init_resp["id"] == 1, "response echoes request id");

        // MCP Roots handshake: a client advertising roots causes the server
        // to issue a roots/list request, and the response updates session state.
        json roots_init = {
            {"jsonrpc", "2.0"}, {"id", 101}, {"method", "initialize"},
            {"params", {{"capabilities", {{"roots", {{"listChanged", true}}}}}}}
        };
        json roots_init_resp = json::parse(server::handle_request(roots_init.dump()));
        check(roots_init_resp.contains("result"), "roots-capable initialize succeeds");
        json roots_poll = json::parse(server::next_pending_request());
        check(roots_poll["method"] == "roots/list", "server polls client with roots/list");
        json roots_response = {
            {"jsonrpc", "2.0"}, {"id", roots_poll["id"]},
            {"result", {{"roots", {{{"uri", "file://" + repo_root}, {"name", "sample"}}}}}}
        };
        check(server::handle_request(roots_response.dump()).empty(), "roots/list response is consumed");
        check(!workspace_roots::empty() && workspace_roots::roots()[0] == repo_root,
              "roots/list response updates workspace roots");
        check(server::handle_request(
                  R"({"jsonrpc":"2.0","method":"notifications/roots/list_changed"})").empty(),
              "roots/list_changed is a notification");
        json roots_refresh = json::parse(server::next_pending_request());
        check(roots_refresh["method"] == "roots/list", "roots/list_changed triggers a refresh poll");
        workspace_roots::clear();

        json list_req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
        json list_resp = json::parse(server::handle_request(list_req.dump()));
        check(list_resp["result"]["tools"].is_array() && list_resp["result"]["tools"].size() >= 10, "tools/list returns tool array");

        json call_req = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                         {"params", {{"name", "file_tree"}, {"arguments", {{"path", repo_root}}}}}};
        json call_resp = json::parse(server::handle_request(call_req.dump()));
        check(call_resp.contains("result") && call_resp["result"].contains("content"), "tools/call wrapped in result");

        // ---- payload deduplication (structuredContent carries the bulk body) ----
        json fc_req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                       {"params", {{"name", "file_content"},
                                   {"arguments", {{"repo_id", repo.repo_id}, {"file_path", "main.cpp"}}}}}};
        json fc_resp = json::parse(server::handle_request(fc_req.dump()));
        check(fc_resp["result"].contains("structuredContent"), "file_content exposes structuredContent");
        check(fc_resp["result"]["structuredContent"].contains("content"), "structuredContent carries the file body");
        json fc_text = json::parse(fc_resp["result"]["content"][0]["text"].get<std::string>());
        check(!fc_text.contains("content"), "text block body trimmed (no duplicated body)");
        check(fc_text.value("content_omitted", false) == true, "text block flags the omitted body");
        check(fc_text.contains("returned_lines") && fc_text.contains("symbols"),
              "text block keeps the metadata and symbols");

        json bad_req = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "bogus/method"}};
        json bad_resp = json::parse(server::handle_request(bad_req.dump()));
        check(bad_resp.contains("error") && bad_resp["error"]["code"] == -32601, "unknown method -> -32601");

        // resources wired through the JSON-RPC dispatcher
        json rlist = {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "resources/list"},
                      {"params", {{"repo_id", repo.repo_id}}}};
        json rlist_resp = json::parse(server::handle_request(rlist.dump()));
        check(rlist_resp["result"]["resources"].is_array() && !rlist_resp["result"]["resources"].empty(),
              "resources/list dispatched over JSON-RPC");

        json rread = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "resources/read"},
                      {"params", {{"uri", "repo://" + repo.repo_id + "/file/main.cpp"}}}};
        json rread_resp = json::parse(server::handle_request(rread.dump()));
        check(rread_resp["result"]["contents"][0]["text"].get<std::string>().find("greet") != std::string::npos,
              "resources/read dispatched over JSON-RPC");

        json rtpl = {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "resources/templates/list"}};
        json rtpl_resp = json::parse(server::handle_request(rtpl.dump()));
        check(rtpl_resp["result"]["resourceTemplates"].is_array(),
              "resources/templates/list dispatched over JSON-RPC");

        json rmissing = {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "resources/read"},
                         {"params", {{"uri", "repo://nope/index"}}}};
        json rmissing_resp = json::parse(server::handle_request(rmissing.dump()));
        check(rmissing_resp["error"]["code"] == -32002, "resources/read unknown resource -> -32002");
        check(rmissing_resp["error"]["data"]["uri"] == "repo://nope/index", "error data carries the uri");

        json rparams = {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "resources/read"},
                        {"params", {{"uri", 42}}}};
        json rparams_resp = json::parse(server::handle_request(rparams.dump()));
        check(rparams_resp["error"]["code"] == -32602, "resources/read invalid params -> -32602");

        json rsub = {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "resources/subscribe"},
                     {"params", {{"uri", "repo://" + repo.repo_id + "/index"}}}};
        json rsub_resp = json::parse(server::handle_request(rsub.dump()));
        check(rsub_resp["error"]["code"] == -32002, "resources/subscribe -> -32002 (not supported)");

        std::string notif = server::handle_request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        check(notif.empty(), "notifications produce no response");

        std::string garbage = server::handle_request("not json at all");
        json g_resp = json::parse(garbage);
        check(g_resp.contains("error") && g_resp["error"]["code"] == -32600, "malformed request -> -32600");
    }

        // ---- DB-01: failed full scans surface their cause through the MCP envelope ----
        {
          bool err;
          std::string gone_root = (tmp / "scan-fails").string();
          fs::create_directories(gone_root);
          write_file(gone_root + "/a.cpp", "int a_fn() { return 1; }\n");

          json initial = unwrap(tools::call_tool("index", {{"path", gone_root}}), err);
          check(!err && initial.value("status", "") == "indexed",
              "initial full index succeeds and establishes the repository");
          std::string scan_id = initial.value("repo_id", "");
          check(!scan_id.empty(), "initial index response returns repo_id");

          // Make the next repository walk fail after the repository has already
          // been registered and indexed. The tool must not claim success, and it
          // must preserve the engine's cause for the caller.
          fs::remove_all(gone_root);
          json failed = unwrap(tools::call_tool("index", {{"repo_id", scan_id}}), err);
          check(!err, "a failed scan is returned as an index result, not a malformed tool response");
          check(failed.value("status", "") == "index_failed",
              "failed full scan is reported as index_failed, never indexed");
          check(!failed.value("hint", "").empty() &&
                failed["hint"].get<std::string>().find("cannot walk repository") != std::string::npos,
              "the MCP hint includes the concrete repository-walk failure");
          check(failed.contains("index_coverage") &&
                !failed["index_coverage"].value("last_scan_error", "").empty(),
              "coverage metadata preserves the failed scan cause");
          check(failed.value("index_status", "") != "index_full" &&
                !failed.contains("files_refreshed"),
              "failed scan is not represented as complete or refreshed");
        }

    fs::remove_all(tmp);
    std::cout << (failures == 0 ? "\n[tools tests] ALL PASSED\n" : "\n[tools tests] " + std::to_string(failures) + " FAILURES\n");
    return failures;
}
