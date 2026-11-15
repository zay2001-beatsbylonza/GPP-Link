#!/usr/bin/env python3
"""Build MPC Link's screen ("Plugin Skins"): a cassette deck and big labelled buttons.

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
DECK = (23, 14, 1234, 318)            # x, y, w, h
LABEL = (60, 30, 1160, 74)            # yellow label strip
STATUS = (330, 36, 880, 62)           # live status text on the label
REELS = [(270, 112), (850, 112)]      # top-left of each 130 x 130 reel
REEL_SIZE, REEL_FRAMES = 130, 12
BARS_X, BARS_Y, BAR_W, BAR_GAP, BAR_H = 70, 258, 20, 8, 62
WAVE_BARS, WAVE_LEVELS, PLAYHEAD = 32, 17, 16
PLAY = (996, 252, 236, 70)
BLUE_DIM, BLUE = (30, 80, 170, 255), (64, 170, 255, 255)
GREY, DARK = (40, 40, 44, 255), (14, 14, 16, 255)
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
    small = font(False, 20)
    size = 34
    while size > 16 and d.textlength(big, font=font(True, size)) > W - 24:
        size -= 1
    bigf = font(True, size)
    tw = d.textlength(top, font=small)
    gap = (H - 20 - 34) / 3                     # spread the two lines over the button height
    d.text(((W - tw) / 2, gap), top, font=small, fill=ink)
    bw = d.textlength(big, font=bigf)
    d.text(((W - bw) / 2, gap * 2 + 20 + (34 - size) / 2), big, font=bigf, fill=ink)
    return img


def deck_art():
    """The cassette shell and label, drawn once as the page's background."""
    W0, H0 = 1280, 628
    img = Image.new("RGBA", (W0, H0), BLACK)
    d = ImageDraw.Draw(img)
    x, y, w, h = DECK
    d.rounded_rectangle([x, y, x + w, y + h], 22, fill=GREY, outline=RED, width=3)
    for sx, sy in [(x + 18, y + 18), (x + w - 18, y + 18), (x + 18, y + h - 18), (x + w - 18, y + h - 18)]:
        d.ellipse([sx - 7, sy - 7, sx + 7, sy + 7], fill=(90, 90, 96, 255))       # screws
        d.line([sx - 4, sy, sx + 4, sy], fill=DARK, width=2)
    lx, ly, lw, lh = LABEL
    d.rounded_rectangle([lx, ly, lx + lw, ly + lh], 10, fill=YELLOW)
    d.text((lx + 22, ly + 8), "MPC LINK", font=font(True, 40), fill=RED)
    d.text((lx + 24, ly + 50), "BeatsbyLonza", font=font(False, 18), fill=BLACK)
    # tape window between the reels
    wx0, wy0 = REELS[0][0] - 20, REELS[0][1] - 4
    wx1, wy1 = REELS[1][0] + REEL_SIZE + 20, REELS[1][1] + REEL_SIZE + 4
    d.rounded_rectangle([wx0, wy0, wx1, wy1], 60, fill=DARK, outline=(70, 70, 76, 255), width=2)
    cy = REELS[0][1] + REEL_SIZE // 2
    d.rectangle([REELS[0][0] + REEL_SIZE // 2, cy + 44, REELS[1][0] + REEL_SIZE // 2, cy + 52], fill=(70, 45, 30, 255))  # tape
    d.text((wx0 + (wx1 - wx0) / 2 - 38, wy1 - 34), "SIDE A", font=font(True, 20), fill=RED)
    # waveform window
    bw = WAVE_BARS * (BAR_W + BAR_GAP) - BAR_GAP
    d.rounded_rectangle([BARS_X - 12, BARS_Y - 8, BARS_X + bw + 12, BARS_Y + BAR_H + 8], 8, fill=DARK)
    # the playhead: the waveform scrolls past this line (bars left of it have played)
    hx = BARS_X + PLAYHEAD * (BAR_W + BAR_GAP) - BAR_GAP / 2
    d.line([hx, BARS_Y - 6, hx, BARS_Y + BAR_H + 6], fill=RED, width=3)
    d.polygon([(hx - 8, BARS_Y - 8), (hx + 8, BARS_Y - 8), (hx, BARS_Y)], fill=RED)
    return img


def reel_strip():
    """A reel hub with three spokes, turning 120 degrees over the strip (it repeats every 120)."""
    import math
    S = REEL_SIZE
    strip = Image.new("RGBA", (S, S * REEL_FRAMES), (0, 0, 0, 0))
    for f in range(REEL_FRAMES):
        fr = Image.new("RGBA", (S * 2, S * 2), (0, 0, 0, 0))       # drawn at 2x, then smoothed down
        d = ImageDraw.Draw(fr)
        c, r = S, S - 6
        d.ellipse([c - r, c - r, c + r, c + r], fill=(26, 26, 28, 255), outline=(110, 110, 116, 255), width=6)
        d.ellipse([c - 70, c - 70, c + 70, c + 70], fill=(225, 225, 228, 255))
        d.ellipse([c - 30, c - 30, c + 30, c + 30], fill=DARK)
        a0 = f * (2 * math.pi / 3) / REEL_FRAMES
        for k in range(3):
            a = a0 + k * 2 * math.pi / 3
            x1, y1 = c + 32 * math.cos(a), c + 32 * math.sin(a)
            x2, y2 = c + 68 * math.cos(a), c + 68 * math.sin(a)
            d.line([x1, y1, x2, y2], fill=RED, width=14)
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
        for b in range(WAVE_BARS):
            deck.append(image_entry("wave %02d" % (b + 1), "wavebar.png", BARS_X + b * (BAR_W + BAR_GAP), BARS_Y,
                                    BAR_W, BAR_H, 2 * WAVE_LEVELS, index["Wave %02d" % (b + 1)]))
        status = copy.deepcopy(value_label)
        status["componentData"]["name"] = "status"
        status["componentData"]["data"]["textStyle"]["colour"] = "ff000000"
        status["componentData"]["data"]["textStyle"]["font"]["height"] = 34.0
        status["handle remapping"] = {"version": 1, "map": [{"key": "Data", "value": "Parameter %d" % index["Status"]}]}
        status["bounds"] = bounds("No", *STATUS)
        deck.append(status)
        play = copy.deepcopy(template)
        play["componentData"]["name"] = "Play"
        play["handle remapping"] = {"version": 1, "map": [{"key": "Data", "value": "Parameter %d" % index["Play"]}]}
        play["bounds"]["bounds"] = "%d %d %d %d" % (PLAY[0] - 6, PLAY[1] - 6, PLAY[2] + 12, PLAY[3] + 12)
        deck.append(play)
        tab[0:0] = deck                                   # behind the controls, background first

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
