#!/bin/bash
# sync-repo.sh — Master sync utility
# Called by individual sync-*.sh scripts via environment variables
#
# Required env vars:
#   SYNC_REPO_URL   - upstream git repo URL
#   SYNC_DEST_DIR   - destination directory (relative to repo root)
#
# Optional env vars:
#   SYNC_REMOTE_PATH   - subdirectory inside upstream repo (default: .)
#   SYNC_COPY_SUBDIRS  - copy only top-level subdirs, not root files (default: false)
#   SYNC_CLEAN_DEST    - wipe destination before sync (default: false)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMP_DIR=$(mktemp -d)

cleanup() { rm -rf "$TEMP_DIR"; }
trap cleanup EXIT

# Validate required vars
if [ -z "${SYNC_REPO_URL:-}" ] || [ -z "${SYNC_DEST_DIR:-}" ]; then
    echo "❌ SYNC_REPO_URL and SYNC_DEST_DIR must be set"
    exit 1
fi

SYNC_REMOTE_PATH="${SYNC_REMOTE_PATH:-.}"
SYNC_COPY_SUBDIRS="${SYNC_COPY_SUBDIRS:-false}"
SYNC_CLEAN_DEST="${SYNC_CLEAN_DEST:-false}"
DEST="$REPO_ROOT/$SYNC_DEST_DIR"

echo "📦 Syncing: $SYNC_REPO_URL → $SYNC_DEST_DIR"

# Shallow clone
git clone --depth 1 --quiet "$SYNC_REPO_URL" "$TEMP_DIR/src"

SOURCE="$TEMP_DIR/src/$SYNC_REMOTE_PATH"

if [ ! -d "$SOURCE" ]; then
    echo "❌ Remote path not found: $SYNC_REMOTE_PATH"
    exit 1
fi

mkdir -p "$DEST"

if [ "$SYNC_CLEAN_DEST" = "true" ]; then
    rm -rf "${DEST:?}"/*
fi

if [ "$SYNC_COPY_SUBDIRS" = "true" ]; then
    # Copy only top-level subdirectories
    for dir in "$SOURCE"/*/; do
        [ -d "$dir" ] && cp -r "$dir" "$DEST/"
    done
else
    cp -r "$SOURCE"/. "$DEST/"
fi

# Remove any nested .git directories
find "$DEST" -name ".git" -type d -exec rm -rf {} + 2>/dev/null || true

echo "✅ Done: $SYNC_DEST_DIR"
