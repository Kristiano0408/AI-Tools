// Shared helpers for the MCP tool layer: result envelopes, repository
// resolution, boundary guarding, self-healing index gate, and pagination.

#include "server/tools_internal.h"

#include "engine/indexer.h"
#include "server/workspace_roots.h"
#include "utils/path_utils.h"
#include "utils/perf_metrics.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace tools {

// ---- result envelopes ----

std::string tool_result(const json& payload) {
    json wrapper;
    wrapper["content"] = json::array({{{"type", "text"}, {"text", payload.dump()}}});
    wrapper["isError"] = false;
    return wrapper.dump();
}

std::string tool_error(const std::string& message) {
    json wrapper;
    wrapper["content"] = json::array({{{"type", "text"}, {"text", message}}});
    wrapper["isError"] = true;
    return wrapper.dump();
}

// ---- repository resolution ----

json& workspace_ambiguity_hints() {
    static json hints = json::object();
    return hints;
}

// Registered repositories (repo_id) from the global registry.
std::vector<std::string> registered_repo_ids() {
    std::vector<std::string> ids;
    knowledge_store::KnowledgeStore registry;
    if (registry.open(repository::registry_db_path())) {
        for (const auto& row : registry.query("SELECT repo_id FROM repositories")) {
            if (!row.empty()) ids.push_back(row[0]);
        }
    }
    return ids;
}

// Resolve a repository: prefer explicit repo_id, else derive from a path
// argument (walking up to a registered root, or auto-registering the
// directory), then fall back to the client-declared MCP workspace roots
// (roots/list), then the server's cwd, then — only when the client announced
// no roots — the single registered repository. The last fallback is skipped
// once roots are known so unrelated projects never leak into results.
// Opens the per-repository store and returns its identity.
std::string resolve_repo(const json& arguments, knowledge_store::KnowledgeStore& store) {
    PERF_SCOPE("tools.resolve_repo");
    workspace_ambiguity_hints() = json::object(); // recompute per call
    if (arguments.contains("repo_id") && !arguments["repo_id"].is_null()) {
        PERF_COUNTER("tools.resolve_repo.by_repo_id");
        std::string repo_id = arguments["repo_id"].get<std::string>();
        repository::RepoInfo info = repository::lookup_repository(repo_id);
        if (info.repo_id.empty()) return "";
        if (!store.open(info.db_path)) return "";
        return repo_id;
    }
    if (arguments.contains("path") && !arguments["path"].is_null()) {
        PERF_COUNTER("tools.resolve_repo.by_path");
        std::string path = arguments["path"].get<std::string>();
        std::error_code ec;
        std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
        if (ec) return "";
        std::string root = std::filesystem::is_directory(p) ? p.string() : p.parent_path().string();

        // If the path lives inside an already-registered repository, resolve
        // to that repository instead of creating a nested one.
        for (const auto& id : registered_repo_ids()) {
            repository::RepoInfo info = repository::lookup_repository(id);
            if (info.repo_id.empty()) continue;
            if (path_utils::is_within_repository(info.root_path, root)) {
                if (!store.open(info.db_path)) return "";
                return info.repo_id;
            }
        }

        repository::RepoInfo info = repository::get_or_create_repository(root);
        if (info.repo_id.empty()) return "";
        if (!store.open(info.db_path)) return "";
        return info.repo_id;
    }

    // Auto-detect: a client-declared MCP workspace root (roots/list) wins
    // over the server's working directory. When roots are announced, only
    // repositories matching them are eligible (a registered repository inside
    // a workspace root, or a workspace root inside a repository root, both
    // count) and unrelated projects never leak into results.
    PERF_COUNTER("tools.resolve_repo.autodetect");
    auto registered = registered_repo_ids();
    const bool have_roots = !workspace_roots::empty();

    if (have_roots) {
        // Several repositories may match the workspace roots: prefer the
        // deepest containment (closest to the folder the client opened) and
        // surface the alternatives so the agent can disambiguate with an
        // explicit repo_id.
        auto matches = workspace_roots::workspace_matching_repos();
        auto containment = [&](const std::string& id) -> size_t {
            repository::RepoInfo info = repository::lookup_repository(id);
            size_t best = 0;
            for (const auto& wr : workspace_roots::roots()) {
                if (path_utils::is_within_repository(info.root_path, wr))
                    best = std::max(best, wr.size() - info.root_path.size());
                else if (path_utils::is_within_repository(wr, info.root_path))
                    best = std::max(best, info.root_path.size() - wr.size());
            }
            return best;
        };
        std::sort(matches.begin(), matches.end(), [&](const std::string& a, const std::string& b) {
            return containment(a) > containment(b);
        });
        if (matches.size() > 1) {
            json hints = json::array();
            for (size_t i = 1; i < matches.size(); ++i) {
                repository::RepoInfo info = repository::lookup_repository(matches[i]);
                hints.push_back({{"repo_id", matches[i]}, {"root_path", info.root_path}});
            }
            workspace_ambiguity_hints() = hints;
        }
        if (!matches.empty()) {
            repository::RepoInfo info = repository::lookup_repository(matches[0]);
            if (!info.repo_id.empty() && store.open(info.db_path)) return info.repo_id;
        }
        return ""; // roots announced but no registered repository matches them
    }

    // No roots announced: legacy cwd-based fallback — the registered
    // repository containing the server's working directory ...
    std::error_code ec;
    std::string cwd = std::filesystem::weakly_canonical(std::filesystem::current_path(), ec).string();
    if (!ec) {
        for (const auto& id : registered) {
            repository::RepoInfo info = repository::lookup_repository(id);
            if (info.repo_id.empty()) continue;
            if (path_utils::is_within_repository(info.root_path, cwd)) {
                if (!store.open(info.db_path)) return "";
                return info.repo_id;
            }
        }
    }
    // ... else the only registered repository (clients like VS Code without
    // roots support never need to manage repo identities).
    if (registered.size() == 1) {
        repository::RepoInfo info = repository::lookup_repository(registered[0]);
        if (!info.repo_id.empty() && store.open(info.db_path)) return info.repo_id;
    }
    return "";
}

// Repository ids an unscoped query may read: the repositories matching the
// client-declared workspace roots when roots are announced; otherwise every
// registered repository (legacy behavior). Sets search_scope on `result`.
std::vector<std::string> search_target_repos(json& result) {
    std::vector<std::string> ids;
    if (!workspace_roots::empty()) {
        ids = workspace_roots::workspace_matching_repos();
        result["search_scope"] = "workspace_roots";
        result["workspace_roots"] = workspace_roots::roots();
    } else {
        ids = registered_repo_ids();
        result["search_scope"] = "all_registered";
    }
    if (const json& hints = workspace_ambiguity_hints(); !hints.empty())
        result["workspace_repo_candidates"] = hints;
    return ids;
}

// Validate that file_path stays inside the repository root; returns the
// canonical absolute path or an empty string when out of bounds.
std::string guarded_path(const repository::RepoInfo& repo, const std::string& file_path) {
    std::filesystem::path candidate;
    if (std::filesystem::path(file_path).is_absolute()) {
        candidate = file_path;
    } else {
        candidate = std::filesystem::path(repo.root_path) / file_path;
    }

    std::error_code ec;
    std::filesystem::path abs = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) return "";
    if (!path_utils::is_within_repository(repo.root_path, abs.string())) return "";
    return abs.string();
}

// ---- self-healing index gate ----

namespace {

// Repositories above this size are never auto-indexed implicitly: the caller
// has to ask for it explicitly instead of being blocked by a long scan.
constexpr int kAutoIndexLimit = 10000;

void coverage_json(const indexer::Coverage& cov, json& out) {
    json c;
    c["coverage"] = indexer::coverage_name(cov.coverage);
    c["complete"] = (cov.coverage == indexer::IndexCoverage::full_fresh);
    c["files_indexed"] = cov.files_indexed;
    c["files_total"] = cov.files_total;
    if (cov.full_index_covered >= 0) c["files_covered_by_last_full_scan"] = cov.full_index_covered;
    if (cov.stale_records > 0) c["stale_files"] = cov.stale_records;
    if (cov.tombstoned_records > 0) c["tombstoned_files"] = cov.tombstoned_records;
    if (cov.walk_truncated) c["walk_truncated"] = true;
    if (cov.walk_failed) c["walk_failed"] = true;
    // IDX-04: records whose stored knowledge no longer matches the bytes on
    // disk, with a capped sample, so a blocked query is diagnosable instead
    // of an empty result.
    if (cov.touched_records > 0) {
        c["touched_files"] = cov.touched_records;
        if (!cov.touched_sample.empty()) c["touched_paths"] = cov.touched_sample;
    }
    if (!cov.coverage_certified_at.empty()) c["coverage_certified_at"] = cov.coverage_certified_at;
    if (!cov.last_full_index_at.empty()) c["last_full_index_at"] = cov.last_full_index_at;
    if (!cov.last_error.empty()) c["last_scan_error"] = cov.last_error;
    // DEP-03: re-indexing one file invalidates the records derived from it, so
    // a per-file update can leave the repository coverage non-fresh even though
    // the updated file itself is now correct. Without the hint the agent sees
    // stale counts with no explanation of what to call next.
    if (cov.stale_records > 0) {
        c["hint"] = std::to_string(cov.stale_records) +
                    " tracked file(s) hold knowledge derived from content that changed (an include or "
                    "import of theirs was re-indexed). They are re-parsed on next access, or now: call "
                    "the index tool with repo_id and no file_path to settle the whole repository.";
    }
    out["index_coverage"] = c;
}

} // namespace

// Report what one single-file update invalidated (DEP-03): the transitive
// dependents whose derived knowledge was dropped. Every re-indexing entry
// point answers with the same shape, so an agent does not have to guess which
// tool propagates and which one silently re-indexes a single file.
// Emitted when the update propagated; `always` keeps the documented edit
// envelope shape even when a leaf file invalidated nobody.
void attach_dependents_report(json& result, const indexer::UpdateReport& report, bool always) {
    if (!always && !report.propagated) return;
    result["dependents_invalidated"] = static_cast<int>(report.dependents.size());
    result["dependents"] = json::array();
    for (size_t i = 0; i < report.dependents.size() && i < 50; ++i)
        result["dependents"].push_back(report.dependents[i]);
    if (report.dependents.size() > 50) result["dependents_truncated"] = true;
    if (report.truncated) {
        result["dependents_capped"] = true;
        result["note_capped"] = "Dependent collection stopped at the safety cap; run a full index scan to reconcile the rest.";
    }
}

void report_index_coverage(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                           json& result, const std::string* status_override) {
    if (repo_id.empty()) return;
    indexer::Coverage cov = indexer::repo_coverage(store, repo_id, 0);
    coverage_json(cov, result);
    result["index_status"] = status_override ? *status_override : indexer::coverage_name(cov.coverage);
}

// Gate for repository-level queries. The persisted index may only be read
// when a completed full scan actually covers the repository: the mere
// presence of indexed rows is never treated as proof of a complete index
// (IDX-01). An index produced by on-demand single-file indexing is reported
// as partial and triggers a full scan; a full scan whose coverage no longer
// matches the repository (files added, removed, or edited outside the
// server) triggers a re-scan.
//
// Returns false when the query must not be answered from the index; the
// result then carries the coverage and a hint on what to call next.
bool ensure_repo_indexed(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                         const repository::RepoInfo& repo, json& result) {
    PERF_SCOPE("tools.ensure_repo_indexed");
    if (repo_id.empty() || repo.root_path.empty()) return true; // nothing to ensure

    // Ground truth walk, bounded: never let a huge repository turn a query
    // into a minutes-long scan. The bound is only a guard here; the full scan
    // itself uses the real coverage.
    const int kBudget = kAutoIndexLimit;

    indexer::Coverage before = indexer::repo_coverage(store, repo_id, kBudget);
    if (before.coverage == indexer::IndexCoverage::full_fresh) {
        PERF_COUNTER("tools.ensure_repo_indexed.full_covered");
        coverage_json(before, result);
        result["index_status"] = "index_full";
        return true;
    }

    coverage_json(before, result);
    PERF_COUNTER("tools.ensure_repo_indexed.needs_full_scan");
    PERF_COUNTER(std::string("tools.ensure_repo_indexed.") + indexer::coverage_name(before.coverage));

    if (before.walk_failed) {
        result["index_status"] = "index_failed";
        result["hint"] = "The repository root cannot be read; a complete index cannot be verified. "
                         "Check repo_id/root_path and retry.";
        return false;
    }

    if (before.walk_truncated || before.files_total > kAutoIndexLimit) {
        // Repository larger than the implicit-index budget, with or without a
        // partial index: say so instead of blocking the call with a scan whose
        // cost nobody asked for.
        result["index_status"] = "not_indexed";
        result["hint"] = "Repository has at least " + std::to_string(before.files_total) +
                         " indexable files and needs a complete index before repo-level queries are "
                         "trustworthy. Call the index tool with repo_id to run the full scan.";
        return false;
    }

    indexer::FullScanResult scan = indexer::index_repository(store, repo_id);
    if (!scan.ok) {
        // DB-01: name the cause the scan reported (scan.error recorded by
        // FullScanResult, mirrored into indexing_state.last_error and shown in
        // index_coverage.last_scan_error). 'The scan did not complete' without
        // the cause is not actionable.
        result["index_status"] = "index_failed";
        result["hint"] = "The full scan did not complete"
                        + (scan.error.empty() ? "" : ": " + scan.error) +
                        ". Answering from this index would report results from an "
                        "incomplete index. Call the index tool with repo_id and retry.";
        return false;
    }

    indexer::Coverage after = indexer::repo_coverage(store, repo_id, kBudget);
    coverage_json(after, result);
    if (before.coverage == indexer::IndexCoverage::partial) {
        result["note"] = "The index only covered individual files; a full repository scan was run first.";
    }

    if (after.coverage == indexer::IndexCoverage::full_fresh) {
        result["index_status"] = (before.coverage == indexer::IndexCoverage::not_indexed)
                                     ? "auto_indexed"
                                     : "reindexed_full";
        return true;
    }

    // IDX-04 guard: the scan completed, yet the re-validated coverage is still
    // not provably fresh. That used to surface as an empty result identical to
    // a search without hits, and every later query re-ran the same inconclusive
    // scan. State it explicitly instead, with the records that block the gate:
    // a repeated gate failure is a condition to report, not to reiterate.
    result["index_status"] = "index_stale_after_scan";
    std::string why = after.touched_records > 0
                          ? std::to_string(after.touched_records) + " tracked file(s) whose stored "
                                "knowledge no longer matches the content on disk"
                          : after.walk_failed ? "the repository root could not be re-read"
                                              : "the tracked records no longer match the certified coverage";
    result["hint"] = "A full scan just completed but coverage is still not verifiable: " + why +
                     ". Repo-level results were not answered from the index. Per-file tools "
                     "(file_content, summary) remain reliable. Check the touched paths above "
                     "(edited, moved, or unreadable since the scan) and call the index tool "
                     "with repo_id to re-scan.";
    return false;
}

// ---- pagination ----

// In-memory pagination for result arrays: keeps responses token-lean.
// Adds <prefix>_total/_returned/_truncated/_next_offset when paging applies.
void paginate_array(json& arr, const json& arguments, json& result,
                    const std::string& prefix, int default_limit) {
    int limit = (arguments.contains("limit") && arguments["limit"].is_number_integer())
                    ? arguments["limit"].get<int>() : default_limit;
    int offset = (arguments.contains("offset") && arguments["offset"].is_number_integer())
                     ? arguments["offset"].get<int>() : 0;
    if (limit < 0) limit = 0;
    if (offset < 0) offset = 0;

    int total = static_cast<int>(arr.size());
    json page = json::array();
    for (int i = offset; i < total && static_cast<int>(page.size()) < limit; ++i)
        page.push_back(arr[i]);
    int returned = static_cast<int>(page.size());
    bool truncated = offset + returned < total;
    bool paged = truncated || offset > 0 ||
                 arguments.contains("limit") || arguments.contains("offset");

    arr = std::move(page);
    if (paged) {
        result[prefix + "_total"] = total;
        result[prefix + "_returned"] = returned;
        result[prefix + "_truncated"] = truncated;
        if (truncated) result[prefix + "_next_offset"] = offset + returned;
    }
}

}
