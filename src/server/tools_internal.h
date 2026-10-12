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

bool ensure_repo_indexed(knowledge_store::KnowledgeStore& store, const std::string& repo_id,
                         const repository::RepoInfo& repo, json& result);

void paginate_array(json& arr, const json& arguments, json& result,
                    const std::string& prefix, int default_limit);

std::string resolve_repo(const json& arguments, knowledge_store::KnowledgeStore& store);

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
