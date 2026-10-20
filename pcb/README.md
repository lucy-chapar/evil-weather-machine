# Digital Sand Toy – ESP32 carrier PCB

KiCad 7 project for a two-layer board that replaces the point-to-point wiring of the
[PrintKits Digital Sand kit](https://printkits.com/blogs/instructions/digital-sand).
It uses the same parts and the same connections; the board holds the parts and does the wiring.

![Board, component side](docs/pcb-front.png)

## Files

| Path | Contents |
|---|---|
| `digital_sand.kicad_pro` | KiCad project (open this) |
| `digital_sand.kicad_sch` | Schematic ([PDF](docs/schematic.pdf)) |
| `digital_sand.kicad_pcb` | Board: 52 × 59.5 mm, 2 layers |
| `lib/` | Project symbols and footprints: ESP32 DevKit V1 30-pin, GY-521, `VBAT` power symbol |
| `docs/` | Schematic PDF and board previews |
| `fab/digital_sand_gerbers.zip` | Gerbers and Excellon drill files |
| `tools/generate_kicad.py` | Script that generates all of the above |

## Connections (same as the kit wiring diagram)

| ESP32 pin | Goes to |
|---|---|
| D5 (GPIO5) | J3 `DIN` → LED panel data in |
| D4 (GPIO4) | SW1 → GND |
| D21 (GPIO21) | GY-521 SDA |
| D22 (GPIO22) | GY-521 SCL |
| 3V3 | GY-521 VCC |
| VIN | `VBAT` rail (battery and LED panel 5 V) |
| GND | Common ground |

As in the original wiring, the 4×AA box (J1) feeds the LED panel's extra power wires (J2) directly.
The ESP32 runs from the same rail through VIN.

## Assembly

- **U1, ESP32 DevKit (30-pin)**: components up, USB connector towards the `USB` arrow at the board edge,
  antenna over the copper-free area marked `ANTENNA`.
  - The header row on the terminal-block side has two rows of holes. Use the outer row (`1.0in`) for
    boards with 25.4 mm between header rows, and the inner row (`0.9in`) for 22.86 mm boards.
  - Solder the board's header pins straight in, or fit 1×15 female headers to keep it removable.
- **U2, GY-521**: components up, square pad = VCC, module body over the board towards the bottom edge.
- **SW1**: the kit's 6 mm button. To use an external button instead, solder one wire to a pad on each side
  of the footprint (one `1` pad and one `2` pad).
- **J1–J3**: 3.5 mm pitch screw terminals with the wire openings facing the board edge.
  You can also solder wires straight into the holes.
  - J1 `BAT`: battery red → `+`, black → `-`
  - J2 `LED PWR`: the panel's extra red wire → `+`, white → `-`
  - J3 `LED IN`: panel input red → `5V`, white → `GND`, green → `DIN`
- **C1 (optional)**: 470–1000 µF electrolytic, rated 10 V or higher, `+` to the square pad.
- **H1–H4**: M3 mounting holes on a 45 × 52.5 mm grid.

## Check before ordering

- **ESP32 width.** Measure the header-to-header spacing on the board the kit ships with.
  The footprint takes 0.9 in and 1.0 in boards.
- **Accelerometer orientation.** The firmware reads gravity from the MPU-6050, and the stock firmware is
  a prebuilt ESPHome image. Mount the board so the GY-521 faces the same way relative to the LED matrix
  as in the original build, or the sand falls the wrong way. A tilt test before final mounting is the
  quickest check.
- **Enclosure.** The board is not matched to the printed case, so the case needs mounting points for it.

## Notes

- Unchanged from the kit: the LED panel runs straight off 4×AA. Fresh alkaline cells give up to about
  6.4 V, and WS2812B LEDs are rated to 5.3 V. NiMH cells (4.8 V) stay within that rating.
- Switch the battery off while the ESP32 is plugged into USB, because USB and the battery share the 5 V rail.
- Fabrication: 2 layers, 1.6 mm, 1 oz copper, 0.2 mm minimum track and space, 0.3 mm minimum drill.
  Any standard PCB service can make it from the zip.
- KiCad 7 cannot run ERC from the command line. Run Inspect → Electrical Rules Checker once in the
  schematic editor. Connectivity is checked by the generator against the kit wiring.

## Regenerating

`tools/generate_kicad.py` writes the libraries, schematic and board. It then checks the schematic netlist
against the kit wiring, fills the copper zones, runs DRC and re-exports `docs/` and `fab/`.
Run it with KiCad 7's bundled Python, which provides `pcbnew`:

```bash
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 tools/generate_kicad.py
```

The script overwrites the KiCad files. Once you start editing the design in KiCad itself, stop using the script.
