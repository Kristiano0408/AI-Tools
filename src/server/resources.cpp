// MCP Resources implementation: exposes repository context as protocol
// resources so clients can attach it to a prompt without any tool call.
//
// URI schemes (custom `repo://` + spec-friendly `file://`):
//   repo://<repo_id>/file/<rel>          text content of a repository file
//   repo://<repo_id>/summary/<rel>       cached semantic summary of that file
//   repo://<repo_id>/summary             stored repository-level summary
//   repo://<repo_id>/index               machine-readable repository overview
//   repo://registry/index                index of all registered repositories
//   file:///<absolute path>              accepted only when the path lives
//                                        inside a registered repository

#include "server/resources.h"

#include "engine/knowledge_store.h"
#include "engine/repository.h"
#include "engine/summary_store.h"
#include "server/workspace_roots.h"
#include "utils/file_utils.h"
#include "utils/path_utils.h"
#include "utils/perf_metrics.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;

namespace resources {

namespace {

constexpr int kMaxListEntries = 5000; // hard cap on enumerated resources
constexpr int kDefaultPageLimit = 200;
constexpr std::uintmax_t kMaxReadBytes = 1024 * 1024; // 1 MiB per text resource
constexpr int kOverviewFileLimit = 200;

// Directories that never belong in a repository context listing.
bool skipped_dir(const std::string& name) {
    static const std::vector<std::string> skipped = {
        ".git", "build", "build-debug", "build-release", "node_modules",
        "__pycache__", "third_party", ".ai-tools", "dist", "venv", ".venv"};
    return std::find(skipped.begin(), skipped.end(), name) != skipped.end();
}

// ---- URI helpers ----

std::string encode_uri_path(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decode_uri_path(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]), lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

struct ParsedUri {
    std::string scheme;   // "repo" or "file"
    std::string authority; // repo_id for repo://
    std::string path;     // decoded, leading '/' kept
};

ParsedUri parse_uri(const std::string& uri) {
    size_t sep = uri.find("://");
    if (sep == std::string::npos) throw resource_error(-32602, "Invalid resource URI: " + uri);

    ParsedUri p;
    p.scheme = uri.substr(0, sep);
    std::string rest = uri.substr(sep + 3);

    size_t slash = rest.find('/');
    if (slash == std::string::npos) {
        p.authority = rest;
        p.path = "/";
    } else {
        p.authority = rest.substr(0, slash);
        p.path = decode_uri_path(rest.substr(slash));
    }
    return p;
}

std::string iso8601(std::time_t t) {
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ---- repository lookup ----

std::vector<std::string> registered_ids() {
    std::vector<std::string> ids;
    knowledge_store::KnowledgeStore registry;
    if (registry.open(repository::registry_db_path())) {
        for (const auto& row : registry.query("SELECT repo_id FROM repositories ORDER BY name")) {
            if (!row.empty()) ids.push_back(row[0]);
        }
    }
    return ids;
}

// Resolve the repository a resource request targets: explicit repo_id, else a
// `path` argument (registered root containing it, else auto-registered), else
// the client-declared MCP workspace roots (roots/list), then — only when no
// roots are announced — the single registered repository. Returns RepoInfo
// (empty repo_id if none). Unrelated repositories never leak in once the
// client has announced its workspace.
repository::RepoInfo resolve(const json& params) {
    if (params.contains("repo_id") && !params["repo_id"].is_null()) {
        if (!params["repo_id"].is_string())
            throw resource_error(-32602, "Invalid params: repo_id must be a string");
        return repository::lookup_repository(params["repo_id"].get<std::string>());
    }
    if (params.contains("path") && !params["path"].is_null()) {
        if (!params["path"].is_string())
            throw resource_error(-32602, "Invalid params: path must be a string");
        std::error_code ec;
        std::filesystem::path p = std::filesystem::weakly_canonical(params["path"].get<std::string>(), ec);
        if (ec) return {};
        std::string dir = std::filesystem::is_directory(p) ? p.string() : p.parent_path().string();

        for (const auto& id : registered_ids()) {
            repository::RepoInfo info = repository::lookup_repository(id);
            if (!info.repo_id.empty() && path_utils::is_within_repository(info.root_path, dir))
                return info;
        }
        return repository::get_or_create_repository(dir);
    }
    auto ids = registered_ids();
    if (!workspace_roots::empty()) {
        // Workspace roots announced: only repositories matching them qualify.
        auto matches = workspace_roots::workspace_matching_repos();
        for (const auto& id : matches) {
            repository::RepoInfo info = repository::lookup_repository(id);
            if (!info.repo_id.empty()) return info;
        }
        return {};
    }
    if (ids.size() == 1) return repository::lookup_repository(ids[0]);
    return {};
}

// The repository an absolute path belongs to (for file:// URIs).
repository::RepoInfo owner_of(const std::string& abs_path) {
    for (const auto& id : registered_ids()) {
        repository::RepoInfo info = repository::lookup_repository(id);
        if (!info.repo_id.empty() && path_utils::is_within_repository(info.root_path, abs_path))
            return info;
    }
    return {};
}

// Repository lookup: unknown ids resolve to an empty RepoInfo.

// Repository root -> guarded absolute path for a repo-relative URI path
// (empty when the relative path escapes the repository root).
std::string guarded_abs(const repository::RepoInfo& repo, const std::string& rel) {
    if (rel.empty()) return "";
    std::error_code ec;
    std::filesystem::path abs =
        std::filesystem::weakly_canonical(std::filesystem::path(repo.root_path) / rel, ec);
    if (ec) return "";
    if (!path_utils::is_within_repository(repo.root_path, abs.string())) return "";
    return abs.string();
}

// rel path as stored in a repo-relative URI
std::string to_rel(const repository::RepoInfo& repo, const std::string& abs) {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(abs, repo.root_path, ec);
    if (ec) return "";
    return rel.string();
}

std::string mime_for(const std::string& path) {
    std::string ext = path_utils::get_extension(path);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "md" || ext == "markdown") return "text/markdown";
    if (ext == "json") return "application/json";
    if (ext == "py") return "text/x-python";
    if (ext == "c" || ext == "h") return "text/x-c";
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "hpp" || ext == "hh") return "text/x-c++";
    if (ext == "cu" || ext == "cuh") return "text/x-cuda";
    if (ext == "txt" || ext == "cmake" || ext == "sh" || ext == "yml" || ext == "yaml" ||
        ext == "toml" || ext == "ini" || ext == "sql" || ext == "ts" || ext == "js")
        return "text/plain";
    return "text/plain";
}

// Resource descriptor (MCP Resource shape).
json make_resource(const std::string& uri, const std::string& name, const std::string& description,
                   const std::string& mime, std::intmax_t size, double priority,
                   const std::string& last_modified = "") {
    json r;
    r["uri"] = uri;
    r["name"] = name;
    r["title"] = name;
    r["description"] = description;
    r["mimeType"] = mime;
    if (size >= 0) r["size"] = size;
    json ann;
    ann["audience"] = json::array({"assistant"});
    ann["priority"] = priority;
    if (!last_modified.empty()) ann["lastModified"] = last_modified;
    r["annotations"] = ann;
    return r;
}

// ---- enumeration ----

// Repository resources: index + summary + one text resource per regular file
// (plus a summary resource per file with a stored summary). Bounded walk so a
// repo-level list never explodes memory.
void enumerate_repo(const repository::RepoInfo& repo, std::vector<json>& out) {
    if (out.size() >= static_cast<size_t>(kMaxListEntries)) return;

    knowledge_store::KnowledgeStore store;
    repository::open_repo_store(repo.root_path, store);

    out.push_back(make_resource("repo://" + repo.repo_id + "/index", repo.name + " index",
                               "Machine-readable overview of repository " + repo.name +
                               " (files, symbols, dependency counts, index status)",
                               "application/json", -1, 0.9));

    if (!summary_store::get_repo_summary(store, repo.repo_id).empty()) {
        out.push_back(make_resource("repo://" + repo.repo_id + "/summary", repo.name + " summary",
                                    "Stored semantic summary of repository " + repo.name,
                                    "text/markdown", -1, 0.9));
    }

    // Files with a valid stored semantic summary.
    struct SumInfo { std::string rel; };
    std::vector<SumInfo> summarized;
    for (const auto& row : store.query(
             "SELECT DISTINCT f.path FROM summaries s JOIN files f ON f.file_id = s.file_id "
             "WHERE f.repo_id = ? AND s.file_id != 0 AND s.is_valid = 1 ORDER BY f.path",
             repo.repo_id)) {
        std::string rel = to_rel(repo, row[0]);
        if (!rel.empty()) summarized.push_back({rel});
    }

    // Bounded filesystem walk: every readable text file becomes a resource.
    std::vector<std::string> rels;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        repo.root_path, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (const auto& entry : it) {
        if (ec) break;
        if (entry.is_directory()) {
            if (skipped_dir(entry.path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file()) continue;
        std::string abs = entry.path().string();
        std::string rel = to_rel(repo, abs);
        if (rel.empty()) continue;
        if (skipped_dir(path_utils::get_basename(rel))) continue;
        if (file_utils::is_binary(abs)) continue;
        rels.push_back(rel);
        if (rels.size() >= static_cast<size_t>(kMaxListEntries)) break;
    }
    std::sort(rels.begin(), rels.end());

    for (const auto& rel : rels) {
        if (out.size() >= static_cast<size_t>(kMaxListEntries)) return;
        std::string abs = path_utils::resolve_path(repo.root_path, rel);
        std::intmax_t size = -1;
        std::string modified;
        std::error_code sec;
        if (auto sz = std::filesystem::file_size(abs, sec); !sec) size = static_cast<std::intmax_t>(sz);
        std::time_t mtime = file_utils::get_modification_time(abs);
        if (mtime != 0) modified = iso8601(mtime);
        out.push_back(make_resource("repo://" + repo.repo_id + "/file/" + encode_uri_path(rel),
                                    rel, "Repository file: " + rel, mime_for(rel), size, 0.4, modified));
    }

    for (const auto& s : summarized) {
        if (out.size() >= static_cast<size_t>(kMaxListEntries)) return;
        out.push_back(make_resource("repo://" + repo.repo_id + "/summary/" + encode_uri_path(s.rel),
                                    s.rel + " summary", "Stored semantic summary of " + s.rel,
                                    "text/markdown", -1, 0.7));
    }
}

void enumerate_registry(std::vector<json>& out) {
    out.push_back(make_resource("repo://registry/index", "Repository registry",
                                "All repositories registered with the ai-tools knowledge engine",
                                "application/json", -1, 0.9));
}

// ---- pagination ----

// Cursor is the numeric offset of the next page (spec allows an opaque token).
json page(std::vector<json>& items, const json& params) {
    int offset = 0;
    if (params.contains("cursor") && !params["cursor"].is_null()) {
        if (!params["cursor"].is_string())
            throw resource_error(-32602, "Invalid params: cursor must be a string");
        try {
            offset = std::max(0, std::stoi(params["cursor"].get<std::string>()));
        } catch (...) {
            throw resource_error(-32602, "Invalid params: cursor is not a numeric offset");
        }
    }
    int limit = kDefaultPageLimit;
    if (params.contains("limit") && !params["limit"].is_null()) {
        if (!params["limit"].is_number_integer())
            throw resource_error(-32602, "Invalid params: limit must be an integer");
        limit = std::max(1, std::min(params["limit"].get<int>(), kMaxListEntries));
    }

    json res = json::object();
    json arr = json::array();
    for (size_t i = static_cast<size_t>(offset);
         i < items.size() && static_cast<int>(arr.size()) < limit; ++i)
        arr.push_back(items[i]);
    res["resources"] = arr;
    size_t next = static_cast<size_t>(offset) + arr.size();
    if (next < items.size()) res["nextCursor"] = std::to_string(next);
    return res;
}

// ---- read handlers ----

json read_repo_file(const repository::RepoInfo& repo, const std::string& rel) {
    std::string abs = guarded_abs(repo, rel);
    if (abs.empty())
        throw resource_error(-32002, "Resource not found (path escapes repository root): " + rel);
    if (!path_utils::file_exists(abs))
        throw resource_error(-32002, "Resource not found: " + rel);
    if (file_utils::is_binary(abs))
        throw resource_error(-32002, "Resource not available as text (binary file): " + rel);

    std::error_code ec;
    std::uintmax_t size = std::filesystem::file_size(abs, ec);
    std::string text;
    bool truncated = false;
    if (ec || size <= kMaxReadBytes) {
        text = file_utils::read_file(abs);
    } else {
        std::string all = file_utils::read_file(abs);
        text = all.substr(0, static_cast<size_t>(kMaxReadBytes));
        truncated = true;
    }

    json content;
    content["uri"] = "repo://" + repo.repo_id + "/file/" + encode_uri_path(rel);
    content["mimeType"] = mime_for(rel);
    content["text"] = truncated ? text + "\n... [resource truncated at " +
                                    std::to_string(kMaxReadBytes) + " bytes; read the file directly]"
                                : text;
    json ann;
    ann["audience"] = json::array({"assistant"});
    ann["priority"] = 0.4;
    content["annotations"] = ann;
    return content;
}

json read_repo_summary(const repository::RepoInfo& repo, knowledge_store::KnowledgeStore& store,
                       const std::string& rel) {
    std::string summary;
    std::string uri;
    if (rel.empty()) {
        summary = summary_store::get_repo_summary(store, repo.repo_id);
        uri = "repo://" + repo.repo_id + "/summary";
        if (summary.empty())
            throw resource_error(-32002, "No repository summary stored for " + repo.repo_id +
                                            " (write one with the summary tool)");
    } else {
        std::string abs = guarded_abs(repo, rel);
        if (abs.empty())
            throw resource_error(-32002, "Resource not found (path escapes repository root): " + rel);
        summary = summary_store::get_file_summary(store, repo.repo_id, abs);
        uri = "repo://" + repo.repo_id + "/summary/" + encode_uri_path(rel);
        if (summary.empty())
            throw resource_error(-32002,
                                 "No valid summary for " + rel +
                                     " (missing, or stale for the current content hash)");
    }

    json content;
    content["uri"] = uri;
    content["mimeType"] = "text/markdown";
    content["text"] = summary;
    json ann;
    ann["audience"] = json::array({"assistant"});
    ann["priority"] = 0.9;
    content["annotations"] = ann;
    return content;
}

json read_repo_index(const repository::RepoInfo& repo, knowledge_store::KnowledgeStore& store) {
    json body;
    body["repo_id"] = repo.repo_id;
    body["name"] = repo.name;
    body["root_path"] = repo.root_path;
    body["updated_at"] = repo.updated_at;

    auto state = store.query(
        "SELECT status, files_indexed, files_total, last_full_index_at FROM indexing_state WHERE repo_id = ?",
        repo.repo_id);
    if (state.empty()) {
        body["index_status"] = "not_indexed";
        body["hint"] = "Call the index tool with repo_id, or any repo-level tool (the server self-indexes).";
    } else {
        body["index_status"] = state[0][0];
        try {
            body["files_indexed"] = std::stoi(state[0][1]);
            body["files_total"] = std::stoi(state[0][2]);
        } catch (...) {}
        body["last_full_index_at"] = state[0][3];
    }

    auto files = store.query(
        "SELECT path, language, file_type, line_count, content_hash FROM files WHERE repo_id = ? "
        "AND is_deleted = 0 ORDER BY path LIMIT " + std::to_string(kOverviewFileLimit),
        repo.repo_id);
    for (const auto& f : files) {
        std::string rel = to_rel(repo, f[0]);
        int symbols = 0;
        auto counts = store.query(
            "SELECT COUNT(*) FROM symbols WHERE file_id IN (SELECT file_id FROM files WHERE repo_id = ? AND path = ?)",
            repo.repo_id, f[0]);
        if (!counts.empty()) {
            try { symbols = std::stoi(counts[0][0]); } catch (...) {}
        }
        json entry;
        entry["path"] = rel.empty() ? f[0] : rel;
        entry["uri"] = "repo://" + repo.repo_id + "/file/" + encode_uri_path(rel.empty() ? f[0] : rel);
        entry["language"] = f[1];
        entry["file_type"] = f[2];
        try { entry["line_count"] = std::stoi(f[3]); } catch (...) {}
        entry["content_hash"] = f[4];
        entry["symbols"] = symbols;
        body["files"].push_back(entry);
    }
    body["file_limit"] = kOverviewFileLimit;

    for (const auto& c : store.query(
             "SELECT symbol_type, COUNT(*) FROM symbols GROUP BY symbol_type ORDER BY symbol_type")) {
        try { body["symbol_counts"][c[0]] = std::stoi(c[1]); } catch (...) {}
    }
    for (const auto& c : store.query(
             "SELECT dep_type, COUNT(*) FROM dependencies GROUP BY dep_type ORDER BY dep_type")) {
        try { body["dependency_counts"][c[0]] = std::stoi(c[1]); } catch (...) {}
    }

    json content;
    content["uri"] = "repo://" + repo.repo_id + "/index";
    content["mimeType"] = "application/json";
    content["text"] = body.dump(2);
    json ann;
    ann["audience"] = json::array({"assistant"});
    ann["priority"] = 0.9;
    content["annotations"] = ann;
    return content;
}

json read_registry_index() {
    json body = json::object();
    body["registry_db"] = repository::registry_db_path();
    json repos = json::array();
    knowledge_store::KnowledgeStore registry;
    if (registry.open(repository::registry_db_path())) {
        for (const auto& row : registry.query(
                 "SELECT repo_id, root_path, name, created_at, updated_at FROM repositories ORDER BY name")) {
            json r;
            r["repo_id"] = row[0];
            r["name"] = row[2];
            r["root_path"] = row[1];
            r["index_uri"] = "repo://" + row[0] + "/index";
            r["created_at"] = row[3];
            r["updated_at"] = row[4];
            repos.push_back(r);
        }
    }
    body["repositories"] = repos;

    json content;
    content["uri"] = "repo://registry/index";
    content["mimeType"] = "application/json";
    content["text"] = body.dump(2);
    json ann;
    ann["audience"] = json::array({"assistant"});
    ann["priority"] = 0.9;
    content["annotations"] = ann;
    return content;
}

} // namespace

json list(const json& params) {
    PERF_SCOPE("resources.list");
    if (!params.is_null() && !params.is_object())
        throw resource_error(-32602, "Invalid params: resources/list params must be an object");
    json p = params.is_object() ? params : json::object();

    std::vector<json> items;

    // Explicit single repository (repo_id or a path inside it).
    repository::RepoInfo single = resolve(p);
    if (!single.repo_id.empty()) {
        enumerate_repo(single, items);
        PERF_VALUE("resources.list.entries", items.size());
        return page(items, p);
    }

    // "all" mode: registry overview plus every registered repository.
    if (p.contains("uri")) {
        if (!p["uri"].is_string())
            throw resource_error(-32602, "Invalid params: uri must be a string");
        ParsedUri u = parse_uri(p["uri"].get<std::string>());
        if (u.scheme == "repo") {
            if (u.authority == "registry") {
                enumerate_registry(items);
                return page(items, p);
            }
            repository::RepoInfo info = repository::lookup_repository(u.authority);
            if (info.repo_id.empty())
                throw resource_error(-32002, "Unknown repository in resource URI: " + u.authority);
            enumerate_repo(info, items);
            return page(items, p);
        }
    }

    enumerate_registry(items);
    for (const auto& id : registered_ids()) {
        repository::RepoInfo info = repository::lookup_repository(id);
        if (info.repo_id.empty()) continue;
        enumerate_repo(info, items);
        if (items.size() >= static_cast<size_t>(kMaxListEntries)) break;
    }
    PERF_VALUE("resources.list.entries", items.size());
    return page(items, p);
}

json templates_list(const json& params) {
    if (!params.is_null() && !params.is_object())
        throw resource_error(-32602, "Invalid params: resources/templates/list params must be an object");

    json out = json::object();
    out["resourceTemplates"] = json::array({
        {{"uriTemplate", "repo://{repo_id}/file/{path}"},
         {"name", "Repository file"},
         {"title", "Repository file"},
         {"description", "Text content of a file inside a registered repository (repo_id from resources/list, path relative to the repository root)"},
         {"mimeType", "text/plain"}},
        {{"uriTemplate", "repo://{repo_id}/summary/{path}"},
         {"name", "File semantic summary"},
         {"title", "File semantic summary"},
         {"description", "Cached semantic summary of a repository file, valid while the content hash matches"},
         {"mimeType", "text/markdown"}},
        {{"uriTemplate", "repo://{repo_id}/summary"},
         {"name", "Repository summary"},
         {"title", "Repository summary"},
         {"description", "Stored repository-level semantic summary"},
         {"mimeType", "text/markdown"}},
        {{"uriTemplate", "repo://{repo_id}/index"},
         {"name", "Repository index overview"},
         {"title", "Repository index overview"},
         {"description", "Machine-readable repository overview: files, symbols, dependency counts, index status"},
         {"mimeType", "application/json"}},
        {{"uriTemplate", "repo://registry/index"},
         {"name", "Repository registry"},
         {"title", "Repository registry"},
         {"description", "Every repository registered with the knowledge engine"},
         {"mimeType", "application/json"}},
        {{"uriTemplate", "file:///{path}"},
         {"name", "File by absolute path"},
         {"title", "File by absolute path"},
         {"description", "Repository file addressed by absolute path; only readable when the path is inside a registered repository"},
         {"mimeType", "text/plain"}},
    });
    return out;
}

json read(const json& params) {
    PERF_SCOPE("resources.read");
    if (!params.is_object())
        throw resource_error(-32602, "Invalid params: resources/read requires a params object");
    if (!params.contains("uri") || !params["uri"].is_string())
        throw resource_error(-32602, "Invalid params: missing required string parameter 'uri'");

    std::string raw_uri = params["uri"].get<std::string>();
    ParsedUri u = parse_uri(raw_uri);

    json result = json::object();
    auto add = [&](json content) {
        // Spec: `contents` is an array of content blocks.
        result["contents"] = json::array({content});
        return result;
    };

    if (u.scheme == "repo") {
        if (u.authority == "registry") {
            if (u.path != "/index")
                throw resource_error(-32002, "Unknown registry resource: " + raw_uri);
            return add(read_registry_index());
        }

        repository::RepoInfo repo = repository::lookup_repository(u.authority);
        if (repo.repo_id.empty())
            throw resource_error(-32002, "Unknown repository in resource URI: " + u.authority);

        knowledge_store::KnowledgeStore store;
        if (!repository::open_repo_store(repo.root_path, store))
            throw resource_error(-32603, "Failed to open the knowledge store for repository: " + u.authority);

        // Path segments after the repo authority.
        std::string path = u.path;
        if (!path.empty() && path[0] == '/') path.erase(0, 1);

        if (path.empty() || path == "index") return add(read_repo_index(repo, store));
        if (path == "summary") return add(read_repo_summary(repo, store, ""));
        if (path.rfind("summary/", 0) == 0) return add(read_repo_summary(repo, store, path.substr(8)));
        if (path.rfind("file/", 0) == 0) return add(read_repo_file(repo, path.substr(5)));

        throw resource_error(-32002, "Unknown repository resource: " + raw_uri);
    }

    if (u.scheme == "file") {
        std::string abs = u.path;
        if (abs.empty())
            throw resource_error(-32602, "Invalid params: file:// resource URI has no path");
        std::error_code ec;
        abs = std::filesystem::weakly_canonical(abs, ec).string();
        if (ec) throw resource_error(-32002, "Resource not found: " + raw_uri);

        repository::RepoInfo repo = owner_of(abs);
        if (repo.repo_id.empty())
            throw resource_error(-32002,
                                 "Resource not found: " + abs +
                                     " is not inside a registered repository (register it with `index` + path, or pass repo_id)");
        std::string rel = to_rel(repo, abs);
        if (rel.empty()) throw resource_error(-32603, "Failed to resolve repository-relative path: " + abs);
        return add(read_repo_file(repo, rel));
    }

    throw resource_error(-32002, "Unsupported resource URI scheme: " + u.scheme);
}

}
