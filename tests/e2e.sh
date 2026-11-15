#!/bin/sh
# End-to-end test: run the hub and drive it with mpclink-cli, as an MPC would.
# Usage: tests/e2e.sh <hub binary> <mpclink-cli binary>
set -eu
HUB=$1
CLI=$2
S=$(mktemp -d)
trap 'kill $HP 2>/dev/null || true; rm -rf "$S"' EXIT

mkdir -p "$S/Beats/Beats 2026" "$S/mpc/Samples" "$S/Beats/Stems/Song B Stems"
head -c 3000000 /dev/urandom > "$S/Beats/Beats 2026/Trap Beat 140.wav"
echo hat > "$S/Beats/hat.wav"
echo notes > "$S/Beats/notes.txt"
for t in drums bass keys; do echo "$t" > "$S/Beats/Stems/Song B Stems/$t.wav"; done
head -c 2500000 /dev/urandom > "$S/mpc/Samples/my chop #1.wav"

"$HUB" --dir "$S/Beats" --port 47800 --name TestHub > "$S/hub.log" 2>&1 &
HP=$!
sleep 1
C="$CLI -n Live-III -h 127.0.0.1"
fail() { echo "FAIL: $*"; cat "$S/hub.log"; exit 1; }

$C find | grep -q 'hub' || fail "find"
$C list | grep -q 'Trap Beat 140.wav' || fail "list"
$C list | grep -q 'notes.txt' && fail "list shows a non-audio file"

$C send "$S/mpc/Samples/my chop #1.wav" >/dev/null || fail "send"
$C send "$S/mpc/Samples/my chop #1.wav" >/dev/null || fail "send again"
cmp "$S/mpc/Samples/my chop #1.wav" "$S/Beats/From MPC/Live-III/my chop #1.wav" || fail "upload bytes"
[ -f "$S/Beats/From MPC/Live-III/my chop #1 (2).wav" ] || fail "no-overwrite on upload"
$C send "$S/mpc/Samples/my chop #1.wav" "Song A_[ProjectData]/my chop #1.wav" >/dev/null || fail "project folder"
$C send "$S/mpc/Samples/my chop #1.wav" "../../escape.wav" >/dev/null 2>&1 && fail "upload traversal accepted"
[ ! -e "$S/Beats/escape.wav" ] || fail "upload escaped the MPC folder"

$C send-gpp "$S/mpc/Samples/my chop #1.wav" >/dev/null || fail "send-gpp"
cmp "$S/mpc/Samples/my chop #1.wav" "$S/Beats/Get Producers Paid/my chop #1.wav" || fail "gpp bytes"
curl -sf -X PUT --data-binary @"$S/mpc/Samples/my chop #1.wav" -H 'X-MPC-Device: Live-III' \
  "localhost:47800/api/upload?path=Song%20A%20Stems/drums.wav&dest=gpp-stems" >/dev/null || fail "stems upload"
[ -f "$S/Beats/Get Producers Paid/Stems/Song A Stems/drums.wav" ] || fail "stems folder"
curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary x -H 'X-MPC-Device: Live-III' \
  "localhost:47800/api/upload?path=x.wav&dest=bogus" | grep -q 400 || fail "unknown destination accepted"
curl -sf "localhost:47800/api/preview?path=hat.wav" -o "$S/pv.wav" && [ "$(head -c 4 "$S/pv.wav")" = "RIFF" ] || true

$C stems | grep -q "Stems/Song B Stems" || fail "stems list"
$C stems | grep -q "Beats 2026" && fail "a 1-file folder listed as stems"
$C send-stems "Stems/Song B Stems" | grep -q "sent 3 stems" || fail "send-stems"
cmp "$S/Beats/Stems/Song B Stems/bass.wav" "$S/Beats/Get Producers Paid/Stems/Song B Stems/bass.wav" || fail "stems bytes"
[ -f "$S/Beats/Stems/Song B Stems/bass.wav" ] || fail "stems source kept"
$C send-stems "Stems/Song B Stems" >/dev/null && [ -d "$S/Beats/Get Producers Paid/Stems/Song B Stems (2)" ] || fail "stems no-overwrite"
$C send-stems "../outside" >/dev/null 2>&1 && fail "stems traversal accepted"

D="$S/mpc/From Computer"
$C get "Beats 2026/Trap Beat 140.wav" "$D" >/dev/null || fail "get"
$C get "Beats 2026/Trap Beat 140.wav" "$D" >/dev/null || fail "get again"
cmp "$S/Beats/Beats 2026/Trap Beat 140.wav" "$D/Trap Beat 140.wav" || fail "download bytes"
[ -f "$D/Trap Beat 140 (2).wav" ] || fail "no-overwrite on download"
$C get "../hub.log" "$D" >/dev/null 2>&1 && fail "download traversal accepted"
$C get "notes.txt" "$D" >/dev/null 2>&1 && fail "non-audio download accepted"

curl -sf -X POST localhost:47800/api/queue -d '{"device":"Live-III","path":"hat.wav"}' || fail "queue"
$C inbox "$D" | grep -q 'received 1' || fail "inbox"
$C inbox "$D" | grep -q 'received 0' || fail "inbox not cleared"
[ -f "$D/hat.wav" ] || fail "inbox file"

kill $HP; wait $HP 2>/dev/null || true
$C list >/dev/null 2>&1 && fail "list with hub down"
echo "e2e: all checks passed"
