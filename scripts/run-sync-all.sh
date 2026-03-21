#!/bin/bash
# run-sync-all.sh — Orchestrates all sync-*.sh scripts
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYNC_DIR="$SCRIPT_DIR/sync"

if [ ! -d "$SYNC_DIR" ]; then
    echo "❌ No sync/ directory found"
    exit 1
fi

scripts=($(find "$SYNC_DIR" -name "sync-*.sh" | sort))

if [ ${#scripts[@]} -eq 0 ]; then
    echo "⚠️  No sync scripts found"
    exit 0
fi

echo "🚀 Running ${#scripts[@]} sync scripts..."
echo ""

for script in "${scripts[@]}"; do
    bash "$script"
done

echo ""
echo "✅ All syncs complete"
