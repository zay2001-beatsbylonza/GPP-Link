# GPP Link (plugin)

Send files between this MPC and the Beats folder on your Mac, over Wi-Fi. By BeatsbyLonza.

Needs the **GPP Link hub** running on the Mac (`Start GPP Link.command`), with the Mac and the MPC on the same network.

## Install

1. The MPC must run **MPC OS 3.10.1 or later**.
2. On the MPC: **Menu → Preferences → Enhancements**, turn on **Enable Enhancements** and **External Plugin Support**. Wait a few seconds, then restart.
3. Unzip the download. Copy the `BeatsbyLonza - VST - GPP Link` folder into a folder named `VST2 Plugins` on the MPC's **internal drive** (create it if needed).
   - **MPC XL and Live III:** connect USB, switch the MPC to **Controller Mode**, and the internal drive appears on the Mac as **MPC User**. Copy it there, eject, and go back to Standalone.
   - **Other MPCs (Key 37, Live II and other Gen 1):** put a `VST2 Plugins` folder at the top of a **USB stick or SD card** (formatted exFAT), copy the folder into it, and put it in the MPC. Or use the Open MPC Plugin Manager.
4. Restart the MPC.

## Use

On any track, add an insert: **External VST Plugins → BeatsbyLonza → GPP Link**. Audio passes straight through, so the track sounds the same.

### The cassette deck

Pick a beat with **Computer Files** and press **PLAY** on the cassette to hear it through this track before you send it anywhere. The reels turn, the blue waveform scrolls past the red playhead like tape moving (bright blue has played, darker blue is coming), and the label shows the name and time. Press the same button (**STOP**) to stop and rewind to the front; the end of the beat rewinds too, and picking another beat stops the tape. MP3, WAV, AIFF, FLAC and M4A all play (your Mac converts them); nothing is saved on the MPC.

### The four blocks

Turn the Q-Link above a block to pick, then press its big button. Nothing is sent until you press.

| Block | Pick with | What the button does |
| --- | --- | --- |
| **SEND TO MPC** | **Computer Files** (your Beats folder, newest first) | Copies the beat into `From Computer` on this MPC |
| **SEND TO FOLDER** | **From MPC** (files in `To Computer`) | Copies it to `Beats/From MPC/<this MPC>/` on the computer |
| **SEND TO GET PRODUCERS PAID** | the same **From MPC** pick | Copies it to `Beats/Get Producers Paid/` on the computer, ready to become a draft in the app |
| **SEND STEMS TO GET PRODUCERS PAID** | **Computer Stems** (stems folders in your Beats folder) | The computer copies the whole folder to `Beats/Get Producers Paid/Stems/` |

**From MPC** stretches across the two buttons it feeds: pick a finished beat once, then send it to your folder, to Get Producers Paid, or both.

A stems folder is any folder in your Beats folder with two or more audio files in it; the picker shows how many files each has. Stems are copied on the Mac, so even big stem sets go instantly.

The cassette label shows the connection (`On <your Mac>`) and what just happened. A button turns yellow while it works.

Files sent from the phone page arrive in `From Computer` by themselves, and the lists refresh on their own.

**Phone page:** on the same Wi-Fi, type the hub window's **Open:** address into Safari, starting with `http://` (for example `http://192.168.1.50:47800`). Then tap **Share → Add to Home Screen**. Full steps are in the main README under *Connect your phone*.

Folders on the MPC drive the plugin is installed on:

- `From Computer/`: files from the Mac land here. Load them from the MPC browser.
- `To Computer/`: save or export finished beats here to send them. Files stay here after sending.

Nothing on either side is ever overwritten; a second `kick.wav` becomes `kick (2).wav`.

**Get Producers Paid:** for now, beats and stems wait in `Beats/Get Producers Paid/` on the Mac. Once the Get Producers Paid server is live, the hub will upload them to your producer account as drafts, where you add the title, price and license and publish.

## If it says "No Mac found"

Some routers block the broadcast the plugin uses to find the Mac. Make a text file named `GPP Link hub.txt` at the top of the MPC's drive, next to `VST2 Plugins`, containing just the Mac's address from the hub window, for example `192.168.1.50`. The plugin tries again every few seconds.

## Notes

- Shares audio (wav, aif, mp3, flac, ogg, m4a), MIDI and MPC files (xpj, xpm, xal, xpn …) only.
- No password: use it on your own network.
- Built on Akai's MPC Plugin SDK (MIT). Source: github.com/zay2001-beatsbylonza/MPCLink
