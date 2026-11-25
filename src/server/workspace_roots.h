#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace workspace_roots {

// Client-declared workspace roots for the active MCP session (MCP Roots:
// the server polls the client with `roots/list` and refreshes on
// `notifications/roots/list_changed`). Paths are stored canonical and
// absolute, in the order announced by the client.
//
// Repository resolution and unscoped searches prefer these roots over the
// server's cwd or the whole registry, so an agent working in an IDE folder
// never silently sees results from unrelated registered projects.

using nlohmann::json;

// Replace the session roots from a `roots/list` result array
// ([{"uri":"file:///path","name":...}]). Invalid entries are skipped.
void set_from_roots_array(const json& roots_array);

// Drop all roots (new session / disconnected client).
void clear();

// Canonical absolute directories announced by the client (possibly empty).
const std::vector<std::string>& roots();
bool empty();

// Registered repositories matching the workspace roots: a repository
// matches when its root is inside a workspace root (IDE folder inside a
// repo) or a workspace root is inside the repository root (repo opened as
// a workspace folder). Empty when no roots are announced.
std::vector<std::string> workspace_matching_repos();

}
