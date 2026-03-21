#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Sync luci-app-passwall2 (moved from xiaorouji to Openwrt-Passwall org)
export SYNC_REPO_URL="https://github.com/Openwrt-Passwall/openwrt-passwall2.git"
export SYNC_DEST_DIR="feeds/passwall2"
export SYNC_COPY_SUBDIRS="true"
export SYNC_CLEAN_DEST="true"
bash "$SCRIPT_DIR/../sync-repo.sh"
