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

# Extract owner/repo from URL (handles both .git and no-.git suffixes)
REPO_PATH=$(echo "$SYNC_REPO_URL" | sed 's|https://github.com/||;s|\.git$||')

# Download via GitHub API tarball endpoint (authenticated, 5000 req/hr limit)
# API endpoint guarantees a real tarball response (unlike raw archive URLs which
# can silently return HTML on rate-limit/auth errors)
TARBALL_URL="https://api.github.com/repos/${REPO_PATH}/tarball/HEAD"
CURL_ARGS=(
    -sL --retry 3 --retry-delay 2 --fail
    -H "Accept: application/vnd.github+json"
    -H "X-GitHub-Api-Version: 2022-11-28"
)
if [ -n "${GITHUB_TOKEN:-}" ]; then
    CURL_ARGS+=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
fi

mkdir -p "$TEMP_DIR/src"
curl "${CURL_ARGS[@]}" "$TARBALL_URL" | tar xz --strip-components=1 -C "$TEMP_DIR/src"

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

echo "✅ Done: $SYNC_DEST_DIR"
