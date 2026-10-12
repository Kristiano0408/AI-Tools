#include "indexer.h"
#include "parser.h"
#include "content_hasher.h"
#include "knowledge_store.h"
#include "../utils/path_utils.h"
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace indexer {

bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id) {
    // Check if file exists in files table
    auto rows = store.query(
        "SELECT content_hash, stale FROM files WHERE repo_id = ? AND path = ?",
        repo_id, file_path
    );

    if (rows.empty()) {
        return true; // Not indexed yet
    }

    // Explicitly marked stale (e.g. transitive header invalidation)
    if (rows[0].size() >= 2 && rows[0][1] == "1") return true;

    // Compare hash
    std::string current_hash = content_hasher::compute_file_hash(file_path);
    return rows[0][0] != current_hash;
}

void index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id) {
    // Get file extension and determine language
    std::string ext = path_utils::get_extension(file_path);
    std::string language = parser::get_language_from_extension(ext);

    if (language == "unknown") {
        return; // Skip unknown file types
    }

    // Parse the file
    parser::FileMetadata metadata = parser::parse_file(file_path, language);

    // Compute content hash
    std::string file_hash = content_hasher::compute_file_hash(file_path);
    if (file_hash.empty()) {
        return; // Unreadable file
    }

    // Get file size
    std::error_code ec;
    int64_t file_size = std::filesystem::file_size(file_path, ec);
    if (ec) file_size = 0;

    // Group all writes for this file into a single transaction
    store.execute("BEGIN");

    // Update the file record in place so file_id stays stable across
    // re-indexes. INSERT OR REPLACE would delete and recreate the row,
    // changing file_id and orphaning summaries (and any other artifact)
    // attached to the previous id.
    store.execute(
        "UPDATE files SET content_hash = ?, language = ?, file_type = ?, size_bytes = ?, "
        "line_count = ?, last_indexed_at = datetime('now'), is_binary = ?, stale = 0 "
        "WHERE repo_id = ? AND path = ?",
        file_hash, language, metadata.file_type, file_size, metadata.line_count,
        metadata.is_binary ? 1 : 0, repo_id, file_path
    );
    store.execute(
        "INSERT INTO files (repo_id, path, content_hash, language, file_type, size_bytes, line_count, last_indexed_at, is_binary) "
        "SELECT ?, ?, ?, ?, ?, ?, ?, datetime('now'), ? "
        "WHERE (SELECT changes()) = 0",
        repo_id, file_path, file_hash, language, metadata.file_type, file_size, metadata.line_count,
        metadata.is_binary ? 1 : 0
    );

    // Resolve the file_id for this file
    auto file_rows = store.query(
        "SELECT file_id FROM files WHERE repo_id = ? AND path = ?",
        repo_id, file_path
    );
    std::string file_id = file_rows.empty() ? "" : file_rows[0][0];

    // Hash-driven revalidation: if the file content matches the hash a
    // previously invalidated summary was stored against (e.g. an edit was
    // reverted), the summary becomes valid again.
    if (!file_id.empty()) {
        store.execute(
            "UPDATE summaries SET is_valid = 1, invalidated_at = NULL "
            "WHERE file_id = ? AND content_hash = ? AND is_valid = 0",
            file_id, file_hash
        );
    }

    // Replace symbols for this file
    if (!file_id.empty()) {
        store.execute("DELETE FROM symbols WHERE file_id = ?", file_id);
        for (const auto& sym : metadata.symbols) {
            store.execute(
                "INSERT INTO symbols (file_id, name, symbol_type, signature, start_line, end_line, scope, is_declaration, is_definition, is_exported, content_hash_at_index) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                file_id, sym.name, sym.symbol_type, sym.signature,
                sym.start_line, sym.end_line, sym.scope,
                sym.is_declaration ? 1 : 0,
                sym.is_definition ? 1 : 0,
                sym.is_exported ? 1 : 0,
                file_hash
            );
        }
    }

    // Replace dependency edges for this file
    store.execute("DELETE FROM dependencies WHERE repo_id = ? AND source_path = ?", repo_id, file_path);
    for (const auto& dep : metadata.dependencies) {
        // Try to resolve the dependency to a real repository file so that
        // reverse lookups and transitive invalidation work.
        std::string resolved = dep;
        if (dep.find('/') != std::string::npos || dep.find('.') != std::string::npos) {
            std::error_code ec;
            std::filesystem::path candidate;
            if (dep.rfind("./", 0) == 0 || dep.rfind("../", 0) == 0) {
                candidate = std::filesystem::path(file_path).parent_path() / dep;
            } else {
                candidate = std::filesystem::path(dep);
                if (!std::filesystem::exists(candidate, ec)) {
                    candidate = std::filesystem::path(file_path).parent_path() / dep;
                }
            }
            std::filesystem::path abs = std::filesystem::weakly_canonical(candidate, ec);
            if (!ec && std::filesystem::exists(abs, ec) && std::filesystem::is_regular_file(abs, ec)) {
                resolved = abs.string();
            }
        }
        store.execute(
            "INSERT INTO dependencies (repo_id, source_path, target_path, dep_type, dep_direction) VALUES (?, ?, ?, ?, ?)",
            repo_id, file_path, resolved, (language == "python") ? "import" : "include", "out"
        );
    }

    // Replace symbol-level dependency edges (call graph) for this file
    store.execute("DELETE FROM dependencies WHERE repo_id = ? AND source_path = ? AND dep_type = 'call'", repo_id, file_path);
    for (const auto& sd : metadata.symbol_deps) {
        store.execute(
            "INSERT INTO dependencies (repo_id, source_path, target_path, source_symbol, target_symbol, dep_type, dep_direction) "
            "VALUES (?, ?, ?, ?, ?, 'call', 'out')",
            repo_id, file_path, file_path, sd.caller, sd.callee
        );
    }

    // Update indexing state
    store.execute(
        "INSERT INTO indexing_state (repo_id, last_incremental_at, status, files_indexed) "
        "VALUES (?, datetime('now'), 'incremental', 1) "
        "ON CONFLICT(repo_id) DO UPDATE SET last_incremental_at = datetime('now'), status = 'incremental', files_indexed = files_indexed + 1",
        repo_id
    );

    store.execute("COMMIT");
}

void index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    // Get repository root
    auto rows = store.query("SELECT root_path FROM repositories WHERE repo_id = ?", repo_id);
    if (rows.empty()) {
        std::cerr << "Repository not found: " << repo_id << std::endl;
        return;
    }

    std::string root_path = rows[0][0];

    // Walk directory and index files
    std::filesystem::path root(root_path);
    std::error_code walk_ec;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, walk_ec);
    if (walk_ec) {
        std::cerr << "Cannot walk repository: " << walk_ec.message() << std::endl;
        return;
    }
    std::vector<std::string> supported_extensions = {".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".cu", ".cuh", ".py"};

    int files_indexed = 0;
    int files_total = 0;

    for (const auto& entry : it) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::string path = entry.path().string();
        std::string ext = path_utils::get_extension(path);
        if (ext.empty()) continue;
        ext = "." + ext;

        files_total++;

        if (std::find(supported_extensions.begin(), supported_extensions.end(), ext) != supported_extensions.end()) {
            // Skip generated files
            if (path.find("/build/") != std::string::npos ||
                path.find("/.git/") != std::string::npos ||
                path.find("/node_modules/") != std::string::npos ||
                path.find("/third_party/") != std::string::npos) {
                continue;
            }

            // Incremental: unchanged files are never reprocessed
            if (!needs_indexing(store, path, repo_id)) {
                continue;
            }

            index_file(store, path, repo_id);
            files_indexed++;
        }
    }

    store.execute(
        "INSERT INTO indexing_state (repo_id, last_full_index_at, status, files_indexed, files_total) "
        "VALUES (?, datetime('now'), 'full', ?, ?) "
        "ON CONFLICT(repo_id) DO UPDATE SET last_full_index_at = datetime('now'), status = 'full', files_indexed = ?, files_total = ?",
        repo_id, files_indexed, files_total, files_indexed, files_total
    );
}

}
