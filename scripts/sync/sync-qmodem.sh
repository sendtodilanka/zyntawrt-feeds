#!/bin/bash
# sync-qmodem.sh — Syncs FUjr/modem_feeds (application/ + luci/) in one download
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO_PATH="FUjr/modem_feeds"
DEST="$REPO_ROOT/feeds/qmodem"

echo "Syncing: https://github.com/${REPO_PATH}.git -> feeds/qmodem (application + luci)"

TEMP_DIR=$(mktemp -d)
trap "rm -rf '$TEMP_DIR'" EXIT

TARBALL_URL="https://api.github.com/repos/${REPO_PATH}/tarball/HEAD"
CURL_ARGS=(
    -sL --retry 3 --retry-delay 2 --fail
    -H "Accept: application/vnd.github+json"
    -H "X-GitHub-Api-Version: 2022-11-28"
)
[ -n "${GITHUB_TOKEN:-}" ] && CURL_ARGS+=(-H "Authorization: Bearer ${GITHUB_TOKEN}")

mkdir -p "$TEMP_DIR/src"
curl "${CURL_ARGS[@]}" "$TARBALL_URL" | tar xz --strip-components=1 -C "$TEMP_DIR/src"

# Clean dest then sync both application/ and luci/ subdirs in one pass
rm -rf "${DEST:?}"/*
mkdir -p "$DEST"
for dir in "$TEMP_DIR/src/application"/*/ "$TEMP_DIR/src/luci"/*/; do
    [ -d "$dir" ] && cp -r "$dir" "$DEST/"
done

echo "Done: feeds/qmodem"
