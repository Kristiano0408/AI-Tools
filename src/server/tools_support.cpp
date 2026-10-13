// Shared helpers for the MCP tool layer: result envelopes, repository
// resolution, boundary guarding, self-healing index gate, and pagination.

#include "server/tools_internal.h"

#include "engine/indexer.h"
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
// directory), then fall back to the cwd or the single registered repository
// (so clients like VS Code never need to manage repo identities).
// Opens the per-repository store and returns its identity.
std::string resolve_repo(const json& arguments, knowledge_store::KnowledgeStore& store) {
    PERF_SCOPE("tools.resolve_repo");
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

    // Auto-detect: registered repository containing the server's working
    // directory, else the only registered repository.
    PERF_COUNTER("tools.resolve_repo.autodetect");
    auto registered = registered_repo_ids();

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
    if (registered.size() == 1) {
        repository::RepoInfo info = repository::lookup_repository(registered[0]);
        if (!info.repo_id.empty() && store.open(info.db_path)) return info.repo_id;
    }
    return "";
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

// Count indexable source files under root, stopping as soon as limit is
// exceeded (bounded walk). Returns the count (capped at limit + 1).
int count_indexable_files(const std::string& root_path, int limit) {
    PERF_SCOPE("tools.count_indexable_files");
    static const std::vector<std::string> supported = {
        ".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".cu", ".cuh", ".py"};
    std::filesystem::path root(root_path);
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return 0;

    int count = 0;
    for (const auto& entry : it) {
        if (!entry.is_regular_file()) continue;
        std::string path = entry.path().string();
        if (path.find("/build/") != std::string::npos ||
            path.find("/.git/") != std::string::npos ||
            path.find("/node_modules/") != std::string::npos ||
            path.find("/third_party/") != std::string::npos) {
            continue;
        }
        std::string ext = path_utils::get_extension(path);
        if (ext.empty()) continue;
        ext = "." + ext;
        if (std::find(supported.begin(), supported.end(), ext) == supported.end()) continue;
        if (++count > limit) return count;
    }
    return count;
}

} // namespace

// Self-healing gate for repository-level queries: a never-indexed repository
// below the size gate is indexed automatically on first use; oversized
// repositories are reported as not_indexed with a hint instead of blocking
// the call with a minutes-long scan. Returns true when the repository is
// (or just became) indexed.
bool ensure_repo_indexed(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                         const repository::RepoInfo& repo, json& result) {
    PERF_SCOPE("tools.ensure_repo_indexed");
    if (repo_id.empty() || repo.root_path.empty()) return true; // nothing to ensure

    auto state = store.query("SELECT status FROM indexing_state WHERE repo_id = ?", repo_id);
    if (!state.empty()) {
        PERF_COUNTER("tools.ensure_repo_indexed.already_indexed");
        return true; // already indexed at least once
    }

    constexpr int kAutoIndexLimit = 10000;
    int files = count_indexable_files(repo.root_path, kAutoIndexLimit);
    if (files <= kAutoIndexLimit) {
        PERF_COUNTER("tools.ensure_repo_indexed.auto_indexed");
        indexer::index_repository(store, repo_id);
        result["index_status"] = "auto_indexed";
        return true;
    }

    result["index_status"] = "not_indexed";
    result["hint"] = "Repository has " + std::to_string(files) +
                     "+ indexable files and was not auto-indexed. Call the index tool with repo_id first.";
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
