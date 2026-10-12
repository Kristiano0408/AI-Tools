#pragma once

#include "knowledge_store.h"
#include <unordered_dense.h>
#include <string>
#include <vector>

namespace dependency_graph {

// Get dependencies for a file
std::vector<std::string> get_dependencies(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Get dependents (files that depend on this file)
std::vector<std::string> get_dependents(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Get all dependencies (transitive)
std::vector<std::string> get_all_dependencies(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Symbol-level: what the symbol calls (callees)
std::vector<std::string> get_callees(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_name);

// Symbol-level: what calls the symbol (callers)
std::vector<std::string> get_callers(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_name);

// Build adjacency list
ankerl::unordered_dense::map<std::string, std::vector<std::string>> build_adjacency_list(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

}
