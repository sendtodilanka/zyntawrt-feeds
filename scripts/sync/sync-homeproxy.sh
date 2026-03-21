#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SYNC_REPO_URL="https://github.com/immortalwrt/homeproxy.git"
export SYNC_DEST_DIR="package/luci-app-homeproxy"
export SYNC_CLEAN_DEST="true"
bash "$SCRIPT_DIR/../sync-repo.sh"
