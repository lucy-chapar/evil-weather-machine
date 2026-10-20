#!/usr/bin/env python3
"""
Render the icons and screens in src/art.h to docs/preview-weather.png and
docs/preview-shot.png (needs rsvg-convert).

Mirrors the particle effects in main.cpp so animated icons can be checked
without hardware.  Run from anywhere:  python3 tools/preview_art.py
"""
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = open(os.path.join(ROOT, "src", "art.h")).read()

PALETTE = {
    "Y": (255, 200, 0), "O": (255, 110, 0), "W": (210, 210, 210), "G": (100, 100, 115),
    "D": (45, 45, 60), "B": (0, 90, 255), "C": (190, 225, 255), "L": (255, 255, 90),
    "M": (210, 210, 160), "S": (120, 120, 170), "F": (90, 90, 100), "f": (50, 50, 56), "R": (255, 0, 0),
    "N": (0, 80, 255), "T": (91, 206, 250), "P": (245, 120, 160), "Z": (0, 220, 60),
}
PHASE = [0, 2, 1, 3]


def art(name):
    body = re.search(r"ART_%s\[\] = \{(.*?)\};" % name, SRC, re.S).group(1)
    return re.findall(r'"([^"]*)"', body)


def icons():
    for m in re.finditer(r"ICON_(\w+) = ICON\(ART_(\w+), (\d+), (FX_\w+)\)", SRC):
        yield m.group(1), art(m.group(2)), m.group(4)


def frame(rows, fx, n):
    count = len(rows) // 8
    grid = [[PALETTE.get(c) for c in row] for row in rows[(n % count) * 8:(n % count) * 8 + 8]]

    def put(x, y, col):
        if 0 <= x < 8 and 0 <= y < 8:
            grid[y][x] = col

    blue = PALETTE["B"]
    if fx in ("FX_RAIN", "FX_STORM"):
        if fx == "FX_STORM":
            blue = tuple(v * 2 // 3 for v in blue)
        for i in range(4):
            t = (n + PHASE[i]) % 4
            x, y = i * 2 + 2 - t, 4 + t
            put(x, y, blue)
            if y > 4:
                put(x + 1, y - 1, tuple(v // 4 for v in blue))
        if fx == "FX_STORM" and n % 16 in (0, 2):
            for dy, row in enumerate(art("BOLT")):
                for x, c in enumerate(row):
                    if c != ".":
                        put(x, 4 + dy, PALETTE[c])
    elif fx == "FX_DRIZZLE":
        for i in range(2):
            put(1 + i * 4, 4 + (n + PHASE[i * 2]) % 4, blue)
    elif fx == "FX_SNOW":
        for i in range(4):
            t = n + PHASE[i]
            put(1 + i * 2 - (t // 4) % 2, 4 + t % 4, PALETTE["C"])
    return grid


def temp_color(c):
    stops = [(-10, 190, 225, 255), (0, 0, 110, 255), (10, 0, 200, 170), (18, 80, 220, 0),
             (24, 255, 200, 0), (30, 255, 90, 0), (36, 255, 0, 0)]
    if c <= stops[0][0]:
        return stops[0][1:]
    for a, b in zip(stops, stops[1:]):
        if c <= b[0]:
            k = (c - a[0]) / (b[0] - a[0])
            return tuple(int(a[i] + k * (b[i] - a[i])) for i in (1, 2, 3))
    return stops[-1][1:]


def temp_screen(temp_f, high):
    """Arrow on rows 0-1, number centred on rows 3-7, as drawTempScreen()."""
    glyphs = {m.group(1): re.findall(r'"([^"]*)"', m.group(2))
              for m in re.finditer(r"\{'(.)', \{(.*?)\}\}", SRC)}
    color = temp_color((temp_f - 32) * 5 / 9)
    cols = []
    for ch in str(temp_f):
        g = glyphs[ch]
        cols += [[g[y][x] == "X" for y in range(5)] for x in range(len(g[0]))] + [[False] * 5]
    cols = cols[:-1]
    grid = [[None] * 8 for _ in range(8)]
    for y, row in enumerate(art("ARROW_UP" if high else "ARROW_DOWN")):
        for x, c in enumerate(row):
            if c == "X":
                grid[y][x] = tuple(v // 2 for v in color)
    x0 = (8 - len(cols)) // 2
    for i, col in enumerate(cols):
        for y in range(5):
            if col[y]:
                grid[3 + y][x0 + i] = color
    return grid


def rain_screen(pct):
    """As drawRainScreen(): filled rows bright, the empty part only as a dim outline."""
    level = (pct * 8 + 50) // 100
    drop = art("DROP")
    blue, dim = PALETTE["B"], tuple(v // 3 for v in PALETTE["B"])

    def inside(x, y):
        return 0 <= x < 8 and 0 <= y < 8 and drop[y][x] == "B"

    grid = [[None] * 8 for _ in range(8)]
    for y in range(8):
        for x in range(8):
            if not inside(x, y):
                continue
            edge = not all(inside(x + dx, y + dy) for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)))
            grid[y][x] = blue if y >= 8 - level else (dim if edge else None)
    return grid


def shot_screen(day, level=4, side=None, sparkles=()):
    """Heart screen as drawHeartScreen(): the flag heart filled 'day' thirds, or on
    shot day ('side' set) full and beating with the green corner dot."""
    filled = 6 if side else day * 2
    grid = [[None] * 8 for _ in range(8)]
    for y, row in enumerate(art("HEART")):
        for x, c in enumerate(row):
            if c != ".":
                col = PALETTE[c]
                lit = y >= 6 - filled
                grid[y + 1][x] = tuple(v * level // 4 if lit else v * 7 // 25 for v in col)
    for x, y in sparkles:
        grid[y][x] = (255, 255, 255)
    if side:
        x0 = 0 if side == "L" else 6
        for y in (6, 7):
            for x in (x0, x0 + 1):
                grid[y][x] = PALETTE["Z"]
    return grid


def svg(blocks):
    px, gap = 12, 16
    width = max(sum(len(g[0]) for g in row) * px + gap * len(row) for _, row in blocks) + 160
    out, y = [], 10
    for label, row in blocks:
        out.append('<text x="10" y="%d" font-family="Helvetica" font-size="14" fill="#ddd">%s</text>'
                   % (y + 52, label))
        x = 150
        for grid in row:
            out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#111"/>'
                       % (x - 2, y - 2, len(grid[0]) * px + 4, 8 * px + 4))
            for gy, line in enumerate(grid):
                for gx, col in enumerate(line):
                    fill = "rgb(%d,%d,%d)" % col if col else "#222"
                    out.append('<circle cx="%d" cy="%d" r="%d" fill="%s"/>'
                               % (x + gx * px + px // 2, y + gy * px + px // 2, px // 2 - 1, fill))
            x += len(grid[0]) * px + gap
        y += 8 * px + 20
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">'
            '<rect width="100%%" height="100%%" fill="#000"/>%s</svg>' % (width, y, "".join(out)))


def write_png(name, blocks):
    svg_path = os.path.join(ROOT, "docs", name + ".svg")
    open(svg_path, "w").write(svg(blocks))
    png = svg_path[:-4] + ".png"
    subprocess.run(["rsvg-convert", "-o", png, svg_path], check=True)
    os.remove(svg_path)
    print(png)


def main():
    weather = []
    for name, rows, fx in icons():
        frames = 4 if fx != "FX_NONE" or len(rows) > 8 else 1
        weather.append((name.lower(), [frame(rows, fx, n) for n in range(frames)]))
    weather.append(("high / low (F)", [temp_screen(t, h) for t, h in ((64, True), (41, False), (88, True), (7, False))]))
    weather.append(("rain 0/25/50/100%", [rain_screen(p) for p in (0, 25, 50, 100)]))
    shot = [
        ("shot day, left", [shot_screen(3, 4, "L", ((0, 0), (7, 1))), shot_screen(3, 2, "L", ((3, 0), (7, 7)))]),
        ("shot day, right", [shot_screen(3, 4, "R", ((7, 0), (0, 1))), shot_screen(3, 2, "R", ((4, 7), (0, 5)))]),
    ]
    os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
    old = os.path.join(ROOT, "docs", "preview.png")
    if os.path.exists(old):
        os.remove(old)
    write_png("preview-weather", weather)
    write_png("preview-shot", shot)


if __name__ == "__main__":
    main()
