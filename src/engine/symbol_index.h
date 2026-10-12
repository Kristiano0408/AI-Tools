#pragma once

#include "knowledge_store.h"
#include <string>
#include <vector>

namespace symbol_index {

// Search symbols by name
std::vector<std::string> search_symbols(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& name);

// Get symbols by type
std::vector<std::string> get_symbols_by_type(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_type);

// Get symbols in a file
std::vector<std::string> get_file_symbols(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Get symbol details
std::vector<std::string> get_symbol_details(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& symbol_name);

}
