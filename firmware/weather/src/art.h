#pragma once
// 8x8 weather icons and a 3x5 font, drawn as text so they are easy to edit.
// Each icon frame is 8 rows of 8 characters; each character is a palette entry
// (see paletteColor() in main.cpp, '.' = off).  tools/preview_art.py renders
// these to a PNG.

#include <stdint.h>

enum Effect : uint8_t { FX_NONE, FX_RAIN, FX_DRIZZLE, FX_SNOW, FX_STORM, FX_MOON };

struct Icon {
  const char* const* rows;  // frameCount * 8 rows
  uint8_t frameCount;
  uint16_t frameMs;
  Effect effect;            // particles below the cloud (rows 4-7), or the moon under the art
};

// Palette: Y sun yellow, O sun orange, W cloud white, G cloud grey, D storm grey,
// B rain blue, C snow, L lightning, M moon, S star, F fog, f fog fade, R red, N network blue,
// T trans-flag blue, P trans-flag pink, Z side-dot green.

// Round sun; rays alternate straight and diagonal so it seems to turn.
static const char* const ART_SUN[] = {
  "...YY...",
  "..OOOO..",
  ".OYYYYO.",
  "YOYYYYOY",
  "YOYYYYOY",
  ".OYYYYO.",
  "..OOOO..",
  "...YY...",

  "Y......Y",
  "..OOOO..",
  ".OYYYYO.",
  ".OYYYYO.",
  ".OYYYYO.",
  ".OYYYYO.",
  "..OOOO..",
  "Y......Y",
};

// Night: the moon is drawn in code in its current phase (FX_MOON), under these.
static const char* const ART_CLEAR_NIGHT[] = {
  "........",
  "........",
  "........",
  "........",
  "........",
  "........",
  "........",
  "........",
};

static const char* const ART_NIGHT_CLOUD[] = {
  "........",
  "........",
  "........",
  "........",
  ".WW.W...",
  "WWWWWW..",
  "WWWWWWW.",
  ".GGGGG..",
};

// Sun peeking out behind a two-puff cloud.
static const char* const ART_PARTLY[] = {
  "...Y.OO.",
  "....OYYO",
  "....OYYO",
  ".WW.WOO.",
  "WWWWWWW.",
  "WWWWWWWW",
  ".GGGGGG.",
  "........",

  ".....OO.",
  "..Y.OYYO",
  "....OYYO",
  ".WW.WOO.",
  "WWWWWWW.",
  "WWWWWWWW",
  ".GGGGGG.",
  "........",
};

// Two-puff cloud drifting a column left and right.
static const char* const ART_CLOUDY[] = {
  "........",
  "....WW..",
  ".WW.WWW.",
  "WWWWWWW.",
  "WWWWWWW.",
  ".GGGGG..",
  "........",
  "........",

  "........",
  ".....WW.",
  "..WW.WWW",
  ".WWWWWWW",
  ".WWWWWWW",
  "..GGGGG.",
  "........",
  "........",
};

// Puffy clouds for rain, snow and storms; the effect draws below row 3.
static const char* const ART_RAIN_CLOUD[] = {
  "....GG..",
  ".GG.GGG.",
  "GGGGGGGG",
  ".DDDDDD.",
  "........",
  "........",
  "........",
  "........",
};

static const char* const ART_SNOW_CLOUD[] = {
  "....WW..",
  ".WW.WWW.",
  "WWWWWWWW",
  ".GGGGGG.",
  "........",
  "........",
  "........",
  "........",
};

static const char* const ART_STORM_CLOUD[] = {
  "....GG..",
  ".GG.GDG.",
  "GDDDDDDG",
  ".DDDDDD.",
  "........",
  "........",
  "........",
  "........",
};

// Mist: every row a line with soft ends, bright and dim rows sliding past each other.
static const char* const ART_FOG[] = {
  "fFFFFFf.",
  ".ffffff.",
  ".fFFFFFf",
  ".ffffff.",
  "fFFFFFf.",
  ".ffffff.",
  ".fFFFFFf",
  ".ffffff.",

  ".fFFFFFf",
  "ffffff..",
  "fFFFFFf.",
  "..ffffff",
  ".fFFFFFf",
  "ffffff..",
  "fFFFFFf.",
  "..ffffff",
};

// Lightning bolt for FX_STORM, rows 4-7.
static const char* const ART_BOLT[] = {
  "...LL...",
  "..LL....",
  "...LL...",
  "..L.....",
};

// Rain-chance drop: filled from the bottom in proportion to the chance.
static const char* const ART_DROP[] = {
  "...BB...",
  "..BBBB..",
  ".BBBBBB.",
  ".BBBBBB.",
  "BBBBBBBB",
  "BBBBBBBB",
  ".BBBBBB.",
  "..BBBB..",
};

// Shot tracker heart in trans-flag stripes, drawn on rows 1-6.  Between shots it
// fills up from the bottom, a third per day.
static const char* const ART_HEART[] = {
  ".TT..TT.",
  "PPPPPPPP",
  "WWWWWWWW",
  ".WWWWWW.",
  "..PPPP..",
  "...TT...",
};

// Shown above the number on the high/low screens (rows 0-1).
static const char* const ART_ARROW_UP[] = {"...XX...", "..XXXX.."};
static const char* const ART_ARROW_DOWN[] = {"..XXXX..", "...XX..."};

static const char* const ART_WIFI_SETUP[] = {
  "........",
  ".NNNNNN.",
  "N......N",
  "..NNNN..",
  ".N....N.",
  "...NN...",
  "...NN...",
  "........",
};

static const char* const ART_ERROR[] = {
  "R......R",
  ".R....R.",
  "..R..R..",
  "...RR...",
  "...RR...",
  "..R..R..",
  ".R....R.",
  "R......R",
};

#define ICON(art, ms, fx) Icon{art, sizeof(art) / sizeof(art[0]) / 8, ms, fx}

static const Icon ICON_SUN = ICON(ART_SUN, 600, FX_NONE);
static const Icon ICON_CLEAR_NIGHT = ICON(ART_CLEAR_NIGHT, 700, FX_MOON);
static const Icon ICON_NIGHT_PARTLY = ICON(ART_NIGHT_CLOUD, 700, FX_MOON);
static const Icon ICON_PARTLY = ICON(ART_PARTLY, 700, FX_NONE);
static const Icon ICON_CLOUDY = ICON(ART_CLOUDY, 1200, FX_NONE);
static const Icon ICON_DRIZZLE = ICON(ART_RAIN_CLOUD, 220, FX_DRIZZLE);
static const Icon ICON_RAIN = ICON(ART_RAIN_CLOUD, 140, FX_RAIN);
static const Icon ICON_SNOW = ICON(ART_SNOW_CLOUD, 350, FX_SNOW);
static const Icon ICON_STORM = ICON(ART_STORM_CLOUD, 140, FX_STORM);
static const Icon ICON_FOG = ICON(ART_FOG, 700, FX_NONE);
static const Icon ICON_WIFI_SETUP = ICON(ART_WIFI_SETUP, 1000, FX_NONE);
static const Icon ICON_ERROR = ICON(ART_ERROR, 1000, FX_NONE);

// 3x5 font for temperatures: 5 rows per glyph, glyph width = row length.
struct Glyph {
  char ch;
  const char* rows[5];
};

static const Glyph FONT[] = {
  {'0', {"XXX", "X.X", "X.X", "X.X", "XXX"}},
  {'1', {".X.", "XX.", ".X.", ".X.", "XXX"}},
  {'2', {"XXX", "..X", "XXX", "X..", "XXX"}},
  {'3', {"XXX", "..X", ".XX", "..X", "XXX"}},
  {'4', {"X.X", "X.X", "XXX", "..X", "..X"}},
  {'5', {"XXX", "X..", "XXX", "..X", "XXX"}},
  {'6', {"XXX", "X..", "XXX", "X.X", "XXX"}},
  {'7', {"XXX", "..X", "..X", ".X.", ".X."}},
  {'8', {"XXX", "X.X", "XXX", "X.X", "XXX"}},
  {'9', {"XXX", "X.X", "XXX", "..X", "XXX"}},
  {'-', {"...", "...", "XXX", "...", "..."}},
};
