#!/usr/bin/env python3
"""
Generate the Digital Sand toy carrier PCB as a KiCad 7 project.

The board replaces the point-to-point wiring of the PrintKits "Digital Sand"
kit (ESP32 DevKit V1 30-pin, GY-521 MPU-6050, 6 mm button, WS2812B 8x8 panel,
4xAA battery box) with a two-layer carrier board.

Run with KiCad's bundled Python, which provides the pcbnew module:

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
        pcb/tools/generate_kicad.py

The script rewrites the project libraries, schematic and board, fills the
copper zones, runs DRC and exports fabrication files and previews.
Edits made by hand in KiCad are overwritten when the script runs again.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
NAME = "digital_sand"
SCH_FILE = os.path.join(PROJ, NAME + ".kicad_sch")
PCB_FILE = os.path.join(PROJ, NAME + ".kicad_pcb")
PRO_FILE = os.path.join(PROJ, NAME + ".kicad_pro")
LIB_DIR = os.path.join(PROJ, "lib")
SYM_LIB = os.path.join(LIB_DIR, "digital_sand.kicad_sym")
FP_LIB = os.path.join(LIB_DIR, "digital_sand.pretty")
FAB_DIR = os.path.join(PROJ, "fab")
DOCS_DIR = os.path.join(PROJ, "docs")

KICAD = "/Applications/KiCad/KiCad.app/Contents"
KICAD_CLI = os.path.join(KICAD, "MacOS", "kicad-cli")
STOCK_SYM = os.path.join(KICAD, "SharedSupport", "symbols")
STOCK_FP = os.path.join(KICAD, "SharedSupport", "footprints")
MODELS = "${KICAD6_3DMODEL_DIR}"

TITLE = "Digital Sand Toy - ESP32 Carrier"
DATE = "2026-10-19"
REV = "1.0"

NS = uuid.UUID("0d9b3f5e-2f4c-4b8e-9d41-7c3a6e5b8f20")


def uid(key):
    """Deterministic UUID so regenerating gives stable files."""
    return str(uuid.uuid5(NS, key))


def f(v):
    """Format a number the way KiCad writes it."""
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def run(cmd):
    print("$ " + " ".join(cmd))
    res = subprocess.run(cmd, capture_output=True, text=True)
    out = (res.stdout + res.stderr).strip()
    if out:
        print(out)
    if res.returncode != 0:
        sys.exit("command failed: %s" % cmd[0])


# ---------------------------------------------------------------------------
# Pinouts
# ---------------------------------------------------------------------------

# ESP32 DevKit V1 30-pin, viewed from the component side with the antenna at
# the top.  Header 1 (pins 1-15) is the left column, header 2 (pins 16-30) the
# right column.  "D#" is the GPIO number printed on the board.
ESP32_LEFT = [  # (pin, name, ERC type), top to bottom
    (1, "EN", "input"), (2, "VP/D36", "input"), (3, "VN/D39", "input"),
    (4, "D34", "input"), (5, "D35", "input"), (6, "D32", "bidirectional"),
    (7, "D33", "bidirectional"), (8, "D25", "bidirectional"),
    (9, "D26", "bidirectional"), (10, "D27", "bidirectional"),
    (11, "D14", "bidirectional"), (12, "D12", "bidirectional"),
    (13, "D13", "bidirectional"), (14, "GND", "power_in"), (15, "VIN", "power_in"),
]
ESP32_RIGHT = [  # top to bottom
    (30, "D23", "bidirectional"), (29, "D22", "bidirectional"),
    (28, "TX0/D1", "bidirectional"), (27, "RX0/D3", "bidirectional"),
    (26, "D21", "bidirectional"), (25, "D19", "bidirectional"),
    (24, "D18", "bidirectional"), (23, "D5", "bidirectional"),
    (22, "TX2/D17", "bidirectional"), (21, "RX2/D16", "bidirectional"),
    (20, "D4", "bidirectional"), (19, "D2", "bidirectional"),
    (18, "D15", "bidirectional"), (17, "GND", "power_in"), (16, "3V3", "power_out"),
]
ESP32_SILK = {n: name.split("/")[0] for n, name, _ in ESP32_LEFT + ESP32_RIGHT}

# GY-521 header order (pin 1 = VCC).
GY521_PINS = [
    (1, "VCC", "power_in"), (2, "GND", "power_in"), (3, "SCL", "input"),
    (4, "SDA", "bidirectional"), (5, "XDA", "bidirectional"), (6, "XCL", "output"),
    (7, "AD0", "input"), (8, "INT", "output"),
]

# Expected schematic connectivity, checked against the exported netlist.
EXPECTED_NETS = {
    "VBAT": {("J1", "1"), ("J2", "1"), ("J3", "3"), ("C1", "1"), ("U1", "15")},
    "GND": {("J1", "2"), ("J2", "2"), ("J3", "2"), ("C1", "2"), ("U1", "14"),
            ("U1", "17"), ("U2", "2"), ("SW1", "2")},
    "+3V3": {("U1", "16"), ("U2", "1")},
    "/SDA": {("U1", "26"), ("U2", "4")},
    "/SCL": {("U1", "29"), ("U2", "3")},
    "/LED_DIN": {("U1", "23"), ("J3", "1")},
    "/BTN": {("U1", "20"), ("SW1", "1")},
}


# ---------------------------------------------------------------------------
# Symbol library
# ---------------------------------------------------------------------------

def lib_prop(key, val, x, y, hide=False, justify=None):
    eff = "(effects (font (size 1.27 1.27))"
    if justify:
        eff += " (justify %s)" % justify
    if hide:
        eff += " hide"
    eff += ")"
    return "    (property %s %s (at %s %s 0)\n      %s\n    )\n" % (q(key), q(val), f(x), f(y), eff)


def lib_pin(ptype, x, y, ang, name, num, length=2.54):
    return ("      (pin %s line (at %s %s %d) (length %s)\n"
            "        (name %s (effects (font (size 1.27 1.27))))\n"
            "        (number %s (effects (font (size 1.27 1.27))))\n"
            "      )\n") % (ptype, f(x), f(y), ang, f(length), q(name), q(str(num)))


def esp32_symbol():
    n = "ESP32_DevKit_V1_30pin"
    s = "  (symbol %s (pin_names (offset 1.016)) (in_bom yes) (on_board yes)\n" % q(n)
    s += lib_prop("Reference", "U", 0, 21.59)
    s += lib_prop("Value", n, 0, -21.59)
    s += lib_prop("Footprint", "DigitalSand:ESP32_DevKit_V1_30pin_DualRow", 0, -24.13, hide=True)
    s += lib_prop("Datasheet", "~", 0, 0, hide=True)
    s += lib_prop("ki_keywords", "ESP32 DevKit DOIT WROOM-32 development board", 0, 0, hide=True)
    s += lib_prop("ki_description",
                  "ESP32 DevKit V1 (DOIT style) 30-pin development board, pins in header order. "
                  "D# labels are GPIO numbers.", 0, 0, hide=True)
    s += lib_prop("ki_fp_filters", "ESP32*DevKit*", 0, 0, hide=True)
    s += ('    (symbol "%s_0_1"\n'
          "      (rectangle (start -10.16 20.32) (end 10.16 -20.32)\n"
          "        (stroke (width 0.254) (type default))\n"
          "        (fill (type background))\n"
          "      )\n"
          "    )\n") % n
    s += '    (symbol "%s_1_1"\n' % n
    for i, (num, name, ptype) in enumerate(ESP32_LEFT):
        s += lib_pin(ptype, -12.7, 17.78 - i * 2.54, 0, name, num)
    for i, (num, name, ptype) in enumerate(ESP32_RIGHT):
        s += lib_pin(ptype, 12.7, 17.78 - i * 2.54, 180, name, num)
    s += "    )\n  )\n"
    return s


def gy521_symbol():
    n = "GY-521_MPU6050"
    s = "  (symbol %s (pin_names (offset 1.016)) (in_bom yes) (on_board yes)\n" % q(n)
    s += lib_prop("Reference", "U", 8.89, 8.89, justify="left")
    s += lib_prop("Value", n, 8.89, 6.35, justify="left")
    s += lib_prop("Footprint", "DigitalSand:GY-521_MPU6050_Module", 0, -20.32, hide=True)
    s += lib_prop("Datasheet", "https://invensense.tdk.com/products/motion-tracking/6-axis/mpu-6050/",
                  0, 0, hide=True)
    s += lib_prop("ki_keywords", "MPU6050 GY-521 accelerometer gyroscope IMU I2C module", 0, 0, hide=True)
    s += lib_prop("ki_description",
                  "GY-521 MPU-6050 6-axis IMU breakout (I2C, on-board 3.3 V regulator, "
                  "AD0 pulled low = address 0x68)", 0, 0, hide=True)
    s += lib_prop("ki_fp_filters", "GY-521*", 0, 0, hide=True)
    s += ('    (symbol "%s_0_1"\n'
          "      (rectangle (start -7.62 10.16) (end 7.62 -12.7)\n"
          "        (stroke (width 0.254) (type default))\n"
          "        (fill (type background))\n"
          "      )\n"
          "    )\n") % n
    s += '    (symbol "%s_1_1"\n' % n
    s += lib_pin("power_in", 0, 12.7, 270, "VCC", 1)
    s += lib_pin("power_in", 0, -15.24, 90, "GND", 2)
    for i, (num, name, ptype) in enumerate(GY521_PINS[2:]):
        s += lib_pin(ptype, -10.16, 5.08 - i * 2.54, 0, name, num)
    s += "    )\n  )\n"
    return s


def stock_block(lib, name):
    """Return the text of one symbol from a stock KiCad symbol library."""
    text = open(os.path.join(STOCK_SYM, lib + ".kicad_sym")).read()
    start = text.index('(symbol "%s" ' % name)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return "  " + text[start:i + 1] + "\n"
    raise ValueError(name)


def vbat_symbol():
    # Same graphic as power:+BATT, but the net is called VBAT.
    return stock_block("power", "+BATT").replace("+BATT", "VBAT")


def write_symbol_library():
    blocks = [esp32_symbol(), gy521_symbol(), vbat_symbol()]
    with open(SYM_LIB, "w") as fh:
        fh.write("(kicad_symbol_lib (version 20220914) (generator kicad_symbol_editor)\n")
        for b in blocks:
            fh.write(b)
        fh.write(")\n")
    return {
        "DigitalSand:ESP32_DevKit_V1_30pin": blocks[0],
        "DigitalSand:GY-521_MPU6050": blocks[1],
        "DigitalSand:VBAT": blocks[2],
    }


def write_lib_tables():
    with open(os.path.join(PROJ, "sym-lib-table"), "w") as fh:
        fh.write('(sym_lib_table\n'
                 '  (lib (name "DigitalSand")(type "KiCad")(uri "${KIPRJMOD}/lib/digital_sand.kicad_sym")'
                 '(options "")(descr "Digital Sand project symbols"))\n)\n')
    with open(os.path.join(PROJ, "fp-lib-table"), "w") as fh:
        fh.write('(fp_lib_table\n'
                 '  (lib (name "DigitalSand")(type "KiCad")(uri "${KIPRJMOD}/lib/digital_sand.pretty")'
                 '(options "")(descr "Digital Sand project footprints"))\n)\n')


# ---------------------------------------------------------------------------
# Footprint library
# ---------------------------------------------------------------------------

class Footprint:
    def __init__(self, name, descr, tags):
        self.name = name
        self.descr = descr
        self.tags = tags
        self.body = []
        self.n = 0

    def _ts(self):
        self.n += 1
        return uid("fp:%s:%d" % (self.name, self.n))

    def text(self, kind, txt, x, y, layer, size=1.0, thick=0.15, angle=0, justify=None, hide=False):
        at = "(at %s %s%s)" % (f(x), f(y), (" %s" % f(angle)) if angle else "")
        eff = "(effects (font (size %s %s) (thickness %s))%s)" % (
            f(size), f(size), f(thick), (" (justify %s)" % justify) if justify else "")
        self.body.append("  (fp_text %s %s %s (layer %s)%s\n    %s\n    (tstamp %s)\n  )\n" % (
            kind, q(txt), at, q(layer), " hide" if hide else "", eff, self._ts()))

    def line(self, x1, y1, x2, y2, layer, width):
        self.body.append("  (fp_line (start %s %s) (end %s %s)\n    (stroke (width %s) (type solid)) (layer %s) (tstamp %s))\n" % (
            f(x1), f(y1), f(x2), f(y2), f(width), q(layer), self._ts()))

    def rect(self, x1, y1, x2, y2, layer, width):
        self.body.append("  (fp_rect (start %s %s) (end %s %s)\n    (stroke (width %s) (type solid)) (fill none) (layer %s) (tstamp %s))\n" % (
            f(x1), f(y1), f(x2), f(y2), f(width), q(layer), self._ts()))

    def poly(self, pts, layer, width=0.12, fill="solid"):
        p = " ".join("(xy %s %s)" % (f(x), f(y)) for x, y in pts)
        self.body.append("  (fp_poly\n    (pts %s)\n    (stroke (width %s) (type solid)) (fill %s) (layer %s) (tstamp %s))\n" % (
            p, f(width), fill, q(layer), self._ts()))

    def pad(self, num, shape, x, y, sx, sy, drill):
        self.body.append('  (pad %s thru_hole %s (at %s %s) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask") (tstamp %s))\n' % (
            q(num), shape, f(x), f(y), f(sx), f(sy), f(drill), self._ts()))

    def model(self, path, offset=(0, 0, 0)):
        self.body.append("  (model %s\n    (offset (xyz %s %s %s))\n    (scale (xyz 1 1 1))\n    (rotate (xyz 0 0 0))\n  )\n" % (
            q(path), f(offset[0]), f(offset[1]), f(offset[2])))

    def write(self):
        with open(os.path.join(FP_LIB, self.name + ".kicad_mod"), "w") as fh:
            fh.write("(footprint %s (version 20221018) (generator pcbnew)\n" % q(self.name))
            fh.write('  (layer "F.Cu")\n')
            fh.write("  (descr %s)\n  (tags %s)\n  (attr through_hole)\n" % (q(self.descr), q(self.tags)))
            fh.writelines(self.body)
            fh.write(")\n")


P254 = 2.54
ROW_WIDE = 25.4     # 1.0 in between header rows
ROW_NARROW = 22.86  # 0.9 in between header rows


def esp32_footprint():
    """
    ESP32 DevKit V1 30-pin carrier footprint, antenna end towards -Y.

    Pins 1-15 sit at x = 0 (1.0 in wide boards) and again at x = 2.54
    (0.9 in wide boards).  Both holes carry the same pad number and their oval
    pads overlap, so either board width plugs straight in.  Pins 16-30 are at
    x = 25.4 and shared by both widths.
    """
    fp = Footprint(
        "ESP32_DevKit_V1_30pin_DualRow",
        "ESP32 DevKit V1 (DOIT style) 30-pin board on 2.54 mm headers. Header 1 has holes for "
        "both 1.0 in (25.4 mm) and 0.9 in (22.86 mm) row spacing; header 2 is shared.",
        "ESP32 DevKit DOIT 30pin WROOM-32 module socket")
    pad_long = 3.2  # oval length; the two header-1 rows overlap by 0.66 mm
    for i in range(15):
        y = i * P254
        fp.pad(str(i + 1), "oval", 0, y, pad_long, 1.7, 1.0)
        fp.pad(str(i + 1), "oval", ROW_WIDE - ROW_NARROW, y, pad_long, 1.7, 1.0)
    for n in range(16, 31):
        fp.pad(str(n), "circle", ROW_WIDE, (30 - n) * P254, 1.7, 1.7, 1.0)

    # Board body: about 28.3 x 51 mm, antenna end 4.9 mm past pin 1, USB end
    # 10.3 mm past pin 15.
    bx0, bx1, by0, by1 = -1.45, 26.85, -4.9, 45.86
    fp.rect(bx0, by0, bx1, by1, "F.Fab", 0.1)
    fp.rect(-2.0, -5.25, 27.2, 46.25, "F.CrtYd", 0.05)
    # Silkscreen: open at the USB end, which may overhang the carrier edge.
    sx0, sx1, sy0, sy1 = -2.1, 26.9, -5.0, 44.0
    fp.line(sx0, sy0, sx1, sy0, "F.SilkS", 0.12)
    fp.line(sx0, sy0, sx0, sy1, "F.SilkS", 0.12)
    fp.line(sx1, sy0, sx1, sy1, "F.SilkS", 0.12)
    fp.poly([(-2.9, -0.6), (-2.3, 0), (-2.9, 0.6)], "F.SilkS")  # pin 1
    fp.text("user", "1.0in", 0, -3.0, "F.SilkS", size=0.8, thick=0.15, angle=90)
    fp.text("user", "0.9in", ROW_WIDE - ROW_NARROW, -3.0, "F.SilkS", size=0.8, thick=0.15, angle=90)
    fp.text("user", "ANTENNA", 12.7, -2.9, "F.SilkS", size=0.8, thick=0.15)
    fp.text("user", "USB", 12.7, 41.2, "F.SilkS", size=1.0, thick=0.15)
    fp.poly([(11.7, 42.4), (13.7, 42.4), (12.7, 43.8)], "F.SilkS")
    # Pin labels are centred: KiCad flips rotated text to keep it upright, and
    # a left/right justified label would then land on the pad.
    for num, _, _ in ESP32_LEFT:
        fp.text("user", ESP32_SILK[num], 6.1, (num - 1) * P254, "F.SilkS", size=0.8, thick=0.15)
    for num, _, _ in ESP32_RIGHT:
        fp.text("user", ESP32_SILK[num], 22.7, (30 - num) * P254, "F.SilkS", size=0.8, thick=0.15)
    fp.text("reference", "REF**", 12.7, 18.0, "F.SilkS")
    fp.text("value", "ESP32_DevKit_V1_30pin", 12.7, 25.0, "F.Fab")
    fp.text("user", "${REFERENCE}", 12.7, 21.0, "F.Fab")
    fp.model(MODELS + "/Connector_PinSocket_2.54mm.3dshapes/PinSocket_1x15_P2.54mm_Vertical.wrl")
    fp.model(MODELS + "/Connector_PinSocket_2.54mm.3dshapes/PinSocket_1x15_P2.54mm_Vertical.wrl",
             offset=(ROW_WIDE, 0, 0))
    fp.write()


def gy521_footprint():
    """
    GY-521 MPU-6050 breakout soldered on its straight header.  Pins run along
    +Y from VCC (pin 1) to INT (pin 8); the module body (21.2 x 16.4 mm) lies
    over the carrier on the +X side, component side up.
    """
    fp = Footprint(
        "GY-521_MPU6050_Module",
        "GY-521 MPU-6050 breakout, 1x8 2.54 mm header, module body 21.2 x 16.4 mm over the board",
        "GY-521 MPU6050 accelerometer gyroscope IMU module")
    for num, name, _ in GY521_PINS:
        y = (num - 1) * P254
        fp.pad(str(num), "rect" if num == 1 else "circle", 0, y, 1.7, 1.7, 1.0)
        fp.text("user", name, 3.3, y, "F.SilkS", size=0.8, thick=0.15)
    bx0, bx1, by0, by1 = -1.5, 14.9, -1.71, 19.49
    fp.rect(bx0, by0, bx1, by1, "F.Fab", 0.1)
    fp.rect(bx0 - 0.15, by0 - 0.15, bx1 + 0.15, by1 + 0.15, "F.SilkS", 0.12)
    fp.rect(bx0 - 0.25, by0 - 0.25, bx1 + 0.25, by1 + 0.25, "F.CrtYd", 0.05)
    fp.text("user", "MPU-6050", 10.0, 8.89, "F.SilkS", size=1.0, thick=0.15, angle=90)
    fp.text("reference", "REF**", 7.0, 8.89, "F.SilkS", angle=90)
    fp.text("value", "GY-521_MPU6050", 12.5, 8.89, "F.Fab", angle=90)
    fp.text("user", "${REFERENCE}", 7.0, 15.0, "F.Fab", angle=90)
    fp.model(MODELS + "/Connector_PinHeader_2.54mm.3dshapes/PinHeader_1x08_P2.54mm_Vertical.wrl")
    fp.write()


# ---------------------------------------------------------------------------
# Schematic
# ---------------------------------------------------------------------------

def sch_xform(lx, ly, ang):
    """Library coordinates (Y up) to schematic offset (Y down) for a rotation."""
    ang %= 360
    if ang == 0:
        return lx, -ly
    if ang == 90:
        return -ly, -lx
    if ang == 180:
        return -lx, ly
    if ang == 270:
        return ly, lx
    raise ValueError(ang)


class Placed:
    def __init__(self, lib_pins, x, y, ang):
        self.lib_pins = lib_pins
        self.x, self.y, self.ang = x, y, ang

    def pin(self, num):
        lx, ly = self.lib_pins[str(num)]
        dx, dy = sch_xform(lx, ly, self.ang)
        return round(self.x + dx, 4), round(self.y + dy, 4)


class Schematic:
    POWER_UP = ("power:+3V3", "DigitalSand:VBAT")   # graphic above the pin; GND's is below

    def __init__(self, libs):
        self.root = uid("sch:root")
        self.libs = libs                 # lib_id -> symbol text
        self.lib_pins = {}
        for lib_id, text in libs.items():
            pins = {}
            for m in re.finditer(r'\(pin \S+ \S+ \(at ([-\d.]+) ([-\d.]+) [-\d.]+\).*?\(number "([^"]+)"',
                                 text, re.S):
                pins.setdefault(m.group(3), (float(m.group(1)), float(m.group(2))))
            self.lib_pins[lib_id] = pins
        self.items = []
        self.symbols = []
        self.n = 0
        self.npwr = 0
        self.nflg = 0
        self.uuids = {}                  # reference -> symbol uuid

    def _u(self, kind):
        self.n += 1
        return uid("sch:%s:%d" % (kind, self.n))

    def wire(self, *pts):
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            self.items.append(
                "  (wire (pts (xy %s %s) (xy %s %s))\n    (stroke (width 0) (type default))\n    (uuid %s)\n  )\n"
                % (f(x1), f(y1), f(x2), f(y2), self._u("wire")))

    def noconn(self, x, y):
        self.items.append("  (no_connect (at %s %s) (uuid %s))\n" % (f(x), f(y), self._u("nc")))

    def label(self, name, x, y, ang=0):
        just = "left bottom" if ang == 0 else "right bottom"
        self.items.append(
            "  (label %s (at %s %s %d)\n    (effects (font (size 1.27 1.27)) (justify %s))\n    (uuid %s)\n  )\n"
            % (q(name), f(x), f(y), ang, just, self._u("label")))

    def text(self, txt, x, y, size=1.27, bold=False):
        """Free text; (x, y) is the top-left corner for multi-line text."""
        font = "(font (size %s %s)%s)" % (f(size), f(size), " bold" if bold else "")
        just = "left top" if "\n" in txt else "left bottom"
        self.items.append(
            "  (text %s (at %s %s 0)\n    (effects %s (justify %s))\n    (uuid %s)\n  )\n"
            % (q(txt), f(x), f(y), font, just, self._u("text")))

    def _field(self, key, val, x, y, hide=False, justify=None, angle=0):
        eff = "(effects (font (size 1.27 1.27))"
        if justify:
            eff += " (justify %s)" % justify
        if hide:
            eff += " hide"
        eff += ")"
        return "    (property %s %s (at %s %s %d)\n      %s\n    )\n" % (q(key), q(val), f(x), f(y), angle, eff)

    def place(self, lib_id, ref, value, x, y, ang=0, footprint="", datasheet="~",
              fields=None, in_bom=True, on_board=True):
        """
        Place a symbol.  'fields' maps Reference/Value to (dx, dy, justify[, angle])
        offsets.  Field angles are relative to the symbol rotation in KiCad.
        """
        su = uid("sch:sym:" + ref)
        self.uuids[ref] = su
        fields = fields or {}
        rdx, rdy, rj, ra = (tuple(fields.get("Reference", (0, -3.81, None))) + (0,))[:4]
        vdx, vdy, vj, va = (tuple(fields.get("Value", (0, 3.81, None))) + (0,))[:4]
        hide_ref = ref.startswith("#")
        s = "  (symbol (lib_id %s) (at %s %s %d) (unit 1)\n" % (q(lib_id), f(x), f(y), ang)
        s += "    (in_bom %s) (on_board %s) (dnp no)\n" % ("yes" if in_bom else "no", "yes" if on_board else "no")
        s += "    (uuid %s)\n" % su
        s += self._field("Reference", ref, x + rdx, y + rdy, hide=hide_ref, justify=rj, angle=ra)
        s += self._field("Value", value, x + vdx, y + vdy, justify=vj, angle=va)
        s += self._field("Footprint", footprint, x, y, hide=True)
        s += self._field("Datasheet", datasheet, x, y, hide=True)
        for num in sorted(self.lib_pins[lib_id], key=lambda p: int(p) if p.isdigit() else 0):
            s += "    (pin %s (uuid %s))\n" % (q(num), uid("sch:pin:%s:%s" % (ref, num)))
        s += ("    (instances\n      (project %s\n        (path %s\n          (reference %s) (unit 1)\n"
              "        )\n      )\n    )\n  )\n") % (q(NAME), q("/" + self.root), q(ref))
        self.symbols.append(s)
        return Placed(self.lib_pins[lib_id], x, y, ang)

    def power(self, lib_id, x, y, ang=0):
        """Power symbol (or PWR_FLAG) with its pin at (x, y)."""
        if lib_id == "power:PWR_FLAG":
            self.nflg += 1
            ref = "#FLG%02d" % self.nflg
            value, up, dist = "PWR_FLAG", True, 3.81
        else:
            self.npwr += 1
            ref = "#PWR%02d" % self.npwr
            value = lib_id.split(":")[1]
            up = lib_id in self.POWER_UP
            dist = 3.556 if up else 3.81
        dx, dy = sch_xform(0, 1 if up else -1, ang)
        if dx == 0:
            vfield = (0, dy * dist, None, 0)
        else:
            # Sideways symbol: centre the name just past the graphic.  A field
            # angle of 90 on a 90/270 degree symbol reads horizontally.
            off = 2.54 + 0.5 + 0.57 * len(value)
            vfield = (dx * off, 0, None, 90)
        self.place(lib_id, ref, value, x, y, ang, datasheet="",
                   fields={"Reference": (0, 0, None, 0), "Value": vfield})

    def write(self):
        with open(SCH_FILE, "w") as fh:
            fh.write("(kicad_sch (version 20230121) (generator eeschema)\n\n")
            fh.write("  (uuid %s)\n\n" % self.root)
            fh.write('  (paper "A4")\n\n')
            fh.write("  (title_block\n    (title %s)\n    (date %s)\n    (rev %s)\n"
                     "    (comment 1 %s)\n    (comment 2 %s)\n  )\n\n" % (
                         q(TITLE), q(DATE), q(REV),
                         q("Replaces the point-to-point wiring of the PrintKits Digital Sand kit"),
                         q("ESP32 DevKit V1 30-pin, GY-521 MPU-6050, WS2812B 8x8 panel, 4xAA")))
            fh.write("  (lib_symbols\n")
            for lib_id in sorted(self.libs):
                text = self.libs[lib_id]
                short = lib_id.split(":")[1]
                text = text.replace('(symbol "%s" ' % short, '(symbol "%s" ' % lib_id, 1)
                fh.write(text)
            fh.write("  )\n\n")
            fh.writelines(self.items)
            fh.write("\n")
            fh.writelines(self.symbols)
            fh.write('\n  (sheet_instances\n    (path "/" (page "1"))\n  )\n)\n')


def build_schematic(custom_libs):
    libs = dict(custom_libs)
    for lib, name in [("power", "GND"), ("power", "+3V3"), ("power", "PWR_FLAG"),
                      ("Switch", "SW_Push"), ("Connector", "Screw_Terminal_01x02"),
                      ("Connector", "Screw_Terminal_01x03"), ("Device", "C_Polarized"),
                      ("Mechanical", "MountingHole")]:
        libs["%s:%s" % (lib, name)] = stock_block(lib, name)
    sch = Schematic(libs)

    term2 = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-2-3.5-H_1x02_P3.50mm_Horizontal"
    term3 = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-3-3.5-H_1x03_P3.50mm_Horizontal"

    # --- Section titles and notes ------------------------------------------
    sch.text("POWER IN / LED PANEL", 33.02, 33.02, size=1.8, bold=True)
    sch.text("ESP32 DEVKIT V1 (30-PIN)", 109.22, 68.58, size=1.8, bold=True)
    sch.text("ACCELEROMETER", 182.88, 68.58, size=1.8, bold=True)
    sch.text("BUTTON", 180.34, 116.84, size=1.8, bold=True)
    sch.text("Optional bulk cap\n470-1000 uF, 10 V min", 78.74, 50.8, size=1.0)
    sch.text("SW1 can sit on the board or be wired\nto an external 6 mm button.", 180.34, 134.62, size=1.0)
    sch.text("MPU-6050 I2C address 0x68\n(AD0 pulled low on the GY-521)", 160.02, 106.68, size=1.0)
    sch.text(
        "NOTES\n"
        "1. Same connections as the kit's point-to-point wiring diagram:\n"
        "   D5 (GPIO5) -> LED panel DIN, D4 (GPIO4) -> button to GND,\n"
        "   D21/D22 -> MPU-6050 SDA/SCL, 3V3 -> MPU-6050 VCC.\n"
        "2. The 4xAA pack (J1, has its own switch) feeds the LED panel's extra red/white\n"
        "   power wires (J2) directly; the ESP32 runs from the same rail through VIN and\n"
        "   its on-board 3.3 V regulator.\n"
        "3. U1 accepts 30-pin ESP32 DevKit boards with 0.9 in (22.86 mm) or\n"
        "   1.0 in (25.4 mm) header spacing: header 1 has both rows of holes.\n"
        "4. J1-J3 accept 3.5 mm screw terminals or wires soldered straight in.",
        25.4, 142.24, size=1.27)

    # --- U1 ESP32 -----------------------------------------------------------
    u1 = sch.place("DigitalSand:ESP32_DevKit_V1_30pin", "U1", "ESP32 DevKit V1 30-pin",
                   127.0, 96.52, footprint="DigitalSand:ESP32_DevKit_V1_30pin_DualRow",
                   fields={"Reference": (0, -22.86, None), "Value": (0, 22.86, None)})
    used = {14, 15, 16, 17, 20, 23, 26, 29}
    for num in range(1, 31):
        if num not in used:
            sch.noconn(*u1.pin(num))
    x, y = u1.pin(14)
    sch.wire((x, y), (x - 7.62, y))
    sch.power("power:GND", x - 7.62, y, 270)
    x, y = u1.pin(15)
    sch.wire((x, y), (x - 7.62, y))
    sch.power("DigitalSand:VBAT", x - 7.62, y, 90)
    x, y = u1.pin(17)
    sch.wire((x, y), (x + 7.62, y))
    sch.power("power:GND", x + 7.62, y, 90)
    x, y = u1.pin(16)
    sch.wire((x, y), (x + 7.62, y))
    sch.power("power:+3V3", x + 7.62, y, 270)
    for num, net in [(29, "SCL"), (26, "SDA"), (23, "LED_DIN"), (20, "BTN")]:
        x, y = u1.pin(num)
        sch.wire((x, y), (x + 7.62, y))
        sch.label(net, x + 7.62, y, 0)

    # --- U2 GY-521 ------------------------------------------------------------
    u2 = sch.place("DigitalSand:GY-521_MPU6050", "U2", "GY-521 (MPU-6050)", 203.2, 91.44,
                   footprint="DigitalSand:GY-521_MPU6050_Module",
                   datasheet="https://invensense.tdk.com/products/motion-tracking/6-axis/mpu-6050/",
                   fields={"Reference": (8.89, -8.89, "left"), "Value": (8.89, -6.35, "left")})
    x, y = u2.pin(1)
    sch.wire((x, y), (x, y - 2.54))
    sch.power("power:+3V3", x, y - 2.54, 0)
    x, y = u2.pin(2)
    sch.wire((x, y), (x, y + 2.54))
    sch.power("power:GND", x, y + 2.54, 0)
    for num, net in [(3, "SCL"), (4, "SDA")]:
        x, y = u2.pin(num)
        sch.wire((x, y), (x - 7.62, y))
        sch.label(net, x - 7.62, y, 180)
    for num in (5, 6, 7, 8):
        sch.noconn(*u2.pin(num))

    # --- SW1 ------------------------------------------------------------------
    sw = sch.place("Switch:SW_Push", "SW1", "6mm tactile", 195.58, 124.46,
                   footprint="Button_Switch_THT:SW_PUSH_6mm",
                   fields={"Reference": (0, -3.81, None), "Value": (0, 2.54, None)})
    x, y = sw.pin(1)
    sch.wire((x, y), (x - 7.62, y))
    sch.label("BTN", x - 7.62, y, 180)
    x, y = sw.pin(2)
    sch.wire((x, y), (x + 7.62, y), (x + 7.62, y + 2.54))
    sch.power("power:GND", x + 7.62, y + 2.54, 0)

    # --- J1 battery, J2 LED power, C1 ---------------------------------------
    for ref, value, yy in [("J1", "Battery 4xAA", 45.72), ("J2", "LED panel power", 68.58)]:
        j = sch.place("Connector:Screw_Terminal_01x02", ref, value, 58.42, yy, footprint=term2,
                      fields={"Reference": (0, -2.54, None), "Value": (0, 5.08, None)})
        x, y = j.pin(1)
        sch.wire((x, y), (x - 7.62, y), (x - 7.62, y - 5.08))
        sch.power("DigitalSand:VBAT", x - 7.62, y - 5.08, 0)
        x, y = j.pin(2)
        sch.wire((x, y), (x - 5.08, y), (x - 5.08, y + 5.08))
        sch.power("power:GND", x - 5.08, y + 5.08, 0)

    # ERC: VBAT and GND are supplied from off-board (the battery on J1).
    sch.power("DigitalSand:VBAT", 91.44, 124.46, 0)
    sch.power("power:PWR_FLAG", 91.44, 124.46, 180)
    sch.power("power:GND", 101.6, 124.46, 0)
    sch.power("power:PWR_FLAG", 101.6, 124.46, 0)

    c1 = sch.place("Device:C_Polarized", "C1", "470uF 10V", 76.2, 46.99,
                   footprint="Capacitor_THT:CP_Radial_D8.0mm_P3.50mm",
                   fields={"Reference": (1.905, -1.27, "left"), "Value": (1.905, 1.27, "left")})
    x, y = c1.pin(1)
    sch.wire((x, y), (x, y - 2.54))
    sch.power("DigitalSand:VBAT", x, y - 2.54, 0)
    x, y = c1.pin(2)
    sch.wire((x, y), (x, y + 2.54))
    sch.power("power:GND", x, y + 2.54, 0)

    # --- J3 LED panel input -----------------------------------------------------
    j3 = sch.place("Connector:Screw_Terminal_01x03", "J3", "LED panel DIN", 58.42, 93.98,
                   footprint=term3,
                   fields={"Reference": (0, -5.08, None), "Value": (0, 7.62, None)})
    x, y = j3.pin(1)
    sch.wire((x, y), (x - 7.62, y))
    sch.label("LED_DIN", x - 7.62, y, 180)
    x, y = j3.pin(2)
    sch.wire((x, y), (x - 7.62, y))
    sch.power("power:GND", x - 7.62, y, 270)
    x, y = j3.pin(3)
    sch.wire((x, y), (x - 7.62, y))
    sch.power("DigitalSand:VBAT", x - 7.62, y, 90)

    # --- Mounting holes ---------------------------------------------------------
    for i in range(4):
        sch.place("Mechanical:MountingHole", "H%d" % (i + 1), "M3", 35.56 + i * 12.7, 124.46,
                  footprint="MountingHole:MountingHole_3.2mm_M3", in_bom=False,
                  fields={"Reference": (0, -5.08, None), "Value": (0, -3.175, None)})

    sch.write()
    return sch


# ---------------------------------------------------------------------------
# Netlist
# ---------------------------------------------------------------------------

def parse_sexpr(text):
    tokens = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+', text)
    stack = [[]]
    for tok in tokens:
        if tok == "(":
            stack.append([])
        elif tok == ")":
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(tok[1:-1] if tok.startswith('"') else tok)
    return stack[0][0]


def read_netlist(path):
    tree = parse_sexpr(open(path).read())

    def child(node, key):
        return [c for c in node if isinstance(c, list) and c and c[0] == key]

    pads = {}      # (ref, pin) -> (net, pinfunction, pintype)
    nets = {}      # net -> set of (ref, pin)
    for net in child(child(tree, "nets")[0], "net"):
        name = child(net, "name")[0][1]
        nets[name] = set()
        for node in child(net, "node"):
            d = {c[0]: c[1] for c in node[1:]}
            pads[(d["ref"], d["pin"])] = (name, d.get("pinfunction", ""), d.get("pintype", ""))
            nets[name].add((d["ref"], d["pin"]))
    comps = {}
    for comp in child(child(tree, "components")[0], "comp"):
        d = {c[0]: c[1] for c in comp[1:] if isinstance(c, list) and len(c) > 1 and isinstance(c[1], str)}
        comps[d["ref"]] = d
    return pads, nets, comps


def check_netlist(nets):
    ok = True
    for name, members in EXPECTED_NETS.items():
        got = nets.get(name)
        if got != members:
            print("NET MISMATCH %s: expected %s got %s" % (name, sorted(members), sorted(got or [])))
            ok = False
    for name, members in nets.items():
        if name not in EXPECTED_NETS and len(members) != 1:
            print("UNEXPECTED NET %s: %s" % (name, sorted(members)))
            ok = False
    if not ok:
        sys.exit("schematic connectivity does not match the kit wiring")
    print("netlist OK: %d nets, %d connected" % (len(nets), len(EXPECTED_NETS)))


# ---------------------------------------------------------------------------
# Board
# ---------------------------------------------------------------------------

OX, OY = 100.0, 100.0          # board top-left corner on the page
BOARD_W, BOARD_H = 52.0, 59.5
CORNER_R = 2.0

U1_AT = (44.56, 13.5)          # pin 1 (EN), wide row
U2_AT = (42.02, 43.7)          # pin 1 (VCC); module body starts ~1.8 mm clear of the ESP32 board
TERM_Y = 5.1                   # J1-J3 and C1 pin row; terminal bodies end 0.6 mm from the edge
BUS_Y = 9.1                    # VBAT bus between the terminal row and the ESP32

PLACEMENT = [
    # ref, library, footprint, x, y, rotation
    ("U1", "DigitalSand", "ESP32_DevKit_V1_30pin_DualRow", U1_AT[0], U1_AT[1], -90),
    ("U2", "DigitalSand", "GY-521_MPU6050_Module", U2_AT[0], U2_AT[1], -90),
    ("SW1", "Button_Switch_THT", "SW_PUSH_6mm", 14.03, 42.7, 0),
    ("J1", "TerminalBlock_Phoenix", "TerminalBlock_Phoenix_PT-1,5-2-3.5-H_1x02_P3.50mm_Horizontal", 21.65, TERM_Y, 180),
    ("J2", "TerminalBlock_Phoenix", "TerminalBlock_Phoenix_PT-1,5-2-3.5-H_1x02_P3.50mm_Horizontal", 29.85, TERM_Y, 180),
    ("J3", "TerminalBlock_Phoenix", "TerminalBlock_Phoenix_PT-1,5-3-3.5-H_1x03_P3.50mm_Horizontal", 41.55, TERM_Y, 180),
    ("C1", "Capacitor_THT", "CP_Radial_D8.0mm_P3.50mm", 9.7, TERM_Y, 0),
    ("H1", "MountingHole", "MountingHole_3.2mm_M3", 3.5, 3.5, 0),
    ("H2", "MountingHole", "MountingHole_3.2mm_M3", 48.5, 3.5, 0),
    ("H3", "MountingHole", "MountingHole_3.2mm_M3", 48.5, 56.0, 0),
    ("H4", "MountingHole", "MountingHole_3.2mm_M3", 3.5, 56.0, 0),
]

# (net, width mm, points) on F.Cu.  Coordinates are relative to the board corner.
TRACKS = [
    # Battery -> LED panel power wires, plus feeds to the panel input, C1 and VIN.
    ("VBAT", 2.0, [(9.0, BUS_Y), (34.55, BUS_Y)]),
    ("VBAT", 2.0, [(21.65, TERM_Y), (21.65, BUS_Y)]),
    ("VBAT", 2.0, [(29.85, TERM_Y), (29.85, BUS_Y)]),
    ("VBAT", 1.0, [(34.55, TERM_Y), (34.55, BUS_Y)]),
    ("VBAT", 1.0, [(9.7, TERM_Y), (9.7, BUS_Y)]),
    ("VBAT", 1.0, [(9.0, BUS_Y), (9.0, 13.5)]),
    # D5 -> J3 DIN, under the ESP32 and between header-1 pins VN and VP.
    ("/LED_DIN", 0.3, [(26.78, 38.9), (26.78, 37.0), (40.75, 23.03), (40.75, 8.2),
                       (41.55, 7.4), (41.55, TERM_Y)]),
    ("/SDA", 0.3, [(34.4, 38.9), (34.4, 43.7)]),
    ("/SCL", 0.3, [(42.02, 38.9), (42.02, 41.3), (36.94, 41.3), (36.94, 43.7)]),
    ("+3V3", 0.4, [(9.0, 38.9), (9.0, 50.2), (42.02, 50.2), (42.02, 43.7)]),
    ("/BTN", 0.3, [(19.16, 38.9), (19.16, 40.0), (20.53, 41.37), (20.53, 42.7)]),
    ("/BTN", 0.3, [(14.03, 42.7), (20.53, 42.7)]),
]

ANTENNA_KEEPOUT = (43.0, 17.0, BOARD_W, 35.8)

FRONT_TEXT = [
    # text, x, y, size, angle
    ("-", 18.15, 9.0, 0.9, 0), ("+", 21.65, 9.0, 0.9, 0),
    ("-", 26.35, 9.0, 0.9, 0), ("+", 29.85, 9.0, 0.9, 0),
    ("5V", 34.55, 9.0, 0.9, 0), ("GND", 38.05, 9.0, 0.9, 0), ("DIN", 41.55, 9.0, 0.9, 0),
    ("J1 BAT", 19.9, 10.3, 0.9, 0), ("J2 LED PWR", 28.1, 10.3, 0.9, 0),
    ("J3 LED IN", 38.05, 10.3, 0.9, 0),
    ("DIGITAL SAND", 15.0, 53.7, 1.2, 0), ("ESP32 CARRIER v" + REV, 15.0, 55.8, 0.8, 0),
]

BACK_TEXT = [
    ("DIGITAL SAND TOY", 26.0, 20.0, 1.5),
    ("ESP32 CARRIER v%s  %s" % (REV, DATE[:7]), 26.0, 22.6, 1.0),
    ("U1: 30-PIN ESP32 DEVKIT", 26.0, 25.2, 1.0),
    ("OUTER ROW = 1.0in BOARDS", 26.0, 27.2, 1.0),
    ("INNER ROW = 0.9in BOARDS", 26.0, 29.2, 1.0),
    ("D5=LED DIN  D4=BUTTON", 26.0, 31.8, 1.0),
    ("D21=SDA  D22=SCL", 26.0, 33.8, 1.0),
]

FOOTPRINT_TEXT = {
    # ref: (x, y, angle, visible)
    "SW1": (11.2, 44.95, 90, True),
    "C1": (6.2, 9.6, 0, True),
    "J1": (0, 0, 0, False), "J2": (0, 0, 0, False), "J3": (0, 0, 0, False),
    "H1": (0, 0, 0, False), "H2": (0, 0, 0, False), "H3": (0, 0, 0, False), "H4": (0, 0, 0, False),
}


def build_pcb(pads, comps, sch_uuids):
    import pcbnew

    MM = pcbnew.FromMM

    def P(x, y):
        return pcbnew.VECTOR2I(MM(OX + x), MM(OY + y))

    for path in (PCB_FILE, PRO_FILE):
        if os.path.exists(path):
            os.remove(path)
    board = pcbnew.NewBoard(PCB_FILE)
    board.SetCopperLayerCount(2)

    tb = board.GetTitleBlock()
    tb.SetTitle(TITLE)
    tb.SetDate(DATE)
    tb.SetRevision(REV)
    tb.SetComment(0, "Replaces the point-to-point wiring of the PrintKits Digital Sand kit")
    tb.SetComment(1, "2-layer, 1.6 mm, 1 oz copper")

    ds = board.GetDesignSettings()
    ds.SetBoardThickness(MM(1.6))
    ds.m_MinClearance = MM(0.2)
    ds.m_TrackMinWidth = MM(0.2)
    ds.m_ViasMinSize = MM(0.6)
    ds.m_MinThroughDrill = MM(0.3)
    ds.m_ViasMinAnnularWidth = MM(0.13)
    ds.m_HoleClearance = MM(0.25)
    ds.m_HoleToHoleMin = MM(0.25)
    ds.m_CopperEdgeClearance = MM(0.5)
    ds.m_SilkClearance = 0
    ds.m_MinSilkTextHeight = MM(0.8)
    ds.m_MinSilkTextThickness = MM(0.15)
    ds.SetAuxOrigin(P(0, 0))
    ds.SetGridOrigin(P(0, 0))

    # Nets, named exactly as the schematic netlist names them.
    netnames = sorted({v[0] for v in pads.values()})
    nets = {}
    for name in netnames:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        nets[name] = ni

    # Footprints.
    for ref, lib, fpname, x, y, rot in PLACEMENT:
        libpath = FP_LIB if lib == "DigitalSand" else os.path.join(STOCK_FP, lib + ".pretty")
        fp = pcbnew.FootprintLoad(libpath, fpname)
        if fp is None:
            sys.exit("footprint not found: %s:%s" % (lib, fpname))
        fp.SetFPIDAsString("%s:%s" % (lib, fpname))
        fp.SetReference(ref)
        fp.SetValue(comps[ref]["value"])
        fp.SetPosition(P(x, y))
        fp.SetOrientationDegrees(rot)
        fp.SetPath(pcbnew.KIID_PATH("/" + sch_uuids[ref]))
        fp.SetProperty("Sheetfile", NAME + ".kicad_sch")
        fp.SetProperty("Sheetname", "")
        board.Add(fp)
        for pad in fp.Pads():
            key = (ref, pad.GetNumber())
            if key in pads:
                net, func, ptype = pads[key]
                pad.SetNet(nets[net])
                pad.SetPinFunction(func)
                pad.SetPinType(ptype)
        if ref in FOOTPRINT_TEXT:
            tx, ty, ta, vis = FOOTPRINT_TEXT[ref]
            fp.Reference().SetVisible(vis)
            if vis:
                fp.Reference().SetPosition(P(tx, ty))
                fp.Reference().SetTextAngleDegrees(ta)

    # Tracks.
    for net, width, pts in TRACKS:
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(P(x1, y1))
            t.SetEnd(P(x2, y2))
            t.SetWidth(MM(width))
            t.SetLayer(pcbnew.F_Cu)
            t.SetNet(nets[net])
            board.Add(t)

    # Board outline with rounded corners.
    r = CORNER_R
    w, h = BOARD_W, BOARD_H

    def seg(a, b):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(P(*a))
        s.SetEnd(P(*b))
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(MM(0.1))
        board.Add(s)

    def arc(a, m, b):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(P(*a), P(*m), P(*b))
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(MM(0.1))
        board.Add(s)

    k = r * (1 - 0.70710678)
    seg((r, 0), (w - r, 0))
    arc((w - r, 0), (w - k, k), (w, r))
    seg((w, r), (w, h - r))
    arc((w, h - r), (w - k, h - k), (w - r, h))
    seg((w - r, h), (r, h))
    arc((r, h), (k, h - k), (0, h - r))
    seg((0, h - r), (0, r))
    arc((0, r), (k, k), (r, 0))

    # Ground pours on both layers.
    for layer, zname in ((pcbnew.F_Cu, "GND_TOP"), (pcbnew.B_Cu, "GND_BOTTOM")):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(nets["GND"])
        z.SetZoneName(zname)
        z.SetLocalClearance(MM(0.3))
        z.SetMinThickness(MM(0.25))
        z.SetThermalReliefGap(MM(0.5))
        z.SetThermalReliefSpokeWidth(MM(0.6))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        outline = z.Outline()
        outline.NewOutline()
        for x, y in ((0, 0), (w, 0), (w, h), (0, h)):
            outline.Append(MM(OX + x), MM(OY + y))
        board.Add(z)

    # No copper under the ESP32 module antenna.
    kx0, ky0, kx1, ky1 = ANTENNA_KEEPOUT
    ko = pcbnew.ZONE(board)
    ko.SetIsRuleArea(True)
    layers = pcbnew.LSET()
    layers.AddLayer(pcbnew.F_Cu)
    layers.AddLayer(pcbnew.B_Cu)
    ko.SetLayerSet(layers)
    ko.SetZoneName("ANTENNA_KEEPOUT")
    ko.SetDoNotAllowCopperPour(True)
    ko.SetDoNotAllowTracks(True)
    ko.SetDoNotAllowVias(True)
    ko.SetDoNotAllowPads(False)
    ko.SetDoNotAllowFootprints(False)
    outline = ko.Outline()
    outline.NewOutline()
    for x, y in ((kx0, ky0), (kx1, ky0), (kx1, ky1), (kx0, ky1)):
        outline.Append(MM(OX + x), MM(OY + y))
    board.Add(ko)

    # Silkscreen text.
    def add_text(txt, x, y, size, layer, angle=0, mirrored=False):
        t = pcbnew.PCB_TEXT(board)
        t.SetText(txt)
        t.SetPosition(P(x, y))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I(MM(size), MM(size)))
        t.SetTextThickness(MM(0.15))
        t.SetTextAngleDegrees(angle)
        t.SetMirrored(mirrored)
        board.Add(t)

    for txt, x, y, size, angle in FRONT_TEXT:
        add_text(txt, x, y, size, pcbnew.F_SilkS, angle)
    for txt, x, y, size in BACK_TEXT:
        add_text(txt, x, y, size, pcbnew.B_SilkS, mirrored=True)

    pcbnew.SaveBoard(PCB_FILE, board)


def configure_project():
    """Net classes and fab-house friendly rules in the .kicad_pro file."""
    pro = json.load(open(PRO_FILE))
    classes = pro["net_settings"]["classes"]
    default = classes[0]
    default.update({"track_width": 0.3, "clearance": 0.2, "via_diameter": 0.6, "via_drill": 0.3})
    power = dict(default)
    power.update({"name": "Power", "track_width": 1.0, "clearance": 0.25})
    pro["net_settings"]["classes"] = [default, power]
    pro["net_settings"]["netclass_patterns"] = [
        {"netclass": "Power", "pattern": "VBAT"},
        {"netclass": "Power", "pattern": "GND"},
    ]
    rules = pro["board"]["design_settings"]["rules"]
    rules.update({"min_clearance": 0.2, "min_track_width": 0.2, "min_via_diameter": 0.6,
                  "min_through_hole_diameter": 0.3, "min_via_annular_width": 0.13,
                  "min_hole_clearance": 0.25, "min_hole_to_hole": 0.25,
                  "min_copper_edge_clearance": 0.5, "min_silk_clearance": 0.0,
                  "min_text_height": 0.8, "min_text_thickness": 0.15})
    pro["board"]["design_settings"]["defaults"]["board_outline_line_width"] = 0.1
    json.dump(pro, open(PRO_FILE, "w"), indent=2, sort_keys=True)


def fill_and_check():
    import pcbnew

    board = pcbnew.LoadBoard(PCB_FILE)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    board.BuildConnectivity()
    pcbnew.SaveBoard(PCB_FILE, board)
    report = os.path.join(tempfile.gettempdir(), NAME + "_drc.txt")
    pcbnew.WriteDRCReport(board, report, pcbnew.EDA_UNITS_MILLIMETRES, True)
    text = open(report).read()
    counts = {}
    for kind in re.findall(r"^\[(\w+)\]", text, re.M):
        counts[kind] = counts.get(kind, 0) + 1
    # Stand-alone scripts do not load KiCad's global footprint table, so every
    # stock footprint is reported as missing from its library.  KiCad itself
    # resolves them.
    env_only = counts.pop("lib_footprint_issues", 0)
    print("DRC: %s" % (", ".join("%s=%d" % kv for kv in sorted(counts.items())) or "no violations"))
    print("     (%d lib_footprint_issues ignored: global library table not loaded headless)" % env_only)
    print("     " + "; ".join(l.strip("* ") for l in text.splitlines()
                              if "unconnected pads" in l or "Footprint errors" in l))
    return counts


# ---------------------------------------------------------------------------
# Outputs
# ---------------------------------------------------------------------------

def export_outputs():
    os.makedirs(DOCS_DIR, exist_ok=True)
    os.makedirs(FAB_DIR, exist_ok=True)
    gerber_dir = tempfile.mkdtemp(prefix=NAME + "_gerbers_")
    run([KICAD_CLI, "sch", "export", "pdf", "-o", os.path.join(DOCS_DIR, "schematic.pdf"), SCH_FILE])
    run([KICAD_CLI, "pcb", "export", "gerbers", "-o", gerber_dir + "/",
         "-l", "F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts",
         "--subtract-soldermask", PCB_FILE])
    run([KICAD_CLI, "pcb", "export", "drill", "-o", gerber_dir + "/", "--format", "excellon",
         "--excellon-separate-th", "--generate-map", "--map-format", "pdf", PCB_FILE])
    zip_path = os.path.join(FAB_DIR, NAME + "_gerbers.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for name in sorted(os.listdir(gerber_dir)):
            zf.write(os.path.join(gerber_dir, name), name)
    shutil.rmtree(gerber_dir)
    # Previews.  Copper and silkscreen are plotted separately and stacked, because
    # the mirrored (back) plot otherwise draws the ground pour over the silkscreen.
    for side, copper, silk, mirror in (
            ("front", "F.Cu,F.Mask,Edge.Cuts", "F.SilkS", False),
            ("back", "B.Cu,B.Mask,Edge.Cuts", "B.SilkS", True)):
        parts = []
        for tag, layers in (("cu", copper), ("silk", silk)):
            svg = os.path.join(DOCS_DIR, "pcb-%s-%s.svg" % (side, tag))
            cmd = [KICAD_CLI, "pcb", "export", "svg", "-o", svg, "-l", layers,
                   "--page-size-mode", "2", "--exclude-drawing-sheet"]
            if mirror:
                cmd.append("--mirror")
            run(cmd + [PCB_FILE])
            parts.append(svg)
        base = open(parts[0]).read()
        top = open(parts[1]).read()
        body = top[top.index(">", top.index("<svg")) + 1:top.rindex("</svg>")]
        svg = os.path.join(DOCS_DIR, "pcb-%s.svg" % side)
        with open(svg, "w") as fh:
            fh.write(base[:base.rindex("</svg>")] + body + "</svg>\n")
        for part in parts:
            os.remove(part)
        if shutil.which("rsvg-convert"):
            png = svg[:-4] + ".png"
            subprocess.run(["rsvg-convert", "-z", "6", "-b", "white", "-o", png, svg], check=True)
            os.remove(svg)


def main():
    os.makedirs(FP_LIB, exist_ok=True)
    custom_libs = write_symbol_library()
    write_lib_tables()
    esp32_footprint()
    gy521_footprint()
    sch = build_schematic(custom_libs)

    netlist = os.path.join(PROJ, NAME + ".net")
    run([KICAD_CLI, "sch", "export", "netlist", "-o", netlist, SCH_FILE])
    pads, nets, comps = read_netlist(netlist)
    os.remove(netlist)
    check_netlist(nets)

    build_pcb(pads, comps, sch.uuids)
    configure_project()
    fill_and_check()
    prl = os.path.join(PROJ, NAME + ".kicad_prl")   # UI state written by pcbnew
    if os.path.exists(prl):
        os.remove(prl)
    if "--no-export" not in sys.argv:
        export_outputs()


if __name__ == "__main__":
    main()
