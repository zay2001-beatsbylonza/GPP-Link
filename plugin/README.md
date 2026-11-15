# MPC Link (plugin)

Send files between this MPC and the Beats folder on your Mac, over Wi-Fi. By BeatsbyLonza.

Needs the **MPC Link hub** running on the Mac (`Start MPC Link.command`), with the Mac and the MPC on the same network.

## Install

1. The MPC must run **MPC OS 3.10.1 or later**.
2. On the MPC: **Menu → Preferences → Enhancements**, turn on **Enable Enhancements** and **External Plugin Support**. Wait a few seconds, then restart.
3. Unzip the download. Copy the `BeatsbyLonza - VST - MPC Link` folder into a folder named `VST2 Plugins` on the MPC's **internal drive** (create it if needed).
   - **MPC XL and Live III:** connect USB, switch the MPC to **Controller Mode**, and the internal drive appears on the Mac as **MPC User**. Copy it there, eject, and go back to Standalone.
   - **Other MPCs:** use the Open MPC Plugin Manager, or Developer mode with an SFTP app.
4. Restart the MPC.

## Use

On any track, add an insert: **External VST Plugins → BeatsbyLonza → MPC Link**. The plugin passes audio straight through, so the track sounds the same.

| Control | What it does |
| --- | --- |
| **Mac** | Connection status: `On <your Mac>` once it finds the hub |
| **Activity** | What just happened (`Got …`, `Sent …`) |
| **Phone Q** | On: files you send from the phone page arrive by themselves |
| **Level** | Output level (leave at 0 dB) |
| **Mac File** | Turn to pick a file from your Beats folder (newest first) |
| **Get** | Download the picked file into `From Computer` |
| **Refresh** | Reconnect and re-read both lists |
| **MPC File** | Turn to pick a file from `To Computer` |
| **Send** | Send the picked file to the Mac |
| **Send All** | Send everything in `To Computer` |

Folders on the MPC drive the plugin is installed on:

- `From Computer/`: files from the Mac land here. Load them from the MPC browser.
- `To Computer/`: save or export anything here to send it. After sending, files move to `To Computer/Sent/`.

On the Mac, files from this MPC land in `Beats/From MPC/<this MPC's name>/`. Nothing on either side is ever overwritten; a second `kick.wav` becomes `kick (2).wav`.

## If it says "No Mac found"

Some routers block the broadcast the plugin uses to find the Mac. Make a text file named `MPC Link hub.txt` at the top of the MPC's drive, next to `VST2 Plugins`, containing just the Mac's address from the hub window, for example `192.168.1.139`. Then press **Refresh**.

## Notes

- Shares audio (wav, aif, mp3, flac, ogg, m4a), MIDI and MPC files (xpj, xpm, xal, xpn …) only.
- No password: use it on your own network.
- Built on Akai's MPC Plugin SDK (MIT). Source: github.com/zay2001-beatsbylonza/MPCLink
