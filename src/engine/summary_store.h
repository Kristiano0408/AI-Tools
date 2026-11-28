#pragma once

#include "knowledge_store.h"
#include <string>

namespace summary_store {

// Generate and store summary (or repository summary). Returns false when the
// write failed (file not in the index, or the store rejected/failed the
// insert): callers should check the result so a failed insert is never
// reported to the agent as a stored summary (DB-01).
bool store_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path, const std::string& summary);
bool store_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& summary);

// Get file summary
std::string get_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path);

// Get repository summary
std::string get_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

}
