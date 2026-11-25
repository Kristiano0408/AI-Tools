// MCP Roots support: client-declared workspace directories for the active
// session. The server polls the client with `roots/list` after initialize
// (when the client declares the `roots` capability) and re-polls on
// `notifications/roots/list_changed`, as required by the MCP spec:
//
//   Server -> Client: {"method":"roots/list"}          (server request)
//   Client -> Server: {"result":{"roots":[{"uri":"file:///...", ...}]}}
//   Client -> Server: {"method":"notifications/roots/list_changed"}
//
// Repository resolution and unscoped searches prefer these roots over the
// server's cwd or the whole registry, so an agent working inside an IDE
// workspace never silently sees results from unrelated registered projects.

#include "server/workspace_roots.h"

#include "engine/knowledge_store.h"
#include "engine/repository.h"
#include "utils/path_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace workspace_roots {
namespace {

// Decode a file:// URI into a canonical absolute local path.
// Returns "" for non-file schemes, remote authorities, or undecodable paths.
std::string root_uri_to_path(const std::string& uri) {
    const std::string scheme = "file://";
    if (uri.rfind(scheme, 0) != 0) return "";
    std::string rest = uri.substr(scheme.size()); // [authority][/path]

    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "" : rest.substr(slash);
    if (!authority.empty() && authority != "localhost") return ""; // remote host: reject
    if (path.empty()) return "";

    // Percent-decode (%XX).
    std::string decoded;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() &&
            std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return c - 'A' + 10;
            };
            decoded.push_back(static_cast<char>(hex(path[i + 1]) * 16 + hex(path[i + 2])));
            i += 2;
        } else {
            decoded.push_back(path[i]);
        }
    }

    if (!decoded.starts_with("/")) return ""; // local absolute paths only

    std::error_code ec;
    std::string canon = std::filesystem::weakly_canonical(decoded, ec).string();
    if (ec) return "";
    return canon;
}

} // namespace

namespace {

// Canonical absolute directories in the order announced by the client.
std::vector<std::string>& roots_ref() {
    static std::vector<std::string> roots;
    return roots;
}

} // namespace

void set_from_roots_array(const json& roots_array) {
    auto& roots = roots_ref();
    roots.clear();
    if (!roots_array.is_array()) return;
    for (const auto& entry : roots_array) {
        if (!entry.is_object() || !entry.contains("uri") || !entry["uri"].is_string())
            continue;
        std::string path = root_uri_to_path(entry["uri"].get<std::string>());
        if (path.empty()) continue;
        if (std::find(roots.begin(), roots.end(), path) == roots.end())
            roots.push_back(path);
    }
}

void clear() { roots_ref().clear(); }

const std::vector<std::string>& roots() { return roots_ref(); }

bool empty() { return roots_ref().empty(); }

std::vector<std::string> workspace_matching_repos() {
    std::vector<std::string> matches;
    if (roots_ref().empty()) return matches;

    knowledge_store::KnowledgeStore registry;
    if (!registry.open(repository::registry_db_path())) return matches;

    for (const auto& row : registry.query("SELECT repo_id, root_path FROM repositories ORDER BY updated_at")) {
        if (row.size() < 2 || row[0].empty()) continue;
        for (const auto& wr : roots_ref()) {
            // Match either direction: the client opened a folder inside the
            // repository, or a repository inside the announced folder.
            if (path_utils::is_within_repository(wr, row[1]) ||
                path_utils::is_within_repository(row[1], wr)) {
                matches.push_back(row[0]);
                break;
            }
        }
    }
    return matches;
}

} // namespace workspace_roots
