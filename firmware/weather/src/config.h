#pragma once

// ---- Location and units ----------------------------------------------------
// Leave both at 0 to look the location up from the network's public IP address.
#define LATITUDE 0.0
#define LONGITUDE 0.0
#define USE_FAHRENHEIT true

// How often to fetch the forecast, how soon to retry after a failure, and how
// old the last forecast may get before the error screen replaces it.
#define REFRESH_MINUTES 15
#define RETRY_MINUTES 2
#define STALE_HOURS 3

// Show the rain screen when the chance of precipitation in the next 12 hours is
// at least this many percent.
#define PRECIP_SHOW_MIN 20

// ---- Over-the-air updates ----------------------------------------------------
// The toy shows up on the network as OTA_HOSTNAME.local.  The password is set
// in ota_password.ini (not committed; see ota_password.example.ini), which
// platformio.ini passes to both the build and the uploader.
#define OTA_HOSTNAME "digitalsand"
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

// ---- Hardware (matches the kit wiring and the carrier PCB) ------------------
#define LED_PIN 5      // D5 -> LED panel DIN
#define BUTTON_PIN 4   // D4 -> button to GND

// ---- LED matrix layout --------------------------------------------------------
// At boot the panel shows an orientation test: the top-left pixel is red, the
// rest of the top row green and the rest of the left column blue.  If that
// pattern shows up elsewhere, change these settings.
#define MATRIX_SERPENTINE true   // every other row runs backwards (most flexible panels)
#define MATRIX_ROTATION 3        // quarter turns, 0-3 (3 for this kit's panel)
#define MATRIX_FLIP_X true       // mirror left/right (true for this kit's panel)

// Brightness steps cycled by a long button press (0-255).  Keep them low on
// batteries: 64 LEDs at full white draw several amps.
#define BRIGHTNESS_LEVELS {6, 16, 40, 90}
#define DEFAULT_BRIGHTNESS_STEP 1

// ---- Shot tracker -------------------------------------------------------------
// On shot days a beating heart joins the rotation, with a green dot in the bottom
// corner on the side to use.  The schedule is pure calendar: a shot every SHOT_EVERY_DAYS days from the shot below,
// alternating sides.  If the schedule shifts, set this to your latest shot.
#define SHOT_EVERY_DAYS 3
#define LAST_SHOT_YEAR 2026
#define LAST_SHOT_MONTH 10
#define LAST_SHOT_DAY 16      // Friday
#define LAST_SHOT_SIDE 'R'    // side of that shot, 'L' or 'R'
