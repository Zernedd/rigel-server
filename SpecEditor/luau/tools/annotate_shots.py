#!/usr/bin/env python3
"""
annotate_shots.py -- turns raw editor screenshots (tests/t_docshots.txt -> %TEMP%\\rigel_shots\\*.bmp) into the pictures the
guides use: red rings / boxes around what the text talks about, numbered badges that the captions refer to, and an
optional crop so the part that matters is big enough to read.

    python tools/annotate_shots.py <raw shots folder>        -> luau/doc-images/*.png

Each entry in SHOTS: source name, output name, crop box (or None), and marks. A mark is
    ("ring", x0, y0, x1, y1, n)   an ellipse around a thing, badge n
    ("box",  x0, y0, x1, y1, n)   a rounded rectangle, badge n
Coordinates are in the RAW screenshot's pixels (980 x 610).
"""
from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "doc-images")
RED = (235, 64, 52)


def font(size):
    for f in ("segoeuib.ttf", "arialbd.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def badge(d, x, y, n, scale):
    r = int(13 * scale)
    d.ellipse((x - r, y - r, x + r, y + r), fill=RED, outline=(255, 255, 255), width=max(2, int(2 * scale)))
    f = font(int(16 * scale))
    t = str(n)
    w = d.textlength(t, font=f)
    d.text((x - w / 2, y - 10 * scale), t, font=f, fill=(255, 255, 255))


def render(src, dst, crop, marks, scale_to=1400):
    im = Image.open(src).convert("RGB")
    if crop:
        im = im.crop(crop)
        ox, oy = crop[0], crop[1]
    else:
        ox = oy = 0
    s = min(2.2, max(1.0, scale_to / im.width))                  # zoom small crops, but not absurdly
    if s != 1.0:
        im = im.resize((int(im.width * s), int(im.height * s)), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    lw = max(3, int(3 * s))
    for m in marks:
        kind, x0, y0, x1, y1, n = m[:6]
        side = m[6] if len(m) > 6 else ""
        a = ((x0 - ox) * s, (y0 - oy) * s, (x1 - ox) * s, (y1 - oy) * s)
        if kind == "ring":
            d.ellipse(a, outline=RED, width=lw)
        else:
            d.rounded_rectangle(a, radius=int(6 * s), outline=RED, width=lw)
        if n:
            # the badge sits OUTSIDE the mark (left of it, or right when there's no room) so it hides nothing
            r = 13 * s
            cy = min(max((a[1] + a[3]) / 2, r + 2), im.height - r - 2)
            cx = a[0] - r - 4 if a[0] - 2 * r - 6 >= 0 else a[2] + r + 4
            if side == "in":                                      # inside the top-left corner (tight grids)
                cx, cy = a[0] + r + 3, a[1] + r + 3
            elif side == "b":
                cx, cy = a[0] + r + 4, a[3] + r + 4
            elif side == "r":
                cx = a[2] + r + 4
            cx = min(cx, im.width - r - 2)
            cy = min(cy, im.height - r - 2)
            badge(d, cx, cy, n, s)
    os.makedirs(OUT, exist_ok=True)
    im.save(os.path.join(OUT, dst), optimize=True)
    print("wrote", dst, im.size)


# (raw shot, output, crop, marks) -- coordinates read off the raw 980 x 610 captures
SHOTS = [
    # "Opening the Game Modes window": 1 the toolbar button, 2 the mode's orange box + label, 3 its team changers
    ("gm_overview.png", "gm-open.png", (0, 0, 980, 400),
     [("ring", 128, 18, 238, 47, 1), ("box", 262, 172, 716, 292, 2), ("box", 262, 305, 716, 394, 3)]),
    # "Making one": 1 + New game mode, 2 name, 3 teams, 4 team names + max players, 5 Create
    ("gm_new_popup.png", "gm-new.png", (35, 60, 885, 380),
     [("ring", 782, 84, 882, 106, 1), ("box", 41, 260, 342, 282, 2), ("box", 41, 283, 342, 305, 3),
      ("box", 41, 306, 347, 352, 4), ("ring", 38, 350, 168, 375, 5)]),
    # the window: 1 your modes, 2 state / round / teams, 3 round controls, 4 teams, 5 place team changer
    ("gmw_0.png", "gm-window.png", (100, 62, 882, 262),
     [("box", 106, 110, 363, 147, 1, "b"), ("box", 373, 129, 642, 165, 2), ("box", 375, 165, 736, 187, 3),
      ("box", 375, 210, 606, 255, 4), ("ring", 598, 208, 772, 257, 5)]),
    # "Rules": 1 how a round starts, 2 the numbers, 3 restart / empty-teams switches
    ("gmw_0.png", "gm-rules.png", (370, 258, 882, 445),
     [("box", 375, 280, 557, 301, 1), ("box", 375, 304, 627, 416, 2), ("box", 375, 418, 837, 440, 3)]),
    # "Pieces and roles": 1 buttons, 2 traps, 3 moving platforms, 4 force field, 5 round timer, 6 scoreboards + table
    ("gmw_1.png", "gm-pieces.png", (370, 105, 882, 560),
     [("box", 378, 131, 632, 239, 1, "in"), ("box", 636, 131, 806, 239, 2, "in"), ("box", 378, 241, 806, 345, 0),
      ("box", 378, 348, 461, 452, 0), ("box", 464, 348, 632, 452, 3, "in"), ("box", 634, 348, 719, 452, 4, "in"),
      ("box", 721, 348, 806, 452, 5, "in"), ("box", 378, 456, 806, 558, 6, "in")]),
    # objects + roles: 1 the controller, 2 role drop-downs, 3 slot names for your code, 4 script buttons
    ("gmw_3.png", "gm-roles.png", (370, 160, 882, 560),
     [("box", 375, 179, 830, 200, 1), ("box", 578, 204, 772, 421, 2), ("box", 770, 254, 862, 396, 3),
      ("box", 375, 487, 864, 510, 4)]),
    # "Your own code": 1 the examples list, 2 Start from example...
    ("gm_example_popup.png", "gm-examples.png", (100, 20, 882, 560),
     [("box", 117, 28, 730, 152, 1), ("ring", 576, 485, 746, 511, 2)]),
    # "Scoreboards": 1 a scoreboard monitor, 2 the classic Score board
    ("gm_scoreboards.png", "gm-scoreboards.png", (195, 250, 746, 396),
     [("ring", 424, 286, 556, 358, 1), ("ring", 646, 304, 746, 376, 2)]),
    # "The examples": the example levels under Local projects
    ("levels_tab.png", "gm-levels.png", (746, 280, 980, 590),
     [("box", 748, 496, 978, 588, 1)]),
]

if __name__ == "__main__":
    raw = sys.argv[1]
    for name, out, crop, marks in SHOTS:
        render(os.path.join(raw, name), out, crop, marks)
