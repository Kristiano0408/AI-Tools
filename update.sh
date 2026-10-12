#!/usr/bin/env bash
# AI-Tools updater
#
# Downloads the latest version of AI-Tools from GitHub, builds it, and
# reinstalls it (reusing install.sh from the downloaded tree). Databases in
# the data directory are preserved: the new binary upgrades them in place
# (schema init is idempotent).
#
# Update source:
#   - If the current directory is a clone of the repo, it is fetched and the
#     chosen ref is checked out there.
#   - Otherwise a shallow clone is made into a temporary directory.
#
# Usage:
#   ./update.sh [options]
#     --repo URL       Repository URL (default: https://github.com/Kristiano0408/AI-Tools.git)
#     --ref REF        Branch, tag, or release to update to (default: latest tag, else default branch)
#     --list-refs      List remote tags/branches and exit
#     --check          Show whether an update is available, without installing
#     --prefix DIR     Passed to install.sh (default: ~/.local)
#     --data-dir DIR   Passed to install.sh (default: <prefix>/share/ai-tools)
#     --no-index       Passed to install.sh (do not pre-index the current directory)
#     --register PATH  Passed to install.sh (extra repository to register)
#     --keep           Keep the downloaded tree instead of a temporary clone
#     --fresh          Always download a fresh clone (never update in place)
#     -h, --help       Show this help

set -euo pipefail

DEFAULT_REPO="https://github.com/Kristiano0408/AI-Tools.git"
REPO_URL="$DEFAULT_REPO"
REF=""
LIST_REFS=0
CHECK_ONLY=0
PREFIX="$HOME/.local"
DATA_DIR=""
NO_INDEX=0
EXTRA_REPOS=()
KEEP=0
FRESH=0

usage() {
    sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo) REPO_URL="$2"; shift 2 ;;
        --ref) REF="$2"; shift 2 ;;
        --list-refs) LIST_REFS=1; shift ;;
        --check) CHECK_ONLY=1; shift ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --data-dir) DATA_DIR="$2"; shift 2 ;;
        --no-index) NO_INDEX=1; shift ;;
        --register) EXTRA_REPOS+=("$2"); shift 2 ;;
        --keep) KEEP=1; shift ;;
        --fresh) FRESH=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

command -v git >/dev/null 2>&1 || { echo "git is required to update" >&2; exit 1; }

# --------------------------------------------------------- resolve the tree --
# Prefer updating in place when cwd is a clone of the same remote URL.
SRC_DIR=""
IN_PLACE=0
if [[ $FRESH -eq 0 ]] && git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
   && [[ "$(git remote get-url origin 2>/dev/null || true)" == "$REPO_URL" ]]; then
    SRC_DIR="$(git rev-parse --show-toplevel)"
    IN_PLACE=1
    echo "Updating in place: $SRC_DIR"
    git -C "$SRC_DIR" fetch --tags --prune origin
else
    if [[ $KEEP -eq 1 ]]; then
        SRC_DIR="$HOME/.ai-tools-update/ai-tools"
        mkdir -p "$HOME/.ai-tools-update"
        rm -rf "$SRC_DIR"
    else
        SRC_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ai-tools-update.XXXXXX")"
    fi
    echo "Downloading $REPO_URL ..."
    git clone --quiet --depth 1 "${REF:+--branch}" ${REF:+"$REF"} "$SRC_DIR" 2>/dev/null \
        || git clone --quiet "$REPO_URL" "$SRC_DIR"
    IN_PLACE=0
    if [[ $KEEP -eq 0 ]]; then
        trap 'rm -rf "$SRC_DIR"' EXIT
    fi
fi

# ------------------------------------------------------------- resolve ref --
# Default ref: the newest tagged release (by version sort); fallback default branch.
resolve_latest_ref() {
    local tag
    tag="$(git -C "$SRC_DIR" tag --sort=-v:refname | head -1 || true)"
    if [[ -n "$tag" ]]; then
        echo "$tag"
        return
    fi
    git -C "$SRC_DIR" rev-parse --abbrev-ref HEAD
}

if [[ $LIST_REFS -eq 1 ]]; then
    echo "Remote tags:"
    git -C "$SRC_DIR" tag --sort=-v:refname | sed 's/^/  /' || echo "  (none)"
    echo "Remote branches:"
    git -C "$SRC_DIR" for-each-ref --sort=-committerdate --format='  %(refname:short) (%(objectname:short), %(committerdate:short))' refs/remotes/origin | grep -v 'HEAD$' || true
    exit 0
fi

if [[ $IN_PLACE -eq 1 ]]; then
    TARGET_REF="${REF:-$(resolve_latest_ref)}"
    CURRENT_REF="$(git -C "$SRC_DIR" describe --tags --always 2>/dev/null || true)"
    CURRENT_SHA="$(git -C "$SRC_DIR" rev-parse --short HEAD)"

    if [[ -n "$REF" ]]; then
        git -C "$SRC_DIR" checkout --quiet "$REF"
        git -C "$SRC_DIR" pull --quiet origin "$REF" 2>/dev/null || true
    else
        # Fast-forward the default branch to the latest upstream state
        BRANCH="$(git -C "$SRC_DIR" rev-parse --abbrev-ref HEAD)"
        git -C "$SRC_DIR" merge --quiet --ff-only "origin/$BRANCH" || {
            echo "Local checkout is not fast-forwardable (local commits or dirty tree)." >&2
            echo "Resolve manually in $SRC_DIR, or use --ref TAG." >&2
            exit 1
        }
    fi
    NEW_SHA="$(git -C "$SRC_DIR" rev-parse --short HEAD)"
    NEW_REF="$(resolve_latest_ref)"
else
    NEW_SHA="$(git -C "$SRC_DIR" rev-parse --short HEAD)"
    NEW_REF="$(resolve_latest_ref)"
    CURRENT_REF=""; CURRENT_SHA=""
fi

echo "Source: $SRC_DIR @ ${NEW_REF} (${NEW_SHA})"

# ----------------------------------------------------------- version check --
BIN_PATH="$PREFIX/bin/ai-tools"
INSTALLED_SHA=""
if [[ -x "$BIN_PATH" ]]; then
    # Best-effort: remember the last updated source sha in a stamp file.
    STAMP="$PREFIX/share/ai-tools/.update-stamp"
    [[ -f "$STAMP" ]] && INSTALLED_SHA="$(cat "$STAMP" 2>/dev/null || true)"
    echo "Installed: ${INSTALLED_SHA:-unknown}"
fi

if [[ $CHECK_ONLY -eq 1 ]]; then
    if [[ -n "$INSTALLED_SHA" && "$INSTALLED_SHA" == "$NEW_SHA" ]]; then
        echo "Already up to date (${NEW_REF}, ${NEW_SHA})."
    else
        echo "Update available: ${NEW_REF} (${NEW_SHA})"
    fi
    exit 0
fi

if [[ -n "$INSTALLED_SHA" && "$INSTALLED_SHA" == "$NEW_SHA" && $IN_PLACE -eq 0 ]]; then
    echo "Already up to date (${NEW_REF}, ${NEW_SHA}). Nothing to do."
    exit 0
fi

# ----------------------------------------------------------------- update --
[[ -x "$SRC_DIR/install.sh" ]] || { echo "install.sh missing in downloaded tree" >&2; exit 1; }

ARGS=(--prefix "$PREFIX")
[[ -n "$DATA_DIR" ]] && ARGS+=(--data-dir "$DATA_DIR")
[[ $NO_INDEX -eq 1 ]] && ARGS+=(--no-index)
for r in ${EXTRA_REPOS[@]+"${EXTRA_REPOS[@]}"}; do ARGS+=(--register "$r"); done

echo "Building and reinstalling from $SRC_DIR ..."
"$SRC_DIR/install.sh" "${ARGS[@]}"

# Record which source revision is installed (used by --check next time).
DATA_RESOLVED="${DATA_DIR:-$PREFIX/share/ai-tools}"
mkdir -p "$DATA_RESOLVED"
echo "$NEW_SHA" > "$DATA_RESOLVED/.update-stamp"

echo
echo "Update complete: ${NEW_REF} (${NEW_SHA})"
echo "  binary : $BIN_PATH"
echo "  data   : $DATA_RESOLVED (preserved)"
echo "Restart VS Code (or 'MCP: Restart Servers') to load the new server."
