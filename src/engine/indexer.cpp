#include "indexer.h"
#include "cache_manager.h"
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
#include <deque>
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

// Reconciled records kept for audit/debug: the files an earlier scan indexed
// and a later scan found gone from disk (IDX-02 tombstone count).
int tombstone_count(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    auto rows = store.query(
        "SELECT COUNT(*) FROM files WHERE repo_id = ? AND is_deleted = 1", repo_id);
    if (rows.empty()) {
        // DB-01: a failed read is not 'no tombstones'. The audit count is a
        // diagnostic (it feeds coverage_json). If it could not be read, say so:
        // an empty count from a store read that failed would present an
        // unknown as a measured zero.
        if (store.last_query_failed()) {
            std::cerr << "ai-tools: cannot count tombstoned records (" << repo_id
                      << "): " << store.get_error() << "\n";
        }
        return 0;
    }
    if (rows[0].empty()) return 0;
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

// Files still on disk that included one of the deleted files. Their record is
// marked stale, while the edges still exist to find them (IDX-02): without
// this, the tombstone swallows the only trace of who depended on the deleted
// file, and those dependents would keep symbols and call-graph edges derived
// from content that no longer exists. Runs inside the caller's transaction, so
// a failing UPDATE poisons the whole reconciliation batch (DB-01); the count is
// provisional because a poisoned scope never commits, so nothing is applied.
int mark_dependents_of_deleted(knowledge_store::KnowledgeStore& store,
                               knowledge_store::KnowledgeStore::Transaction& tx,
                               const std::string& repo_id,
                               const std::vector<std::string>& gone) {
    std::unordered_set<std::string> marked;
    int marked_count = 0;
    for (const std::string& path : gone) {
        auto dependents = store.query(
            "SELECT DISTINCT source_path FROM dependencies WHERE repo_id = ? AND target_path = ? AND dep_type != 'call'",
            repo_id, path);
        if (dependents.empty() && store.last_query_failed()) {
            // An empty result is ambiguous: nobody depended on the file, or the
            // read failed. For reconciliation the two are not interchangeable —
            // tombstoning the file while its dependents went unread would leave
            // them holding knowledge derived from content that no longer exists
            // with no trace left to find them (the edges are deleted below).
            // Signal the failure; the caller cancels the whole batch.
            return -1;
        }
        for (const auto& dep : dependents) {
            if (dep.empty() || dep[0] == path) continue;
            if (!marked.insert(dep[0]).second) continue;
            tx.exec("UPDATE files SET stale = 1 WHERE repo_id = ? AND path = ?", repo_id, dep[0]);
            marked_count++;
        }
    }
    PERF_VALUE("indexer.mark_dependents_of_deleted.stale", marked_count);
    return marked_count;
}

// Outcome of the deleted-file reconciliation step.
struct ReconcileOutcome {
    bool ok = false;      // the whole batch committed
    int dependents_marked = 0; // live files invalidated by a deleted include/import
    int tombstoned = 0;   // records reconciled as deleted
    std::string error;    // failure cause, when ok == false
};

// Tombstone the tracked files that no longer exist and drop the knowledge
// derived from them (symbols, summaries, dependency edges), limited to this
// repository (IDX-02) — together with the stale-marking of the live files that
// included them.
//
// One verified RAII transaction for the whole step (DB-01): the dependent
// marking must happen while the edges exist, and the tombstone writes are what
// removes those edges, so the two halves are one unit. Previously the marking
// was a per-row autocommit batch and only the tombstone half was transacted:
// a failure left some dependents marked stale and others untouched, or some
// files tombstoned while others still claimed live knowledge. Now either the
// reconciliation happened completely or not at all; on failure nothing is
// reconciled, the records stay tracked, and the scan reports the cause instead
// of certifying coverage (repo_coverage then reports stale coverage rather than
// a false 'full' claim).
ReconcileOutcome reconcile_deleted_files(knowledge_store::KnowledgeStore& store,
                                         const std::string& repo_id,
                                         const std::vector<std::string>& gone) {
    ReconcileOutcome out;
    if (gone.empty()) {
        out.ok = true;
        return out;
    }

    knowledge_store::KnowledgeStore::Transaction tx(store, "reconcile_deleted_files");
    if (tx.begin_failed()) {
        out.error = tx.error();
        return out;
    }

    // Dependents first, while the edges recorded by the previous scan are
    // still there to identify them: without this step the tombstone would
    // swallow the only trace of who depended on a deleted file.
    out.dependents_marked = mark_dependents_of_deleted(store, tx, repo_id, gone);
    if (out.dependents_marked < 0 || tx.poisoned()) {
        // Either a write failed (poisoned) or a dependent set could not be
        // read: in both cases the batch cannot be trusted, so it is cancelled
        // — no tombstone, no stale marks, no dropped edges.
        out.error = tx.poisoned() ? tx.error() : store.get_error();
        if (out.error.empty()) out.error = "deleted-file dependent lookup failed";
        tx.rollback();
        return out;
    }

    for (const std::string& path : gone) {
        tx.exec(
            "UPDATE files SET is_deleted = 1, stale = 0, last_indexed_at = datetime('now') "
            "WHERE repo_id = ? AND path = ?",
            repo_id, path);
        tx.exec(
            "DELETE FROM symbols WHERE file_id IN "
            "(SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo_id, path);
        tx.exec(
            "UPDATE summaries SET is_valid = 0, invalidated_at = datetime('now') "
            "WHERE file_id IN (SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo_id, path);
        tx.exec(
            "DELETE FROM dependencies WHERE repo_id = ? AND (source_path = ? OR target_path = ?)",
            repo_id, path, path);
    }

    if (!tx.commit()) {
        out.error = tx.error();
        std::cerr << "ai-tools: deleted-file reconciliation rolled back (" << repo_id
                  << "): " << out.error << "\n";
        return out;
    }

    out.ok = true;
    out.tombstoned = static_cast<int>(gone.size());
    PERF_VALUE("indexer.reconcile_deleted_files.tombstoned", out.tombstoned);
    return out;
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

// Records whose derived knowledge no longer provably matches the bytes on
// disk (IDX-04). The question this answers is 'does the knowledge we stored
// still describe the current content?', not 'was the file written after the
// last scan?' — the second is a question an incremental scan, which skips
// unchanged files on purpose, can never answer.
//
// Two-step verdict, cheap path first:
//   1. mtime <= content_verified_at -> fresh without touching the content.
//      content_verified_at is the instant a scan last proved that
//      hash(disk bytes) == content_hash, so a file quieter than that moment
//      is provably the content the index derived its knowledge from. This is
//      the normal case right after a scan and costs zero reads.
//   2. Otherwise the timestamp alone proves nothing: a file can be newer than
//      its last verification without its content having changed (a touched
//      file, a re-download, a checkout rewriting identical bytes). Compare the
//      hash on disk with the stored content_hash. Equal -> the derived
//      knowledge still matches, not a touched record. Divergent -> genuinely
//      touched, and its path is reported to the caller. A verification older
//      than the disk copy can never certify freshness, which is why this step
//      always re-reads instead of trusting a stale stamp.
//
// Cost guard (FS-01 tension): hashing every suspect record turns a coverage
// check into a repository-wide re-hash, so at most kHashVerifyLimit suspects
// are hashed and suspects beyond the budget stay counted as touched. The
// genuinely divergent ones are cheap to spot — needs_indexing() re-reads them
// on the very next scan.
//
// SQLite's datetime resolution is one second, so a modification in the same
// second as its verification counts as fresh.
//
// A record whose copy cannot be stat'ed or read is reported as touched, not
// skipped: "we cannot confirm this is still the indexed content" must never
// be read as "it is".
constexpr int kTouchedSampleLimit = 20; // touched paths reported back as a diagnostic hint
constexpr int kHashVerifyLimit = 2000;  // max content re-reads per coverage check

int touched_records(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::vector<std::string>& files_on_disk,
                    std::vector<std::string>& touched_paths) {
    PERF_SCOPE("indexer.touched_records");
    int touched = 0;
    int hashed = 0;

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

    auto note_touched = [&](const std::string& path) {
        touched++;
        if (static_cast<int>(touched_paths.size()) < kTouchedSampleLimit) touched_paths.push_back(path);
    };

    for (const auto& row : store.query(
             "SELECT path, content_hash, content_verified_at FROM files "
             "WHERE repo_id = ? AND is_deleted = 0 AND stale = 0",
             repo_id)) {
        if (row.empty()) continue;

        // Same file, either spelling (exact first, then canonical fallback).
        std::string disk_path;
        for (const std::string& candidate : files_on_disk) {
            if (same_file_spelling(candidate, row[0])) { disk_path = candidate; break; }
        }
        if (disk_path.empty()) { note_touched(row[0]); continue; }

        auto [mtime, failed] = disk_time(disk_path);
        if (failed) { note_touched(row[0]); continue; } // unreadable: not provably fresh

        const std::string stored_hash = row.size() > 1 ? row[1] : "";
        const std::string verified_at = row.size() > 2 ? row[2] : "";

        // (1) Fresh per verification: the disk copy is quieter than the moment
        // its content was last proven to match. An empty stamp (a row never
        // stamped) proves nothing and falls through to the content check.
        if (!verified_at.empty() && !stored_hash.empty() && to_db_time(mtime) <= verified_at) {
            continue;
        }

        // (2) Fresh per content: hash(disk bytes) == stored content_hash.
        if (!stored_hash.empty() && hashed < kHashVerifyLimit) {
            hashed++;
            std::string current = content_hasher::compute_file_hash(disk_path);
            if (!current.empty() && current == stored_hash) continue;
        }

        note_touched(row[0]);
    }
    PERF_VALUE("indexer.touched_records.hashed", hashed);
    PERF_VALUE("indexer.touched_records.touched", touched);
    return touched;
}

// DEP-03: how many propagation rounds one full scan may run to settle the
// dependents its own re-indexing invalidated. Include chains are normally
// shallow; the bound only exists so a pathological or cyclic graph cannot
// make a single scan spin. When it is reached the scan leaves the remaining
// records stale instead of certifying a coverage it did not settle.
constexpr int kMaxStabilizationPasses = 4;

// Live record spelling for a path the reverse graph reported, when that
// dependent still needs re-parsing: the record must exist, be live, be
// explicitly marked stale, and still be on disk. Returns "" when there is
// nothing to do — a fresh record already matches, a tombstone has no
// knowledge to refresh, and a file that vanished is reconciliation's job
// (IDX-02), not the stabilization pass's.
std::string stale_dependent_to_refresh(knowledge_store::KnowledgeStore& store,
                                       const std::string& repo_id, const std::string& path) {
    auto rows = store.query("SELECT stale, is_deleted FROM files WHERE repo_id = ? AND path = ?",
                            repo_id, path);
    if (rows.empty() || rows[0].empty()) return "";
    if (rows[0].size() > 1 && rows[0][1] == "1") return "";
    if (rows[0][0] != "1") return "";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return "";
    return path;
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
    cov.tombstoned_records = tombstone_count(store, repo_id);
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
    cov.coverage_certified_at = st.coverage_computed_at;
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

    // Every tracked record's copy on disk must be provably the content the
    // index derived its knowledge from (IDX-04): a file edited outside the
    // server after its last verification is genuinely touched; a file touched
    // without changing its content is not. The second case used to count as
    // stale forever, disabling repo-level queries on a healthy index.
    std::vector<std::string> touched_paths;
    int touched = touched_records(store, repo_id, files_on_disk, touched_paths);
    cov.touched_records = touched;
    cov.touched_sample = std::move(touched_paths);
    if (touched > 0) {
        stale_claim();
        return cov;
    }

    cov.coverage = IndexCoverage::full_fresh;
    return cov;
}

bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id) {
    return needs_indexing(store, file_path, repo_id, nullptr);
}

bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path,
                    const std::string& repo_id, std::string* out_hash) {
    PERF_SCOPE("indexer.needs_indexing");
    // Check if file exists in files table
    auto rows = store.query(
        "SELECT content_hash, stale, is_deleted FROM files WHERE repo_id = ? AND path = ?",
        repo_id, file_path
    );

    if (rows.empty()) {
        PERF_COUNTER("indexer.needs_indexing.not_indexed");
        return true; // Not indexed yet
    }

    // A tombstoned record (IDX-02) is not live knowledge: the file is back on
    // disk, so it must be re-indexed, which resurrects the record.
    if (rows[0].size() >= 3 && rows[0][2] == "1") {
        PERF_COUNTER("indexer.needs_indexing.was_tombstoned");
        return true;
    }

    // Explicitly marked stale (e.g. transitive header invalidation)
    if (rows[0].size() >= 2 && rows[0][1] == "1") {
        PERF_COUNTER("indexer.needs_indexing.marked_stale");
        return true;
    }

    // Compare hash. The hash read here is handed back through out_hash so a
    // scan loop that has to re-index can reuse it: without this, a changed
    // file gets read once for the comparison and again inside index_file
    // (IDX-03's double-read shape reappearing at loop level).
    std::string current_hash = content_hasher::compute_file_hash(file_path);
    if (out_hash) *out_hash = current_hash;
    bool changed = rows[0][0] != current_hash;
    PERF_COUNTER(changed ? "indexer.needs_indexing.hash_changed"
                         : "indexer.needs_indexing.hash_unchanged");
    return changed;
}

UpdateReport index_snapshot(knowledge_store::KnowledgeStore& store, const std::string& file_path,
                            const std::string& repo_id, const std::string& content,
                            bool force_propagate) {
    PERF_SCOPE("indexer.index_snapshot");
    // Single source of truth for one file's derived knowledge (IDX-03): the
    // caller hands over the exact bytes to be hashed, and every artifact
    // written below — content_hash, symbols (content_hash_at_index),
    // dependencies, size_bytes, line_count, is_binary — is derived from that
    // one buffer. The file is never reopened here, so no concurrent edit can
    // make the stored hash describe a different version than the stored
    // symbols. Centralizing the write here is also the single update entry
    // point DEP-03 asks for.
    //
    // Because this is the only place that knows a stored version is being
    // replaced, it is also the only place that invalidates what the replaced
    // version produced: the propagation lives here, so no handler has to
    // remember a separate invalidation call and no path (edit, single-file
    // index, self-heal on read, full scan) can skip it.
    UpdateReport report;

    // Language gate before touching the content: a file type the index cannot
    // handle is skipped without parsing or writing anything.
    std::string ext = path_utils::get_extension(file_path);
    std::string language = parser::get_language_from_extension(ext);

    if (language == "unknown") {
        PERF_COUNTER("indexer.index_snapshot.skipped_unknown");
        report.skipped_language = true;
        return report; // Skip unknown file types
    }

    // Hash the snapshot (identical to hashing the file: same byte sequence).
    std::string file_hash = content_hasher::compute_string_hash(content);
    report.content_hash = file_hash;

    // Supersession gate: the stored knowledge is replaced only when a live
    // record holds a different hash, or when the record was tombstoned (the
    // file came back after a scan reconciled it as deleted) and its knowledge
    // is therefore already gone. A brand-new file supersedes nothing, and an
    // identical snapshot supersedes nothing: re-verifying unchanged bytes must
    // not discard the dependents' still-valid knowledge nor cost a graph
    // traversal on every read-triggered self-heal.
    auto stored = repo_id.empty()
        ? store.query("SELECT content_hash, is_deleted FROM files WHERE path = ?", file_path)
        : store.query("SELECT content_hash, is_deleted FROM files WHERE repo_id = ? AND path = ?", repo_id, file_path);
    bool had_record = !stored.empty() && !stored[0].empty();
    bool superseded = had_record &&
                      (stored[0][0] != file_hash || (stored[0].size() > 1 && stored[0][1] == "1"));
    report.changed = superseded;

    // DEP-01 ordering: collect the transitive dependents BEFORE any write,
    // while the edges that identify them are still in the database. Read-only
    // traversal shared with cache_manager::invalidate, so both entry points
    // agree on who depends on whom.
    const bool propagate = superseded || force_propagate;
    if (propagate) {
        report.dependents = cache_manager::collect_dependents(store, file_path, repo_id, report.truncated);
        report.propagated = !report.dependents.empty();
    } else {
        PERF_COUNTER("indexer.index_snapshot.propagation_skipped_unchanged");
    }

    // Parse the same snapshot.
    parser::FileMetadata metadata = parser::parse_file(file_path, language, content);

    PERF_VALUE("indexer.index_snapshot.symbols", metadata.symbols.size());
    PERF_VALUE("indexer.index_snapshot.dependencies", metadata.dependencies.size());
    PERF_VALUE("indexer.index_snapshot.symbol_deps", metadata.symbol_deps.size());

    // Size describes the hashed snapshot, not a stat taken at another moment:
    // a separate filesystem::file_size() could report a size different from the
    // content the hash and the symbols were produced from.
    long long file_size = static_cast<long long>(metadata.size_bytes);

    // One verification instant (IDX-04): the same wall-clock second stamps
    // both last_indexed_at ('when the derived knowledge was written') and
    // content_verified_at ('when we proved the bytes on disk match
    // content_hash'). Because they share one snapshot instant, a coverage
    // revalidation immediately after this write can certify freshness without
    // re-reading the file.
    std::string now_stamp = to_db_time(std::time(nullptr));

    // Group all writes for this file into one verified RAII transaction
    // (DB-01): the dependents' invalidation and this file's fresh knowledge
    // commit together, so a failure can never leave a dependent stale while
    // this file still claims the superseded content (or the reverse). Every
    // write goes through tx.exec(): the first failing statement poisons the
    // scope and commit() refuses. Previously ~10 writes here were unchecked,
    // so a mid-batch failure (a symbols INSERT, a dependency edge, the state
    // row) was still COMMITTED, and 'indexing succeeded' could describe an
    // index where the file record was updated but its knowledge was missing.
    knowledge_store::KnowledgeStore::Transaction tx(store, "index_snapshot");
    if (tx.begin_failed()) {
        report.error = tx.error();
        return report;
    }

    // DEP-03: drop the dependents' derived knowledge first, inside this
    // transaction. Their own edges stay (only their symbols/summaries go and
    // the record is marked stale), so the reverse graph is still intact for
    // the next traversal and for the scan that will re-parse them. A failing
    // reap poisons the scope, so the fresh snapshot below can never commit on
    // top of a half-applied invalidation.
    for (const auto& dependent : report.dependents) {
        cache_manager::reap_dependent(tx, repo_id, dependent);
    }
    if (tx.poisoned()) {
        report.error = tx.error();
        std::cerr << "ai-tools: file update cancelled (" << file_path
                  << "): " << report.error << "\n";
        // Nothing was written: the dependents were not invalidated either, so
        // the report must not claim they were.
        report.dependents.clear();
        report.propagated = false;
        return report;
    }
    if (!report.dependents.empty()) {
        PERF_VALUE("indexer.index_snapshot.propagated_dependents", report.dependents.size());
        if (report.truncated) {
            PERF_COUNTER("indexer.index_snapshot.dependents_truncated");
        }
    }

    // Update the file record in place so file_id stays stable across
    // re-indexes. INSERT OR REPLACE would delete and recreate the row,
    // changing file_id and orphaning summaries (and any other artifact)
    // attached to the previous id.
    // Clearing is_deleted resurrects a record that a previous full scan
    // tombstoned because the file had disappeared (IDX-02): an indexed,
    // live file must never keep counting as a tombstone.
    tx.exec(
        "UPDATE files SET content_hash = ?, language = ?, file_type = ?, size_bytes = ?, "
        "line_count = ?, last_indexed_at = ?, content_verified_at = ?, is_binary = ?, stale = 0, is_deleted = 0 "
        "WHERE repo_id = ? AND path = ?",
        file_hash, language, metadata.file_type, file_size, metadata.line_count,
        now_stamp, now_stamp,
        metadata.is_binary ? 1 : 0, repo_id, file_path
    );
    tx.exec(
        "INSERT INTO files (repo_id, path, content_hash, language, file_type, size_bytes, line_count, last_indexed_at, content_verified_at, is_binary) "
        "SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
        "WHERE (SELECT changes()) = 0",
        repo_id, file_path, file_hash, language, metadata.file_type, file_size, metadata.line_count,
        now_stamp, now_stamp, metadata.is_binary ? 1 : 0
    );

    // Resolve the file_id for this file
    auto file_rows = store.query(
        "SELECT file_id FROM files WHERE repo_id = ? AND path = ?",
        repo_id, file_path
    );
    if (file_rows.empty() || file_rows[0].empty()) {
        // The row was UPDATEd/INSERTed inside this very transaction, so an
        // empty lookup means the read failed or the write did not land. Either
        // way the symbols and summaries keyed by file_id would be skipped while
        // the file record claims success — the exact half-state DB-01 exists to
        // prevent. Cancel the scope instead of continuing.
        report.error = store.get_error();
        if (report.error.empty()) report.error = "file record not found after write";
        std::cerr << "ai-tools: file update cancelled (" << file_path
                  << "): " << report.error << "\n";
        report.dependents.clear();
        report.propagated = false;
        return report; // the destructor rolls the transaction back
    }
    std::string file_id = file_rows[0][0];

    // Hash-driven revalidation: if the file content matches the hash a
    // previously invalidated summary was stored against (e.g. an edit was
    // reverted), the summary becomes valid again.
    tx.exec(
        "UPDATE summaries SET is_valid = 1, invalidated_at = NULL "
        "WHERE file_id = ? AND content_hash = ? AND is_valid = 0",
        file_id, file_hash
    );

    // Replace symbols for this file
    tx.exec("DELETE FROM symbols WHERE file_id = ?", file_id);
    for (const auto& sym : metadata.symbols) {
        tx.exec(
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

    // Replace dependency edges for this file
    tx.exec("DELETE FROM dependencies WHERE repo_id = ? AND source_path = ?", repo_id, file_path);
    for (const auto& dep : metadata.dependencies) {
        // Try to resolve the dependency to a real repository file so that
        // reverse lookups and transitive invalidation work.
        std::string resolved = dep;
        bool drop_edge = false;
        if (language == "python") {
            // Python's parser emits module names (for example "utils"), not
            // filesystem paths. Resolve repository-local modules before
            // persisting the edge; otherwise invalidation cannot follow an
            // import back to the file that changed. External modules remain
            // as unresolved names, as before.
            auto roots = store.query("SELECT root_path FROM repositories WHERE repo_id = ?", repo_id);
            if (!roots.empty() && !roots[0].empty()) {
                std::string module_path = dep;
                std::replace(module_path.begin(), module_path.end(), '.', '/');
                std::filesystem::path root(roots[0][0]);
                std::filesystem::path module_file = root / (module_path + ".py");
                std::filesystem::path package_file = root / module_path / "__init__.py";
                std::error_code ec;
                if (std::filesystem::is_regular_file(module_file, ec)) {
                    resolved = std::filesystem::weakly_canonical(module_file, ec).string();
                } else {
                    ec.clear();
                    if (std::filesystem::is_regular_file(package_file, ec)) {
                        resolved = std::filesystem::weakly_canonical(package_file, ec).string();
                    }
                }
            }
        }
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
            } else if (!ec) {
                // The include names a file the index already reconciled as
                // deleted (a tombstone, IDX-02). Storing an edge to that path
                // would resurrect a dead path as live graph knowledge for later
                // queries, so the edge is dropped instead: the dependent was
                // marked stale and re-parsed, and its records stop referencing
                // the file that is gone.
                auto dead = store.query(
                    "SELECT 1 FROM files WHERE repo_id = ? AND path = ? AND is_deleted = 1 LIMIT 1",
                    repo_id, abs.string());
                if (dead.empty()) {
                    dead = store.query(
                        "SELECT 1 FROM files WHERE repo_id = ? AND path = ? AND is_deleted = 1 LIMIT 1",
                        repo_id, candidate.string());
                }
                if (!dead.empty()) {
                    drop_edge = true;
                    PERF_COUNTER("indexer.index_snapshot.dropped_edge_to_tombstoned_file");
                }
            }
        }
        if (drop_edge) continue;
        tx.exec(
            "INSERT INTO dependencies (repo_id, source_path, target_path, dep_type, dep_direction) VALUES (?, ?, ?, ?, ?)",
            repo_id, file_path, resolved, (language == "python") ? "import" : "include", "out"
        );
    }

    // Replace symbol-level dependency edges (call graph) for this file
    tx.exec("DELETE FROM dependencies WHERE repo_id = ? AND source_path = ? AND dep_type = 'call'", repo_id, file_path);
    for (const auto& sd : metadata.symbol_deps) {
        tx.exec(
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
    tx.exec(
        std::string("INSERT INTO indexing_state (repo_id, last_incremental_at, status, files_indexed) ") +
        "VALUES (?, datetime('now'), 'partial', " + kTrackedCount + ") " +
        "ON CONFLICT(repo_id) DO UPDATE SET "
        "last_incremental_at = datetime('now'), "
        "status = CASE WHEN indexing_state.full_index_covered IS NULL THEN 'partial' ELSE indexing_state.status END, "
        "files_indexed = " + kTrackedCount,
        repo_id, repo_id, repo_id
    );

    // commit() refuses when any exec() above failed, which is the guarantee
    // DB-01 is about: a failed symbols INSERT used to be followed by a COMMIT
    // anyway. On a refused commit nothing was applied (the scope is cancelled),
    // so the report must not claim the dependents were invalidated either.
    if (!tx.commit()) {
        report.error = tx.error();
        std::cerr << "ai-tools: file update rolled back (" << file_path
                  << "): " << report.error << "\n";
        report.dependents.clear();
        report.propagated = false;
        return report;
    }

    report.indexed = true;
    return report;
}

UpdateReport index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id) {
    PERF_SCOPE("indexer.index_file");
    // Read-once wrapper over index_snapshot: the file is opened a single time
    // and the buffer read here is the one hashed and parsed (IDX-03). The
    // extension gate is applied before reading so files the index cannot
    // handle are never read from disk.
    //
    // index_snapshot owns the propagation, so this entry point is safe for a
    // file with dependents: whoever detects the change (single-file index
    // tool, self-heal on a query, a summary write) gets the same invalidation
    // the edit tool has always had (DEP-03).
    std::string ext = path_utils::get_extension(file_path);
    std::string language = parser::get_language_from_extension(ext);

    if (language == "unknown") {
        PERF_COUNTER("indexer.index_file.skipped_unknown");
        UpdateReport report;
        report.skipped_language = true;
        return report; // Skip unknown file types
    }

    std::string content;
    if (!parser::detail::read_file_content(file_path, content)) {
        PERF_COUNTER("indexer.index_file.unreadable");
        UpdateReport report;
        report.unreadable = true;
        return report; // Unreadable file
    }

    return index_snapshot(store, file_path, repo_id, content);
}

bool has_dependents(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::string& file_path) {
    // Diagnostic only: it answers whether re-indexing only file_path would
    // leave somebody's cached knowledge stale, without writing anything. It
    // delegates to the same traversal index_snapshot() invalidates with, so
    // the question and the propagation can never disagree (the previous copy
    // of this BFS in index_file's neighbourhood was a second implementation
    // of the same walk).
    bool truncated = false;
    return !cache_manager::collect_dependents(store, file_path, repo_id, truncated).empty();
}

std::vector<std::string> index_dependent_chain(knowledge_store::KnowledgeStore& store,
                                                const std::string& file_path, const std::string& repo_id) {
    PERF_SCOPE("indexer.index_dependent_chain");
    // Reconcile one file AND everything derived from it, even when its content
    // hash is unchanged: a dependent can be stale while this file's bytes are
    // not (the staleness came from a deleted file, from an invalidation pass,
    // or from a previous scan), and this is the entry point used to bring the
    // chain back. index_snapshot does the whole job in one transaction: it
    // collects the transitive dependents while the edges are still in place
    // (DEP-01), drops their symbols/summaries, marks them stale, and rebuilds
    // this file's knowledge and edges from a single fresh read (IDX-03). It
    // also revives a stored summary when the exact previous snapshot
    // reappears, preserving the self-heal semantics callers relied on.
    std::string content;
    if (!parser::detail::read_file_content(file_path, content)) {
        PERF_COUNTER("indexer.index_dependent_chain.unreadable");
        return {};
    }

    auto report = index_snapshot(store, file_path, repo_id, content, /*force_propagate=*/true);
    return report.dependents;
}

FullScanResult index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id) {
    PERF_SCOPE("indexer.index_repository");
    FullScanResult scan_result;

    auto fail = [&](const std::string& reason) {
        // An incomplete scan must never leave the index claiming full
        // coverage: record the failure and drop the coverage snapshot.
        // (DB-01) The record itself is verified: if even the failure write
        // fails, the error is surfaced, because an unrecorded failure would
        // let a later query trust a coverage snapshot this scan invalidated.
        if (store.execute(
                "INSERT INTO indexing_state (repo_id, status, last_error) VALUES (?, 'failed', ?) "
                "ON CONFLICT(repo_id) DO UPDATE SET status = 'failed', last_error = ?, "
                "full_index_covered = NULL, coverage_computed_at = NULL",
                repo_id, reason, reason) != SQLITE_OK) {
            std::cerr << "ai-tools: cannot record scan failure (" << repo_id << "): "
                      << store.get_error() << "\n";
        }
        scan_result.ok = false;
        scan_result.error = reason;
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

    // Reconciliation set (IDX-02): live records for files that no longer exist
    // on disk.
    std::vector<std::string> gone = disappeared_files(store, repo_id, disk.files);
    if (!gone.empty()) {
        // Dependent stale-marking and tombstone writes are ONE verified
        // transaction (mark_dependents first, while the edges recorded by the
        // previous scan are still there to identify them; reconcile_deleted_files
        // owns that ordering). Running reconciliation before the incremental
        // re-index below matters: a re-parsed dependent that still includes the
        // deleted path must see it as reconciled (and drop the edge), not
        // re-record it as live knowledge.
        ReconcileOutcome rec = reconcile_deleted_files(store, repo_id, gone);
        if (!rec.ok) return fail("deleted-file reconciliation failed: " + rec.error);
        scan_result.dependents_invalidated = rec.dependents_marked;
        scan_result.files_reconciled = rec.tombstoned;
    }

    // Incremental: unchanged files are never reprocessed. The dependents of the
    // deleted files just became 'changed' (they are marked stale), so they are
    // re-parsed here against the repository as it is now.
    //
    // Read budget per file (IDX-03): 0 filesystem reads for a file the DB
    // comparison proves fresh against a verification stamp we already trust,
    // exactly 1 read plus hash for a file that needs re-indexing.
    // needs_indexing hands back the hash it computed from the bytes on disk;
    // the buffer read next must hash to it, or the file moved under the scan
    // — retry the read once, bounded. Whatever buffer ends up being indexed,
    // its hash and its symbols derive from the same snapshot: the retry only
    // reduces the chance of storing a version already superseded.
    //
    // IDX-04: a skipped-as-unchanged record is not left untouched. The hash
    // comparison just proved the bytes on disk are the bytes the index
    // derived its knowledge from, so the fact has to be recorded: the record
    // is stamped with the instant of THIS verification. Without the stamp, a
    // skipped record keeps the timestamp of an older scan, and the coverage
    // revalidation right below would read 'file written after it was indexed'
    // forever — a claim no incremental scan could ever satisfy.
    //
    // The stamp is written here, at the verification moment, and never as a
    // batch at the end of the scan: an end-of-scan stamp would be later than
    // files edited in the middle of the scan and would certify those edits as
    // verified content, which is the opposite of what it must prove.
    //
    // pending_dependents collects what each re-indexed file invalidated
    // (DEP-03); refreshed makes sure the stabilization pass re-parses any
    // given record at most once.
    std::unordered_set<std::string> pending_dependents;
    std::unordered_set<std::string> refreshed;
    for (const std::string& path : disk.files) {
        std::string disk_hash;
        if (!needs_indexing(store, path, repo_id, &disk_hash)) {
            PERF_COUNTER("indexer.index_repository.unchanged_skipped");
            std::string verified_at = to_db_time(std::time(nullptr));
            if (store.execute(
                    "UPDATE files SET content_verified_at = ? "
                    "WHERE repo_id = ? AND path = ? AND is_deleted = 0",
                    verified_at, repo_id, path) != SQLITE_OK) {
                return fail("content verification stamping failed: " + store.get_error());
            }
            scan_result.files_verified++;
            continue;
        }

        std::string content;
        if (!parser::detail::read_file_content(path, content)) {
            // The file disappeared or became unreadable between the walk and
            // this read; the next full scan's reconciliation (IDX-02) handles
            // the tombstone. Skipping here keeps the index consistent rather
            // than half-writing a file that is gone.
            PERF_COUNTER("indexer.index_repository.unreadable");
            continue;
        }
        if (!disk_hash.empty() && content_hasher::compute_string_hash(content) != disk_hash) {
            PERF_COUNTER("indexer.index_repository.snapshot_retry");
            if (parser::detail::read_file_content(path, content)) {
                // Even if it changed again, the snapshot indexed below is
                // internally consistent: hash and parse see these bytes.
            }
        }

        auto report = index_snapshot(store, path, repo_id, content);
        if (!report.indexed)
            return fail("file update failed for " + path + ": " + report.error);
        scan_result.files_refreshed++;
        // DEP-03: re-indexing a changed file inside the scan propagates
        // through the same transaction the snapshot was written in, so a
        // header refreshed here never leaves its includers holding symbols
        // derived from the superseded version.
        scan_result.dependents_invalidated += static_cast<int>(report.dependents.size());
        for (const auto& dependent : report.dependents) pending_dependents.insert(dependent);
    }

    // DEP-03 stabilization pass. The walk above visits files in directory
    // order, so a dependent can be visited BEFORE the file it includes: the
    // dependent is then refreshed from its own current bytes and only later
    // invalidated by the include that changed. index_snapshot propagates
    // correctly in both directions, but the invalidation has to be settled
    // before the scan certifies full coverage, otherwise the scan would
    // record a coverage snapshot while records it just marked stale are
    // still unrefreshed — the shape that made repo-level queries rescan
    // forever. Re-parse everything the propagation touched, following the
    // chain until nothing is left; bounded so a pathological graph cannot
    // make one scan spin.
    std::deque<std::string> queue(pending_dependents.begin(), pending_dependents.end());
    while (!queue.empty() && scan_result.stabilization_passes < kMaxStabilizationPasses) {
        std::vector<std::string> round(queue.begin(), queue.end());
        queue.clear();
        scan_result.stabilization_passes++;

        for (const std::string& candidate : round) {
            std::string record = stale_dependent_to_refresh(store, repo_id, candidate);
            if (record.empty()) continue; // fresh already, tombstoned, or gone

            std::string content;
            if (!parser::detail::read_file_content(record, content)) {
                PERF_COUNTER("indexer.index_repository.stabilization_unreadable");
                continue; // reconciliation (IDX-02) owns a file that vanished
            }

            auto report = index_snapshot(store, record, repo_id, content);
            if (!report.indexed)
                return fail("dependent refresh failed for " + record + ": " + report.error);
            scan_result.dependents_refreshed++;
            scan_result.dependents_invalidated += static_cast<int>(report.dependents.size());
            for (const auto& dependent : report.dependents) {
                if (refreshed.insert(dependent).second) queue.push_back(dependent);
            }
        }
    }
    if (!queue.empty()) {
        // The bound was reached with work still pending: leave the records
        // stale rather than certify a coverage the scan did not settle.
        // repo_coverage() reports full_stale and the next scan continues.
        PERF_COUNTER("indexer.index_repository.stabilization_truncated");
        std::cerr << "ai-tools: dependent stabilization stopped after "
                  << scan_result.stabilization_passes << " passes (" << queue.size()
                  << " dependents left stale, repo " << repo_id << ")\n";
    }

    PERF_VALUE("indexer.index_repository.walked", scan_result.files_total);
    PERF_VALUE("indexer.index_repository.indexed", scan_result.files_refreshed);
    PERF_VALUE("indexer.index_repository.dependents_refreshed", scan_result.dependents_refreshed);
    PERF_VALUE("indexer.index_repository.stabilization_passes", scan_result.stabilization_passes);

    // Record the coverage snapshot this scan just certified. From now on a
    // repo-level query may treat the index as complete, and only for the file
    // count measured here (repo_coverage validates it again before use).
    //
    // DB-01: this write IS the certification, so it must not be fire-and-forget.
    // If it fails, the scan did not record coverage, and reporting ok=true would
    // let the next query read results from an uncertified index. Verify it and
    // fail the scan instead: the re-indexed files stay in the database (each was
    // its own committed transaction) but no 'full' claim exists, so
    // repo_coverage() reports partial and a later scan certifies properly.
    int covered = tracked_count(store, repo_id);
    knowledge_store::KnowledgeStore::Transaction state_tx(store, "index_repository_state");
    if (state_tx.begin_failed()) {
        return fail("cannot record scan coverage: " + state_tx.error());
    }
    state_tx.exec(
        "INSERT INTO indexing_state (repo_id, last_full_index_at, status, files_indexed, files_total, "
        "full_index_covered, coverage_computed_at, last_error) "
        "VALUES (?, datetime('now'), 'full', ?, ?, ?, datetime('now'), NULL) "
        "ON CONFLICT(repo_id) DO UPDATE SET last_full_index_at = datetime('now'), status = 'full', "
        "files_indexed = ?, files_total = ?, full_index_covered = ?, coverage_computed_at = datetime('now'), "
        "last_error = NULL",
        repo_id, covered, scan_result.files_total, covered, covered, scan_result.files_total, covered);
    if (!state_tx.commit()) {
        return fail("cannot record scan coverage: " + state_tx.error());
    }

    scan_result.ok = true;
    return scan_result;
}

}
