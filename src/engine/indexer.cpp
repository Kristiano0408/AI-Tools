#include "indexer.h"
#include "parser.h"
#include "content_hasher.h"
#include "knowledge_store.h"
#include "../utils/path_utils.h"
#include "../utils/perf_metrics.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace indexer {

namespace {

// Directories that never belong in an index. Applied while walking so large
// generated trees are not traversed any further than the iterator itself.
bool in_skipped_directory(const std::string& path) {
    return path.find("/build/") != std::string::npos ||
           path.find("/.git/") != std::string::npos ||
           path.find("/node_modules/") != std::string::npos ||
           path.find("/third_party/") != std::string::npos;
}

bool indexable_extension(const std::string& path) {
    std::string ext = path_utils::get_extension(path);
    if (ext.empty()) return false;
    ext = "." + ext;
    const auto& supported = supported_source_extensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

std::string canonical_path(const std::string& path) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::weakly_canonical(path, ec);
    if (ec) return path;
    return abs.string();
}

// Live (not tombstoned) records tracked for this repository: canonical path
// -> explicitly marked stale.
//
// The stored spelling is kept as the key: file_id, summaries and dependency
// edges all reference it, so updates must target the exact row. Canonical
// spellings are exposed separately for matching against a filesystem walk.
void tracked_files(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                   std::unordered_map<std::string, bool>& out) {
    out.clear();
    for (const auto& row : store.query(
             "SELECT path, stale FROM files WHERE repo_id = ? AND is_deleted = 0", repo_id)) {
        if (!row.empty()) out[row[0]] = (row.size() >= 2 && row[1] == "1");
    }
}

// Canonical form of every tracked path, used to compare the database against a
// filesystem walk (which always yields canonicalized absolute paths).
std::unordered_set<std::string> tracked_canonical_paths(
        const std::unordered_map<std::string, bool>& tracked) {
    std::unordered_set<std::string> out;
    out.reserve(tracked.size());
    for (const auto& [path, is_stale] : tracked) out.insert(canonical_path(path));
    return out;
}

int tracked_count(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    auto rows = store.query(
        "SELECT COUNT(*) FROM files WHERE repo_id = ? AND is_deleted = 0", repo_id);
    if (rows.empty() || rows[0].empty()) return 0;
    try { return std::stoi(rows[0][0]); } catch (...) { return 0; }
}

// File records that the index still tracks although they no longer exist on
// disk (IDX-01/IDX-02 reconciliation): only meaningful when the walk of the
// repository succeeded and was not truncated by a limit.
std::vector<std::string> disappeared_files(knowledge_store::KnowledgeStore& store,
                                           const std::string& repo_id,
                                           const std::vector<std::string>& files_on_disk) {
    std::unordered_set<std::string> present(files_on_disk.begin(), files_on_disk.end());
    std::unordered_map<std::string, bool> tracked;
    tracked_files(store, repo_id, tracked);

    // Tracked rows are keyed by their stored spelling; the walk yields
    // canonical paths. Compare canonical forms but tombstone using the exact
    // stored value, which is what every derived row references.
    std::vector<std::string> gone;
    for (const auto& [path, stale] : tracked) {
        if (present.find(canonical_path(path)) == present.end()) gone.push_back(path);
    }
    std::sort(gone.begin(), gone.end());
    return gone;
}

// Tombstone the tracked files that no longer exist and drop the knowledge
// derived from them (symbols, summaries, dependency edges), in one
// transaction, limited to this repository.
int reconcile_deleted_files(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                            const std::vector<std::string>& files_on_disk) {
    std::vector<std::string> gone = disappeared_files(store, repo_id, files_on_disk);
    if (gone.empty()) return 0;

    PERF_VALUE("indexer.reconcile_deleted_files.tombstoned", static_cast<int>(gone.size()));
    store.execute("BEGIN");
    for (const std::string& path : gone) {
        store.execute(
            "UPDATE files SET is_deleted = 1, stale = 0, last_indexed_at = datetime('now') "
            "WHERE repo_id = ? AND path = ?",
            repo_id, path);
        store.execute(
            "DELETE FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo_id, path);
        store.execute(
            "UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') "
            "WHERE file_id IN (SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo_id, path);
        store.execute(
            "DELETE FROM dependencies WHERE repo_id = ? AND (source_path = ? OR target_path = ?)",
            repo_id, path, path);
    }
    store.execute("COMMIT");
    return static_cast<int>(gone.size());
}

// The coverage snapshot recorded by the last completed full scan.
struct StateSnapshot {
    bool has_row = false;
    std::string status;
    bool has_snapshot = false;      // a full scan certified a file count
    int covered = 0;                // full_index_covered
    std::string coverage_computed_at;
    std::string last_full_index_at;
    std::string last_error;
};

StateSnapshot read_state(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    StateSnapshot st;
    auto rows = store.query(
        "SELECT status, files_indexed, files_total, full_index_covered, coverage_computed_at, "
        "last_full_index_at, last_error "
        "FROM indexing_state WHERE repo_id = ?",
        repo_id);
    if (rows.empty() || rows[0].size() < 7) return st;

    st.has_row = true;
    st.status = rows[0][0];
    st.has_snapshot = !rows[0][3].empty();
    if (st.has_snapshot) {
        try {
            size_t consumed = 0;
            int n = std::stoi(rows[0][3], &consumed);
            if (consumed == rows[0][3].size()) st.covered = n;
        } catch (...) {
            st.has_snapshot = false;
        }
    }
    st.coverage_computed_at = rows[0][4];
    st.last_full_index_at = rows[0][5];
    st.last_error = rows[0][6];
    return st;
}

// SQLite's datetime('now') renders as "YYYY-MM-DD HH:MM:SS" (UTC). The format
// is fixed-width, so comparing those strings lexicographically compares them
// chronologically: no calendar library is needed.
std::string to_db_time(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) == 0) return "";
    return buf;
}

// Translate a filesystem clock time point into an absolute wall-clock time.
std::time_t wall_time(const std::filesystem::file_time_type& ft) {
    using file_clock = std::filesystem::file_time_type::clock;
    auto offset = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        ft.time_since_epoch() - file_clock::now().time_since_epoch() +
        std::chrono::system_clock::now().time_since_epoch());
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(offset));
}

// True when two spellings of the same file are interchangeable for coverage
// bookkeeping. The walk produces canonicalized paths while records written by
// older code paths may keep the spelling the caller supplied, so equality has
// to accept the canonical form of either side.
//
// An unresolved spelling is reported as unknown (false): the caller treats
// that as not matching, which is the conservative direction for coverage.
bool same_file_spelling(const std::string& a, const std::string& b) {
    if (a == b) return true;
    std::error_code ec;
    std::filesystem::path ca = std::filesystem::weakly_canonical(a, ec);
    if (!ec && ca.string() == b) return true;
    ec.clear();
    std::filesystem::path cb = std::filesystem::weakly_canonical(b, ec);
    if (!ec && cb.string() == a) return true;
    return (!ec && ca == cb);
}

// Live records whose copy on disk is newer than the moment they were indexed:
// knowledge derived from them can no longer be trusted. SQLite's datetime
// resolution is one second, so a modification in the same second as its
// re-index counts as fresh.
//
// A record whose copy cannot be stat'ed is reported as touched, not skipped:
// "we cannot confirm this is still the indexed content" must never be read
// as "it is".
int touched_records(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::vector<std::string>& files_on_disk) {
    PERF_SCOPE("indexer.touched_records");
    int touched = 0;

    // Resolve each disk spelling once, then reuse it for every record, so a
    // large repository costs one stat per file instead of one per pair.
    std::unordered_map<std::string, std::pair<std::time_t, bool>> resolved;
    resolved.reserve(files_on_disk.size());
    auto disk_time = [&](const std::string& path) -> std::pair<std::time_t, bool> {
        auto it = resolved.find(path);
        if (it != resolved.end()) return it->second;
        std::error_code ec;
        std::time_t t = wall_time(std::filesystem::last_write_time(path, ec));
        auto result = std::make_pair(ec ? 0 : t, static_cast<bool>(ec));
        resolved.emplace(path, result);
        return result;
    };

    for (const auto& row : store.query(
             "SELECT path, last_indexed_at FROM files WHERE repo_id = ? AND is_deleted = 0 AND stale = 0",
             repo_id)) {
        if (row.empty()) continue;

        // Same file, either spelling (exact first, then canonical fallback).
        std::string disk_path;
        for (const std::string& candidate : files_on_disk) {
            if (same_file_spelling(candidate, row[0])) { disk_path = candidate; break; }
        }
        if (disk_path.empty()) { touched++; continue; }

        auto [mtime, failed] = disk_time(disk_path);
        if (failed) { touched++; continue; }        // unreadable: not provably fresh
        if (row[1].empty()) { touched++; continue; } // legacy record without index timestamp
        if (to_db_time(mtime) <= row[1]) continue;   // indexed at or after the last write
        touched++;
    }
    return touched;
}

} // namespace

const std::vector<std::string>& supported_source_extensions() {
    static const std::vector<std::string> supported = {
        ".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".cu", ".cuh", ".py"};
    return supported;
}

std::string coverage_name(IndexCoverage coverage) {
    switch (coverage) {
        case IndexCoverage::not_indexed: return "not_indexed";
        case IndexCoverage::partial: return "index_partial";
        case IndexCoverage::full_stale: return "index_stale";
        case IndexCoverage::full_fresh: return "index_full";
    }
    return "not_indexed";
}

RepoScan scan_indexable_files(const std::string& root_path, int limit) {
    PERF_SCOPE("indexer.scan_indexable_files");
    RepoScan scan;
    std::filesystem::path root(root_path);
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return scan; // ok = false: the repository could not be walked

    for (const auto& entry : it) {
        if (!entry.is_regular_file()) continue;
        std::string path = entry.path().string();
        if (in_skipped_directory(path)) continue;
        if (!indexable_extension(path)) continue;
        scan.files.push_back(canonical_path(path));
        if (limit > 0 && static_cast<int>(scan.files.size()) > limit) {
            scan.over_limit = true;
            break;
        }
    }
    scan.ok = true;
    PERF_VALUE("indexer.scan_indexable_files.found", static_cast<int>(scan.files.size()));
    return scan;
}

// Honest measurement of what the persisted index really covers. Indexed rows
// in the database are never taken as proof of completeness: an index produced
// by on-demand single-file indexing is reported as partial, and a 'full' claim
// is trusted only while the tracked records still match the snapshot a
// completed scan certified and nothing is marked stale (IDX-01).
Coverage repo_coverage(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                       int verify_limit) {
    PERF_SCOPE("indexer.repo_coverage");
    Coverage cov;
    StateSnapshot st = read_state(store, repo_id);

    // Live (not tombstoned) records for this repository: stored spelling ->
    // explicitly marked stale. The stored spelling is the key because file_id,
    // summaries and dependency edges all reference it, so updates must target
    // the exact row; canonical spellings are derived separately when comparing
    // against a filesystem walk.
    std::unordered_map<std::string, bool> tracked;
    tracked_files(store, repo_id, tracked);
    cov.files_indexed = static_cast<int>(tracked.size());
    for (const auto& [path, is_stale] : tracked) {
        if (is_stale) cov.stale_records++;
    }
    cov.full_index_covered = st.has_snapshot ? st.covered : -1;
    cov.last_error = st.last_error;
    cov.last_full_index_at = st.last_full_index_at;

    const bool claims_full = st.has_snapshot && st.status == "full";

    // Optional on-disk cross-check, measured once. The files the index already
    // tracks count against verify_limit, so the walk inspects at most
    // verify_limit files beyond what is already known: a huge repository can
    // never turn a cheap status question into an unbounded scan. A negative
    // verify_limit means "no cap at all".
    //
    // The walk is skipped only when the caller asked for no cross-check and
    // the stored claim already looks self-consistent.
    bool walked = false;
    std::vector<std::string> files_on_disk;
    if (verify_limit != 0 || !claims_full || cov.stale_records > 0 ||
        cov.files_indexed != st.covered) {
        auto rows = store.query("SELECT root_path FROM repositories WHERE repo_id = ?", repo_id);
        if (rows.empty()) {
            cov.walk_failed = true; // the repository root is not resolvable
        } else {
            int walk_limit = (verify_limit < 0) ? -1
                                                : verify_limit + static_cast<int>(tracked.size());
            RepoScan disk = scan_indexable_files(rows[0][0], walk_limit);
            cov.walk_failed = !disk.ok;
            cov.walk_truncated = disk.over_limit;
            if (disk.ok) {
                walked = true;
                files_on_disk = std::move(disk.files);
                cov.files_total = static_cast<int>(files_on_disk.size());
            }
        }
    }

    // No completed full scan ever certified this index: whatever is in the
    // database came from on-demand single-file indexing (or from a scan that
    // did not complete). That is a partial index whatever the stored status
    // text says, and the presence of rows must not be read as completeness.
    if (!claims_full) {
        cov.coverage = (cov.files_indexed > 0 || st.has_row || !st.last_error.empty())
                           ? IndexCoverage::partial
                           : IndexCoverage::not_indexed;
        if (cov.files_total == 0) cov.files_total = cov.files_indexed;
        return cov;
    }

    // status == "full" with a coverage snapshot: the claim is re-validated
    // before anyone is allowed to read the index as complete.
    auto stale_claim = [&]() {
        cov.coverage = IndexCoverage::full_stale;
        if (cov.files_total == 0) cov.files_total = cov.files_indexed;
    };

    // When a cross-check was requested, its result is the ground truth for
    // "how many indexable files does this repository have". A claim that lost
    // tracked records or has any stale record cannot be complete regardless of
    // what the walk found.
    if (walked && cov.files_indexed != st.covered) {
        cov.files_total = std::max(cov.files_total, static_cast<int>(tracked.size()));
    }

    if (cov.walk_failed || cov.stale_records > 0 || cov.files_indexed != st.covered) {
        stale_claim();
        return cov;
    }

    if (!walked) {
        // No cross-check requested: report the coverage exactly as the last
        // completed scan certified it, valid as of that scan. A caller that
        // intends to answer a repository-wide search asks for a walk (nonzero
        // verify_limit) so changes made outside the server cannot go unseen.
        cov.coverage = IndexCoverage::full_fresh;
        cov.files_total = st.covered;
        return cov;
    }

    // Covered only if the walk completed without being truncated and its file
    // set is the very set the index tracks. Counting alone is not enough: with
    // a truncated walk (or an unreadable subtree) the disk set is a subset of
    // both the repository and the index, so containment has to be explicit.
    std::unordered_set<std::string> tracked_paths = tracked_canonical_paths(tracked);
    if (cov.walk_truncated || files_on_disk.size() != tracked_paths.size()) {
        stale_claim();
        return cov;
    }
    for (const std::string& path : files_on_disk) {
        if (tracked_paths.find(path) == tracked_paths.end()) {
            stale_claim();
            return cov;
        }
    }

    // Every tracked record's copy on disk must be at most as new as the moment
    // it was indexed: a file edited outside the server after the scan has a
    // newer mtime than its last_indexed_at.
    if (touched_records(store, repo_id, files_on_disk) > 0) {
        stale_claim();
        return cov;
    }

    cov.coverage = IndexCoverage::full_fresh;
    return cov;
}

bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id) {
    PERF_SCOPE("indexer.needs_indexing");
    // Check if file exists in files table
    auto rows = store.query(
        "SELECT content_hash, stale FROM files WHERE repo_id = ? AND path = ?",
        repo_id, file_path
    );

    if (rows.empty()) {
        PERF_COUNTER("indexer.needs_indexing.not_indexed");
        return true; // Not indexed yet
    }

    // Explicitly marked stale (e.g. transitive header invalidation)
    if (rows[0].size() >= 2 && rows[0][1] == "1") {
        PERF_COUNTER("indexer.needs_indexing.marked_stale");
        return true;
    }

    // Compare hash
    std::string current_hash = content_hasher::compute_file_hash(file_path);
    bool changed = rows[0][0] != current_hash;
    PERF_COUNTER(changed ? "indexer.needs_indexing.hash_changed"
                         : "indexer.needs_indexing.hash_unchanged");
    return changed;
}

void index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id) {
    PERF_SCOPE("indexer.index_file");
    // Get file extension and determine language
    std::string ext = path_utils::get_extension(file_path);
    std::string language = parser::get_language_from_extension(ext);

    if (language == "unknown") {
        PERF_COUNTER("indexer.index_file.skipped_unknown");
        return; // Skip unknown file types
    }

    // Parse the file
    parser::FileMetadata metadata = parser::parse_file(file_path, language);

    // Compute content hash
    std::string file_hash = content_hasher::compute_file_hash(file_path);
    if (file_hash.empty()) {
        PERF_COUNTER("indexer.index_file.unreadable");
        return; // Unreadable file
    }

    PERF_VALUE("indexer.index_file.symbols", metadata.symbols.size());
    PERF_VALUE("indexer.index_file.dependencies", metadata.dependencies.size());
    PERF_VALUE("indexer.index_file.symbol_deps", metadata.symbol_deps.size());

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

    // Update indexing state. Indexing one file is a PARTIAL operation: it
    // must not present the repository as fully indexed (IDX-01). The status
    // stays 'partial' until a completed index_repository() scan records a
    // coverage snapshot (full_index_covered); once such a snapshot exists the
    // scan status is preserved and only the tracked-record count is refreshed.
    static const char* kTrackedCount =
        "(SELECT COUNT(*) FROM files WHERE repo_id = ? AND is_deleted = 0)";
    store.execute(
        std::string("INSERT INTO indexing_state (repo_id, last_incremental_at, status, files_indexed) ") +
        "VALUES (?, datetime('now'), 'partial', " + kTrackedCount + ") " +
        "ON CONFLICT(repo_id) DO UPDATE SET "
        "last_incremental_at = datetime('now'), "
        "status = CASE WHEN indexing_state.full_index_covered IS NULL THEN 'partial' ELSE indexing_state.status END, "
        "files_indexed = " + kTrackedCount,
        repo_id, repo_id, repo_id
    );

    store.execute("COMMIT");
}

FullScanResult index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    PERF_SCOPE("indexer.index_repository");
    FullScanResult scan_result;

    auto fail = [&](const std::string& reason) {
        // An incomplete scan must never leave the index claiming full
        // coverage: record the failure and drop the coverage snapshot.
        store.execute(
            "INSERT INTO indexing_state (repo_id, status, last_error) VALUES (?, 'failed', ?) "
            "ON CONFLICT(repo_id) DO UPDATE SET status = 'failed', last_error = ?, "
            "full_index_covered = NULL, coverage_computed_at = NULL",
            repo_id, reason, reason);
        std::cerr << "ai-tools: repository scan failed (" << repo_id << "): " << reason << "\n";
        return scan_result;
    };

    // Get repository root
    auto rows = store.query("SELECT root_path FROM repositories WHERE repo_id = ?", repo_id);
    if (rows.empty()) return fail("repository not found");

    const std::string& root_path = rows[0][0];

    // Discover every indexable file under the repository. The walk is the
    // discovery step of a full scan: unbounded, because the coverage snapshot
    // written below has to describe the whole repository.
    RepoScan disk = scan_indexable_files(root_path, -1);
    if (!disk.ok) return fail("cannot walk repository: " + root_path);

    scan_result.files_total = static_cast<int>(disk.files.size());

    // Incremental: unchanged files are never reprocessed
    for (const std::string& path : disk.files) {
        if (!needs_indexing(store, path, repo_id)) {
            PERF_COUNTER("indexer.index_repository.unchanged_skipped");
            continue;
        }
        index_file(store, path, repo_id);
        scan_result.files_refreshed++;
    }

    // Reconcile: records the index still tracks for files that no longer
    // exist on disk are tombstoned, together with their symbols, summaries
    // and dependency edges (IDX-02).
    reconcile_deleted_files(store, repo_id, disk.files);

    PERF_VALUE("indexer.index_repository.walked", scan_result.files_total);
    PERF_VALUE("indexer.index_repository.indexed", scan_result.files_refreshed);

    // Record the coverage snapshot this scan just certified. From now on a
    // repo-level query may treat the index as complete, and only for the file
    // count measured here (repo_coverage validates it again before use).
    int covered = tracked_count(store, repo_id);
    store.execute(
        "INSERT INTO indexing_state (repo_id, last_full_index_at, status, files_indexed, files_total, "
        "full_index_covered, coverage_computed_at, last_error) "
        "VALUES (?, datetime('now'), 'full', ?, ?, ?, datetime('now'), NULL) "
        "ON CONFLICT(repo_id) DO UPDATE SET last_full_index_at = datetime('now'), status = 'full', "
        "files_indexed = ?, files_total = ?, full_index_covered = ?, coverage_computed_at = datetime('now'), "
        "last_error = NULL",
        repo_id, covered, scan_result.files_total, covered, covered, scan_result.files_total, covered);

    scan_result.ok = true;
    return scan_result;
}

}
