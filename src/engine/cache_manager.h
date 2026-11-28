#pragma once

#include "knowledge_store.h"
#include <string>
#include <vector>

namespace cache_manager {

// Result of an invalidation pass (DEP-01/DEP-02): the full transitive
// dependent set was collected before any derived data was deleted, and is
// returned so callers can report what needs re-indexing.
//
// success/error (DB-01): the reaps are one SQLite transaction, so either every
// listed dependent is invalidated or none is. When the transaction failed,
// success == false, dependents is empty (nothing was applied), and error holds
// the cause — a caller must never read 'invalidated N files' from a failed run.
struct InvalidationResult {
    bool success = true;                   // false when the transaction failed: nothing was applied
    std::vector<std::string> dependents;   // transitively invalidated files, nearest first
    bool truncated = false;                // traversal stopped at the safety cap
    std::string error;                     // storage failure cause, when success == false
};

// Read-only BFS over the reverse dependency graph: from file_path, collect
// every file that directly OR transitively depends on it through an
// include/import edge (dep_type != 'call', so C/C++/CUDA includes and Python
// imports all propagate), with a visited set against cycles and a safety cap.
// Shared by CacheManager::invalidate() and indexer::index_snapshot() (DEP-03):
// the propagation a changed file must trigger is computed by ONE traversal
// implementation, not by a copy in each module. It performs no writes: the
// edges that identify the dependents must still be in the database when this
// runs (DEP-01: collecting after deleting the edges is what made the old
// transitive invalidation a silent no-op).
std::vector<std::string> collect_dependents(knowledge_store::KnowledgeStore& store,
                                            const std::string& file_path,
                                            const std::string& repo_id,
                                            bool& truncated);

// Drop one file's derived knowledge (symbols deleted, summaries invalidated,
// record marked stale) WITHOUT touching its own dependency edges: the edges
// stay so later traversals still see the file until its re-index rebuilds
// them. Shared `reap` of the invalidation flow.
//
// The write-taking overload runs through the caller's open Transaction: every
// statement failure poisons the scope, so the entire invalidation batch
// (this file plus every dependent reaped in the same transaction) cancels at
// commit instead of half-applying (DB-01). The store-taking overload is the
// standalone form and wraps the writes in their own verified transaction.
void reap_dependent(knowledge_store::KnowledgeStore::Transaction& tx,
                    const std::string& repo_id, const std::string& path);
void reap_dependent(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::string& path);

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
