# MPC Link

Send files between your MPCs and the Beats folder on your Mac, over Wi-Fi.

- **MPC → Mac:** files land in `Beats/From MPC/<MPC name>/`. Nothing on the Mac is ever overwritten; a second `kick.wav` becomes `kick (2).wav`.
- **Mac → MPC:** each MPC pulls files from the Beats folder into `From Computer` on its internal drive, picked on the MPC or sent from your phone.
- Only audio (wav, aif, mp3, flac, ogg, m4a), MIDI and MPC files (xpj, xpm, xal, xpn, …) are shared. The MPCs can't see or touch anything else, or anything outside the Beats folder.

## Parts

| Folder | What it is | Status |
| --- | --- | --- |
| `hub/` | The Mac app (Go, no dependencies) plus the phone web page | Working, tested |
| `plugin-core/` | C library the MPC plugin uses, and `mpclink-cli` for testing | Working, tested against the hub |
| plugin wrapper | VST2 plugin + MPC skin for Gen1 (armv7) and Gen2 (aarch64), on Akai's MPC Plugin SDK | Next step |

Every push builds and tests everything in GitHub Actions (the **Actions** tab). Each run has downloads at the bottom of its page: `MPC-Link-Mac` and `mpclink-cli` for each MPC generation.

## Run the hub on the Mac

1. Download `MPC-Link-Mac` from the latest Actions run and unzip it.
2. If your beats aren't in `~/Music/Beats`, open `Start MPC Link.command` in TextEdit and change `BEATS_FOLDER`.
3. Right-click `Start MPC Link.command` → **Open** → **Open** (first time only; it's unsigned).
4. When macOS asks whether to accept incoming network connections, click **Allow**.
5. Open the address it prints on your phone to see the phone page.

## Test from an MPC before the plugin exists

With Developer mode on, copy `mpclink-cli` (the build for that MPC's generation) to the MPC over SFTP, then over SSH:

```
chmod +x mpclink-cli
./mpclink-cli -n "Live II" find
./mpclink-cli -n "Live II" list
./mpclink-cli -n "Live II" send "/path/to/sample.wav"
./mpclink-cli -n "Live II" get "Beats 2026/Trap Beat 140.wav"
./mpclink-cli -n "Live II" inbox
```

If `find` gets no answer (some routers block broadcasts), add `-h <Mac IP>`.

## Protocol (for the plugin)

- Discovery: UDP broadcast `MPCLINK-DISCOVER 1` to port 47801; the hub answers `MPCLINK-HUB 1 <http port> <name>`.
- HTTP on port 47800; every MPC request carries `X-MPC-Device: <name>`.
  - `GET /api/hello`
  - `GET /api/list?format=tsv`, one `size<TAB>mtime<TAB>path` line per file, newest first
  - `GET /api/file?path=<p>`
  - `PUT /api/upload?path=<p>`
  - `GET /api/inbox`, files queued from the phone, then `POST /api/inbox/ack?path=<p>` for each one received
  - phone page: `GET /api/devices`, `POST /api/queue {"device","path"}`

## To confirm on the hardware

- The internal-drive path MPC's browser shows: the core tries `/media/az01-internal`, then `/nvme`, then `/sdcard`.
- That a plugin can open sockets from inside MPC (the Open MPC docs list web-fetching plugins as supported).

## Note

There's no password. Anyone on your Wi-Fi can read the shared file types in the Beats folder and drop files into `From MPC` while the hub is running. Use it on your home or studio network, and close the hub window when you're done.
