#pragma once

// MCP Resources interface: exposes repository context (indexed files,
// semantic summaries, repository index overviews) as protocol resources, so
// clients (Claude Desktop, Cursor, VS Code) can attach codebase context to
// the model without spending tool calls.

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace resources {

// Resource-layer failure: code is a JSON-RPC error code following the MCP
// resources spec (-32602 invalid params, -32002 resource not found,
// -32603 internal error). The server layer maps it to an error response.
struct resource_error : public std::runtime_error {
    int code;
    resource_error(int code, const std::string& message) : std::runtime_error(message), code(code) {}
};

// MCP `resources/list`: enumerate resources of one or all registered
// repositories. Params (all optional): repo_id, path, cursor (offset
// pagination), limit (page size, default 200).
nlohmann::json list(const nlohmann::json& params);

// MCP `resources/templates/list`: URI templates for parameterized resources.
nlohmann::json templates_list(const nlohmann::json& params);

// MCP `resources/read`: read one resource by URI. Schemes:
//   repo://<repo_id>/file/<repo-relative-path>  — text file inside the repo
//   repo://<repo_id>/summary                    — stored repository summary
//   repo://<repo_id>/index                      — machine-readable overview
//   file:///<absolute-path>                     — only when inside a
//                                                 registered repository
nlohmann::json read(const nlohmann::json& params);

}
