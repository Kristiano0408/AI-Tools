#include "cache_manager.h"
#include "content_hasher.h"
#include "../utils/perf_metrics.h"
#include <deque>
#include <unordered_set>

namespace cache_manager {

namespace {

// Safety cap: a pathological or cyclic include graph must never make one
// invalidation spin indefinitely or silently mark thousands of files stale.
// When it is reached the result reports truncated = true.
constexpr size_t kMaxDependents = 1000;

} // namespace

// Read-only BFS over the reverse dependency graph (see cache_manager.h): from
// file_path, collect every file that directly OR transitively depends on it
// through an include/import edge (dep_type != 'call', so C/C++/CUDA includes
// and Python imports all propagate), with a visited set against cycles.
// It performs no writes: this is the exact set invalidate() then marks stale,
// and it must be computed while the edges are still in the database (DEP-01:
// deleting the edges first is what made the old transitive invalidation a
// silent no-op when handle_edit called invalidate() before it). indexer::
// index_snapshot() uses the same traversal for the propagation a re-indexed
// file must trigger (DEP-03), so both entry points share one implementation.
std::vector<std::string> collect_dependents(knowledge_store::KnowledgeStore& store,
                                            const std::string& file_path,
                                            const std::string& repo_id,
                                            bool& truncated) {
    std::vector<std::string> found;
    std::unordered_set<std::string> visited;
    std::deque<std::string> queue;

    visited.insert(file_path);
    queue.push_back(file_path);

    while (!queue.empty()) {
        std::string current = queue.front();
        queue.pop_front();

        auto rows = repo_id.empty()
            ? store.query("SELECT DISTINCT source_path FROM dependencies WHERE target_path = ? AND dep_type != 'call'", current)
            : store.query("SELECT DISTINCT source_path FROM dependencies WHERE repo_id = ? AND target_path = ? AND dep_type != 'call'", repo_id, current);

        for (const auto& row : rows) {
            if (row.empty() || row[0].empty() || row[0] == file_path) continue;
            if (!visited.insert(row[0]).second) continue;
            if (found.size() >= kMaxDependents) {
                truncated = true;
                queue.clear();
                break;
            }
            found.push_back(row[0]);
            queue.push_back(row[0]);
        }
    }
    return found;
}

bool reap_dependent(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::string& path) {
    // Invalidate symbols
    if (repo_id.empty()) {
        bool ok = store.execute("DELETE FROM symbols WHERE file_id IN (SELECT file_id FROM files WHERE path = ?)", path) == SQLITE_OK;
        // Invalidate summaries
        ok = store.execute("UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') WHERE file_id IN (SELECT file_id FROM files WHERE path = ?)", path) == SQLITE_OK && ok;
        // Mark stale: needs_indexing() and a full scan will re-index it
        ok = store.execute("UPDATE files SET stale = 1 WHERE path = ?", path) == SQLITE_OK && ok;
        return ok;
    }
    bool ok = store.execute("DELETE FROM symbols WHERE file_id IN (SELECT file_id FROM files WHERE repo_id = ? AND path = ?)", repo_id, path) == SQLITE_OK;
    ok = store.execute("UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') WHERE file_id IN (SELECT file_id FROM files WHERE repo_id = ? AND path = ?)", repo_id, path) == SQLITE_OK && ok;
    ok = store.execute("UPDATE files SET stale = 1 WHERE repo_id = ? AND path = ?", repo_id, path) == SQLITE_OK && ok;
    return ok;
}

CacheManager::CacheManager(knowledge_store::KnowledgeStore& store) : store_(store) {}

bool CacheManager::is_valid(const std::string& file_path, const std::string& current_hash) {
    auto results = store_.query("SELECT content_hash FROM files WHERE path = ?", file_path);
    if (results.empty() || results[0].empty()) return false;
    return results[0][0] == current_hash;
}

InvalidationResult CacheManager::invalidate(const std::string& file_path, const std::string& repo_id) {
    PERF_SCOPE("cache_manager.invalidate");
    InvalidationResult out;

    // DEP-01 fix: COLLECT FIRST, reading only. The edges that identify the
    // dependents are still present at this point; the old code deleted them
    // (source_path = ? OR target_path = ?) before anyone looked for them.
    out.dependents = collect_dependents(store_, file_path, repo_id, out.truncated);

    reap_dependent(store_, repo_id, file_path);

    // Transitive dependents: lazy invalidation symbols and summaries go
    // away and the records are marked stale, but their own edges are kept, so
    // later traversals still see them until their re-index rebuilds the edges.
    for (const auto& dependent : out.dependents) {
        reap_dependent(store_, repo_id, dependent);
    }

    PERF_VALUE("cache_manager.invalidate.dependents", out.dependents.size());
    if (out.truncated) {
        PERF_COUNTER("cache_manager.invalidate.dependents_truncated");
    }
    return out;
}

void CacheManager::invalidate_transitive(const std::string& file_path) {
    PERF_SCOPE("cache_manager.invalidate_transitive");
    // Deprecated compatibility wrapper (DEP-01): invalidate() now collects the
    // transitive dependent set itself, before deleting anything, so keeping a
    // separate call ordering around this function no longer matters.
    invalidate(file_path);
}

std::vector<std::string> CacheManager::get_stale_files() {
    PERF_SCOPE("cache_manager.get_stale_files");
    // A file is stale when explicitly marked stale or when its stored content
    // hash no longer matches the file on disk.
    std::vector<std::string> stale;
    auto results = store_.query("SELECT path, content_hash, stale FROM files WHERE is_deleted = 0");

    for (const auto& row : results) {
        if (row.size() >= 3) {
            if (row[2] == "1") {
                stale.push_back(row[0]);
                continue;
            }
            std::string current = content_hasher::compute_file_hash(row[0]);
            if (current.empty() || current != row[1]) {
                stale.push_back(row[0]);
            }
        }
    }

    return stale;
}

}
