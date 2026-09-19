#!/bin/sh
set -eu
APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$APP_DIR"
export ASSETS_DIR="$APP_DIR/assets"
export LD_LIBRARY_PATH="$APP_DIR/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
CONFIG="${1:-assets/config.json}"
if [ "$#" -eq 0 ] && [ -f "$APP_DIR/run.config" ]; then
    CONFIG="assets/$(cat "$APP_DIR/run.config")"
fi
exec "$APP_DIR/vision_analysis" "$CONFIG"
