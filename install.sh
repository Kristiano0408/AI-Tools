#!/usr/bin/env bash
# AI-Tools installer
#
# Builds the MCP server, installs it into a prefix directory, creates the
# database area (separate from the executable), and writes the MCP config into
# VS Code.
#
# Layout (default prefix ~/.local):
#   <prefix>/bin/ai-tools                      executable
#   <data-dir>/registry.db                     global registry
#   <data-dir>/repos/<repo_id>/knowledge.db    per-repository databases
#
# Default data dir: <prefix>/share/ai-tools (compiled into the binary and
# overridable at runtime with AI_TOOLS_DATA_DIR).
#
# Usage:
#   ./install.sh [options]
#     --prefix DIR        Install prefix (default: ~/.local)
#     --data-dir DIR      Data/database directory (default: <prefix>/share/ai-tools)
#     --bin-dir DIR       Executable directory (default: <prefix>/bin)
#     --workspace DIR     Also write .vscode/mcp.json into this workspace
#     --no-vscode         Skip writing the VS Code user mcp.json
#     --no-index          Do not pre-index the current directory
#     --register PATH     Register (and index) an extra repository (repeatable)
#     --dry-run           Show what would be done without making changes
#     -y, --yes           Skip confirmation prompts
#     --uninstall         Remove installed binary + data dir + VS Code entry
#     -h, --help          Show this help
#
# Requires: cmake, a C++23 compiler, pkg-config, sqlite3, openssl dev headers.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$HOME/.local"
BIN_DIR=""
DATA_DIR=""
WORKSPACE=""
WRITE_VSCODE=1
PRE_INDEX=1
DRY_RUN=0
CONFIRM=1
UNINSTALL=0
EXTRA_REPOS=()

usage() {
    sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# Resolve the platform-specific VS Code User directory
get_vscode_user_dir() {
    case "$(uname -s)" in
        Darwin*)
            echo "$HOME/Library/Application Support/Code/User"
            ;;
        CYGWIN*|MINGW*|MSYS*)
            if [[ -n "${APPDATA:-}" ]]; then
                echo "$APPDATA/Code/User"
            else
                echo "$HOME/AppData/Roaming/Code/User"
            fi
            ;;
        *)
            # Linux and other Unix-like systems
            echo "${XDG_CONFIG_HOME:-$HOME/.config}/Code/User"
            ;;
    esac
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --data-dir) DATA_DIR="$2"; shift 2 ;;
        --bin-dir) BIN_DIR="$2"; shift 2 ;;
        --workspace) WORKSPACE="$2"; shift 2 ;;
        --no-vscode) WRITE_VSCODE=0; shift ;;
        --no-index) PRE_INDEX=0; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -y|--yes) CONFIRM=0; shift ;;
        --register) EXTRA_REPOS+=("$2"); shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

BIN_DIR="${BIN_DIR:-$PREFIX/bin}"
DATA_DIR="${DATA_DIR:-$PREFIX/share/ai-tools}"
BIN_PATH="$BIN_DIR/ai-tools"
VSCODE_USER_DIR="$(get_vscode_user_dir)"

# ---------------------------------------------------------------- uninstall --
if [[ $UNINSTALL -eq 1 ]]; then
    echo "AI-Tools Uninstaller"
    echo "  Binary:      $BIN_PATH"
    echo "  Data dir:    $DATA_DIR"
    echo "  VS Code cfg: $VSCODE_USER_DIR/mcp.json"
    echo

    if [[ $DRY_RUN -eq 1 ]]; then
        echo "[DRY-RUN] Would remove $BIN_PATH (if exists)"
        echo "[DRY-RUN] Would remove $DATA_DIR (if exists)"
        echo "[DRY-RUN] Would remove 'ai-tools' server from $VSCODE_USER_DIR/mcp.json"
        exit 0
    fi

    if [[ $CONFIRM -eq 1 ]]; then
        read -r -p "Are you sure you want to uninstall AI-Tools? [y/N] " response
        case "$response" in
            [yY][eE][sS]|[yY]) ;;
            *) echo "Aborted."; exit 0 ;;
        esac
    fi

    echo "Uninstalling:"
    if [[ -f "$BIN_PATH" ]]; then rm -f "$BIN_PATH"; echo "  removed $BIN_PATH"; fi
    if [[ -d "$DATA_DIR" ]]; then rm -rf "$DATA_DIR"; echo "  removed $DATA_DIR"; fi
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$BIN_PATH" "$VSCODE_USER_DIR/mcp.json" <<'PY'
import json, os, sys
bin_path = sys.argv[1]
cfg_path = sys.argv[2]
if not os.path.exists(cfg_path):
    print(f"  no VS Code config at {cfg_path}")
    sys.exit(0)
try:
    with open(cfg_path) as f:
        cfg = json.load(f)
except Exception as e:
    print(f"  could not parse {cfg_path}: {e}")
    sys.exit(0)
servers = cfg.get("servers", {})
removed = [k for k, v in servers.items() if v.get("command") == bin_path]
for k in removed:
    del servers[k]
if removed:
    with open(cfg_path, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    print(f"  removed VS Code server entries: {', '.join(removed)}")
else:
    print("  no VS Code ai-tools entry found")
PY
    fi
    echo "Done. Restart VS Code to drop the MCP server."
    exit 0
fi

# ------------------------------------------------------------ dependencies --
for tool in cmake pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required tool: $tool" >&2
        exit 1
    fi
done
if ! pkg-config --exists sqlite3; then
    echo "Missing dependency: sqlite3 development headers (install libsqlite3-dev or brew install sqlite)" >&2
    exit 1
fi
if ! pkg-config --exists openssl; then
    echo "Missing dependency: openssl development headers (install libssl-dev or brew install openssl@3)" >&2
    exit 1
fi

echo "AI-Tools Installer"
echo "  Prefix:      $PREFIX"
echo "  Binary:      $BIN_PATH"
echo "  Data dir:    $DATA_DIR"
if [[ $WRITE_VSCODE -eq 1 ]]; then
    echo "  VS Code cfg: $VSCODE_USER_DIR/mcp.json"
fi
if [[ -n "$WORKSPACE" ]]; then
    echo "  Workspace:   $WORKSPACE/.vscode/mcp.json"
fi
echo

if [[ $DRY_RUN -eq 1 ]]; then
    echo "[DRY-RUN] Would build ai-tools in release mode"
    echo "[DRY-RUN] Would install executable to $BIN_PATH"
    echo "[DRY-RUN] Would initialize database at $DATA_DIR"
    if [[ $WRITE_VSCODE -eq 1 ]]; then
        echo "[DRY-RUN] Would configure MCP server in $VSCODE_USER_DIR/mcp.json"
    fi
    if [[ -n "$WORKSPACE" ]]; then
        echo "[DRY-RUN] Would write $WORKSPACE/.vscode/mcp.json"
    fi
    exit 0
fi

if [[ $CONFIRM -eq 1 ]]; then
    read -r -p "Proceed with installation? [Y/n] " response
    case "$response" in
        [nN][oO]|[nN]) echo "Aborted."; exit 0 ;;
        *) ;;
    esac
fi

# ------------------------------------------------------------------- build --
# Dedicated build directory so the installer never disturbs a development build/.
BUILD_DIR="$SCRIPT_DIR/build-release"
echo "Building in $BUILD_DIR ..."
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DAI_TOOLS_DATA_DIR="$DATA_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null
echo "Build OK"

# ----------------------------------------------------------------- install --
mkdir -p "$BIN_DIR" "$DATA_DIR"
install -m 0755 "$BUILD_DIR/ai-tools" "$BIN_PATH"
echo "Installed executable: $BIN_PATH"

# Create the database area (separate from the executable) and the global registry.
"$BIN_PATH" --init-db
echo "Created database area: $DATA_DIR"

# Register repositories (current workspace by default + extras).
REPOS=()
if [[ $PRE_INDEX -eq 1 ]]; then
    REPOS+=("$PWD")
fi
REPOS+=("${EXTRA_REPOS[@]+"${EXTRA_REPOS[@]}"}")
if [[ ${#REPOS[@]} -gt 0 ]]; then
    "$BIN_PATH" --register "${REPOS[@]}"
fi

# --------------------------------------------------------------- mcp.json ----
MCP_SNIPPET=$(cat <<JSON
{
  "ai-tools": {
    "type": "stdio",
    "command": "$BIN_PATH",
    "args": [],
    "env": {
      "AI_TOOLS_DATA_DIR": "$DATA_DIR"
    }
  }
}
JSON
)

if [[ $WRITE_VSCODE -eq 1 ]]; then
    mkdir -p "$VSCODE_USER_DIR"
    CFG_PATH="$VSCODE_USER_DIR/mcp.json"
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$CFG_PATH" "$BIN_PATH" "$DATA_DIR" <<'PY'
import json, os, sys

cfg_path, bin_path, data_dir = sys.argv[1:4]

cfg = {}
if os.path.exists(cfg_path):
    try:
        with open(cfg_path) as f:
            cfg = json.load(f)
    except Exception:
        backup = cfg_path + ".bak"
        os.replace(cfg_path, backup)
        print(f"  existing {cfg_path} was not valid JSON; backed up to {backup}")
        cfg = {}

cfg.setdefault("servers", {})
cfg["servers"]["ai-tools"] = {
    "type": "stdio",
    "command": bin_path,
    "args": [],
    "env": {"AI_TOOLS_DATA_DIR": data_dir},
}

with open(cfg_path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
print(f"  wrote {cfg_path} (server 'ai-tools')")
PY
    else
        echo "python3 not found — add this manually to $CFG_PATH:" >&2
        echo "$MCP_SNIPPET"
    fi
fi

if [[ -n "$WORKSPACE" ]]; then
    mkdir -p "$WORKSPACE/.vscode"
    WS_PATH="$WORKSPACE/.vscode/mcp.json"
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$WS_PATH" "$BIN_PATH" "$DATA_DIR" "$WORKSPACE" <<'PY'
import json, os, sys

ws_path, bin_path, data_dir, workspace = sys.argv[1:5]
cfg = {"servers": {"ai-tools": {
    "type": "stdio",
    "command": bin_path,
    "args": [workspace],
    "env": {"AI_TOOLS_DATA_DIR": data_dir},
}}}
with open(ws_path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
print(f"  wrote {ws_path}")
PY
    fi
fi

echo
echo "Installation complete."
echo "  binary : $BIN_PATH"
echo "  data   : $DATA_DIR"
echo
echo "Restart VS Code (or run 'MCP: Restart Servers') to pick up the server."
echo "To uninstall: $0 --uninstall"
