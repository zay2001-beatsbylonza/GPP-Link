# GPP Link

Send files between your MPCs and the Beats folder on your Mac or Windows PC, over Wi-Fi.

**Turn on Wi-Fi on each MPC** (Menu → Preferences → Wi-Fi) and join the same network as your Mac. GPP Link can't reach the Mac without it.

- **MPC → Mac:** files land in `Beats/From MPC/<MPC name>/`. Nothing on the Mac is ever overwritten; a second `kick.wav` becomes `kick (2).wav`.
- **Mac → MPC:** each MPC pulls files from the Beats folder into `From Computer` on its internal drive, picked on the MPC or sent from your phone.
- Only audio (wav, aif, mp3, flac, ogg, m4a), MIDI and MPC files (xpj, xpm, xal, xpn, …) are shared. The MPCs can't see or touch anything else, or anything outside the Beats folder.

## Parts

| Folder | What it is | Status |
| --- | --- | --- |
| `hub/` | The computer app for Mac and Windows (Go, no dependencies) plus the phone web page | Working, tested on Mac and Windows (with an MPC, an iPhone and Get Producers Paid) |
| `plugin-core/` | C library the MPC plugin uses, and `mpclink-cli` for testing | Working, tested against the hub |
| `plugin/` | The MPC plugin (VST2, Gen1 + Gen2) on Akai's MPC Plugin SDK; see `plugin/README.md` | Tested in a pretend MPC; first hardware test on the MPC XL |
| `tests/` | End-to-end tests GitHub runs on every push | |
| `tools/make_skin.py` | Rebuilds `plugin/skins/` (the MPC screen: cassette deck, waveform, big labelled buttons) after layout or parameter changes | |

Every push builds and tests everything in GitHub Actions (the **Actions** tab). Each run has downloads at the bottom of its page: `GPP-Link-Mac`, `GPP-Link-Plugin`, `GPP-Link-Windows`, and `mpclink-cli` for each MPC generation.

## Run the hub on the Mac

1. Download `GPP-Link-Mac` from the latest Actions run and unzip it.
2. If your beats aren't in `~/Music/Beats`, open `Start GPP Link.command` in TextEdit and change `BEATS_FOLDER`.
3. Right-click `Start GPP Link.command` → **Open** → **Open** (first time only; it's unsigned).
4. When macOS asks whether to accept incoming network connections, click **Allow**.
5. The window shows your folder, the hub name and an **Open:** address. Leave the window open while you use your MPCs.

## Run the hub on Windows

1. Download `GPP-Link-Windows` (from the release, or the latest Actions run), unzip it, and move the `GPP Link` folder to your **Desktop** (it doesn't run from Downloads). Read `Windows - Read Me First.txt`.
2. If your beats aren't in `Music\Beats`, right-click `Start GPP Link.bat` → **Edit** and change the `BEATS_FOLDER` line, for example `set "BEATS_FOLDER=%USERPROFILE%\Desktop\BEATS FOLDER"`.
3. Check that `gpp-link-hub.exe` is in the folder. If it's missing, **Windows Security** removed it by mistake (it does that with new, unsigned programs):
   - Open **Windows Security → Virus & threat protection → Protection history**, click the entry for `gpp-link-hub.exe`, then **Actions → Allow on device** (or **Restore**).
   - So it doesn't happen again: **Virus & threat protection settings → Manage settings → Exclusions → Add or remove exclusions → Add an exclusion → Folder**, and pick the `GPP Link` folder.
   - If nothing is listed, delete the folder, add the exclusion first, then download and extract again.
4. Double-click `Start GPP Link.bat`. If Windows says **Windows protected your PC**, click **More info → Run anyway** (first time only; it's unsigned).
5. When Windows Firewall asks, tick **Private networks** and click **Allow**. Your Wi-Fi must be set to **Private** in Windows (Settings → Network & internet → Wi-Fi).
6. Leave the window open while you use your MPCs. Everything else (the plugin, the phone page) works the same as with a Mac.

Cassette previews of MP3, M4A, FLAC and other non-WAV files need the free **ffmpeg** converter on Windows: run `winget install ffmpeg` once, then restart the hub. WAV previews and all sending work without it.

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

## Connect Get Producers Paid

Beats you send with **SEND TO GET PRODUCERS PAID** or **SEND STEMS TO GET PRODUCERS PAID** land in `Get Producers Paid/` in your Beats folder. Connect the hub to your store once, and it uploads each of them to the Get Producers Paid app as a **draft** by itself. Then finish and publish it in the app.

1. In the Get Producers Paid app: **Producer → Profile → Connect GPP Link → Get pairing code**. The code looks like `K7Q4-MZP2` and works once, for 10 minutes.
2. On the computer the hub runs on, open **http://localhost:47800**. The **Get Producers Paid** box is on the hub page. (A phone can see the box but can't connect or disconnect; that only works on the computer itself.)
3. Type the code and click **Connect**. The box says **Connected to** your store name.

From then on:
- Every audio file in `Get Producers Paid/`, and every stem in `Get Producers Paid/Stems/<song>/` (sent as "<song> - <stem>.wav"), goes up as a draft once it has finished writing. Name beats like `Title_140bpm_F#min.wav` and the app fills in the title, BPM and key.
- Each file goes **once**. The hub remembers what it sent (and the server refuses duplicates too), so restarts don't send anything twice. Beats already in the folder when you connect are sent too.
- Files the app can't take (over 300 MB, or not audio) are listed in the box and skipped.
- If you tap **Disconnect** for this computer in the app, the box shows "This hub was disconnected in the app" and uploads stop until you pair again.
- **Disconnect this computer** on the hub page forgets the connection here; also disconnect it in the app to remove it there.

**Where the connection is kept:** in the macOS Keychain, or Windows Credential Manager on a PC. It can only add drafts to your store: it can't publish, change prices, or see sales, payouts or artists.

**Server address:** `https://app.beatsbylonza.com/api/mpclink`. To use a different one, start the hub with `--gpp-server <address>` or set `GPP_SERVER`.

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
  - `PUT /api/upload?path=<p>`, or `&dest=gpp` for `Beats/Get Producers Paid/` (the hub uploads these to the Get Producers Paid app as drafts once connected)
  - `GET /api/gpp` (connection status for the hub page); `POST /api/gpp/pair` and `POST /api/gpp/disconnect` (only from the hub's own computer, with header `X-GPP-Link: 1`)
  - `GET /api/preview?path=<p>`, the beat as 16-bit stereo 44.1 kHz WAV for the cassette (converted with `afconvert` on the Mac)
  - `GET /api/stems?format=tsv` (stems folders: `files<TAB>mtime<TAB>path`) and `POST /api/stems/send?path=<folder>` (the Mac copies it to `Get Producers Paid/Stems/`)
  - `GET /api/inbox`, files queued from the phone, then `POST /api/inbox/ack?path=<p>` for each one received
  - phone page: `GET /api/devices`, `POST /api/queue {"device","path"}`

## To confirm on the hardware

- The internal drive (MPC User): the core uses the first writable mount of `/nvme` (Live III, XL), `/storage` (Gen2 without NVMe) or `/sdcard` (Gen1, Force), and falls back to the drive the plugin is installed on.
- That a plugin can open sockets from inside MPC (the Open MPC docs list web-fetching plugins as supported).

## Note

There's no password. Anyone on your Wi-Fi can read the shared file types in the Beats folder and drop files into `From MPC` while the hub is running. Use it on your home or studio network, and close the hub window when you're done.
