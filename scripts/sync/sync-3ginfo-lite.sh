#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Uses our own fork with the Dialog HBB CEREG fix
export SYNC_REPO_URL="https://github.com/4IceG/luci-app-3ginfo-lite.git"
export SYNC_DEST_DIR="package/luci-app-3ginfo-lite"
export SYNC_CLEAN_DEST="true"
bash "$SCRIPT_DIR/../sync-repo.sh"
