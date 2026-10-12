#pragma once

#include "parser.h"
#include "knowledge_store.h"
#include <string>
#include <vector>

namespace indexer {

// Index a single file
void index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);

// Index all files in a repository
void index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

// Check if file needs re-indexing
bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);

}
