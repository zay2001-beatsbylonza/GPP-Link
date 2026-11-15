#!/bin/sh
# Run the real plugin (native build) in a pretend MPC against a live hub.
# Usage: tests/plugin_test.sh <hub binary> <plugin_host_test binary> <plugin .so>
set -eu
HUB=$1
HOSTTEST=$2
PLUGIN=$3
S=$(mktemp -d)
trap 'kill $HP $QP 2>/dev/null || true; rm -rf "$S"' EXIT

PF="$S/drive/VST2 Plugins/BeatsbyLonza - VST - MPC Link"
mkdir -p "$S/Beats/Beats 2026" "$PF" "$S/drive/To Computer/Chops"
cp "$(dirname "$0")/../plugin/plugin.json" "$PF/"
head -c 3000000 /dev/urandom > "$S/Beats/Beats 2026/Trap Beat 140.wav"
echo hat > "$S/Beats/hat.wav"
touch -t 202001010000 "$S/Beats/hat.wav"                      # older, so Trap Beat is first
head -c 1500000 /dev/urandom > "$S/drive/To Computer/my chop #1.wav"
touch -t 202001010000 "$S/drive/To Computer/my chop #1.wav"
head -c 200000 /dev/urandom > "$S/drive/To Computer/Chops/snare.wav"
echo x > "$S/drive/To Computer/notes.txt"                      # not sendable: must stay put

"$HUB" --dir "$S/Beats" --port 47800 --name TestMac > "$S/hub.log" 2>&1 &
HP=$!
sleep 0.5

# act as the phone: once the MPC shows up, queue hat.wav for it
(
  for i in $(seq 1 120); do
    D=$(curl -s localhost:47800/api/devices | sed -n 's/.*"name":"\([^"]*\)".*/\1/p')
    if [ -n "$D" ]; then
      sleep 6
      curl -s -X POST localhost:47800/api/queue -d "{\"device\":\"$D\",\"path\":\"hat.wav\"}"
      exit 0
    fi
    sleep 0.5
  done
) &
QP=$!

MPC_PLUGIN_FOLDER="$PF" timeout 150 "$HOSTTEST" "$PLUGIN" "$S/drive"
[ -f "$S/drive/To Computer/notes.txt" ] || { echo "FAIL: non-audio file was touched"; exit 1; }
[ -f "$S/Beats/From MPC/"*"/Chops/snare.wav" ] || { echo "FAIL: subfolder not kept on the Mac"; exit 1; }
echo "plugin test: all checks passed"
