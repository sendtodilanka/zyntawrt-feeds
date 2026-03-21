#!/bin/bash
# check-changes.sh — Detects uncommitted changes after sync
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

changed=$(git status --porcelain | wc -l | tr -d ' ')

if [ "$changed" -gt 0 ]; then
    echo "true" > .changes-detected
    echo "📝 $changed file(s) changed"
    git status --short | head -20
else
    echo "false" > .changes-detected
    echo "✅ No changes detected"
fi
