# Weather and shot-day firmware

Turns the Digital Sand toy into a daily weather display that also reminds you on injection days.
The 8x8 panel flips through these screens:

1. **Now**: the current temperature, a number with no arrow, coloured from blue (cold) to red (hot).
2. **Weather**: an animated picture of the weather right now. The sun only shows by day. At night,
   clear and partly cloudy skies show the moon in its current phase.
3. **High**: ▲ arrow over today's high temperature.
4. **Low**: ▼ arrow over today's low temperature.
5. **Rain** (only when it's likely): a raindrop that fills from the bottom with the highest chance of
   rain or snow in the next 12 hours. A quarter full means 25%, full means certain. It's skipped
   below 20%.
6. **Shot day** (only on injection days): a trans-flag heart that beats and sparkles. A green dot in
   the bottom-left or bottom-right corner marks the side to use.

Temperatures of 100° and above, or -10° and below, don't fit in 8 columns, so they scroll.

![Weather screens](docs/preview-weather.png)

![Shot-day screens](docs/preview-shot.png)

### Shot schedule

A shot every `SHOT_EVERY_DAYS` days (3: ten shots in any 30 days), alternating sides, counted from
the `LAST_SHOT_*` date and side in [src/config.h](src/config.h): Friday 16 Oct 2026, right side.
It runs purely from the calendar, so there's nothing to log. If your schedule shifts, for example
after a late shot, set those to your latest shot and reflash. Dates use internet time and the local
timezone from the weather service. If WiFi drops while the toy is on, it keeps counting; after a
power cycle it needs internet time again, because the ESP32 has no battery-backed clock.

## Build and flash

Needs [PlatformIO](https://platformio.org/). First set an over-the-air update password: copy
`ota_password.example.ini` to `ota_password.ini` and change the password. That file isn't committed;
without it the build stops with `No section: 'ota'`. Then plug the ESP32 into USB and run:

```bash
pio run -t upload
```

### Updating over WiFi

Once the normal firmware has been flashed over USB, later updates can go over WiFi. The toy shows up on
your network as `digitalsand.local`:

```bash
pio run -e ota -t upload
```

The panel fills up in blue while the update uploads. Updates use the password from `ota_password.ini`,
which was built into the toy at the last USB flash. After changing the password, flash once more over
USB so the toy and the uploader agree. The demo build has no WiFi, so it can only be replaced over USB.

### Demo

To see every screen without waiting for the right weather or a shot day, flash the demo build.
It loops through all the weather animations, the eight moon phases and a partly cloudy night, sample
temperatures (including ones that scroll), the rain drop at 25/50/100%, shot day on each side, and the
setup, loading and error screens. It needs no WiFi.
Flash the normal build again afterwards:

```bash
pio run -e demo -t upload   # demo
pio run -t upload           # back to normal
```

Flashing replaces the kit's sand firmware. You can put that back at any time with the
ESPHome web installer, as in the kit instructions.

## First boot

1. **Orientation test (1.5 s).** The top-left pixel is red, the rest of the top row green and
   the rest of the left column blue. If the pattern is somewhere else, change `MATRIX_ROTATION`,
   `MATRIX_FLIP_X` or `MATRIX_SERPENTINE` in [src/config.h](src/config.h).
2. **WiFi setup (blue WiFi symbol).** On a phone, join the `DigitalSand-Weather` network.
   Pick your WiFi network on the page that opens, and the toy remembers it.
3. **Forecast.** A spinning blue dot means it's fetching. A red X means it couldn't get a forecast,
   and it retries every 2 minutes.

## Using it

| Button | Action |
|---|---|
| Short press | Next screen |
| Hold ~1 s | Next brightness step (remembered) |
| Hold while switching on | Forget the WiFi network and start setup again |

Screens also move on by themselves: the weather picture stays for 8 seconds, everything else
for about 4 seconds. The forecast refreshes every 15 minutes in the background. If it can't be
refreshed for 3 hours, the error screen replaces the old numbers instead of showing them as current.

## Settings ([src/config.h](src/config.h))

- `LATITUDE` / `LONGITUDE`: leave at 0 to locate by IP address (city-level; a VPN or mobile
  hotspot can put it somewhere else).
- `USE_FAHRENHEIT`, `REFRESH_MINUTES`, `STALE_HOURS`.
- `PRECIP_SHOW_MIN`: the rain screen appears at this chance or higher (20%).
- `BRIGHTNESS_LEVELS`: kept low because the panel runs on AA batteries.

## How it works

- Forecast: [Open-Meteo](https://open-meteo.com/), in the local timezone, with no API key:
  - current temperature, weather code and whether it's day or night;
  - today's high and low;
  - hourly precipitation probability, of which the highest over the next 12 hours is shown.
- Moon phase is worked out on the toy from the date, as a mean lunar month from a known new moon.
  That's accurate to within about a day. South of the equator the lit side is mirrored.
- The HTTPS request doesn't check the server certificate, because the board has no certificate bundle.
  That's fine for public weather data, but don't reuse the pattern for anything private.
- Icons, the raindrop and the 3×5 digits are text art in [src/art.h](src/art.h). Run
  `python3 tools/preview_art.py` to re-render the preview images in `docs/` after editing them; it needs `rsvg-convert`.

| WMO code (current) | Icon |
|---|---|
| 0–1 | Sun; at night the moon in its phase |
| 2 | Sun behind cloud; at night the moon behind cloud |
| 3 | Cloud |
| 45, 48 | Fog |
| 51–57 | Drizzle |
| 61–67, 80–82 | Rain |
| 71–77, 85–86 | Snow |
| 95–99 | Thunderstorm |
