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
    int dependents_invalidated = 0; // live dependents of deleted files marked stale (IDX-02)
};

// Full scan of a repository: re-index every changed file, reconcile tracked
// records against the filesystem, and record the coverage snapshot that
// repo_coverage() validates future repo-level queries against.
FullScanResult index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

// Index a single file. This is a partial operation: it can never make the
// index 'full' — only a completed index_repository() scan can (IDX-01).
// Reads the file once and forwards the buffer to index_snapshot.
void index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);

// Re-index a single file from disk AND propagate the invalidation to every
// file that directly or transitively includes/imports it (DEP-01/DEP-02):
// cache_manager::invalidate collects the full transitive dependent set while
// the edges still exist, then index_snapshot rebuilds this file's knowledge
// from one fresh read. This is the single high-level entry point DEP-03 asks
// for: external-change self-heal and single-file indexing must use it instead
// of index_file(), so a changed header never leaves its dependents' cached
// symbols or summaries stale. Returns the dependent paths (nearest first).
std::vector<std::string> index_dependent_chain(knowledge_store::KnowledgeStore& store,
                                               const std::string& file_path, const std::string& repo_id);

// True when any live record of this repository directly or transitively
// includes/imports file_path (dep_type != 'call'), traversing the reverse
// graph with a visited set. Used by single-file self-heal: an edit made
// outside the server must not silently re-index only the changed file and
// leave its dependents' derived knowledge stale (DEP-03 guard).
bool has_dependents(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                    const std::string& file_path);

// Index a file from an already-read byte snapshot (IDX-03). This is the ONE
// place that writes a file's derived knowledge: content_hash, symbols,
// dependency edges, size and line count all derive from `content`, so they
// provably describe one version of the file. Callers that already hold the
// bytes (an edit's new content, a scan that read the file to compare hashes)
// should call this directly instead of re-reading from disk.
void index_snapshot(knowledge_store::KnowledgeStore& store, const std::string& file_path,
                    const std::string& repo_id, const std::string& content);

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
