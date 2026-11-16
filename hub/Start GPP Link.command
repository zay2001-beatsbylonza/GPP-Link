#!/bin/bash
# Double-click to start GPP Link. Leave the window open while you use your MPCs.
#
# Change this to your Beats folder if it lives somewhere else:
BEATS_FOLDER="$HOME/Music/Beats"

cd "$(dirname "$0")"
if [ "$(uname -m)" = "arm64" ]; then APP=./gpp-link-hub-apple-silicon; else APP=./gpp-link-hub-intel; fi
xattr -d com.apple.quarantine "$APP" 2>/dev/null
chmod +x "$APP"
exec "$APP" --dir "$BEATS_FOLDER"
