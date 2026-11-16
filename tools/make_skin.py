#!/usr/bin/env python3
"""Build GPP Link's screen ("Plugin Skins"): a cassette deck and big labelled buttons.

Akai's gen_ui.py draws a bool parameter as a small pill switch. This runs gen_ui.py
with plugin/ui.json, then swaps each send switch for a full-size button whose
caption is drawn into its images (black, red and yellow, like the LINK key):
red with yellow text when idle, yellow with black text while it works.
(Captions are baked into the images because a text label on top of a Button
swallows taps on the MPC.)

It also draws the cassette deck across the top of the page: the shell and label
art, a status line on the label, two reels that turn (a film strip following
the "Reel" value), 32 blue waveform bars (film strips following "Wave 01".."Wave 32",
scrolling past a red playhead: bright behind it, dim ahead) and a big PLAY / STOP button.

The result goes to plugin/skins/, which the SDK's build.py copies verbatim.
Re-run this after changing parameter names or ui.json:

    python3 tools/make_skin.py <path to mpc-plugin-builder-sdk>

Needs Pillow and a native build first (it reads plugin/build/params.json).
"""
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(os.path.dirname(HERE), "plugin")

# parameter name -> (small top line, big bottom line)
BUTTONS = {
    "Send to MPC":    ("SEND TO", "MPC"),
    "Send to Folder": ("SEND TO", "FOLDER"),
    "Send to GPP":    ("SEND TO", "GET PRODUCERS PAID"),
    "Send Stems":     ("SEND STEMS TO", "GET PRODUCERS PAID"),
}

WIDE_PICKER, WIDE_EXTRA = "From MPC", 320   # one column = 320 px

# Cassette deck geometry (absolute, on the 1280 x 628 page; the controls start at y 346)
DECK = (382, 12, 516, 322)            # x, y, w, h: a real cassette is about 1.6 : 1
LABEL = (408, 30, 464, 72)            # yellow label strip across the top: the waveform runs on it
STATUS = (34, 150, 330, 42)           # live status text, in the display left of the cassette
REELS = [(450, 108), (706, 104)]      # top-left of each 124 x 124 reel
REEL_SIZE, REEL_FRAMES = 124, 12
BARS_X, BARS_Y, BAR_W, BAR_GAP, BAR_H = 452, 38, 20, 6, 56
WAVE_BARS, WAVE_LEVELS, PLAYHEAD = 32, 17, 16
SHOW_BARS = range(8, 24)              # the 16 bars around the playhead fit a real label
PLAY = (936, 96, 300, 150)
DISPLAY = (24, 40, 348, 262)          # the status display box
MAGENTA = (204, 38, 140, 255)
BLUE_DIM, BLUE = (70, 110, 200, 255), (18, 52, 200, 255)   # on yellow: ahead = soft blue, played = deep blue
GREY, DARK = (40, 40, 44, 255), (14, 14, 16, 255)
SMOKE, EDGE = (44, 48, 58, 255), (128, 134, 146, 255)       # clear smoky plastic
RED, YELLOW, BLACK = (229, 57, 53, 255), (255, 212, 0, 255), (0, 0, 0, 255)
W, H = 266, 95                      # the button inside a 278 x 107 cell
FONT_DIRS = ["/usr/share/fonts/truetype/dejavu", "/System/Library/Fonts/Supplemental", "/Library/Fonts"]


def font(bold, size):
    names = ["DejaVuSansCondensed-Bold.ttf", "Arial Bold.ttf"] if bold else ["DejaVuSansCondensed.ttf", "Arial.ttf"]
    for d in FONT_DIRS:
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    sys.exit("make_skin: no DejaVu or Arial font found")


def button_image(top, big, lit, W=W, H=H):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fill, ink = (YELLOW, BLACK) if lit else (RED, YELLOW)
    d.rounded_rectangle([0, 0, W - 1, H - 1], 12, fill=fill)
    k = min(H / 95.0, 1.8)                      # bigger buttons get bigger lettering
    sm, top_size = int(20 * k), int(34 * k)
    small = font(False, sm)
    size = top_size
    while size > 16 and d.textlength(big, font=font(True, size)) > W - 24:
        size -= 1
    bigf = font(True, size)
    tw = d.textlength(top, font=small)
    gap = (H - sm - top_size) / 3               # spread the two lines over the button height
    d.text(((W - tw) / 2, gap), top, font=small, fill=ink)
    bw = d.textlength(big, font=bigf)
    d.text(((W - bw) / 2, gap * 2 + sm + (top_size - size) / 2), big, font=bigf, fill=ink)
    return img


def deck_art():
    """The page background: a clear smoky cassette in real proportions in the middle (yellow top
    strip with the waveform, tape packs, a Get Producers Paid block, the bottom rail), a status
    display on the left. Original artwork, no other brand names."""
    W0, H0 = 1280, 628
    img = Image.new("RGBA", (W0, H0), BLACK)
    d = ImageDraw.Draw(img)

    # --- left: the display ---
    dx, dy, dw, dh = DISPLAY
    d.rounded_rectangle([dx, dy, dx + dw, dy + dh], 16, fill=(18, 18, 22, 255), outline=(70, 70, 76, 255), width=2)
    d.text((dx + 20, dy + 18), "GPP", font=font(True, 44), fill=RED)
    d.text((dx + 20 + d.textlength("GPP ", font=font(True, 44)), dy + 18), "LINK", font=font(True, 44), fill=YELLOW)
    d.text((dx + 22, dy + 70), "BeatsbyLonza", font=font(False, 20), fill=(170, 170, 176, 255))
    d.line([dx + 20, dy + 104, dx + dw - 20, dy + 104], fill=(60, 60, 66, 255), width=2)
    d.text((dx + 20, dy + 186), "NOW PLAYING  ·  STATUS", font=font(False, 16), fill=(120, 120, 128, 255))

    # --- middle: the cassette ---
    x, y, w, h = DECK
    d.rounded_rectangle([x, y, x + w, y + h], 18, fill=SMOKE, outline=EDGE, width=4)
    d.rounded_rectangle([x + 10, y + 8, x + w - 10, y + h - 8], 14, outline=(70, 76, 88, 255), width=2)
    for k in range(5):
        d.line([x + 24, y + 5 + k, x + w - 24, y + 5 + k], fill=(90 - 9 * k, 96 - 9 * k, 110 - 9 * k, 255))
    for sx, sy in [(x + 16, y + 16), (x + w - 16, y + 16), (x + 16, y + h - 16), (x + w - 16, y + h - 16)]:
        d.ellipse([sx - 7, sy - 7, sx + 7, sy + 7], fill=(150, 154, 162, 255))
        d.line([sx - 4, sy - 4, sx + 4, sy + 4], fill=DARK, width=2)
    lx, ly, lw, lh = LABEL
    d.rounded_rectangle([lx, ly, lx + lw, ly + lh], 8, fill=YELLOW)
    d.text((lx + 10, ly + 14), "A", font=font(True, 36), fill=(110, 90, 10, 255))
    # the playhead on the strip: the waveform scrolls past it
    hx = BARS_X + (PLAYHEAD - SHOW_BARS[0]) * (BAR_W + BAR_GAP) - BAR_GAP / 2
    d.line([hx, ly + 4, hx, ly + lh - 4], fill=RED, width=3)
    d.polygon([(hx - 7, ly + 2), (hx + 7, ly + 2), (hx, ly + 11)], fill=RED)
    # orange side tab
    d.rectangle([x + 14, y + 104, x + 40, y + 170], fill=(232, 160, 30, 255))
    d.text((x + 18, y + 120), "A", font=font(True, 22), fill=(255, 255, 255, 255))
    # tape window and the two tape packs
    cx0, cx1 = REELS[0][0] + REEL_SIZE // 2, REELS[1][0] + REEL_SIZE // 2
    cy = REELS[0][1] + REEL_SIZE // 2
    d.rounded_rectangle([cx0 - 20, cy - 46, cx1 + 20, cy + 46], 30, fill=(26, 28, 36, 255))
    for c, r in ((cx0, 76), (cx1, 66)):
        d.ellipse([c - r, cy - r, c + r, cy + r], fill=(58, 40, 30, 255), outline=(84, 60, 44, 255), width=2)
    # blue triangle under the left reel
    d.polygon([(x + 30, y + h - 12), (x + 128, y + h - 12), (x + 79, y + 228)], fill=(32, 58, 170, 255))
    # bottom rail with the head and capstan holes
    rx0, rx1 = x + 176, x + w - 186
    d.polygon([(rx0 - 26, y + h - 8), (rx0, y + 258), (rx1, y + 258), (rx1 + 26, y + h - 8)], fill=(74, 80, 92, 255), outline=EDGE)
    for hx2 in (rx0 + 22, rx0 + 54, rx1 - 54, rx1 - 22):
        d.ellipse([hx2 - 10, y + 276, hx2 + 10, y + 296], fill=DARK)
    mid = (rx0 + rx1) // 2
    d.ellipse([mid - 9, y + 266, mid + 9, y + 284], fill=(150, 154, 162, 255))
    # Get Producers Paid block, lower right: under the right reel, beside the rail
    mx0, my0, mx1, my1 = x + w - 160, y + 190, x + w - 16, y + 292
    d.rounded_rectangle([mx0, my0, mx1, my1], 10, fill=MAGENTA)
    for txt, size, ty in (("Get Producers", 18, my1 - 58), ("Paid\u2122", 28, my1 - 36)):
        f = font(True, size)
        tw = d.textlength(txt, font=f)
        d.text((mx0 + (mx1 - mx0 - tw) / 2, ty), txt, font=f, fill=(255, 255, 255, 255))
    return img


def reel_strip():
    """A yellow reel: a ring of holes around a black eight-tooth hub with a silver pin. Eight teeth
    and 24 holes repeat every 45 degrees, so 12 frames over 45 degrees loop without a jump."""
    import math
    S = REEL_SIZE
    strip = Image.new("RGBA", (S, S * REEL_FRAMES), (0, 0, 0, 0))
    for f in range(REEL_FRAMES):
        fr = Image.new("RGBA", (S * 2, S * 2), (0, 0, 0, 0))          # drawn at 2x, then smoothed down
        d = ImageDraw.Draw(fr)
        c = S
        k2 = S / 140.0                                                    # drawn for 140 px, scaled
        a0 = f * (math.pi / 4) / REEL_FRAMES
        d.ellipse([c - 120 * k2, c - 120 * k2, c + 120 * k2, c + 120 * k2], fill=(236, 196, 58, 255), outline=(190, 150, 30, 255), width=4)
        d.ellipse([c - 92 * k2, c - 92 * k2, c + 92 * k2, c + 92 * k2], outline=(200, 160, 36, 255), width=4)
        for k in range(24):                                              # the ring of holes
            a = a0 + k * 2 * math.pi / 24
            hx, hy = c + 106 * k2 * math.cos(a), c + 106 * k2 * math.sin(a)
            d.ellipse([hx - 6 * k2, hy - 6 * k2, hx + 6 * k2, hy + 6 * k2], fill=(120, 60, 40, 255))
        d.ellipse([c - 74 * k2, c - 74 * k2, c + 74 * k2, c + 74 * k2], fill=(18, 18, 20, 255))
        for k in range(8):                                               # the hub's teeth
            a = a0 + k * 2 * math.pi / 8
            pts = []
            for da, rr in ((-0.16, 46 * k2), (0.16, 46 * k2), (0.12, 68 * k2), (-0.12, 68 * k2)):
                pts.append((c + rr * math.cos(a + da), c + rr * math.sin(a + da)))
            d.polygon(pts, fill=(236, 196, 58, 255))
        d.ellipse([c - 44 * k2, c - 44 * k2, c + 44 * k2, c + 44 * k2], fill=(10, 10, 12, 255))
        d.ellipse([c - 14 * k2, c - 14 * k2, c + 14 * k2, c + 14 * k2], fill=(225, 228, 235, 255))
        strip.paste(fr.resize((S, S), Image.LANCZOS), (0, f * S))
    return strip


def bar_strip():
    """Frames 0..16: a dim blue bar of that height (0 = flat line); 17..33: the same, bright."""
    total = 2 * WAVE_LEVELS
    strip = Image.new("RGBA", (BAR_W, BAR_H * total), (0, 0, 0, 0))
    d = ImageDraw.Draw(strip)
    for f in range(total):
        level = f % WAVE_LEVELS
        colour = BLUE if f >= WAVE_LEVELS else BLUE_DIM
        top = f * BAR_H
        half = max(1, round(level / (WAVE_LEVELS - 1) * (BAR_H / 2 - 1)))
        mid = top + BAR_H // 2
        d.rounded_rectangle([2, mid - half, BAR_W - 3, mid + half], 3, fill=colour)
    return strip


def bounds(acc, x, y, w, h):
    return {"version": 1, "acceptsHWFocus": acc, "showWhenDataModelInvalid": "Show", "whenVisible": "Always",
            "boundsType": "Absolute", "bounds": "%d %d %d %d" % (x, y, w, h)}


def image_entry(name, image, x, y, w, h, frames=None, param=None):
    if frames:
        data = {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": image,
                "numFrames": frames, "handleName": "Data"}
    else:
        data = {"version": 2, "imageType": "Regular", "colour": "0", "image": image}
    remap = [{"key": "Data", "value": "Parameter %d" % param}] if param is not None else []
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": "Image", "data": data},
            "handle remapping": {"version": 1, "map": remap}, "bounds": bounds("No", x, y, w, h)}


def slug(name):
    return "".join(c.lower() if c.isalnum() else "_" for c in name).strip("_")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sdk = sys.argv[1]
    params = os.path.join(PLUGIN, "build", "params.json")
    if not os.path.exists(params):
        sys.exit("make_skin: build the plugin first (plugin/build/params.json is missing)")
    out = os.path.join(PLUGIN, "skins")
    with tempfile.TemporaryDirectory() as tmp:
        gen = os.path.join(tmp, "Plugin Skins")
        subprocess.run([sys.executable, os.path.join(sdk, "tools", "gen_ui.py"), "--params", params,
                        "--ui", os.path.join(PLUGIN, "ui.json"), "--out", gen, "--kit-cache", tmp, "--quiet"],
                       check=True)
        tui_path = os.path.join(gen, "TUI.json")
        tui = json.load(open(tui_path))
        page = tui["pageData"]
        locals_ = page["componentDefinitions"]["localComponentDefinitions"]
        toggle = next(lc["value"] for lc in locals_ if lc["key"] == "toggle")
        focus = next(c for c in toggle["componentsData"] if c["componentData"]["type"] == "Focus")
        button = next(c for c in toggle["componentsData"] if c["componentData"]["type"] == "Button")

        index = {p["name"]: p.get("index", i) for i, p in enumerate(json.load(open(params))["params"])}
        tab = page["tabs"][0]["componentDefinition"]["componentsData"]
        template = next(c for c in tab if c["componentData"]["type"] == "toggle")
        value_label = next(c for c in toggle["componentsData"] if c["componentData"]["name"] == "value")

        # --- the cassette deck ---
        deck_art().save(os.path.join(gen, "deck.png"))
        reel_strip().save(os.path.join(gen, "reel.png"))
        bar_strip().save(os.path.join(gen, "wavebar.png"))
        deck = [image_entry("deck", "deck.png", 0, 0, 1280, 628)]
        for k, (rx, ry) in enumerate(REELS):
            deck.append(image_entry("reel %d" % (k + 1), "reel.png", rx, ry, REEL_SIZE, REEL_SIZE,
                                    REEL_FRAMES, index["Reel"]))
        for b in SHOW_BARS:
            deck.append(image_entry("wave %02d" % (b + 1), "wavebar.png", BARS_X + (b - SHOW_BARS[0]) * (BAR_W + BAR_GAP), BARS_Y,
                                    BAR_W, BAR_H, 2 * WAVE_LEVELS, index["Wave %02d" % (b + 1)]))
        status = copy.deepcopy(value_label)
        status["componentData"]["name"] = "status"
        status["componentData"]["data"]["textStyle"]["colour"] = "ffffffff"
        status["componentData"]["data"]["textStyle"]["font"]["height"] = 19.0
        status["handle remapping"] = {"version": 1, "map": [{"key": "Data", "value": "Parameter %d" % index["Status"]}]}
        status["bounds"] = bounds("No", *STATUS)
        deck.append(status)
        play = copy.deepcopy(template)
        play["componentData"]["name"] = "Play"
        play["handle remapping"] = {"version": 1, "map": [{"key": "Data", "value": "Parameter %d" % index["Play"]}]}
        play["bounds"]["bounds"] = "%d %d %d %d" % (PLAY[0] - 6, PLAY[1] - 6, PLAY[2] + 12, PLAY[3] + 12)
        deck.append(play)
        tab[0:0] = deck                                   # behind the controls, background first

        # File pickers browse with the Q-Link / data wheel only: drop the pop-up knob
        # (double tap / Enter) that the MPC would otherwise show over them.
        slider_def0 = next(lc["value"] for lc in locals_ if lc["key"] == "sliderWithFocus")
        slider_def0["actions"] = [a for a in slider_def0["actions"] if a.get("handler") != "Show Overlay"]

        # "From MPC" feeds two buttons (SEND TO FOLDER and SEND TO GET PRODUCERS PAID):
        # stretch it across both columns
        slider_def = next(lc["value"] for lc in locals_ if lc["key"] == "sliderWithFocus")
        wide = copy.deepcopy(slider_def)
        for part in wide["componentsData"]:
            bx, by, bw, bh = map(int, part["bounds"]["bounds"].split())
            part["bounds"]["bounds"] = "%d %d %d %d" % (bx, by, bw + WIDE_EXTRA, bh)
        locals_.append({"key": "sliderWide", "value": wide})
        for inst in tab:
            if inst["componentData"]["type"] == "sliderWithFocus" and inst["componentData"]["name"] == WIDE_PICKER:
                inst["componentData"]["type"] = "sliderWide"
                x, y, w, h = map(int, inst["bounds"]["bounds"].split())
                inst["bounds"]["bounds"] = "%d %d %d %d" % (x, y, w + WIDE_EXTRA, h)

        swapped = 0
        for inst in tab:
            name = inst["componentData"]["name"]
            if inst["componentData"]["type"] != "toggle" or (name not in BUTTONS and name != "Play"):
                continue
            key = "bigButton_" + slug(name)
            on_png, off_png = "btn_%s_on.png" % slug(name), "btn_%s_off.png" % slug(name)
            if name == "Play":   # off = PLAY (press to start), lit = STOP (press to stop)
                button_image("CASSETTE", "STOP", True, PLAY[2], PLAY[3]).save(os.path.join(gen, on_png))
                button_image("CASSETTE", "PLAY", False, PLAY[2], PLAY[3]).save(os.path.join(gen, off_png))
            else:
                top, big = BUTTONS[name]
                button_image(top, big, True).save(os.path.join(gen, on_png))
                button_image(top, big, False).save(os.path.join(gen, off_png))
            b = copy.deepcopy(button)
            b["componentData"]["name"] = "button"
            b["componentData"]["data"]["onImage"] = on_png
            b["componentData"]["data"]["offImage"] = off_png
            b["componentData"]["data"]["buttonId"] = 1          # lit while the value is 1
            b["componentData"]["data"]["numButtonsInGroup"] = 1
            b["bounds"]["bounds"] = "6 6 %d %d" % ((PLAY[2], PLAY[3]) if name == "Play" else (W, H))
            comp = copy.deepcopy(toggle)
            comp["componentsData"] = [copy.deepcopy(focus), b]
            locals_.append({"key": key, "value": comp})
            inst["componentData"]["type"] = key
            swapped += 1
        if swapped != len(BUTTONS) + 1:
            sys.exit("make_skin: expected %d buttons, found %d" % (len(BUTTONS) + 1, swapped))
        # the pill switch is no longer used: drop its definition and images (the
        # validator rejects a definition whose images are missing)
        if not any(c["componentData"]["type"] == "toggle" for c in tab):
            locals_[:] = [lc for lc in locals_ if lc["key"] != "toggle"]
        for f in ("toggle_on.png", "toggle_off.png"):
            p = os.path.join(gen, f)
            if os.path.exists(p):
                os.remove(p)
        json.dump(tui, open(tui_path, "w"), indent=1)
        if os.path.isdir(out):
            shutil.rmtree(out)
        shutil.copytree(gen, out)
    print("make_skin: wrote %s (cassette deck, %d big buttons)" % (out, swapped))


if __name__ == "__main__":
    main()
