#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SYNC_REPO_URL="https://github.com/FUjr/modem_feeds.git"
export SYNC_DEST_DIR="feeds/qmodem"
export SYNC_REMOTE_PATH="luci"
export SYNC_COPY_SUBDIRS="true"
export SYNC_CLEAN_DEST="true"
bash "$SCRIPT_DIR/../sync-repo.sh"
