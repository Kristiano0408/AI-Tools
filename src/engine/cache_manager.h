#pragma once

#include "knowledge_store.h"
#include <string>
#include <vector>

namespace cache_manager {

// Result of an invalidation pass (DEP-01/DEP-02): the full transitive
// dependent set was collected before any derived data was deleted, and is
// returned so callers can report what needs re-indexing.
struct InvalidationResult {
    std::vector<std::string> dependents; // transitively invalidated files, nearest first
    bool truncated = false;              // traversal stopped at the safety cap
};

class CacheManager {
public:
    CacheManager(knowledge_store::KnowledgeStore& store);
    
    // Check if a file's cached data is valid
    bool is_valid(const std::string& file_path, const std::string& current_hash);
    
    // Invalidate the edited file AND, before any dependency edge is dropped,
    // collect its transitive dependents (C/C++/CUDA header includes and
    // Python module imports) and mark their derived knowledge stale. The
    // correctness is a property of this API, not of a call order handlers
    // must keep (DEP-01), and the traversal is a visited-guarded BFS over the
    // reverse graph, so changes propagate through arbitrarily long include
    // chains (DEP-02). An empty repo_id keeps the legacy path-only scope. The
    // edited file itself is marked stale so needs_indexing()/a full scan will
    // re-index it; the dependents are reported to the caller.
    InvalidationResult invalidate(const std::string& file_path, const std::string& repo_id = "");

    // Deprecated: compatibility wrapper. invalidate() now performs the
    // transitive collection itself, so no call ordering around it matters
    // anymore.
    void invalidate_transitive(const std::string& file_path);
    
    // Get stale files
    std::vector<std::string> get_stale_files();

private:
    knowledge_store::KnowledgeStore& store_;
};

}
