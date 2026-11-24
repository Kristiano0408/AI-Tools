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
    bool walk_failed = false;    // the repository root could not be read at all
    bool walk_truncated = false; // the bounded walk stopped early: files_total is a floor
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
};

// Full scan of a repository: re-index every changed file, reconcile tracked
// records against the filesystem, and record the coverage snapshot that
// repo_coverage() validates future repo-level queries against.
FullScanResult index_repository(knowledge_store::KnowledgeStore& store, const std::string& repo_id);

// Index a single file. This is a partial operation: it can never make the
// index 'full' — only a completed index_repository() scan can (IDX-01).
void index_file(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);

// Check if file needs re-indexing
bool needs_indexing(knowledge_store::KnowledgeStore& store, const std::string& file_path, const std::string& repo_id);

}
