#pragma once

#include "knowledge_store.h"
#include <string>

namespace summary_store {

// Generate and store file summary
void store_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path, const std::string& summary);

// Get file summary
std::string get_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Generate and store repository summary
void store_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& summary);

// Get repository summary
std::string get_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

}
