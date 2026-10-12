# AI-Tools — Repository Intelligence MCP Server

## Architecture Overview

AI-Tools is a production-quality Model Context Protocol (MCP) server that provides an AI coding agent with **persistent, structured knowledge** of a code repository. It is not a collection of stateless filesystem utilities — it is an **internal repository knowledge engine** that owns, maintains, and reuses repository intelligence across tool calls and server restarts.

---

## Design Philosophy

| Principle | Implementation |
|-----------|----------------|
| **Knowledge ownership** | The MCP server owns all repository state. The AI agent never rebuilds knowledge manually. |
| **Incremental over full** | Changed files trigger targeted refreshes, never full rescans. |
| **Hash-driven validity** | Every cached artifact is tied to a file content hash. Staleness is detected automatically. |
| **Repository isolation** | Each repository has a unique identity. Knowledge from Repository A never leaks to Repository B. |
| **Graceful degradation** | A parsing failure in one file never breaks the rest of the repository. |

---

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    MCP Client (AI Agent)                    │
└──────────────────────┬──────────────────────────────────────┘
                        │  JSON-RPC over stdio
┌──────────────────────┴──────────────────────────────────────┐
│                   AI-Tools MCP Server                       │
│                                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │  tree()  │  │ inspect()│  │  read()  │  │ search() │  │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │  deps()  │  │  edit()  │  │ execute()│  │ summary()│  │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │              Tool Router / Validation                │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │           Repository Knowledge Engine                │  │
│  │                                                      │  │
│  │  ┌─────────────┐  ┌──────────────┐  ┌───────────┐  │  │
│  │  │ Repository  │  │ Knowledge    │  │  Indexer  │  │  │
│  │  │ Identity    │  │ Store        │  │ (tree-    │  │  │
│  │  │ Manager     │  │ (SQLite)     │  │  sitter)  │  │  │
│  │  └─────────────┘  └──────────────┘  └───────────┘  │  │
│  │  ┌─────────────┐  ┌──────────────┐  ┌───────────┐  │  │
│  │  │ Content     │  │ Cache        │  │  Deps     │  │  │
│  │  │ Hasher      │  │ Manager      │  │  Analyzer │  │  │
│  │  └─────────────┘  └──────────────┘  └───────────┘  │  │
│  │  ┌─────────────┐  ┌──────────────┐                 │  │
│  │  │ Semantic    │  │  Command     │                 │  │
│  │  │ Summary     │  │  Executor    │                 │  │
│  │  │ Store       │  │             │                 │  │
│  │  └─────────────┘  └──────────────┘                 │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                        │
┌──────────────────────┴──────────────────────────────────────┐
│              Persistent Knowledge Store                     │
│              (SQLite — one file per repository)             │
│                                                             │
│  tables:                                                    │
│    repositories   — repo identity & metadata               │
│    files          — file metadata (path, hash, lang, etc.) │
│    symbols        — parsed symbol info                     │
│    dependencies   — dependency graph edges                 │
│    summaries      — semantic summaries with validity       │
│    indexing_state — last-indexed timestamps & status       │
└─────────────────────────────────────────────────────────────┘
```

---

## Component Details

### 1. Repository Identity Manager

**Purpose**: Distinguish repositories and associate knowledge with the correct one.

**Mechanism**:
- Each repository root is assigned a stable identity derived from its absolute path
- The `repositories` table stores: `repo_id`, `root_path`, `name`, `created_at`, `updated_at`
- Repository boundaries are enforced on every tool call — operations cannot escape the target root
- Multiple repositories can coexist without cross-contamination

### 2. Knowledge Store (SQLite)

**Purpose**: Persistent, structured storage for all repository intelligence.

**Design**:
- One SQLite database file per repository, stored in a deterministic location (`~/.ai-tools/repos/<repo_hash>/knowledge.db`)
- External to the source repository — no pollution of the working tree
- All tables use content-hash associations for validity tracking
- Indexed columns for efficient lookup (path, symbol name, dependency edges)

**Schema**:

```
repositories
  └─ repo_id (PK), root_path, name, created_at, updated_at

files
  └─ file_id (PK), repo_id (FK), path, content_hash, language,
     file_type, size_bytes, line_count, last_indexed_at,
     is_binary, is_deleted

symbols
  └─ symbol_id (PK), file_id (FK), name, symbol_type, signature,
     start_line, end_line, scope, is_declaration, is_definition,
     is_exported, content_hash_at_index

dependencies
  └─ dep_id (PK), repo_id (FK), source_path, target_path,
     source_symbol, target_symbol, dep_type, dep_direction

summaries
  └─ summary_id (PK), file_id (FK), content_hash, summary_text,
     created_at, invalidated_at, is_valid

indexing_state
  └─ repo_id (PK), last_full_index_at, last_incremental_at,
     status, files_indexed, files_total
```

### 3. Content Hasher

**Purpose**: Detect file changes without comparing full contents.

**Mechanism**:
- SHA-256 hash of raw file bytes
- Every cached artifact (symbols, dependencies, summaries) stores the content hash at the time of indexing
- On `inspect()` or any read operation, the current hash is compared to the stored hash
- Mismatch → stale → invalidate and re-index
- Match → reuse cached data

### 4. Cache Manager

**Purpose**: Manage cache validity and trigger invalidation.

**Mechanism**:
- **File-level invalidation**: When a file changes, only that file's cached artifacts are invalidated
- **Transitive invalidation**: When a header file changes, all files that include it are marked stale
- **Summary invalidation**: Semantic summaries are invalidated when their source file's hash changes
- **Dependency graph invalidation**: When a file's symbols change, its dependency edges are invalidated
- **No full-repo invalidation**: A single file change never triggers a full rescan

### 5. Indexer (Tree-Sitter Based)

**Purpose**: Parse source files and extract structural information.

**Language Support** (priority order):

| Priority | Languages | Parser |
|----------|-----------|--------|
| 1 | C, C++, CUDA | tree-sitter-c, tree-sitter-cpp, tree-sitter-cuda |
| 2 | Python | tree-sitter-python (Python AST fallback) |
| 3 | Other | Regex-based fallback |

**Parsing Strategy**:
- Tree-sitter AST traversal for C/C++/CUDA: extracts classes, methods, structs, namespaces, enums, macros, CUDA kernels, host/device functions
- Python AST module for Python files: extracts classes, functions, imports, decorators
- Regex fallback for unsupported languages: extracts basic patterns (class, function, import declarations)
- **Parsing failures are isolated**: A failure in one file never prevents indexing of other files
- **Partial compilation tolerance**: Static analysis works even when the repository does not compile

**Indexed Information**:
- File-level: path, language, file type, size, line count, binary flag
- Symbol-level: name, type, signature, start/end lines, scope, declaration/definition status, export status
- Dependency-level: includes, imports, forward references

### 6. Dependency Analyzer

**Purpose**: Build and query the dependency graph.

**Mechanism**:
- **File dependencies**: `#include`, `#import`, `import`, `from ... import` statements, resolved to real repository files where possible
- **Symbol dependencies**: function/method call edges extracted from the AST (`call_expression` / `call` nodes), attributed to the enclosing function with qualified caller names (`indexer::index_file`, `Config.validate`)
- **Direction support**: `in` (what target depends on / callers), `out` (what depends on target / callees), `both`
- **Depth support**: 1 (direct) or N (transitive, file-level)
- **Graph storage**: Directed edges in the `dependencies` table (`dep_type = 'include' | 'import' | 'call'`)
- **Graph traversal**: Recursive CTE queries for transitive dependencies
- **Stored and reused**: The dependency graph is persisted and refreshed incrementally

### 7. Semantic Summary Store

**Purpose**: Store and manage human-readable semantic summaries for files.

**Mechanism**:
- Summaries are associated with the content hash of their source file
- `is_valid` flag tracks whether the summary matches the current file state
- `invalidated_at` timestamp records when a summary became stale
- `inspect()` returns cached summaries when valid
- `write_summary()` stores a new summary and marks the old one as invalidated
- Summaries include: module responsibility, architectural role, exported symbols, dependencies, callers

### 8. Command Executor

**Purpose**: Execute build, test, and verification commands.

**Mechanism**:
- Commands execute in the repository root directory
- Working directory is validated to be within the repository boundary
- Output captured: stdout, stderr, exit code, duration
- Errors are never silently swallowed
- Security: path validation, repository boundary enforcement

### 9. Tool Router & Validation

**Purpose**: Validate tool arguments and route to the correct handler.

**Mechanism**:
- All tool inputs are validated before execution
- Path arguments are resolved relative to the repository root
- Invalid line ranges are rejected with clear error messages
- Repository boundary checks prevent access to unrelated files

---

## Tool Specifications

Implemented MCP tools (see `src/server/tools.cpp`):

### `file_tree(path, repo_id?, max_depth?)`
Returns the directory structure as a nested JSON tree. Skips `.git`, `build`, `node_modules`, `__pycache__`. Configurable depth limit.

### `file_content(file_path, repo_id?, start_line?, end_line?)`
`inspect()` + `read()`: path, language, file type, size, line count, content hash, symbols (name, type, signature, location, scope), cached semantic summary, and index status (`fresh`/`stale`). Optional 1-based line range; invalid ranges are rejected. Prefers cached data; reports staleness instead of silently re-indexing.

### `repository_search(query, repo_id?, scope?, file_type?)`
Searches the repository index for files (path match) and symbols. Optional scope: directory subtree and file-type filter. Returns path, language, line count, symbol context.

### `symbol_search(name, repo_id?, symbol_type?)`
Searches indexed symbols across one or all registered repositories. Returns signature, file, line range, and scope.

### `dependency_graph(file_path? | symbol?, repo_id?, direction?, depth?)`
Analyzes dependencies for files or symbols. `direction`: `in` (what depends on target / callers), `out` (what target depends on / callees), `both`. `depth`: 1 = direct, >1 = transitive BFS. Include/import edges are resolved to real repository paths so reverse lookups work. With `symbol`, returns call-graph callers/callees extracted from AST call edges. `symbol_search` also reports callers. Uses the persisted dependency graph.

### `edit(file_path, repo_id?, start_line, end_line, new_content)`
Safe line-oriented editing. Replaces exactly the requested range, preserves unrelated content. After edit: writes the file, invalidates symbols/dependencies/summaries, re-indexes the file, and lazily invalidates transitive dependents for headers.

### `execute(command, repo_id)`
Executes commands in the repository root (boundary enforced). Returns stdout, stderr, exit code, duration. Errors are never silently swallowed.

### `index(repo_id?, path?, file_path?)`
Full or incremental indexing. `index_repository` walks the tree and **skips files whose content hash is unchanged**; `file_path` indexes a single file. Records `indexing_state` (status, files indexed/total). **Usually optional** — see Self-Healing below; use it to warm/force an index, for repos above the auto-index size gate, or after bulk changes.

### `summary(repo_id, file_path?, write?)`
Reads a valid cached summary (validated against the current content hash) or stores a new one, marking the old summary invalidated. Repository-level summaries supported.

### `code_analysis(file_path, repo_id?)`
Line statistics (code/comment/blank) plus symbol-type counts from the index.

---

## Indexing Strategy

### Initial Indexing
Triggered on first use of a new repository or when no indexing state exists.

1. Walk the repository tree (respecting `.gitignore` and common exclusions)
2. Classify each file: language, file type, binary vs text
3. Compute content hash for each file
4. Parse source files with tree-sitter (or Python AST, or regex fallback)
5. Extract symbols and dependency edges
6. Store all results in the knowledge store
7. Record indexing state

### Incremental Indexing
Triggered when a file changes (detected via content hash).

1. Detect changed file (current hash ≠ stored hash)
2. Re-parse only the changed file
3. Update file metadata and content hash
4. Invalidate symbols from the changed file
5. Invalidate dependency edges involving the changed file
6. Invalidate semantic summaries for the changed file
7. Mark transitive dependents as stale (for headers)
8. **Unchanged files are never reprocessed**

### Header Dependency Propagation (C/C++)
When a header file changes:
1. Find all files that include the header (stored in dependency graph)
2. Mark those files' cached data as stale
3. On next access to a stale file, re-parse it
4. This is lazy — not eager — to avoid unnecessary work

---

## Parser / Indexer Strategy

### C / C++ / CUDA (Priority 1)
- **Parser**: tree-sitter with `tree-sitter-c`, `tree-sitter-cpp`, `tree-sitter-cuda` grammars
- **AST traversal**: Custom visitor that extracts:
  - Classes (with inheritance, methods, access specifiers)
  - Structs (with members)
  - Namespaces (with nesting)
  - Free functions (with signatures, overloads)
  - Enums (with values)
  - Macros (with definitions)
  - CUDA kernels (`__global__` functions)
  - Host/device functions (`__host__`, `__device__`)
  - Templates (with template parameters)
  - Include relationships (`#include`)
  - Forward declarations vs definitions
- **Graceful fallback**: If tree-sitter parsing fails, fall back to regex-based extraction

### Python (Priority 2)
- **Parser**: Python `ast` module (built-in)
- **Extracts**: classes, functions, imports, decorators, type annotations
- **No external dependency**: Uses Python's standard library

### Other Languages (Priority 3)
- **Parser**: Regex-based fallback
- **Extracts**: basic patterns (class, function, import declarations)
- **Limitation**: No semantic understanding, but provides structural metadata

---

## Cache / Invalidation Strategy

### Validity Model
Every cached artifact has an associated content hash. Validity is determined by comparing the current file hash to the hash at the time of caching.

### Invalidation Levels

| Level | Trigger | Scope |
|-------|---------|-------|
| File | Content hash change | Single file's symbols, dependencies, summary |
| Transitive | Header file change | All files that include the header |
| Summary | Source file change | Semantic summary for that file |
| Dependency | Symbol change | Dependency edges involving that symbol |

### No Full-Repo Invalidation
A single file change never triggers a full repository rescan. Only affected artifacts are invalidated and refreshed on demand.

### Stale Data Detection
Before returning cached data, the system checks:
1. Is the content hash current?
2. Is the summary invalidated?
3. Is the dependency graph stale?
If any check fails, the data is refreshed before return.

---

## File Structure

```
AI-Tools/
├── README.md                    # This file
├── CMakeLists.txt               # Build configuration
├── install.sh                   # Installer: build + install + DB init + VS Code mcp.json
├── update.sh                    # Updater: fetch latest from GitHub, rebuild, reinstall
├── src/
│   ├── main.cpp                 # Server entry point
│   ├── server/
│   │   ├── server.cpp           # JSON-RPC / MCP transport
│   │   ├── server.h
│   │   ├── tools.cpp            # Tool router: schemas + dispatch
│   │   ├── tools.h
│   │   ├── tools_internal.h     # ToolContext + shared helper contract
│   │   ├── tools_support.cpp    # Repo resolution, self-healing, pagination
│   │   ├── tools_query.cpp      # Read-only handlers (search/content/graph)
│   │   ├── tools_actions.cpp    # summary/index/edit/execute handlers
│   ├── engine/
│   │   ├── repository.cpp       # Repository identity manager
│   │   ├── knowledge_store.cpp  # SQLite knowledge store
│   │   ├── content_hasher.cpp   # SHA-256 content hashing
│   │   ├── cache_manager.cpp    # Cache validity & invalidation
│   │   ├── indexer.cpp          # Incremental indexing driver
│   │   ├── parser.cpp           # Language dispatch + basic metadata
│   │   ├── parser/
│   │   │   ├── cpp_parser.cpp   # C/C++ tree-sitter parser
│   │   │   ├── cuda_parser.cpp  # CUDA tree-sitter parser
│   │   │   └── python_parser.cpp# Python tree-sitter parser
│   │   ├── dependency_graph.cpp # Dependency analysis
│   │   ├── symbol_index.cpp     # Symbol queries
│   │   └── summary_store.cpp    # Semantic summary store
│   └── utils/
│       ├── path_utils.cpp       # Path validation & repository boundary
│       └── file_utils.cpp       # File I/O utilities
├── tests/
│   ├── test_main.cpp            # Aggregate test runner
│   ├── test_parsers.cpp         # Parser + indexer end-to-end tests
│   ├── test_store.cpp           # Store / cache / repository tests
│   └── test_tools.cpp           # Tool layer + JSON-RPC server tests
└── third_party/
    ├── tree-sitter/             # tree-sitter runtime
    └── grammars/                # Pre-generated C/C++/Python grammars
```

---

## Build & Run

### Prerequisites
- C++23 compiler (gcc 13+)
- CMake 3.16+
- SQLite3 and OpenSSL development libraries
- pkg-config

### Build
```bash
cmake -S . -B build
cmake --build build -j
```

### Run
```bash
./build/ai-tools   # MCP server over stdio (JSON-RPC 2.0)
```

Optional: pass repository roots as arguments to register and index them at startup:
```bash
./build/ai-tools /path/to/repo
```

One-shot helpers (used by the installer, also handy manually):
```bash
./build/ai-tools --init-db              # create data dir + registry database, exit
./build/ai-tools --register /path/repo  # register+index a repo without starting the server
```

### Install (`install.sh`)
```bash
./install.sh                 # build Release, install, create databases, write VS Code config
./install.sh --prefix ~/.local --workspace /path/to/project
./install.sh --uninstall     # remove binary + data dir + VS Code entry
```
What it does:
1. Builds in a dedicated `build-release/` directory (`-DAI_TOOLS_DATA_DIR=<data-dir>` compiles the data location into the binary).
2. Installs the executable to `<prefix>/bin/ai-tools`.
3. Creates the database area in `<prefix>/share/ai-tools` — **separate from the executable** (`registry.db` + `repos/<repo_id>/knowledge.db`); overridable anytime with `AI_TOOLS_DATA_DIR`.
4. Registers (and indexes) the current directory plus any `--register PATH` repositories.
5. Merges an `ai-tools` entry into the VS Code user `mcp.json` (`~/.config/Code/User/mcp.json`), preserving existing servers; `--workspace DIR` also writes a project `.vscode/mcp.json`; `--no-vscode` skips both.

Restart VS Code (or "MCP: Restart Servers") after installing.

### Update (`update.sh`)
```bash
./update.sh                # fetch latest (newest tag, else default branch), rebuild, reinstall
./update.sh --check        # only report whether an update is available
./update.sh --list-refs    # list remote tags/branches
./update.sh --ref v1.2.0   # update to a specific tag/branch
./update.sh --fresh        # ignore the local clone, download a temporary one
```
- If the current directory is a clone of the repository, the update runs in place (`fetch` + fast-forward); otherwise the latest version is shallow-cloned from GitHub into a temporary directory (removed afterwards; `--keep` retains it).
- Reinstallation reuses `install.sh` from the fetched tree, so the layout, VS Code config, and data directory handling stay identical.
- **Databases are preserved**: the installed data dir is untouched; the new binary upgrades the schema in place. A stamp file (`<data-dir>/.update-stamp`) records the installed source revision for `--check`.

### Data directory resolution
1. `AI_TOOLS_DATA_DIR` environment variable (set by the installer in `mcp.json`)
2. the compiled-in default (`-DAI_TOOLS_DATA_DIR=...` at configure time)
3. `$HOME/.ai-tools` (fallback used by plain dev builds)

### Use from VS Code (MCP)
This workspace ships a `.vscode/mcp.json` that launches the server and registers
the current repository. Open any file with Copilot Chat and the AI-Tools tools
become available (Output → MCP → ai-tools shows the server log).

For another workspace, create `.vscode/mcp.json`:
```json
{
  "servers": {
    "ai-tools": {
      "type": "stdio",
      "command": "/absolute/path/to/ai-tools/build/ai-tools",
      "args": ["${workspaceFolder}"]
    }
  }
}
```

Tools also auto-resolve the repository from a `path` argument, the working
directory, or the single registered repository — `repo_id` is optional for
everything except `execute` (which requires it for the security boundary).

Recommended agent flow: `index` (first use) → `file_tree` / `repository_search`
→ `file_content` → `summary` (write after first read) → cached reuse.

### Test
```bash
cmake --build build
cd build && ctest --output-on-failure   # or: ./run_tests
```

---

## Known Limitations

1. **CUDA kernel analysis**: CUDA kernel launch configuration (`<<<grid, block>>>`) is parsed but not fully analyzed for runtime behavior.
2. **Template instantiation**: Template argument deduction is not performed; only template declarations are indexed.
3. **Conditional compilation**: `#ifdef` / `#ifndef` blocks are parsed but not evaluated; all branches are indexed.
4. **Build-system integration**: Makefile/CMake dependency analysis is regex-based, not semantic.
5. **Large repositories**: Repositories with >50,000 files may have slower initial indexing. Incremental updates remain fast.
6. **Non-UTF8 files**: Binary detection handles most cases, but exotic encodings may fall back to regex parsing.
7. **Call graph resolution**: Symbol call edges are name-based (AST `call` nodes), not semantically resolved. Overloaded or shadowed callees are matched by name; virtual dispatch and function pointers are not resolved.
