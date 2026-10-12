#include "engine/symbol_index.h"
#include <sstream>

namespace symbol_index {

std::vector<std::string> search_symbols(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& name) {
    std::vector<std::string> results;
    auto rows = store.query(
        "SELECT s.name, s.symbol_type, f.path, s.start_line FROM symbols s "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND s.name LIKE ?",
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
        "WHERE f.repo_id = ? AND s.symbol_type = ?",
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
        "WHERE f.repo_id = ? AND f.path = ?",
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
        "WHERE f.repo_id = ? AND s.name = ?",
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
