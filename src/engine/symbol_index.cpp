#include "engine/symbol_index.h"
#include <sstream>

namespace symbol_index {

namespace {

const char* kRegionColumns =
    "SELECT s.file_id, f.path, s.name, s.qualified_name, s.symbol_type, s.scope, "
    "s.start_line, s.end_line, s.body_hash, f.content_hash "
    "FROM symbols s JOIN files f ON f.file_id = s.file_id ";

SymbolRegion to_region(const std::vector<std::string>& row) {
    SymbolRegion r;
    if (row.size() < 10) return r;
    r.file_id = std::stoll(row[0]);
    r.file_path = row[1];
    r.name = row[2];
    r.qualified_name = row[3].empty() ? row[2] : row[3]; // pre-SUM-01 rows: name alone
    r.symbol_type = row[4];
    r.scope = row[5];
    r.start_line = std::stoll(row[6]);
    r.end_line = std::stoll(row[7]);
    r.has_body_hash = !row[8].empty();
    r.body_hash = row[8];
    r.file_content_hash = row[9];
    return r;
}

} // namespace

std::vector<SymbolRegion> list_file_symbol_regions(knowledge_store::KnowledgeStore& store,
                                                   const std::string& repo_id,
                                                   const std::string& file_path) {
    std::vector<SymbolRegion> out;
    auto rows = store.query(
        std::string(kRegionColumns) +
        "WHERE f.repo_id = ? AND f.path = ? AND f.is_deleted = 0 ORDER BY s.start_line, s.symbol_id",
        repo_id, file_path);
    if (store.last_query_failed()) return out; // DB-01: a failed read is not 'no symbols'
    for (const auto& row : rows) out.push_back(to_region(row));
    return out;
}

std::vector<SymbolRegion> find_symbol_regions(knowledge_store::KnowledgeStore& store,
                                              const std::string& repo_id,
                                              const std::string& file_path,
                                              const std::string& query,
                                              const std::string& symbol_type) {
    auto all = list_file_symbol_regions(store, repo_id, file_path);
    if (all.empty()) return all;

    std::vector<SymbolRegion> matches;
    for (const auto& r : all) {
        if (!query.empty() && r.qualified_name != query && r.name != query) continue;
        if (!symbol_type.empty() && r.symbol_type != symbol_type) continue;
        matches.push_back(r);
    }
    // Qualified spellings win: an exact qualified_name match must not compete
    // with a bare-name hit on a different symbol.
    std::vector<SymbolRegion> qualified;
    for (const auto& r : matches)
        if (!query.empty() && r.qualified_name == query) qualified.push_back(r);
    return qualified.empty() ? matches : qualified;
}

std::vector<std::string> search_symbols(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& name) {
    std::vector<std::string> results;
    auto rows = store.query(
        "SELECT s.name, s.symbol_type, f.path, s.start_line FROM symbols s "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND f.is_deleted = 0 AND s.name LIKE ?",
        repo_id, "%" + name + "%"
    );
    for (const auto& row : rows) {
        std::ostringstream oss;
        oss << row[0] << " (" << row[1] << ") in "
            << row[2] << ":" << row[3];
        results.push_back(oss.str());
    }
    return results;
}

std::vector<std::string> get_symbols_by_type(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_type) {
    std::vector<std::string> results;
    auto rows = store.query(
        "SELECT s.name, f.path, s.start_line FROM symbols s "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND f.is_deleted = 0 AND s.symbol_type = ?",
        repo_id, symbol_type
    );
    for (const auto& row : rows) {
        std::ostringstream oss;
        oss << row[0] << " in " << row[1] << ":" << row[2];
        results.push_back(oss.str());
    }
    return results;
}

std::vector<std::string> get_file_symbols(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path) {
    std::vector<std::string> results;
    auto rows = store.query(
        "SELECT s.name, s.symbol_type, s.start_line FROM symbols s "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND f.path = ? AND f.is_deleted = 0",
        repo_id, file_path
    );
    for (const auto& row : rows) {
        std::ostringstream oss;
        oss << row[1] << " " << row[0] << " at line " << row[2];
        results.push_back(oss.str());
    }
    return results;
}

std::vector<std::string> get_symbol_details(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_name) {
    std::vector<std::string> results;
    auto rows = store.query(
        "SELECT s.name, s.symbol_type, s.signature, f.path, s.start_line, s.end_line, s.scope "
        "FROM symbols s JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND f.is_deleted = 0 AND s.name = ?",
        repo_id, symbol_name
    );
    for (const auto& row : rows) {
        std::ostringstream oss;
        oss << row[0] << " (" << row[1] << ")\n"
            << "  Signature: " << row[2] << "\n"
            << "  File: " << row[3] << ":" << row[4] << "-" << row[5] << "\n"
            << "  Scope: " << row[6];
        results.push_back(oss.str());
    }
    return results;
}

}
