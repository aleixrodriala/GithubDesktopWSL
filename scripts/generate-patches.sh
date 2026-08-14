#!/usr/bin/env bash
#
# Regenerate patches from the fork repo (aleixrodriala/desktop).
#
# Usage:
#   ./scripts/generate-patches.sh /path/to/desktop/fork
#
# The fork must have the WSL commits on top of an upstream release commit.
# This script finds the first "wsl:" commit and generates diffs from the
# commit just before it.

set -euo pipefail

FORK_DIR="${1:?Usage: $0 /path/to/desktop/fork}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCHES_DIR="$(dirname "$SCRIPT_DIR")/patches"

if [[ ! -d "$FORK_DIR/.git" ]]; then
    echo "Error: $FORK_DIR is not a git repository" >&2
    exit 1
fi

cd "$FORK_DIR"

# Find the base commit (last non-wsl commit before the WSL patches)
BASE_COMMIT=$(git log --oneline --reverse --all | grep -m1 'wsl:' | awk '{print $1}')
if [[ -z "$BASE_COMMIT" ]]; then
    echo "Error: no 'wsl:' commits found in the fork" >&2
    exit 1
fi
BASE_COMMIT=$(git rev-parse "${BASE_COMMIT}^")
echo "Base commit (upstream): $(git log --oneline -1 "$BASE_COMMIT")"
echo "Generating patches from $BASE_COMMIT..HEAD"
echo ""

# Clean old patches
rm -f "$PATCHES_DIR"/*.patch

# Patch 01: WSL path detection (new wsl.ts + repository.ts isWSL getter)
echo "01-wsl-path-detection.patch"
git diff "$BASE_COMMIT"..HEAD -- app/src/lib/wsl.ts app/src/models/repository.ts \
    > "$PATCHES_DIR/01-wsl-path-detection.patch"

# Patch 02: Git routing (core.ts WSL branch)
echo "02-git-routing.patch"
git diff "$BASE_COMMIT"..HEAD -- app/src/lib/git/core.ts \
    > "$PATCHES_DIR/02-git-routing.patch"

# Patch 03: File operations (import swaps + gitignore WSL)
echo "03-file-operations.patch"
git diff "$BASE_COMMIT"..HEAD -- \
    app/src/lib/git/diff.ts \
    app/src/lib/git/rebase.ts \
    app/src/lib/git/cherry-pick.ts \
    app/src/lib/git/merge.ts \
    app/src/lib/git/description.ts \
    app/src/lib/git/gitignore.ts \
    app/src/lib/git/submodule.ts \
    app/src/lib/stores/app-store.ts \
    > "$PATCHES_DIR/03-file-operations.patch"

# Patch 04: Branding (package.json, dist-info.ts)
echo "04-branding.patch"
git diff "$BASE_COMMIT"..HEAD -- app/package.json script/dist-info.ts \
    > "$PATCHES_DIR/04-branding.patch"

# Patch 05: Daemon lifecycle (main.ts quit/trash, squirrel-updater.ts uninstall)
echo "05-daemon-lifecycle.patch"
git diff "$BASE_COMMIT"..HEAD -- \
    app/src/main-process/main.ts \
    app/src/main-process/squirrel-updater.ts \
    > "$PATCHES_DIR/05-daemon-lifecycle.patch"

# Patch 06: Build + packaging (build.ts, package.ts)
echo "06-build-packaging.patch"
git diff "$BASE_COMMIT"..HEAD -- script/build.ts script/package.ts \
    > "$PATCHES_DIR/06-build-packaging.patch"

# Patch 07: Worktree path translation (POSIX → UNC, helpers live in wsl.ts / patch 01)
echo "07-worktree-path-translation.patch"
git diff "$BASE_COMMIT"..HEAD -- app/src/lib/git/worktree.ts \
    > "$PATCHES_DIR/07-worktree-path-translation.patch"

echo ""
echo "Generated patches:"
ls -la "$PATCHES_DIR"/*.patch
echo ""
echo "NOTE: After generating, manually update the update URL in 04-branding.patch"
echo "      to point to aleixrodriala/github-desktop-wsl/releases/latest/download"
