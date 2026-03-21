#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SYNC_REPO_URL="https://github.com/jerrykuku/luci-theme-argon.git"
export SYNC_DEST_DIR="package/luci-theme-argon"
export SYNC_CLEAN_DEST="true"
bash "$SCRIPT_DIR/../sync-repo.sh"
