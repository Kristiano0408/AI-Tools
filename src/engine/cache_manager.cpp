#include "cache_manager.h"
#include "content_hasher.h"
#include "../utils/perf_metrics.h"
#include <iostream>

namespace cache_manager {

CacheManager::CacheManager(knowledge_store::KnowledgeStore& store) : store_(store) {}

bool CacheManager::is_valid(const std::string& file_path, const std::string& current_hash) {
    auto results = store_.query("SELECT content_hash FROM files WHERE path = ?", file_path);
    if (results.empty() || results[0].empty()) return false;
    return results[0][0] == current_hash;
}

void CacheManager::invalidate(const std::string& file_path) {
    PERF_SCOPE("cache_manager.invalidate");
    // Invalidate symbols
    store_.execute("DELETE FROM symbols WHERE file_id IN (SELECT file_id FROM files WHERE path = ?)", file_path);

    // Invalidate dependencies
    store_.execute("DELETE FROM dependencies WHERE source_path = ? OR target_path = ?", file_path, file_path);

    // Invalidate summaries
    store_.execute("UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') WHERE file_id IN (SELECT file_id FROM files WHERE path = ?)", file_path);

    // Mark the file itself stale
    store_.execute("UPDATE files SET stale = 1 WHERE path = ?", file_path);
}

void CacheManager::invalidate_transitive(const std::string& file_path) {
    PERF_SCOPE("cache_manager.invalidate_transitive");
    // Find files that include this file and mark their cached artifacts stale.
    // Lazy: we invalidate symbols/summaries but keep the dependency edges so we
    // still know who depends on the changed header.
    auto dependents = store_.query("SELECT DISTINCT source_path FROM dependencies WHERE target_path = ? AND dep_type = 'include'", file_path);

    for (const auto& row : dependents) {
        if (!row.empty() && row[0] != file_path) {
            PERF_COUNTER("cache_manager.invalidate_transitive.dependents");
            store_.execute("DELETE FROM symbols WHERE file_id IN (SELECT file_id FROM files WHERE path = ?)", row[0]);
            store_.execute("UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') WHERE file_id IN (SELECT file_id FROM files WHERE path = ?)", row[0]);
            store_.execute("UPDATE files SET stale = 1 WHERE path = ?", row[0]);
        }
    }
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
