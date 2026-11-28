#include "engine/summary_store.h"
#include "engine/content_hasher.h"
#include <iostream>

namespace summary_store {

// Store (or replace) a file's semantic summary.
//
// DB-01: the invalidation of the previous summary and the insertion of the new
// one are one transaction. They used to be two independent autocommit writes:
// if the INSERT failed, the file ended up with its old summary invalidated and
// no replacement — the store would then report 'no valid summary' for a file
// that does have knowledge, and the agent would regenerate (or lose) it for a
// failure that was really a write error. Returns false when nothing was
// applied: the caller must not report a stored summary on false.
bool store_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path, const std::string& summary) {
    auto file_rows = store.query(
        "SELECT file_id, content_hash FROM files WHERE repo_id = ? AND path = ?",
        repo_id, file_path
    );
    if (file_rows.empty()) {
        if (store.last_query_failed()) {
            std::cerr << "ai-tools: cannot look up file for summary (" << file_path
                      << "): " << store.get_error() << "\n";
        }
        return false;
    }
    const std::string& file_id = file_rows[0][0];
    const std::string& content_hash = file_rows[0][1];

    knowledge_store::KnowledgeStore::Transaction tx(store, "store_file_summary");
    if (tx.begin_failed()) return false;

    // Mark any previous summary for this file as invalidated
    tx.exec(
        "UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') WHERE file_id = ? AND is_valid = 1",
        file_id
    );

    tx.exec(
        "INSERT INTO summaries (file_id, content_hash, summary_text, created_at, is_valid) "
        "VALUES (?, ?, ?, datetime('now'), 1)",
        file_id, content_hash, summary
    );

    if (!tx.commit()) {
        std::cerr << "ai-tools: summary write rolled back (" << file_path
                  << "): " << tx.error() << "\n";
        return false;
    }
    return true;
}

std::string get_file_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& file_path) {
    auto rows = store.query(
        "SELECT s.summary_text, s.content_hash FROM summaries s "
        "JOIN files f ON f.file_id = s.file_id "
        "WHERE f.repo_id = ? AND f.path = ? AND s.is_valid = 1 "
        "ORDER BY s.summary_id DESC LIMIT 1",
        repo_id, file_path
    );
    if (rows.empty()) {
        return "";
    }
    // A summary is only valid while it matches the current content hash of the file
    std::string current_hash = content_hasher::compute_file_hash(file_path);
    if (current_hash.empty() || current_hash != rows[0][1]) {
        return "";
    }
    return rows[0][0];
}

bool store_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id, const std::string& summary) {
    // Repository-level summaries are stored with file_id = 0 as a sentinel
    // (DB-01: checked — a failed write must not read as a stored summary).
    if (store.execute(
            "INSERT INTO summaries (file_id, content_hash, summary_text, created_at, is_valid) "
            "VALUES (0, ?, ?, datetime('now'), 1)",
            repo_id, summary) != SQLITE_OK) {
        std::cerr << "ai-tools: repository summary write failed (" << repo_id
                  << "): " << store.get_error() << "\n";
        return false;
    }
    return true;
}

std::string get_repo_summary(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    auto rows = store.query(
        "SELECT summary_text FROM summaries WHERE file_id = 0 AND content_hash = ? AND is_valid = 1 "
        "ORDER BY summary_id DESC LIMIT 1",
        repo_id
    );
    if (rows.empty()) {
        return "";
    }
    return rows[0][0];
}

}
