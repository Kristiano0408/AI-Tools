// Tests for the MCP tool layer (tools::call_tool) and the JSON-RPC server.

#include "server/tools.h"
#include "server/server.h"
#include "engine/repository.h"

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

        // A brand-new repository is auto-indexed on first repo-level query.
        std::string heal_root = (tmp / "heal").string();
        fs::create_directories(heal_root);
        write_file(heal_root + "/a.cpp", "int helper() { return 1; }\nint use() { return helper(); }\n");

        json s = unwrap(tools::call_tool("repository_search", {{"path", heal_root}, {"query", "helper"}}), err);
        check(!err, "search on new repo succeeds");
        check(s.value("index_status", "") == "auto_indexed", "new repo auto-indexed on first search");
        check(!s["symbols"].empty(), "auto-indexed search finds symbols");
        std::string heal_id = s.value("repo_id", "");

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

    // ---- server JSON-RPC ----
    std::cout << "[server JSON-RPC]\n";
    {
        json init_req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}};
        json init_resp = json::parse(server::handle_request(init_req.dump()));
        check(init_resp["result"].contains("protocolVersion"), "initialize returns protocolVersion");
        check(init_resp["result"]["capabilities"].contains("tools"), "capabilities advertise tools");
        check(init_resp["id"] == 1, "response echoes request id");

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

        std::string notif = server::handle_request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        check(notif.empty(), "notifications produce no response");

        std::string garbage = server::handle_request("not json at all");
        json g_resp = json::parse(garbage);
        check(g_resp.contains("error") && g_resp["error"]["code"] == -32600, "malformed request -> -32600");
    }

    fs::remove_all(tmp);
    std::cout << (failures == 0 ? "\n[tools tests] ALL PASSED\n" : "\n[tools tests] " + std::to_string(failures) + " FAILURES\n");
    return failures;
}
