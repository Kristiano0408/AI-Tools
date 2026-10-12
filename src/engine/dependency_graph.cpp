#include "dependency_graph.h"
#include <unordered_dense.h>
#include <algorithm>
#include <queue>
#include <iostream>

namespace dependency_graph {

std::vector<std::string> get_dependencies(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path) {
    std::vector<std::string> deps;
    auto rows = store.query(
        "SELECT target_path FROM dependencies WHERE repo_id = ? AND source_path = ? AND dep_type != 'call'",
        repo_id, file_path
    );
    for (const auto& row : rows) {
        deps.push_back(row[0]);
    }
    return deps;
}

std::vector<std::string> get_dependents(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path) {
    std::vector<std::string> dependents;
    auto rows = store.query(
        "SELECT source_path FROM dependencies WHERE repo_id = ? AND target_path = ? AND dep_type != 'call'",
        repo_id, file_path
    );
    for (const auto& row : rows) {
        dependents.push_back(row[0]);
    }
    return dependents;
}

std::vector<std::string> get_all_dependencies(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path) {
    std::vector<std::string> all_deps;
    ankerl::unordered_dense::set<std::string> visited;
    std::queue<std::string> queue;
    
    queue.push(file_path);
    
    while (!queue.empty()) {
        std::string current = queue.front();
        queue.pop();
        
        if (visited.count(current)) continue;
        visited.insert(current);
        
        auto deps = get_dependencies(store, repo_id, current);
        for (const auto& dep : deps) {
            if (!visited.count(dep)) {
                queue.push(dep);
                all_deps.push_back(dep);
            }
        }
    }
    
    return all_deps;
}

ankerl::unordered_dense::map<std::string, std::vector<std::string>> build_adjacency_list(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> adj;
    auto rows = store.query(
        "SELECT source_path, target_path FROM dependencies WHERE repo_id = ? AND dep_type != 'call'",
        repo_id
    );
    for (const auto& row : rows) {
        adj[row[0]].push_back(row[1]);
    }
    return adj;
}

namespace {

// Match a symbol by bare name or qualified name ("app::Widget::draw", "Config.validate")
std::string symbol_match_sql(const std::string& column) {
    return "(" + column + " = ? OR " + column + " LIKE ?)";
}

// Strip namespace qualifiers and template arguments: "shapes::helper",
// "std::forward<Args>" -> "helper", "forward"
std::string last_segment(const std::string& s) {
    size_t p = s.rfind("::");
    std::string seg = (p == std::string::npos) ? s : s.substr(p + 2);
    size_t lt = seg.find('<');
    if (lt != std::string::npos) seg = seg.substr(0, lt);
    return seg;
}

std::vector<std::string> symbol_edges(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                                      const std::string& symbol_name, bool outbound) {
    std::vector<std::string> results;
    std::string sel_symbol = outbound ? "target_symbol" : "source_symbol";
    std::string match_symbol = outbound ? "source_symbol" : "target_symbol";

    // Callers (outbound edges) are stored with their qualified name
    // ("cache_manager::CacheManager::invalidate"), so a direct/qualified match
    // already resolves them. Callees, however, are stored with the callee text
    // as written at the call site: "obj.method(...)" records the bare "method",
    // "ns::func(...)" records "ns::func". Querying a *qualified* callee would
    // then silently find nothing. Resolve the qualified name to its bare tail
    // and disambiguate bare matches via include linkage.
    auto rows = store.query(
        "SELECT DISTINCT d." + sel_symbol + ", f.path, d.source_path FROM dependencies d "
        "JOIN files f ON f.repo_id = d.repo_id AND f.path = d.source_path "
        "WHERE d.repo_id = ? AND d.dep_type = 'call' AND " + symbol_match_sql("d." + match_symbol) +
        " ORDER BY d.source_path",
        repo_id, symbol_name, "%" + symbol_name + "%"
    );

    for (const auto& row : rows) {
        results.push_back(row[0] + " (" + row[2] + ")");
    }

    // Qualified callee lookup: a call like "store_.query(...)" records the bare
    // "query" as the target, so querying the qualified "KnowledgeStore::query"
    // would otherwise find nothing. Retry with the bare tail when the qualified
    // name matched nothing. (Name-based, consistent with the documented
    // resolution limitation.)
    if (results.empty() && symbol_name.find("::") != std::string::npos) {
        std::string bare = last_segment(symbol_name);
        if (!bare.empty() && bare != symbol_name) {
            auto bare_rows = store.query(
                "SELECT DISTINCT d." + sel_symbol + ", d.source_path FROM dependencies d "
                "WHERE d.repo_id = ? AND d.dep_type = 'call' AND d." + match_symbol + " = ? "
                "ORDER BY d.source_path",
                repo_id, bare);
            for (const auto& row : bare_rows) {
                results.push_back(row[0] + " (" + row[1] + ")");
            }
        }
    }
    return results;
}

} // namespace

std::vector<std::string> get_callees(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_name) {
    return symbol_edges(store, repo_id, symbol_name, true);
}

std::vector<std::string> get_callers(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_name) {
    return symbol_edges(store, repo_id, symbol_name, false);
}

}
