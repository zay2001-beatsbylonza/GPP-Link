#!/usr/bin/env python3
"""Build GPP Link's screen ("Plugin Skins"): a cassette deck and big labelled buttons.

Akai's gen_ui.py draws a bool parameter as a small pill switch. This runs gen_ui.py
with plugin/ui.json, then swaps each send switch for a full-size button whose
caption is drawn into its images (black, red and yellow, like the LINK key):
red with yellow text when idle, yellow with black text while it works.
(Captions are baked into the images because a text label on top of a Button
swallows taps on the MPC.)

It also draws the cassette deck across the top of both tabs (LINK = side A,
RECEIVED = side B): the shell and label art with a BEATS BY LONZA sticker, a
status line, two reels that turn (a film strip following the "Reel" value),
32 bars on the label that bounce to the sound (film strips following
"Wave 01".."Wave 32": mirrored around the centre line, red tips when loud) and a
big PLAY / STOP button.

The file pickers are drawn without the stock slider bar: the list's name in red,
the picked file big in yellow (a Value label), and "3 / 48" (a readout parameter)
at the right. An invisible value-only knob on top takes the touches.

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
    "Send to MPC":        ("SEND TO", "MPC"),
    "Send to Folder":     ("SEND TO", "FOLDER"),
    "Send to GPP":        ("SEND TO", "GET PRODUCERS PAID"),
    "Send Stems":         ("SEND STEMS TO", "GET PRODUCERS PAID"),
    "Received to Folder": ("SEND TO", "FOLDER"),
    "Received to GPP":    ("SEND TO", "GET PRODUCERS PAID"),
}

# picker -> (its "3 / 48" readout, extra width: one column = 320 px)
PICKERS = {
    "Computer Files": ("Files Pos", 0),
    "From MPC":       ("MPC Pos", 320),       # feeds SEND TO FOLDER and SEND TO GET PRODUCERS PAID
    "Computer Stems": ("Stems Pos", 0),
    "Received":       ("Received Pos", 640),  # the RECEIVED tab's one long picker
}
SLOT_X, PICK_Y, SLOT_W, SLOT_H = [23, 343, 663, 983], 346, 278, 107
COUNTER = (983, 346, 278, 107)        # RECEIVED tab: how many files this MPC has received
QLINK_STRIP = 10                      # height of the MPC's Q-Link highlight, at the top of each column

# Cassette deck geometry (absolute, on the 1280 x 628 page; the controls start at y 346)
DECK = (382, 12, 516, 322)            # x, y, w, h: a real cassette is about 1.6 : 1
LABEL = (408, 30, 464, 72)            # yellow label strip across the top: the waveform runs on it
STATUS = (34, 150, 330, 42)           # live status text, in the display left of the cassette
REELS = [(450, 108), (706, 104)]      # top-left of each 124 x 124 reel
REEL_SIZE, REEL_FRAMES = 124, 12
BARS_X, BARS_Y, BAR_W, BAR_STEP, BAR_H = 454, 38, 9, 12.8, 56   # 32 thin bars across the label
WAVE_BARS, WAVE_LEVELS = 32, 17
SHOW_BARS = range(WAVE_BARS)
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


def sticker(size):
    """The BEATS BY LONZA square: black, yellow frame, three cassette stripes, stacked lettering."""
    S = 4
    n = size * S
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, n - 1, n - 1], n // 10, fill=BLACK, outline=YELLOW, width=max(2, n // 40))
    for k, c in enumerate([RED, YELLOW, RED]):
        x0 = n * (0.08 + k * 0.055)
        d.rectangle([x0, n * 0.14, x0 + n * 0.032, n * 0.86], fill=c)
    tx, room = n * 0.30, n * 0.62
    z = int(n * 0.36)
    while max(d.textlength(w, font=font(True, z)) for w in ("BEATS", "LONZA")) > room:
        z -= 2
    big = font(True, z)
    d.text((tx, n * 0.10), "BEATS", font=big, fill=(255, 255, 255, 255))
    d.rectangle([tx, n * 0.45, tx + n * 0.19, n * 0.56], fill=RED)
    d.text((tx + n * 0.095, n * 0.505), "BY", font=font(True, int(n * 0.11)), fill=YELLOW, anchor="mm")
    d.text((tx, n * 0.57), "LONZA", font=big, fill=YELLOW)
    return im.resize((size, size), Image.LANCZOS)


def picker_frame(d, x, y, w, h):
    d.rounded_rectangle([x + 6, y + 6, x + w - 6, y + h - 6], 12, fill=(20, 20, 22, 255), outline=(66, 66, 72, 255), width=2)


def deck_art(side="A"):
    """A tab's background: a clear smoky cassette in real proportions in the middle (yellow top
    strip for the bouncing bars, tape packs, a Get Producers Paid block, a BEATS BY LONZA
    sticker, the bottom rail), a status display on the left, and the picker frames.
    Side A is the LINK tab, side B (red side marks) the RECEIVED tab. Original artwork."""
    W0, H0 = 1280, 628
    img = Image.new("RGBA", (W0, H0), BLACK)
    d = ImageDraw.Draw(img)

    # --- left: the display ---
    dx, dy, dw, dh = DISPLAY
    d.rounded_rectangle([dx, dy, dx + dw, dy + dh], 16, fill=(18, 18, 22, 255), outline=(70, 70, 76, 255), width=2)
    d.text((dx + 20, dy + 18), "GPP", font=font(True, 44), fill=RED)
    d.text((dx + 20 + d.textlength("GPP ", font=font(True, 44)), dy + 18), "LINK", font=font(True, 44), fill=YELLOW)
    if side == "A":
        d.text((dx + 22, dy + 70), "BeatsbyLonza", font=font(False, 20), fill=(170, 170, 176, 255))
    else:
        tag = "B-SIDE  ·  RECEIVED"
        tw = d.textlength(tag, font=font(True, 17))
        d.rounded_rectangle([dx + 20, dy + 70, dx + 40 + tw, dy + 98], 6, fill=RED)
        d.text((dx + 30, dy + 84), tag, font=font(True, 17), fill=YELLOW, anchor="lm")
    d.line([dx + 20, dy + 104, dx + dw - 20, dy + 104], fill=(60, 60, 66, 255), width=2)
    if side == "A":
        d.text((dx + 20, dy + 186), "NOW PLAYING  ·  STATUS", font=font(False, 16), fill=(120, 120, 128, 255))
    else:
        d.text((dx + 20, dy + 168), "Load them in the MPC browser:", font=font(False, 16), fill=(150, 150, 156, 255))
        d.text((dx + 20, dy + 192), "MPC User  \u203a  From Computer", font=font(True, 17), fill=YELLOW)
        d.text((dx + 20, dy + 224), "NEWEST FIRST  ·  TAP, THEN TURN THE WHEEL", font=font(False, 13), fill=(110, 110, 116, 255))

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
    if side == "A":
        d.text((lx + 10, ly + 14), "A", font=font(True, 36), fill=(110, 90, 10, 255))
    else:
        d.rounded_rectangle([lx, ly, lx + 38, ly + lh], 8, fill=RED)
        d.rectangle([lx + 20, ly, lx + 38, ly + lh], fill=RED)
        d.text((lx + 19, ly + lh / 2), "B", font=font(True, 34), fill=YELLOW, anchor="mm")
    # the centre line the bars bounce from
    d.line([BARS_X - 2, BARS_Y + BAR_H // 2, BARS_X + WAVE_BARS * BAR_STEP, BARS_Y + BAR_H // 2], fill=(200, 160, 0, 255), width=1)
    # side tab: orange on A, red on B
    d.rectangle([x + 14, y + 104, x + 40, y + 170], fill=(232, 160, 30, 255) if side == "A" else RED)
    d.text((x + 27, y + 137), side, font=font(True, 22), fill=(255, 255, 255, 255) if side == "A" else YELLOW, anchor="mm")
    # tape window and the two tape packs
    cx0, cx1 = REELS[0][0] + REEL_SIZE // 2, REELS[1][0] + REEL_SIZE // 2
    cy = REELS[0][1] + REEL_SIZE // 2
    d.rounded_rectangle([cx0 - 20, cy - 46, cx1 + 20, cy + 46], 30, fill=(26, 28, 36, 255))
    for c, r in ((cx0, 76), (cx1, 66)):
        d.ellipse([c - r, cy - r, c + r, cy + r], fill=(58, 40, 30, 255), outline=(84, 60, 44, 255), width=2)
    # BEATS BY LONZA sticker under the left reel
    img.alpha_composite(sticker(88), (x + 36, y + 224))
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

    # --- the picker frames (the pickers draw their text on top) ---
    if side == "A":
        picker_frame(d, SLOT_X[0], PICK_Y, SLOT_W, SLOT_H)
        picker_frame(d, SLOT_X[1], PICK_Y, SLOT_W + 320, SLOT_H)
        picker_frame(d, SLOT_X[3], PICK_Y, SLOT_W, SLOT_H)
    else:
        picker_frame(d, SLOT_X[0], PICK_Y, SLOT_W + 640, SLOT_H)
        cx, cy2, cw, ch = COUNTER
        d.rounded_rectangle([cx + 6, cy2 + 6, cx + cw - 6, cy2 + ch - 6], 12, fill=RED)
        d.text((cx + cw / 2, cy2 + ch - 26), "FILES RECEIVED", font=font(True, 16), fill=YELLOW, anchor="mm")
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
    """Frames 0..16: a deep blue bar mirrored around the centre, that tall (1 = a dot on the
    line); 17..33: the same with red tips (the loud ones)."""
    total = 2 * WAVE_LEVELS
    strip = Image.new("RGBA", (BAR_W, BAR_H * total), (0, 0, 0, 0))
    d = ImageDraw.Draw(strip)
    for f in range(total):
        level = f % WAVE_LEVELS
        top = f * BAR_H
        mid = top + BAR_H // 2
        half = max(1, round(level / (WAVE_LEVELS - 1) * (BAR_H / 2 - 1)))
        d.rounded_rectangle([1, mid - half, BAR_W - 2, mid + half], 3, fill=BLUE)
        if f >= WAVE_LEVELS and half > 6:
            d.rounded_rectangle([1, mid - half, BAR_W - 2, mid - half + 5], 2, fill=RED)
            d.rounded_rectangle([1, mid + half - 5, BAR_W - 2, mid + half], 2, fill=RED)
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
        value_label = next(c for c in toggle["componentsData"] if c["componentData"]["name"] == "value")
        slider_def = next(lc["value"] for lc in locals_ if lc["key"] == "sliderWithFocus")

        index = {p["name"]: p.get("index", i) for i, p in enumerate(json.load(open(params))["params"])}
        tabs = [t["componentDefinition"]["componentsData"] for t in page["tabs"]]
        if len(tabs) != 2:
            sys.exit("make_skin: expected 2 tabs (LINK, RECEIVED), found %d" % len(tabs))
        template = next(c for c in tabs[0] if c["componentData"]["type"] == "toggle")

        def text_style(height, colour, just, case="Original", style="Regular"):
            return {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": style, "height": float(height)},
                    "colour": colour, "justification": just, "case": case}

        def label(name, kind, handle, x, y, w, h, style):
            lab = copy.deepcopy(value_label)
            lab["componentData"]["name"] = name
            lab["componentData"]["data"] = {"version": 1, "textStyle": style, "type": kind, "handleName": handle}
            lab["handle remapping"] = {"version": 1, "map": []}
            lab["bounds"] = bounds("No", x, y, w, h)
            return lab

        # --- the cassette deck, on both tabs: side A (LINK) and side B (RECEIVED) ---
        reel_strip().save(os.path.join(gen, "reel.png"))
        bar_strip().save(os.path.join(gen, "wavebar.png"))
        for t, (tab, side) in enumerate(zip(tabs, "AB")):
            art = "deck.png" if side == "A" else "deck_b.png"
            deck_art(side).save(os.path.join(gen, art))
            deck = [image_entry("deck", art, 0, 0, 1280, 628)]
            for k, (rx, ry) in enumerate(REELS):
                deck.append(image_entry("reel %d" % (k + 1), "reel.png", rx, ry, REEL_SIZE, REEL_SIZE,
                                        REEL_FRAMES, index["Reel"]))
            for b in SHOW_BARS:
                deck.append(image_entry("wave %02d" % (b + 1), "wavebar.png", round(BARS_X + b * BAR_STEP), BARS_Y,
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
            if side == "B":   # the files-received counter, a big number in the red box
                cx, cy, cw, ch = COUNTER
                count = label("received count", "Value", "Data", cx + 6, cy + 10, cw - 12, 56,
                              text_style(44, "ffffd400", "horizontallyCentred verticallyCentred", style="SemiBold"))
                count["handle remapping"] = {"version": 1, "map": [{"key": "Data", "value": "Parameter %d" % index["Received Count"]}]}
                deck.append(count)
            tab[0:0] = deck                                   # behind the controls, background first

        # --- the pickers: no slider bar; red list name, big yellow pick, "3 / 48" at the right.
        # The pick is a value-only knob, so the Q-Link, the data wheel and a drag still turn it.
        # No pop-up knob on double tap / Enter.
        actions = [a for a in slider_def["actions"] if a.get("handler") != "Show Overlay"]
        made = set()
        for tab in tabs:
            for inst in tab:
                name = inst["componentData"]["name"]
                if inst["componentData"]["type"] != "sliderWithFocus" or name not in PICKERS:
                    continue
                pos, extra = PICKERS[name]
                w = SLOT_W + extra
                key = "picker%d" % w
                if key not in made:
                    made.add(key)
                    f = copy.deepcopy(focus)
                    f["componentData"]["data"] = {"version": 1, "backgroundColour": "0dffffff", "outlineColour": "ffffd400",
                                                  "backgroundInset": 8.0, "outlineThickness": 3.0}
                    f["bounds"]["bounds"] = "0 0 %d %d" % (w, SLOT_H)
                    pick = copy.deepcopy(value_label)
                    pick["componentData"]["name"] = "pick"
                    pick["componentData"]["type"] = "Knob"
                    # the knob only takes the turns and drags: its own text is see-through, because the
                    # MPC doesn't draw a value-only knob's text; the "value" label under it shows the pick
                    pick["componentData"]["data"] = {"version": 4, "knobType": "ValueSlider",
                                                     "textStyle": text_style(26, "00000000", "left verticallyCentred"),
                                                     "dragOrientation": "Horizontal", "handleName": "Data"}
                    pick["handle remapping"] = {"version": 1, "map": []}
                    pick["bounds"] = bounds("No", 20, 42, w - 40, 54)
                    comp = copy.deepcopy(slider_def)
                    comp["actions"] = copy.deepcopy(actions)
                    comp["componentsData"] = [
                        f,
                        label("name", "Name", "Data", 20, 12, w - 160, 30, text_style(19, "ffe53935", "left verticallyCentred", "Upper Case", "SemiBold")),
                        label("pos", "Value", "Pos", w - 150, 12, 130, 30, text_style(18, "ff9a9a9a", "right verticallyCentred")),
                        label("value", "Value", "Data", 20, 42, w - 40, 54,
                              text_style(32 if w > SLOT_W else 26, "ffffd400", "left verticallyCentred", style="SemiBold")),
                        pick,   # on top, so it gets the touches
                    ]
                    locals_.append({"key": key, "value": comp})
                inst["componentData"]["type"] = key
                x, y, _, h = map(int, inst["bounds"]["bounds"].split())
                inst["bounds"]["bounds"] = "%d %d %d %d" % (x, y, w, h)
                inst["handle remapping"]["map"].append({"key": "Pos", "value": "Parameter %d" % index[pos]})
        if len(made) != 3:
            sys.exit("make_skin: expected pickers of 3 widths, made %s" % sorted(made))
        if not any(c["componentData"]["type"] == "sliderWithFocus" for tab in tabs for c in tab):
            locals_[:] = [lc for lc in locals_ if lc["key"] != "sliderWithFocus"]

        # --- the big buttons ---
        swapped = 0
        for tab in tabs:
            for inst in tab:
                name = inst["componentData"]["name"]
                if inst["componentData"]["type"] != "toggle" or (name not in BUTTONS and name != "Play"):
                    continue
                key = "bigButton_" + slug(name)
                on_png, off_png = "btn_%s_on.png" % slug(name), "btn_%s_off.png" % slug(name)
                if not any(lc["key"] == key for lc in locals_):
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
        want = 4 + 4 + 2   # LINK: 4 buttons + PLAY; RECEIVED: 4 buttons + PLAY
        if swapped != want:
            sys.exit("make_skin: expected %d buttons, found %d" % (want, swapped))
        # the pill switch is no longer used: drop its definition and images (the
        # validator rejects a definition whose images are missing)
        if not any(c["componentData"]["type"] == "toggle" for tab in tabs for c in tab):
            locals_[:] = [lc for lc in locals_ if lc["key"] != "toggle"]
        for f in ("toggle_on.png", "toggle_off.png"):
            p = os.path.join(gen, f)
            if os.path.exists(p):
                os.remove(p)
        # The MPC's Q-Link highlight: only a thin strip at the top of each column, so it shows
        # which Q-Link is live without a box over the controls.
        for t in page["tabs"]:
            t["qlinkBoundsData"] = ["%d 0 248 %d" % (x, QLINK_STRIP) for x in (38, 358, 678, 998)]
        json.dump(tui, open(tui_path, "w"), indent=1)

        # Q-Links only scroll the lists. Buttons and the cassette are tapped: a knob turned
        # by accident must never send a file or start the tape.
        ql_path = os.path.join(gen, "Q-Links.json")
        ql = json.load(open(ql_path))
        lists = {index[n] for n in PICKERS}
        for m in ql["Screen Mode Q-Links"]["map"]:
            for k, v in m["Q-Links"].items():
                if v not in lists:
                    m["Q-Links"][k] = -1
        program = ["Computer Files", "From MPC", "Computer Stems", "Received"]   # track / program Q-Link mode
        ql["Program Mode Q-Links"] = {"Q-Link %d" % (i + 1): (index[program[i]] if i < len(program) else -1) for i in range(16)}
        json.dump(ql, open(ql_path, "w"), indent=4)
        if os.path.isdir(out):
            shutil.rmtree(out)
        shutil.copytree(gen, out)
    print("make_skin: wrote %s (2 tabs, cassette deck, %d big buttons)" % (out, swapped))


if __name__ == "__main__":
    main()
