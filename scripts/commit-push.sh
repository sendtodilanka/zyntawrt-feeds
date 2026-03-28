#!/bin/bash
# commit-push.sh — Commits and pushes if changes detected
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.."; pwd)"
cd "$REPO_ROOT"

FORCE="${1:-false}"
CHANGES_FILE=".changes-detected"

if [ ! -f "$CHANGES_FILE" ]; then
    echo "Run check-changes.sh first"
    exit 1
fi

changes=$(cat "$CHANGES_FILE")

if [ "$changes" != "true" ] && [ "$FORCE" != "true" ]; then
    echo "No changes — skipping commit"
    exit 0
fi

TIMESTAMP=$(date -u '+%Y-%m-%d %H:%M:%S')
BRANCH=$(git rev-parse --abbrev-ref HEAD)

git config user.name  "GitHub Actions"
git config user.email "actions@github.com"
git config core.autocrlf false

git add .
git commit -m "Sync $TIMESTAMP"
git push origin "$BRANCH"

echo "Pushed: Sync $TIMESTAMP"
