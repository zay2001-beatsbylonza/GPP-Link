# GPP Link

Send files between your MPCs and the Beats folder on your Mac, over Wi-Fi.

**Turn on Wi-Fi on each MPC** (Menu → Preferences → Wi-Fi) and join the same network as your Mac. GPP Link can't reach the Mac without it.

- **MPC → Mac:** files land in `Beats/From MPC/<MPC name>/`. Nothing on the Mac is ever overwritten; a second `kick.wav` becomes `kick (2).wav`.
- **Mac → MPC:** each MPC pulls files from the Beats folder into `From Computer` on its internal drive, picked on the MPC or sent from your phone.
- Only audio (wav, aif, mp3, flac, ogg, m4a), MIDI and MPC files (xpj, xpm, xal, xpn, …) are shared. The MPCs can't see or touch anything else, or anything outside the Beats folder.

## Parts

| Folder | What it is | Status |
| --- | --- | --- |
| `hub/` | The Mac app (Go, no dependencies) plus the phone web page | Working, tested |
| `plugin-core/` | C library the MPC plugin uses, and `mpclink-cli` for testing | Working, tested against the hub |
| `plugin/` | The MPC plugin (VST2, Gen1 + Gen2) on Akai's MPC Plugin SDK; see `plugin/README.md` | Tested in a pretend MPC; first hardware test on the MPC XL |
| `tests/` | End-to-end tests GitHub runs on every push | |
| `tools/make_skin.py` | Rebuilds `plugin/skins/` (the MPC screen: cassette deck, waveform, big labelled buttons) after layout or parameter changes | |

Every push builds and tests everything in GitHub Actions (the **Actions** tab). Each run has downloads at the bottom of its page: `GPP-Link-Mac`, `GPP-Link-Plugin`, and `mpclink-cli` for each MPC generation.

## Run the hub on the Mac

1. Download `GPP-Link-Mac` from the latest Actions run and unzip it.
2. If your beats aren't in `~/Music/Beats`, open `Start GPP Link.command` in TextEdit and change `BEATS_FOLDER`.
3. Right-click `Start GPP Link.command` → **Open** → **Open** (first time only; it's unsigned).
4. When macOS asks whether to accept incoming network connections, click **Allow**.
5. The window shows your folder, the hub name and an **Open:** address. Leave the window open while you use your MPCs.

## Connect your phone

The phone page lets you browse your Beats folder and send beats to any MPC that has GPP Link open.

1. Put your phone on the **same Wi-Fi** as your computer.
2. In the hub window, find the **Open:** line, for example `http://192.168.1.50:47800`. Yours will be different; use the one your window shows.
3. On the phone, open **Safari** and type that address exactly, **starting with `http://`**.
4. Tap **Share → Add to Home Screen**, so next time it's one tap.
5. Find a beat (or search), then tap:
   - **Cue**: selects it in **Computer Files** on that MPC, so you can PLAY or SEND TO MPC there.
   - **Send to <MPC>**: sends it straight to that MPC's `From Computer` folder. Each MPC with GPP Link open has its own button.

If the page won't load:

- Check the hub window is still open.
- Make sure you typed `http://` and the exact address.
- If two **Open:** lines appear, try the other one.
- The address can change after your router restarts, so check the hub window again.
- If you clicked **Deny** when macOS asked about incoming connections, allow it in **System Settings → Network → Firewall**.

A step-by-step guide with pictures is in [`docs/GPP-Link-How-To-Guide.pdf`](docs/GPP-Link-How-To-Guide.pdf).

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
  - `PUT /api/upload?path=<p>`, or `&dest=gpp` for `Beats/Get Producers Paid/` (drafts for the Get Producers Paid app)
  - `GET /api/preview?path=<p>`, the beat as 16-bit stereo 44.1 kHz WAV for the cassette (converted with `afconvert` on the Mac)
  - `GET /api/stems?format=tsv` (stems folders: `files<TAB>mtime<TAB>path`) and `POST /api/stems/send?path=<folder>` (the Mac copies it to `Get Producers Paid/Stems/`)
  - `GET /api/inbox`, files queued from the phone, then `POST /api/inbox/ack?path=<p>` for each one received
  - phone page: `GET /api/devices`, `POST /api/queue {"device","path"}`

## To confirm on the hardware

- The internal drive (MPC User): the core uses the first writable mount of `/nvme` (Live III, XL), `/storage` (Gen2 without NVMe) or `/sdcard` (Gen1, Force), and falls back to the drive the plugin is installed on.
- That a plugin can open sockets from inside MPC (the Open MPC docs list web-fetching plugins as supported).

## Note

There's no password. Anyone on your Wi-Fi can read the shared file types in the Beats folder and drop files into `From MPC` while the hub is running. Use it on your home or studio network, and close the hub window when you're done.
