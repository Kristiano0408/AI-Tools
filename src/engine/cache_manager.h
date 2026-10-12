#pragma once

#include "knowledge_store.h"
#include <string>
#include <vector>

namespace cache_manager {

class CacheManager {
public:
    CacheManager(knowledge_store::KnowledgeStore& store);
    
    // Check if a file's cached data is valid
    bool is_valid(const std::string& file_path, const std::string& current_hash);
    
    // Invalidate cached data for a file
    void invalidate(const std::string& file_path);
    
    // Invalidate transitive dependencies (for headers)
    void invalidate_transitive(const std::string& file_path);
    
    // Get stale files
    std::vector<std::string> get_stale_files();

private:
    knowledge_store::KnowledgeStore& store_;
};

}
