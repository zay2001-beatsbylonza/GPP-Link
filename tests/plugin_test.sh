#!/bin/sh
# Run the real plugin (native build) in a pretend MPC against a live hub.
# Usage: tests/plugin_test.sh <hub binary> <plugin_host_test binary> <plugin .so>
set -eu
HUB=$1
HOSTTEST=$2
PLUGIN=$3
S=$(mktemp -d)
trap 'kill $HP $QP 2>/dev/null || true; rm -rf "$S"' EXIT

# The plugin is installed on a card; beats live on the internal drive ("MPC User"), $S/drive.
PF="$S/card/VST2 Plugins/BeatsbyLonza - VST - GPP Link"
mkdir -p "$S/Beats/Beats 2026" "$PF" "$S/drive/To Computer/Chops"
cp "$(dirname "$0")/../plugin/plugin.json" "$PF/"
# a real 3-second WAV (a 220 Hz tone that swells), so the cassette can decode and play it
# and its waveform changes from bar to bar
python3 - "$S/Beats/Beats 2026/Trap Beat 140.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"".join(struct.pack("<hh", v, v) for v in
                       (int(16000 * (0.1 + 0.9 * i / (3 * 44100)) * math.sin(2 * math.pi * 220 * i / 44100)) for i in range(3 * 44100))))
w.close()
PY
mkdir -p "$S/Beats/Stems/Song B Stems"
for t in drums bass; do echo "$t" > "$S/Beats/Stems/Song B Stems/$t.wav"; done
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

MPCLINK_USER_DRIVE="$S/drive" MPC_PLUGIN_FOLDER="$PF" timeout 150 "$HOSTTEST" "$PLUGIN" "$S/drive"
[ ! -e "$S/card/From Computer" ] && [ ! -e "$S/card/To Computer" ] || { echo "FAIL: folders made on the card, not the internal drive"; exit 1; }
[ -f "$S/drive/To Computer/notes.txt" ] || { echo "FAIL: non-audio file was touched"; exit 1; }
[ -f "$S/Beats/From MPC/"*"/Chops/snare.wav" ] || { echo "FAIL: subfolder not kept on the Mac"; exit 1; }
[ -f "$S/Beats/Get Producers Paid/my chop #1.wav" ] || { echo "FAIL: GPP file not in Get Producers Paid"; exit 1; }
[ -f "$S/Beats/Get Producers Paid/Stems/Song B Stems/bass.wav" ] || { echo "FAIL: stems not copied to GPP"; exit 1; }
[ ! -e "$S/Beats/From MPC/"*"/my chop #1.wav" ] || { echo "FAIL: GPP send also went to From MPC"; exit 1; }
[ -f "$S/Beats/Get Producers Paid/Trap Beat 140.wav" ] || { echo "FAIL: received beat not sent to GPP"; exit 1; }
[ -f "$S/Beats/From MPC/"*"/Trap Beat 140.wav" ] || { echo "FAIL: received beat not sent to From MPC"; exit 1; }
[ -f "$S/drive/From Computer/Trap Beat 140.wav" ] || { echo "FAIL: the MPC should keep the received beat"; exit 1; }
echo "plugin test: all checks passed"
