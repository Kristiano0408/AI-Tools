#pragma once

#include "parser.h"
#include "knowledge_store.h"
#include <string>
#include <vector>

namespace indexer {

// How much of a repository the persisted index actually covers. The presence
// of indexed data is never proof of a complete index: repo-level queries may
// only trust full_fresh.
enum class IndexCoverage {
    not_indexed,  // nothing recorded for this repository yet
    partial,      // only individual files indexed on demand: NOT safe for repo-level queries
    full_stale,   // a complete scan happened, but coverage no longer matches the repository
    full_fresh    // a complete scan covers the repository as it is on disk right now
};

struct Coverage {
    IndexCoverage coverage = IndexCoverage::not_indexed;
    int files_indexed = 0;       // live (not tombstoned) records tracked in the index
    int files_total = 0;         // indexable files on disk (lower bound when over_limit)
    int full_index_covered = -1; // file count a completed full scan certified (-1: none)
    int stale_records = 0;       // tracked records explicitly marked stale
    int tombstoned_records = 0;  // records kept as tombstones for files no longer on disk
    bool walk_failed = false;    // the repository root could not be read at all
    bool walk_truncated = false; // the bounded walk stopped early: files_total is a floor
    int touched_records = 0;     // live records whose derived knowledge no longer matches the bytes on disk (IDX-04)
    std::vector<std::string> touched_sample; // capped list of touched record paths (diagnostic hint)
    std::string last_error;      // failure recorded for the last scan, if any
    std::string last_full_index_at;
    std::string coverage_certified_at; // when the certified coverage was measured
};

std::string coverage_name(IndexCoverage coverage);

// Extensions considered indexable, shared by the full scan and the coverage
// measurement so both count the same set of files.
const std::vector<std::string>& supported_source_extensions();

// Bounded walk of the indexable source files under root_path. When limit > 0
// the walk stops as soon as limit is exceeded (files then hold limit + 1
// entries and over_limit is set). Paths are canonicalized absolute paths.
struct RepoScan {
    bool ok = false;                 // the walk itself succeeded
    bool over_limit = false;         // walk stopped early: files is a lower bound
    std::vector<std::string> files;  // canonical absolute paths of indexable files
};
RepoScan scan_indexable_files(const std::string& root_path, int limit);

// Honest measurement of how much of the repository the persisted index
// really covers. Indexed data existing in the database is never taken as
// proof of completeness: a partial index (individual files indexed on
// demand) is reported as partial, and a 'full' claim is trusted only while
// the tracked records still match the snapshot the completed scan certified
// and nothing is marked stale.
//
// verify_limit controls the on-disk cross-check that re-proves the claim:
//   0  -> never touch the filesystem (the answer describes coverage as of the
//         last completed scan; changes made outside the server are not seen)
//   -1 -> unbounded walk
//   >0 -> bounded walk: at most verify_limit indexable files are visited, so
//         files_total is a lower bound and walk_truncated is set
Coverage repo_coverage(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                       int verify_limit = 0);

struct FullScanResult {
    bool ok = false;           // repository found and walked
    int files_total = 0;       // indexable files observed on disk
    int files_refreshed = 0;   // files actually re-parsed (unchanged ones skipped)
    int files_verified = 0;    // unchanged records whose content was verified and re-stamped in place (IDX-04)
    int files_reconciled = 0;  // tracked records tombstoned because the file is gone (IDX-02)
    int dependents_invalidated = 0; // live dependents marked stale by this scan (IDX-02 deletions + DEP-03 changes)
    int dependents_refreshed = 0;   // dependents re-parsed by the stabilization pass (DEP-03)
    int stabilization_passes = 0;   // propagation passes run after the main loop (DEP-03)
};

// Outcome of one single-file update (DEP-03). index_snapshot() is the ONE
// place that writes a file's derived knowledge, so it is also the one place
// that knows whether that knowledge was superseded — and therefore the one
// place that can invalidate the files derived from it. Every re-indexing
// entry point returns this report instead of relying on the caller to run a
// separate invalidation pass.
struct UpdateReport {
    bool indexed = false;      // derived knowledge was written for this snapshot
    bool changed = false;      // the snapshot superseded the previously stored content
    bool propagated = false;   // dependents were invalidated (changed, or forced)
    bool skipped_language = false; // extension maps to no supported parser
    bool unreadable = false;   // the file could not be read (index_file only)
    bool truncated = false;    // dependent collection stopped at the safety cap
    std::string content_hash;  // hash of the snapshot that was indexed
    std::vector<std::string> dependents; // files invalidated, nearest first
    std::string error;         // storage failure, when indexed == false
};

// Full scan of a repository: re-index every changed file, reconcile tracked
// records against the filesystem, and record the coverage snapshot that
// repo_coverage() validates future repo-level queries against.
FullScanResult index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

// Index a single file. This is a partial operation: it can never make the
// index 'full' — only a completed index_repository() scan can (IDX-01).
// Reads the file once and forwards the buffer to index_snapshot. Because
// index_snapshot propagates, this entry point is safe for a file with
// dependents: a change made outside the server (editor, Git, another
// process) never leaves the dependents' cached knowledge stale (DEP-03).
UpdateReport index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);

// Re-index a single file from disk AND propagate the invalidation to every
// file that directly or transitively includes/imports it (DEP-01/DEP-02),
// even when the content hash is unchanged: this is the entry point for
// reconciling a file whose dependents are already marked stale. Returns the
// dependent paths (nearest first).
std::vector<std::string> index_dependent_chain(knowledge_store::KnowledgeStore& store,
                                               const std::string& file_path, const std::string& repo_id);

// True when any live record of this repository directly or transitively
// includes/imports file_path (dep_type != 'call'). Diagnostic/reporting
// helper: it answers the question without writing anything, and delegates to
// the same reverse traversal index_snapshot() uses, so the two can never
// disagree about who depends on whom.
bool has_dependents(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::string& file_path);

// Index a file from an already-read byte snapshot (IDX-03). This is the ONE
// place that writes a file's derived knowledge: content_hash, symbols,
// dependency edges, size and line count all derive from `content`, so they
// provably describe one version of the file. Callers that already hold the
// bytes (an edit's new content, a scan that read the file to compare hashes)
// should call this directly instead of re-reading from disk.
//
// It is also the ONE place that invalidates what the superseded knowledge
// produced (DEP-03): when the snapshot replaces a different stored content
// (or `force_propagate` is set), the transitive dependents are collected
// before anything is deleted and their symbols/summaries are dropped inside
// the same transaction that writes the new snapshot. A single-file update
// therefore cannot leave a dependent holding symbols derived from content
// that no longer exists, whatever path the change arrived through.
// An unchanged snapshot propagates nothing: re-verifying identical bytes
// must not cost a graph traversal or discard knowledge that still matches.
UpdateReport index_snapshot(knowledge_store::KnowledgeStore& store, const std::string& file_path,
                            const std::string& repo_id, const std::string& content,
                            bool force_propagate = false);

// Check if file needs re-indexing
bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);

// Overload returning the on-disk hash read during the check: when the file
// is fresh (result false), *out_hash holds the current contents' hash; when
// re-indexing is needed it is the hash of the last successful read (empty if
// the file is unreadable). A scan loop can reuse that buffer/hash instead of
// reading the file a second time (IDX-03).
bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path,
                    const std::string& repo_id, std::string* out_hash);

}
