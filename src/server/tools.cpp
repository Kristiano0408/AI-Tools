// MCP tool router: schema listing and dispatch to the per-tool handlers.
// Shared helpers live in tools_support.cpp; query handlers in tools_query.cpp;
// mutating handlers in tools_actions.cpp.

#include "server/tools.h"
#include "server/tools_internal.h"

#include "engine/knowledge_store.h"
#include "engine/repository.h"
#include "utils/perf_metrics.h"

#include <nlohmann/json.hpp>
#include <string>

using namespace nlohmann;

namespace tools {

std::string list_tools() {
    json tools = json::parse(R"json([
        {
            "name": "repository_search",
            "description": "Search the repository index for files and symbols. Prefer this over grep: results come from the persistent index (fast, structured). With repo_id omitted, searches all registered repositories. Supports directory scope and file-type filters.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Search query (substring match on paths and symbol names)"},
                    "repo_id": {"type": "string", "description": "Repository ID (optional; omit to search all registered repositories)"},
                    "scope": {"type": "string", "description": "Directory subtree to limit the search"},
                    "file_type": {"type": "string", "description": "Filter by file type (source, header, etc.)"},
                    "symbol_type": {"type": "string", "description": "Filter by symbol type (class, function, etc.)"},
                    "limit": {"type": "integer", "description": "Max results per array (default 50)"},
                    "offset": {"type": "integer", "description": "Result offset for pagination"}
                },
                "required": ["query"]
            }
        },
        {
            "name": "symbol_search",
            "description": "Look up indexed symbols by name or pattern: signature, file, line range, scope. With repo_id it also returns call-graph callers of the symbol.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Symbol name or pattern"},
                    "repo_id": {"type": "string", "description": "Repository ID (optional)"},
                    "symbol_type": {"type": "string", "description": "Filter by symbol type"},
                    "limit": {"type": "integer", "description": "Max results per array (default 50)"},
                    "offset": {"type": "integer", "description": "Result offset for pagination"}
                },
                "required": ["name"]
            }
        },
        {
            "name": "file_content",
            "description": "Inspect + read a file: metadata, language, content hash, indexed symbols, cached semantic summary, index status, and content (optional 1-based line range). Use line ranges to limit output. Stale or never-indexed files are re-parsed automatically (self-healing); normally no explicit index call is needed.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {"type": "string", "description": "Path to the file (absolute, or relative to repo root)"},
                    "repo_id": {"type": "string", "description": "Repository ID (optional; resolved from path/cwd when omitted)"},
                    "start_line": {"type": "integer", "description": "First line to return (1-based, optional)"},
                    "end_line": {"type": "integer", "description": "Last line to return (1-based inclusive, optional)"}
                },
                "required": ["file_path"]
            }
        },
        {
            "name": "dependency_graph",
            "description": "Dependency analysis from the persisted graph. Files: include/import edges (direction in/out/both, depth>1 transitive). With 'symbol': call-graph callers/callees extracted from AST call edges. Use to answer 'who calls X' or 'what breaks if I change this file'.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {"type": "string", "description": "Path to the file"},
                    "symbol": {"type": "string", "description": "Symbol name (file_path optional when given)"},
                    "repo_id": {"type": "string", "description": "Repository ID (required for symbol queries)"},
                    "direction": {"type": "string", "enum": ["in", "out", "both"], "description": "in=what target depends on, out=what depends on target"},
                    "depth": {"type": "integer", "description": "Traversal depth (1=direct, >1=transitive)"},
                    "limit": {"type": "integer", "description": "Max edges per array (default 100)"},
                    "offset": {"type": "integer", "description": "Result offset for pagination"}
                }
            }
        },
        {
            "name": "summary",
            "description": "Read or write semantic summaries. After reading a file for the first time, write a summary (responsibility, architectural role, key symbols, dependencies): it is stored with the content hash and returned by file_content while valid; edits invalidate it automatically. Omit file_path for the repository-level summary.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {"type": "string", "description": "File path (optional for repo summary)"},
                    "repo_id": {"type": "string", "description": "Repository ID"},
                    "write": {"type": "string", "description": "Summary text to store (associates with current content hash)"}
                },
                "required": ["repo_id"]
            }
        },
        {
            "name": "index",
            "description": "Index a repository or a single file (incremental, hash-driven: unchanged files are skipped). Usually NOT required: the server self-heals (auto-indexes small repos on first query, re-parses stale files inline). Use to force/warm an index, to (re)index a repository above the auto-index size gate, or after bulk changes.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "repo_id": {"type": "string", "description": "Repository ID"},
                    "path": {"type": "string", "description": "Repository root to register/index (when repo_id unknown)"},
                    "file_path": {"type": "string", "description": "Specific file to index (optional)"}
                }
            }
        },
        {
            "name": "file_tree",
            "description": "Directory structure as a nested tree (skips .git, build, node_modules, __pycache__). Omit path to use the resolved repository root.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Directory path (optional when repo_id resolves a repository)"},
                    "repo_id": {"type": "string", "description": "Repository ID"},
                    "max_depth": {"type": "integer", "description": "Maximum recursion depth (default 4)"}
                }
            }
        },
        {
            "name": "edit",
            "description": "Safe line-oriented editing: replaces exactly the requested line range, preserves everything else, then re-indexes the file and invalidates caches (summaries, dependencies, transitive header dependents). Prefer this over raw file writes so the index stays consistent.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {"type": "string", "description": "Path to the file"},
                    "repo_id": {"type": "string", "description": "Repository ID (optional; resolved from path/cwd when omitted)"},
                    "start_line": {"type": "integer", "description": "First line to replace (1-based)"},
                    "end_line": {"type": "integer", "description": "Last line to replace (1-based inclusive)"},
                    "new_content": {"type": "string", "description": "Replacement content"}
                },
                "required": ["file_path", "start_line", "end_line", "new_content"]
            }
        },
        {
            "name": "execute",
            "description": "Execute a build/test/verification command in the repository root (boundary enforced). Returns stdout, stderr, exit code, duration. Errors are never swallowed.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "command": {"type": "string", "description": "Shell command to execute"},
                    "repo_id": {"type": "string", "description": "Repository ID (command runs in its root)"}
                },
                "required": ["command", "repo_id"]
            }
        },
        {
            "name": "code_analysis",
            "description": "Analyze code for size, comments, and symbol statistics",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "file_path": {"type": "string", "description": "File path"},
                    "repo_id": {"type": "string", "description": "Repository ID"},
                    "analysis_type": {"type": "string", "description": "Type of analysis (stats, symbols)"}
                },
                "required": ["file_path"]
            }
        }
    ])json");

    return tools.dump();
}

std::string call_tool(const std::string& tool_name, const json& arguments) {
    PERF_SCOPE("tools.call_tool." + tool_name);
    PERF_COUNTER("tools.call_tool.total");
    // Per-repository store: resolved from repo_id or a path argument
    knowledge_store::KnowledgeStore store;
    std::string repo_id = resolve_repo(arguments, store);

    repository::RepoInfo repo;
    if (!repo_id.empty()) {
        repo = repository::lookup_repository(repo_id);
    }

    ToolContext ctx{store, repo_id, repo, arguments};

    if (tool_name == "repository_search") return handle_repository_search(ctx);
    if (tool_name == "symbol_search") return handle_symbol_search(ctx);
    if (tool_name == "file_content") return handle_file_content(ctx);
    if (tool_name == "dependency_graph") return handle_dependency_graph(ctx);
    if (tool_name == "file_tree") return handle_file_tree(ctx);
    if (tool_name == "code_analysis") return handle_code_analysis(ctx);
    if (tool_name == "summary") return handle_summary(ctx);
    if (tool_name == "index") return handle_index(ctx);
    if (tool_name == "edit") return handle_edit(ctx);
    if (tool_name == "execute") return handle_execute(ctx);

    return tool_error("Unknown tool: " + tool_name);
}

}
