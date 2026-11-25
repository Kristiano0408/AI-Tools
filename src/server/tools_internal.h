#pragma once

// Internal shared declarations for the tool layer. Not part of the public
// MCP API (that is tools.h); these are the building blocks the per-tool
// handler files (tools_query.cpp, tools_actions.cpp) share.

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "engine/knowledge_store.h"
#include "engine/repository.h"

namespace tools {

using nlohmann::json;
// Shared per-call context: the resolved repository, its opened store, and
// the tool arguments. Built once by call_tool() and passed to each handler.
struct ToolContext {
    knowledge_store::KnowledgeStore& store;
    const std::string& repo_id;
    const repository::RepoInfo& repo;
    const json& arguments;
};

// ---- shared helpers (implemented in tools_support.cpp) ----

std::string tool_result(const json& payload);
std::string tool_error(const std::string& message);

std::vector<std::string> registered_repo_ids();

// Gate for repository-level queries: guarantees that the persisted index
// really covers the whole repository before a repo-level query reads it, and
// describes the coverage in the result (index_status + index_coverage).
// Returns false when the query must not be answered from the index.
bool ensure_repo_indexed(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                         const repository::RepoInfo& repo, json& result);

// Attach the current coverage of a repository index to a tool result so an
// agent can tell a complete index from a partial or stale one.
void report_index_coverage(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                           json& result, const std::string* status_override = nullptr);

void paginate_array(json& arr, const json& arguments, json& result,
                    const std::string& prefix, int default_limit);

std::string resolve_repo(const json& arguments, knowledge_store::KnowledgeStore& store);

// When several registered repositories match the client-declared workspace
// roots, resolve_repo picks one and records the alternatives here so the
// tool layer can surface them (the agent should then pass repo_id).
json& workspace_ambiguity_hints();

// Repositories an unscoped query may read: those matching the client's MCP
// workspace roots when announced, else all registered ones. Tags `result`
// with search_scope/workspace_roots and any ambiguity candidates.
std::vector<std::string> search_target_repos(json& result);

std::string guarded_path(const repository::RepoInfo& repo, const std::string& file_path);

// ---- tool handlers (query tools in tools_query.cpp, action tools in tools_actions.cpp) ----

std::string handle_repository_search(const ToolContext& ctx);
std::string handle_symbol_search(const ToolContext& ctx);
std::string handle_file_content(const ToolContext& ctx);
std::string handle_dependency_graph(const ToolContext& ctx);
std::string handle_file_tree(const ToolContext& ctx);
std::string handle_code_analysis(const ToolContext& ctx);

std::string handle_summary(const ToolContext& ctx);
std::string handle_index(const ToolContext& ctx);
std::string handle_edit(const ToolContext& ctx);
std::string handle_execute(const ToolContext& ctx);

}
