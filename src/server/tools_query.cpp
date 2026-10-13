// Read-only tool handlers: repository_search, symbol_search, file_content,
// dependency_graph, file_tree, code_analysis.

#include "server/tools_internal.h"

#include "engine/content_hasher.h"
#include "engine/dependency_graph.h"
#include "engine/indexer.h"
#include "engine/summary_store.h"
#include "engine/symbol_index.h"
#include "utils/file_utils.h"
#include "utils/path_utils.h"
#include "utils/perf_metrics.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace tools {

std::string handle_repository_search(const ToolContext& ctx) {
    PERF_SCOPE("handler.repository_search");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    if (arguments.find("query") == arguments.end()) return tool_error("Missing required argument: query");
    std::string query = arguments["query"].get<std::string>();

    json results = json::object();

    if (!repo_id.empty()) {
        results["repo_id"] = repo_id;
        if (!ensure_repo_indexed(store, repo_id, repo, results)) return tool_result(results);
    }
    if (repo_id.empty()) {
        // Search across all registered repositories
        knowledge_store::KnowledgeStore registry;
        if (registry.open(repository::registry_db_path())) {
            auto repos = registry.query("SELECT repo_id, root_path FROM repositories");
            for (const auto& r : repos) {
                knowledge_store::KnowledgeStore repo_store;
                if (!repository::open_repo_store(r[1], repo_store)) continue;

                auto files = repo_store.query(
                    "SELECT path, language, line_count FROM files WHERE path LIKE ? LIMIT 200",
                    "%" + query + "%");
                for (const auto& f : files) {
                    results["files"].push_back({{"path", f[0]}, {"language", f[1]}, {"line_count", std::stoi(f[2])}});
                }

                auto syms = symbol_index::search_symbols(repo_store, r[0], query);
                for (const auto& s : syms) results["symbols"].push_back(s);
            }
        }
    } else {
        std::string scope = arguments.contains("scope") ? arguments["scope"].get<std::string>() : "";
        std::string file_type = arguments.contains("file_type") ? arguments["file_type"].get<std::string>() : "";

        std::string sql = "SELECT path, language, line_count FROM files WHERE repo_id = ? AND path LIKE ?";
        std::vector<std::string> params = {repo_id, "%" + query + "%"};
        if (!scope.empty()) { sql += " AND path LIKE ?"; params.push_back(scope + "%"); }
        if (!file_type.empty()) { sql += " AND file_type = ?"; params.push_back(file_type); }
        sql += " LIMIT 500";

        auto files = store.query(sql, params);
        for (const auto& f : files) {
            results["files"].push_back({{"path", f[0]}, {"language", f[1]}, {"line_count", std::stoi(f[2])}});
        }

        // Symbol matches within this repository
        auto syms = symbol_index::search_symbols(store, repo_id, query);
        results["symbols"] = json(syms);
    }

    paginate_array(results["files"], arguments, results, "files", 50);
    paginate_array(results["symbols"], arguments, results, "symbols", 50);
    return tool_result(results);
}

std::string handle_symbol_search(const ToolContext& ctx) {
    PERF_SCOPE("handler.symbol_search");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    if (arguments.find("name") == arguments.end()) return tool_error("Missing required argument: name");
    std::string name = arguments["name"].get<std::string>();

    json result = json::object();
    if (!repo_id.empty()) {
        result["repo_id"] = repo_id;
        if (!ensure_repo_indexed(store, repo_id, repo, result)) return tool_result(result);
    }
    if (repo_id.empty()) {
        // Search across all registered repositories
        knowledge_store::KnowledgeStore registry;
        if (registry.open(repository::registry_db_path())) {
            auto repos = registry.query("SELECT repo_id, root_path FROM repositories");
            for (const auto& r : repos) {
                knowledge_store::KnowledgeStore repo_store;
                if (!repository::open_repo_store(r[1], repo_store)) continue;
                auto details = symbol_index::get_symbol_details(repo_store, r[0], name);
                for (const auto& d : details) result["symbols"].push_back(d);
            }
        }
    } else {
        auto details = symbol_index::get_symbol_details(store, repo_id, name);
        result["symbols"] = json(details);
        auto callers = dependency_graph::get_callers(store, repo_id, name);
        result["callers"] = json(callers);
    }
    paginate_array(result["symbols"], arguments, result, "symbols", 50);
    if (result.contains("callers"))
        paginate_array(result["callers"], arguments, result, "callers", 50);
    return tool_result(result);
}

std::string handle_file_content(const ToolContext& ctx) {
    PERF_SCOPE("handler.file_content");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    if (arguments.find("file_path") == arguments.end()) return tool_error("Missing required argument: file_path");
    std::string file_path = arguments["file_path"].get<std::string>();

    std::string abs_path = file_path;
    if (!repo_id.empty()) {
        abs_path = guarded_path(repo, file_path);
        if (abs_path.empty()) return tool_error("Path escapes repository boundary: " + file_path);
    }

    if (!path_utils::file_exists(abs_path)) return tool_error("File not found: " + abs_path);

    // Lazy self-healing: a stale or never-indexed file is re-parsed
    // inline (cheap, single file) so results are always fresh without
    // requiring an explicit index call.
    if (!repo_id.empty() && indexer::needs_indexing(store, abs_path, repo_id)) {
        indexer::index_file(store, abs_path, repo_id);
    }

    json result = json::object();
    result["path"] = abs_path;

    // Cached metadata when indexed and fresh
    auto rows = store.query(
        "SELECT language, file_type, size_bytes, line_count, content_hash FROM files WHERE path = ? ORDER BY last_indexed_at DESC LIMIT 1",
        abs_path);
    std::string stored_hash;
    if (!rows.empty()) {
        result["language"] = rows[0][0];
        result["file_type"] = rows[0][1];
        result["size_bytes"] = std::stoll(rows[0][2]);
        result["line_count"] = std::stoi(rows[0][3]);
        stored_hash = rows[0][4];
        std::string current_hash = content_hasher::compute_file_hash(abs_path);
        result["content_hash"] = current_hash;
        result["index_status"] = (current_hash == stored_hash) ? "fresh" : "stale";
    } else {
        result["index_status"] = repo_id.empty() ? "not_indexed" : "fresh";
    }

    // Symbols from the index
    auto symbols = store.query(
        "SELECT name, symbol_type, start_line, end_line, scope FROM symbols "
        "WHERE file_id IN (SELECT file_id FROM files WHERE path = ?) ORDER BY start_line",
        abs_path);
    for (const auto& s : symbols) {
        result["symbols"].push_back({{"name", s[0]}, {"type", s[1]}, {"start_line", std::stoi(s[2])}, {"end_line", std::stoi(s[3])}, {"scope", s[4]}});
    }

    // Valid cached summary
    std::string summary = summary_store::get_file_summary(store, repo_id, abs_path);
    if (!summary.empty()) result["summary"] = summary;

    // Content, optionally ranged
    int start_line = arguments.contains("start_line") ? arguments["start_line"].get<int>() : 1;
    int end_line = arguments.contains("end_line") ? arguments["end_line"].get<int>() : -1;
    if (start_line < 1) return tool_error("start_line must be >= 1");
    if (end_line != -1 && end_line < start_line) return tool_error("end_line must be >= start_line");

    auto lines = file_utils::read_lines(abs_path, start_line, end_line);
    std::string content;
    for (size_t i = 0; i < lines.size(); ++i) {
        content += lines[i];
        if (i + 1 < lines.size()) content += "\n";
    }
    result["content"] = content;
    result["returned_lines"] = static_cast<int>(lines.size());

    return tool_result(result);
}

std::string handle_dependency_graph(const ToolContext& ctx) {
    PERF_SCOPE("handler.dependency_graph");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    std::string direction = arguments.contains("direction") ? arguments["direction"].get<std::string>() : "both";
    int depth = arguments.contains("depth") ? arguments["depth"].get<int>() : 1;
    if (depth < 1) return tool_error("depth must be >= 1");

    // Symbol-level call graph query
    if (arguments.contains("symbol")) {
        if (repo_id.empty()) return tool_error("symbol dependency query requires repo_id");
        std::string symbol = arguments["symbol"].get<std::string>();

        json result = {{"symbol", symbol}};
        // A call-graph query reads the persisted index: ensure it exists,
        // otherwise an empty result would look like "no callers" instead
        // of "not indexed".
        if (!ensure_repo_indexed(store, repo_id, repo, result)) return tool_result(result);
        if (direction == "out" || direction == "both")
            result["callees"] = json(dependency_graph::get_callees(store, repo_id, symbol));
        if (direction == "in" || direction == "both")
            result["callers"] = json(dependency_graph::get_callers(store, repo_id, symbol));
        if (result.contains("callees"))
            paginate_array(result["callees"], arguments, result, "callees", 100);
        if (result.contains("callers"))
            paginate_array(result["callers"], arguments, result, "callers", 100);
        return tool_result(result);
    }

    if (arguments.find("file_path") == arguments.end()) return tool_error("Missing required argument: file_path or symbol");
    std::string file_path = arguments["file_path"].get<std::string>();

    // Normalize to the canonical absolute path used in the stored graph
    if (!repo_id.empty()) {
        std::string abs = guarded_path(repo, file_path);
        if (abs.empty()) return tool_error("Path escapes repository boundary: " + file_path);
        file_path = abs;
    } else {
        std::error_code ec;
        file_path = std::filesystem::weakly_canonical(file_path, ec).string();
    }

    json result = json::object();
    if (!repo_id.empty()) {
        result["repo_id"] = repo_id;
        if (!ensure_repo_indexed(store, repo_id, repo, result)) return tool_result(result);
    }
    if (direction == "out" || direction == "both") {
        auto deps = (depth > 1)
            ? dependency_graph::get_all_dependencies(store, repo_id, file_path)
            : dependency_graph::get_dependencies(store, repo_id, file_path);
        result["dependencies"] = json(deps);
    }
    if (direction == "in" || direction == "both") {
        auto dependents = dependency_graph::get_dependents(store, repo_id, file_path);
        result["dependents"] = json(dependents);
    }
    if (result.contains("dependencies"))
        paginate_array(result["dependencies"], arguments, result, "dependencies", 100);
    if (result.contains("dependents"))
        paginate_array(result["dependents"], arguments, result, "dependents", 100);
    return tool_result(result);
}

std::string handle_file_tree(const ToolContext& ctx) {
    PERF_SCOPE("handler.file_tree");
    const json& arguments = ctx.arguments;
    const repository::RepoInfo& repo = ctx.repo;

    std::string path;
    if (arguments.find("path") != arguments.end() && !arguments["path"].is_null()) {
        path = arguments["path"].get<std::string>();
    } else if (!repo.root_path.empty()) {
        path = repo.root_path; // default to the resolved repository root
    } else {
        return tool_error("Missing required argument: path (or a resolvable repo_id)");
    }
    int max_depth = arguments.contains("max_depth") ? arguments["max_depth"].get<int>() : 4;

    std::filesystem::path root(path);
    if (!std::filesystem::is_directory(root)) return tool_error("Not a directory: " + path);

    auto walk = [&](auto&& self, const std::filesystem::path& dir, int depth) -> json {
        json entries = json::array();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::string name = entry.path().filename().string();
            if (name == ".git" || name == "build" || name == "node_modules" || name == "__pycache__") continue;
            json item = {{"name", name}};
            if (entry.is_directory()) {
                item["type"] = "directory";
                if (depth < max_depth) item["children"] = self(self, entry.path(), depth + 1);
            } else {
                item["type"] = "file";
            }
            entries.push_back(item);
        }
        std::sort(entries.begin(), entries.end(), [](const json& a, const json& b) {
            if (a["type"] != b["type"]) return a["type"] == "directory";
            return a["name"] < b["name"];
        });
        return entries;
    };

    return tool_result(json{{"path", path}, {"tree", walk(walk, root, 1)}});
}

std::string handle_code_analysis(const ToolContext& ctx) {
    PERF_SCOPE("handler.code_analysis");
    const json& arguments = ctx.arguments;
    const std::string& repo_id = ctx.repo_id;
    knowledge_store::KnowledgeStore& store = ctx.store;
    const repository::RepoInfo& repo = ctx.repo;

    if (arguments.find("file_path") == arguments.end()) return tool_error("Missing required argument: file_path");
    std::string file_path = arguments["file_path"].get<std::string>();
    if (!repo_id.empty()) {
        file_path = guarded_path(repo, file_path);
        if (file_path.empty()) return tool_error("Path escapes repository boundary");
    }
    if (!path_utils::file_exists(file_path)) return tool_error("File not found: " + file_path);

    // Symbol statistics read the index: self-heal a stale/missing entry so
    // counts are never silently empty for an out-of-date file.
    if (!repo_id.empty() && indexer::needs_indexing(store, file_path, repo_id)) {
        indexer::index_file(store, file_path, repo_id);
    }

    auto lines = file_utils::read_lines(file_path, 1, -1);
    int code_lines = 0, comment_lines = 0, blank_lines = 0;
    for (const auto& line : lines) {
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) { blank_lines++; continue; }
        std::string t = line.substr(first);
        if (t.rfind("//", 0) == 0 || t.rfind("#", 0) == 0 || t.rfind("/*", 0) == 0 || t.rfind("*", 0) == 0)
            comment_lines++;
        else
            code_lines++;
    }

    json result = {
        {"file", file_path},
        {"total_lines", static_cast<int>(lines.size())},
        {"code_lines", code_lines},
        {"comment_lines", comment_lines},
        {"blank_lines", blank_lines}};

    // Symbol statistics from the index
    auto counts = store.query(
        "SELECT symbol_type, COUNT(*) FROM symbols WHERE file_id IN (SELECT file_id FROM files WHERE path = ?) GROUP BY symbol_type",
        file_path);
    for (const auto& c : counts) result["symbols"][c[0]] = std::stoi(c[1]);

    return tool_result(result);
}

}
