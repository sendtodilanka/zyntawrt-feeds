#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Sync luci-app-passwall (moved from xiaorouji to Openwrt-Passwall org)
export SYNC_REPO_URL="https://github.com/Openwrt-Passwall/openwrt-passwall.git"
export SYNC_DEST_DIR="feeds/passwall"
export SYNC_COPY_SUBDIRS="true"
export SYNC_CLEAN_DEST="true"
bash "$SCRIPT_DIR/../sync-repo.sh"

# Sync passwall dependency packages
export SYNC_REPO_URL="https://github.com/Openwrt-Passwall/openwrt-passwall-packages.git"
export SYNC_DEST_DIR="package/passwall-packages"
export SYNC_COPY_SUBDIRS="true"
export SYNC_CLEAN_DEST="false"
bash "$SCRIPT_DIR/../sync-repo.sh"
