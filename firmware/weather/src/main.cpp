// Weather display for the Digital Sand toy.
//
// Flips through the current temperature, an animated icon for the weather right
// now (the moon in its current phase at night), today's high and low (arrow +
// number) and, when rain is likely in the next 12 hours, the chance of it (a
// raindrop that fills up).  On shot days it adds a beating trans-flag heart with
// a green dot in the bottom corner on the side to use; the injection schedule
// comes from config.h.
// Forecasts come from Open-Meteo (no API key).  WiFi is set up from a phone
// through the "DigitalSand-Weather" access point on first boot.
//
// Firmware updates over WiFi: pio run -e ota -t upload (see platformio.ini).
//
// Button (D4): short press = next screen, hold = next brightness step,
// hold while powering on = forget the WiFi network.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <time.h>

#include "art.h"
#include "config.h"

static Adafruit_NeoPixel strip(64, LED_PIN, NEO_GRB + NEO_KHZ800);
static Preferences prefs;

static const uint8_t BRIGHTNESS[] = BRIGHTNESS_LEVELS;
static const uint8_t BRIGHTNESS_COUNT = sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]);
static uint8_t brightnessStep = DEFAULT_BRIGHTNESS_STEP;

struct Forecast {
  bool valid = false;
  uint32_t fetchedMs = 0;  // millis() of the last successful fetch
  float nowC = 0;          // current temperature
  int code = 0;            // current WMO weather code
  bool isDay = true;       // current
  float highC = 0;         // today
  float lowC = 0;          // today
  int precipPct = 0;       // highest chance of precipitation over the next 12 hours
};

static Forecast forecast;

static float latitude = LATITUDE;
static float longitude = LONGITUDE;
static bool fetchFailed = false;
static uint32_t nextFetchMs = 0;

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static uint16_t pixelIndex(int x, int y) {
  if (MATRIX_FLIP_X) x = 7 - x;  // before rotating, so it always mirrors left/right
  for (int r = 0; r < MATRIX_ROTATION; r++) {
    int t = x;
    x = 7 - y;
    y = t;
  }
  if (MATRIX_SERPENTINE && (y & 1)) x = 7 - x;
  return y * 8 + x;
}

static void setPixel(int x, int y, uint32_t color) {
  if (x < 0 || x > 7 || y < 0 || y > 7) return;
  strip.setPixelColor(pixelIndex(x, y), color);
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return Adafruit_NeoPixel::Color(r, g, b); }

static uint32_t scale(uint32_t c, uint8_t num, uint8_t den) {
  return rgb(((c >> 16) & 0xFF) * num / den, ((c >> 8) & 0xFF) * num / den, (c & 0xFF) * num / den);
}

static uint32_t paletteColor(char c) {
  switch (c) {
    case 'Y': return rgb(255, 200, 0);
    case 'O': return rgb(255, 110, 0);
    case 'W': return rgb(210, 210, 210);
    case 'G': return rgb(100, 100, 115);
    case 'D': return rgb(45, 45, 60);
    case 'B': return rgb(0, 90, 255);
    case 'C': return rgb(190, 225, 255);
    case 'L': return rgb(255, 255, 90);
    case 'M': return rgb(210, 210, 160);
    case 'S': return rgb(120, 120, 170);
    case 'F': return rgb(90, 90, 100);
    case 'f': return rgb(50, 50, 56);
    case 'R': return rgb(255, 0, 0);
    case 'N': return rgb(0, 80, 255);
    case 'T': return rgb(91, 206, 250);
    case 'P': return rgb(245, 120, 160);
    case 'Z': return rgb(0, 220, 60);
    default: return 0;
  }
}

static void drawRows(const char* const* rows, int count, int top = 0, uint8_t num = 1, uint8_t den = 1) {
  for (int y = 0; y < count; y++)
    for (int x = 0; x < 8; x++)
      if (rows[y][x] != '.') setPixel(x, top + y, scale(paletteColor(rows[y][x]), num, den));
}

// Particles falling from a cloud, rows 4-7.  Columns and phases are staggered
// so the drops don't move in lockstep.
static void drawEffect(Effect fx, uint32_t frame) {
  static const int8_t phase[] = {0, 2, 1, 3};
  switch (fx) {
    case FX_RAIN:
    case FX_STORM: {
      uint32_t blue = paletteColor('B');
      if (fx == FX_STORM) blue = scale(blue, 2, 3);
      // Slanted streaks: each drop moves down and left, with a dim tail.
      for (int i = 0; i < 4; i++) {
        int t = (frame + phase[i]) % 4;
        int x = i * 2 + 2 - t, y = 4 + t;
        setPixel(x, y, blue);
        if (y > 4) setPixel(x + 1, y - 1, scale(blue, 1, 4));
      }
      // Two quick flashes every 16 frames.
      uint32_t f = frame % 16;
      if (fx == FX_STORM && (f == 0 || f == 2)) drawRows(ART_BOLT, 4, 4);
      break;
    }
    case FX_DRIZZLE:
      for (int i = 0; i < 2; i++) setPixel(1 + i * 4, 4 + (frame + phase[i * 2]) % 4, paletteColor('B'));
      break;
    case FX_SNOW:
      for (int i = 0; i < 4; i++) {
        uint32_t t = frame + phase[i];
        setPixel(1 + i * 2 - (t / 4) % 2, 4 + t % 4, paletteColor('C'));
      }
      break;
    default:
      break;
  }
}

static float demoPhase = -1;  // demo build: force the moon phase

// Moon phase from 0 to 1: 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter.
// Mean synodic month counted from the new moon of 2000-01-06 18:14 UTC; good to
// within a day, which is finer than an 8x8 moon can show.
static float moonPhase() {
  if (demoPhase >= 0) return demoPhase;
  time_t now = time(nullptr);
  if (now < 1700000000) return 0.5f;  // clock not set yet: full disc
  double p = fmod((now - 947182440) / 86400.0 / 29.530588853, 1.0);
  return p < 0 ? p + 1 : p;
}

static bool inMoon(int x, int y) {
  float nx = (x - 3.5f) / 4, ny = (y - 3.5f) / 4;
  return nx * nx + ny * ny <= 1;
}

// The moon as an 8-pixel disc lit for its phase.  The unlit part shows only as
// a dim outline, so a new moon reads as an empty circle.  A star twinkles in the
// top corner on the dark side.  South of the equator the lit side is mirrored.
static void drawMoon(uint32_t frame) {
  float p = moonPhase();
  bool waxing = p < 0.5f;
  float c = cosf(2 * PI * p);
  uint32_t lit = paletteColor('M'), dim = scale(lit, 1, 3);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      if (!inMoon(x, y)) continue;
      float nx = (x - 3.5f) / 4, ny = (y - 3.5f) / 4;
      if (latitude < 0) nx = -nx;
      float t = c * sqrtf(1 - ny * ny);  // terminator position on this row
      bool on = waxing ? nx > t : nx < -t;
      bool edge = !inMoon(x - 1, y) || !inMoon(x + 1, y) || !inMoon(x, y - 1) || !inMoon(x, y + 1);
      if (on) setPixel(x, y, lit);
      else if (edge) setPixel(x, y, dim);
    }
  bool darkLeft = waxing != (latitude < 0);
  setPixel(darkLeft ? 0 : 7, 0, frame % 3 ? scale(paletteColor('S'), 1, 3) : paletteColor('S'));
}

static void drawIcon(const Icon& icon, uint32_t frame) {
  strip.clear();
  if (icon.effect == FX_MOON) drawMoon(frame);  // under the art (night clouds)
  drawRows(icon.rows + (frame % icon.frameCount) * 8, 8);
  drawEffect(icon.effect, frame);
  strip.show();
}

// Small spinner around the edge while connecting or fetching.
static void drawSpinner(uint32_t frame) {
  static const int8_t ring[][2] = {{3, 0}, {4, 0}, {5, 1}, {6, 2}, {7, 3}, {7, 4}, {6, 5}, {5, 6},
                                   {4, 7}, {3, 7}, {2, 6}, {1, 5}, {0, 4}, {0, 3}, {1, 2}, {2, 1}};
  strip.clear();
  for (int t = 0; t < 4; t++) {
    const int8_t* p = ring[(frame + 16 - t) % 16];
    setPixel(p[0], p[1], scale(paletteColor('N'), 4 - t, 4));
  }
  strip.show();
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

struct Column {
  uint8_t bits;  // bit 0 = top row of the glyph
  uint32_t color;
};

static Column textCols[96];
static int textWidth = 0;

static const Glyph* findGlyph(char ch) {
  for (const Glyph& g : FONT)
    if (g.ch == ch) return &g;
  return nullptr;
}

static void textClear() { textWidth = 0; }

static void textAdd(const String& s, uint32_t color) {
  for (char ch : s) {
    const Glyph* g = findGlyph(ch);
    if (!g) continue;
    int w = strlen(g->rows[0]);
    for (int x = 0; x < w && textWidth < 96; x++) {
      uint8_t bits = 0;
      for (int y = 0; y < 5; y++)
        if (g->rows[y][x] == 'X') bits |= 1 << y;
      textCols[textWidth++] = {bits, color};
    }
    if (textWidth < 96) textCols[textWidth++] = {0, 0};  // letter spacing
  }
}

// Draws the text on rows top..top+4.  Text that fits is centred; anything
// wider (100 degrees, -12 degrees) scrolls, 'step' columns in.  Returns false
// once scrolling text has gone past.
static bool drawText(int top, uint32_t step) {
  int ink = textWidth - 1;  // without the trailing letter space
  bool fits = ink <= 8;
  int shift = fits ? -(8 - ink) / 2 : (int)step - 8;
  for (int x = 0; x < 8; x++) {
    int c = x + shift;
    if (c < 0 || c >= textWidth) continue;
    for (int y = 0; y < 5; y++)
      if (textCols[c].bits & (1 << y)) setPixel(x, top + y, textCols[c].color);
  }
  return fits || shift < textWidth;
}

// ---------------------------------------------------------------------------
// Forecast
// ---------------------------------------------------------------------------

// Icon for the current WMO weather code: sun by day, the moon by night.
static const Icon& iconFor(int code, bool day) {
  switch (code) {
    case 0:
    case 1: return day ? ICON_SUN : ICON_CLEAR_NIGHT;
    case 2: return day ? ICON_PARTLY : ICON_NIGHT_PARTLY;
    case 3: return ICON_CLOUDY;
    case 45:
    case 48: return ICON_FOG;
    case 51 ... 57: return ICON_DRIZZLE;
    case 61 ... 67:
    case 80 ... 82: return ICON_RAIN;
    case 71 ... 77:
    case 85:
    case 86: return ICON_SNOW;
    case 95 ... 99: return ICON_STORM;
    default: return ICON_CLOUDY;
  }
}

// Cold blue through green and yellow to hot red, by temperature in Celsius.
static uint32_t tempColor(float c) {
  static const struct { float t; uint8_t r, g, b; } stops[] = {
      {-10, 190, 225, 255}, {0, 0, 110, 255}, {10, 0, 200, 170}, {18, 80, 220, 0},
      {24, 255, 200, 0},    {30, 255, 90, 0}, {36, 255, 0, 0}};
  const int n = sizeof(stops) / sizeof(stops[0]);
  if (c <= stops[0].t) return rgb(stops[0].r, stops[0].g, stops[0].b);
  for (int i = 1; i < n; i++) {
    if (c <= stops[i].t) {
      float k = (c - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
      return rgb(stops[i - 1].r + k * (stops[i].r - stops[i - 1].r),
                 stops[i - 1].g + k * (stops[i].g - stops[i - 1].g),
                 stops[i - 1].b + k * (stops[i].b - stops[i - 1].b));
    }
  }
  return rgb(stops[n - 1].r, stops[n - 1].g, stops[n - 1].b);
}

static String displayTemp(float c) {
  float t = USE_FAHRENHEIT ? c * 9 / 5 + 32 : c;
  return String((int)lroundf(t));
}

static bool httpGet(const String& url, String& body) {
  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  bool https = url.startsWith("https");
  if (https) secure.setInsecure();  // public weather data; no certificate bundle on the device
  http.setTimeout(10000);
  if (!(https ? http.begin(secure, url) : http.begin(plain, url))) return false;
  int status = http.GET();
  if (status == HTTP_CODE_OK) body = http.getString();
  http.end();
  if (status != HTTP_CODE_OK) Serial.printf("GET %s -> %d\n", url.c_str(), status);
  return status == HTTP_CODE_OK;
}

static bool locate() {
  if (latitude != 0 || longitude != 0) return true;
  String body;
  if (!httpGet("http://ip-api.com/json/?fields=status,lat,lon", body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["status"] != "success") return false;
  latitude = doc["lat"];
  longitude = doc["lon"];
  Serial.printf("Location from IP: %.3f, %.3f\n", latitude, longitude);
  return true;
}

static bool fetchForecast() {
  if (WiFi.status() != WL_CONNECTED || !locate()) return false;
  // Current conditions, today's high and low, and hourly rain chances for today
  // and tomorrow (so the next 12 hours are covered late in the evening).
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 3) +
               "&longitude=" + String(longitude, 3) +
               "&current=temperature_2m,weather_code,is_day"
               "&daily=temperature_2m_max,temperature_2m_min"
               "&hourly=precipitation_probability&timezone=auto&forecast_days=2";
  String body;
  if (!httpGet(url, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject current = doc["current"], daily = doc["daily"];
  const char* nowTime = current["time"];  // local time, "YYYY-MM-DDTHH:MM"
  if (!nowTime || strlen(nowTime) < 13 || current["temperature_2m"].isNull()) return false;
  forecast.nowC = current["temperature_2m"];
  forecast.code = current["weather_code"] | 0;
  forecast.isDay = (current["is_day"] | 1) == 1;
  forecast.highC = daily["temperature_2m_max"][0];
  forecast.lowC = daily["temperature_2m_min"][0];
  // Hourly values start at local midnight today, so this hour is index HH.
  JsonArray rain = doc["hourly"]["precipitation_probability"];
  int hour = atoi(nowTime + 11);
  forecast.precipPct = 0;
  for (int i = hour; i < hour + 12 && i < (int)rain.size(); i++)
    forecast.precipPct = max(forecast.precipPct, rain[i] | 0);
  forecast.valid = true;
  forecast.fetchedMs = millis();
  int32_t offset = doc["utc_offset_seconds"] | 0;
  if (offset != prefs.getInt("utcoff", INT32_MIN)) prefs.putInt("utcoff", offset);
  Serial.printf("Now %.1fC code %d %s; today high %.1fC low %.1fC; rain next 12 h %d%%\n",
                forecast.nowC, forecast.code, forecast.isDay ? "day" : "night", forecast.highC,
                forecast.lowC, forecast.precipPct);
  return true;
}

// ---------------------------------------------------------------------------
// Shot tracker
// ---------------------------------------------------------------------------

// Days since 1970-01-01 for a calendar date (Howard Hinnant's algorithm).
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Today's local day number, from NTP time and the timezone offset Open-Meteo
// reported (saved, so it keeps working offline).  False until the clock is set.
static bool localDay(int32_t& day) {
  int32_t offset = prefs.getInt("utcoff", INT32_MIN);
  time_t now = time(nullptr);
  if (offset == INT32_MIN || now < 1700000000) return false;
  day = (now + offset) / 86400;
  return true;
}

// The schedule is pure calendar: a shot every SHOT_EVERY_DAYS days from the
// LAST_SHOT_* date, alternating sides.  Nothing is logged on the device.
static const int32_t FIRST_SHOT_DAY = daysFromCivil(LAST_SHOT_YEAR, LAST_SHOT_MONTH, LAST_SHOT_DAY);

// Days into the current cycle (0 = shot day), or -1 while the date is unknown.
static int cycleDay() {
  int32_t today;
  if (!localDay(today) || today < FIRST_SHOT_DAY) return -1;
  return (today - FIRST_SHOT_DAY) % SHOT_EVERY_DAYS;
}

static bool isShotDay() { return cycleDay() == 0; }

static char demoSide = 0;  // demo build: force the shot-day side

// Side for the most recent scheduled shot (today's, on shot day).
static char shotSide() {
  if (demoSide) return demoSide;
  int32_t today;
  if (!localDay(today)) return LAST_SHOT_SIDE;
  bool flipped = ((today - FIRST_SHOT_DAY) / SHOT_EVERY_DAYS) & 1;
  return flipped == (LAST_SHOT_SIDE == 'R') ? 'L' : 'R';
}

static const uint8_t BEAT[] = {4, 2, 4, 2, 2, 2, 2, 2};  // lub-dub, pause (quarters)

static void sparkle(int x, int y) { setPixel(x, y, rgb(255, 255, 255)); }

// Shot day: the trans-flag heart (rows 1-6) beats and sparkles, with a green
// dot in the bottom corner on the side to use (rows 6-7, columns 0-1 or 6-7,
// which the heart leaves empty).
static void drawHeartScreen(uint32_t step) {
  strip.clear();
  drawRows(ART_HEART, 6, 1, BEAT[step % 8], 4);
  for (int i = 0; i < 2; i++) {
    int x = esp_random() % 8, y = esp_random() % 8;
    if (y == 0 || y == 7 || ART_HEART[y - 1][x] == '.') sparkle(x, y);
  }
  int x0 = shotSide() == 'L' ? 0 : 6;
  for (int y = 6; y < 8; y++)
    for (int x = x0; x < x0 + 2; x++) setPixel(x, y, paletteColor('Z'));
  strip.show();
}

// ---------------------------------------------------------------------------
// Screens and button
// ---------------------------------------------------------------------------

enum Screen { SCREEN_NOW, SCREEN_ICON, SCREEN_HIGH, SCREEN_LOW, SCREEN_RAIN, SCREEN_HEART };
static Screen screen = SCREEN_NOW;
static uint32_t screenStartMs = 0;
static uint32_t lastStepMs = 0;
static uint32_t step = 0;  // animation frame, scroll offset or fill level

static void showScreen(Screen s) {
  screen = s;
  screenStartMs = millis();
  lastStepMs = 0;
  step = 0;
  textClear();
  if (s == SCREEN_NOW) textAdd(displayTemp(forecast.nowC), tempColor(forecast.nowC));
  if (s == SCREEN_HIGH) textAdd(displayTemp(forecast.highC), tempColor(forecast.highC));
  if (s == SCREEN_LOW) textAdd(displayTemp(forecast.lowC), tempColor(forecast.lowC));
}

static bool screenWanted(Screen s) {
  if (s == SCREEN_RAIN) return forecast.precipPct >= PRECIP_SHOW_MIN;
  if (s == SCREEN_HEART) return isShotDay();
  return true;
}

// Now, icon, high, low, then rain when it's likely and the heart on shot days.
static void nextScreen() {
  Screen s = screen;
  do {
    s = s >= SCREEN_HEART ? SCREEN_NOW : (Screen)(s + 1);
  } while (!screenWanted(s));
  showScreen(s);
}

// Temperatures.  Now: the number alone on rows 2-6.  High and low: an arrow on
// rows 0-1 and the number on rows 3-7.
static bool drawTempScreen(Screen s, uint32_t step) {
  float c = s == SCREEN_NOW ? forecast.nowC : s == SCREEN_HIGH ? forecast.highC : forecast.lowC;
  strip.clear();
  if (s != SCREEN_NOW) {
    const char* const* arrow = s == SCREEN_HIGH ? ART_ARROW_UP : ART_ARROW_DOWN;
    for (int y = 0; y < 2; y++)
      for (int x = 0; x < 8; x++)
        if (arrow[y][x] == 'X') setPixel(x, y, scale(tempColor(c), 1, 2));
  }
  bool more = drawText(s == SCREEN_NOW ? 2 : 3, step);
  strip.show();
  return more;
}

static bool inDrop(int x, int y) { return x >= 0 && x < 8 && y >= 0 && y < 8 && ART_DROP[y][x] == 'B'; }

// Rain chance: a drop that fills from the bottom, one row per 12.5%.  The empty
// part shows only the drop's outline.
static void drawRainScreen(uint32_t step) {
  int target = (forecast.precipPct * 8 + 50) / 100;
  int level = min((int)step, target);
  uint32_t blue = paletteColor('B');
  strip.clear();
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      if (!inDrop(x, y)) continue;
      bool edge = !inDrop(x - 1, y) || !inDrop(x + 1, y) || !inDrop(x, y - 1) || !inDrop(x, y + 1);
      if (y >= 8 - level) setPixel(x, y, blue);
      else if (edge) setPixel(x, y, scale(blue, 1, 3));
    }
  strip.show();
}

// Night hours (NIGHT_START_HOUR to NIGHT_END_HOUR, local time).  False while the
// clock isn't set, so the toy starts at its normal brightness.
static bool isNightHours() {
  int32_t offset = prefs.getInt("utcoff", INT32_MIN);
  time_t now = time(nullptr);
  if (offset == INT32_MIN || now < 1700000000) return false;
  int hour = ((now + offset) % 86400) / 3600;
  if (NIGHT_START_HOUR <= NIGHT_END_HOUR) return hour >= NIGHT_START_HOUR && hour < NIGHT_END_HOUR;
  return hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR;  // window crosses midnight
}

// The chosen brightness step, capped at NIGHT_BRIGHTNESS during night hours.
// Checked once a second; 'now' applies a changed step straight away.
static void updateBrightness(bool now = false) {
  static int current = -1;
  static uint32_t checkedMs = 0;
  if (!now && current >= 0 && millis() - checkedMs < 1000) return;
  checkedMs = millis();
  int level = BRIGHTNESS[brightnessStep];
  if (isNightHours()) level = min(level, NIGHT_BRIGHTNESS);
  if (level != current) {
    current = level;
    strip.setBrightness(level);
  }
}

static void applyBrightness() {
  prefs.putUChar("bright", brightnessStep);
  updateBrightness(true);
}

static void handleButton() {
  static bool down = false, held = false;
  static uint32_t pressedMs = 0;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (pressed && !down) {
    down = true;
    held = false;
    pressedMs = now;
  } else if (pressed && down && !held) {
    uint32_t t = now - pressedMs;
    if (t > 700) {
      held = true;
      brightnessStep = (brightnessStep + 1) % BRIGHTNESS_COUNT;
      applyBrightness();
    }
  } else if (!pressed && down) {
    down = false;
    if (!held && now - pressedMs > 30 && forecast.valid) nextScreen();
  }
}

static void orientationTest() {
  strip.clear();
  for (int i = 1; i < 8; i++) {
    setPixel(i, 0, rgb(0, 255, 0));
    setPixel(0, i, rgb(0, 0, 255));
  }
  setPixel(0, 0, rgb(255, 0, 0));
  strip.show();
  delay(1500);
}

// ---------------------------------------------------------------------------

#ifdef DEMO_MODE
// Demo build (pio run -e demo): plays every screen back to back, no WiFi needed.
enum DemoKind : uint8_t { D_ICON, D_TEMP, D_RAIN, D_SHOT, D_SPINNER };

struct DemoItem {
  const char* name;
  uint16_t ms;        // how long to show it
  DemoKind kind;
  const Icon* icon;   // D_ICON
  float value;        // moon phase (night icons), temperature in C, rain %, or shot side
  Screen screen;      // D_TEMP: which temperature screen
};

static const DemoItem DEMO[] = {
    {"sun", 4000, D_ICON, &ICON_SUN, -1, SCREEN_NOW},
    {"partly cloudy", 4000, D_ICON, &ICON_PARTLY, -1, SCREEN_NOW},
    {"cloudy", 4000, D_ICON, &ICON_CLOUDY, -1, SCREEN_NOW},
    {"fog", 4000, D_ICON, &ICON_FOG, -1, SCREEN_NOW},
    {"drizzle", 4000, D_ICON, &ICON_DRIZZLE, -1, SCREEN_NOW},
    {"rain", 4000, D_ICON, &ICON_RAIN, -1, SCREEN_NOW},
    {"snow", 4000, D_ICON, &ICON_SNOW, -1, SCREEN_NOW},
    {"thunderstorm", 5000, D_ICON, &ICON_STORM, -1, SCREEN_NOW},
    {"new moon", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.0, SCREEN_NOW},
    {"waxing crescent", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.125, SCREEN_NOW},
    {"first quarter", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.25, SCREEN_NOW},
    {"waxing gibbous", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.375, SCREEN_NOW},
    {"full moon", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.5, SCREEN_NOW},
    {"waning gibbous", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.625, SCREEN_NOW},
    {"last quarter", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.75, SCREEN_NOW},
    {"waning crescent", 2000, D_ICON, &ICON_CLEAR_NIGHT, 0.875, SCREEN_NOW},
    {"partly cloudy night", 4000, D_ICON, &ICON_NIGHT_PARTLY, 0.375, SCREEN_NOW},
    {"now 58F", 3000, D_TEMP, nullptr, 14.4, SCREEN_NOW},
    {"high 62F", 3000, D_TEMP, nullptr, 16.7, SCREEN_HIGH},
    {"low 45F", 3000, D_TEMP, nullptr, 7.2, SCREEN_LOW},
    {"high 101F (scrolls)", 5500, D_TEMP, nullptr, 38.3, SCREEN_HIGH},
    {"low -12F (scrolls)", 5500, D_TEMP, nullptr, -24.4, SCREEN_LOW},
    {"rain 25%", 2500, D_RAIN, nullptr, 25, SCREEN_NOW},
    {"rain 50%", 2500, D_RAIN, nullptr, 50, SCREEN_NOW},
    {"rain 100%", 3000, D_RAIN, nullptr, 100, SCREEN_NOW},
    {"shot day, left", 5000, D_SHOT, nullptr, 'L', SCREEN_NOW},
    {"shot day, right", 5000, D_SHOT, nullptr, 'R', SCREEN_NOW},
    {"wifi setup", 2000, D_ICON, &ICON_WIFI_SETUP, -1, SCREEN_NOW},
    {"loading", 2500, D_SPINNER, nullptr, 0, SCREEN_NOW},
    {"error", 2000, D_ICON, &ICON_ERROR, -1, SCREEN_NOW},
};
static const int DEMO_COUNT = sizeof(DEMO) / sizeof(DEMO[0]);

static void demoStart(const DemoItem& d) {
  Serial.printf("demo: %s\n", d.name);
  demoPhase = d.kind == D_ICON ? d.value : -1;
  if (d.kind == D_TEMP) {
    forecast.nowC = forecast.highC = forecast.lowC = d.value;
    showScreen(d.screen);
  }
  if (d.kind == D_RAIN) forecast.precipPct = d.value;
  if (d.kind == D_SHOT) demoSide = (char)d.value;
}

static uint16_t demoFrameMs(const DemoItem& d) {
  switch (d.kind) {
    case D_ICON: return d.icon->frameMs;
    case D_TEMP: return 85;
    case D_RAIN: return 120;
    case D_SHOT: return 150;
    default: return 80;
  }
}

static void demoFrame(const DemoItem& d, uint32_t f) {
  switch (d.kind) {
    case D_ICON: drawIcon(*d.icon, f); break;
    case D_TEMP: drawTempScreen(d.screen, f); break;
    case D_RAIN: drawRainScreen(f); break;
    case D_SHOT: drawHeartScreen(f); break;
    case D_SPINNER: drawSpinner(f); break;
  }
}

static void demoLoop() {
  static int i = -1;
  static uint32_t startMs = 0, lastMs = 0, frame = 0;
  uint32_t now = millis();
  if (i < 0 || now - startMs >= DEMO[i].ms) {
    i = (i + 1) % DEMO_COUNT;
    startMs = now;
    lastMs = 0;
    frame = 0;
    demoStart(DEMO[i]);
  }
  if (lastMs == 0 || now - lastMs >= demoFrameMs(DEMO[i])) {
    lastMs = now;
    demoFrame(DEMO[i], frame++);
  }
}
#endif

// Over-the-air updates.  The panel fills up row by row in blue while uploading.
static void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([] { Serial.println("OTA update starting"); });
  ArduinoOTA.onProgress([](unsigned done, unsigned total) {
    int lit = total ? done * 64 / total : 0;
    strip.clear();
    for (int i = 0; i < lit; i++) setPixel(i % 8, 7 - i / 8, paletteColor('N'));
    strip.show();
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("OTA error %u\n", err);
    drawIcon(ICON_ERROR, 0);
  });
  ArduinoOTA.begin();
  Serial.printf("OTA ready: %s.local\n", OTA_HOSTNAME);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  prefs.begin("weather", false);
  brightnessStep = prefs.getUChar("bright", DEFAULT_BRIGHTNESS_STEP) % BRIGHTNESS_COUNT;
  strip.begin();
  strip.setBrightness(BRIGHTNESS[brightnessStep]);

#ifdef DEMO_MODE
  orientationTest();
  return;
#endif
  WiFiManager wm;
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Button held at boot: forgetting WiFi settings");
    wm.resetSettings();
    drawIcon(ICON_ERROR, 0);
    delay(1000);
  }
  orientationTest();

  wm.setAPCallback([](WiFiManager*) { drawIcon(ICON_WIFI_SETUP, 0); });
  wm.setConfigPortalTimeout(300);
  drawSpinner(0);
  if (!wm.autoConnect("DigitalSand-Weather")) ESP.restart();
  WiFi.setAutoReconnect(true);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setupOta();
  showScreen(SCREEN_NOW);
}

void loop() {
#ifdef DEMO_MODE
  demoLoop();
  delay(5);
  return;
#endif
  ArduinoOTA.handle();
  uint32_t now = millis();
  handleButton();
  updateBrightness();

  if ((int32_t)(now - nextFetchMs) >= 0) {
    if (!forecast.valid) drawSpinner(now / 80);  // background refreshes stay invisible
    bool ok = fetchForecast();
    fetchFailed = !ok;
    nextFetchMs = millis() + (ok ? REFRESH_MINUTES : RETRY_MINUTES) * 60000UL;
    if (ok && screenWanted(screen)) showScreen(screen);  // redraw with the new numbers
    else if (ok) nextScreen();
  }
  // Don't show old weather as if it were current.
  if (forecast.valid && millis() - forecast.fetchedMs > STALE_HOURS * 3600000UL) forecast.valid = false;

  if (!forecast.valid) {
    // No forecast yet: still show the shot-day heart if the clock is known.
    if (isShotDay()) drawHeartScreen(now / 150);
    else if (fetchFailed) drawIcon(ICON_ERROR, 0);
    else drawSpinner(now / 80);
    delay(20);
    return;
  }

  if (screen == SCREEN_HEART) {
    if (lastStepMs == 0 || now - lastStepMs >= 150) {
      lastStepMs = now;
      drawHeartScreen(step++);
    }
    if (now - screenStartMs > 4000) nextScreen();
  } else if (screen == SCREEN_ICON) {
    const Icon& icon = iconFor(forecast.code, forecast.isDay);
    if (lastStepMs == 0 || now - lastStepMs >= icon.frameMs) {
      lastStepMs = now;
      drawIcon(icon, step++);
    }
    if (now - screenStartMs > 8000) nextScreen();
  } else if (screen == SCREEN_RAIN) {
    if (now - lastStepMs >= 120) {
      lastStepMs = now;
      drawRainScreen(step++);
    }
    if (now - screenStartMs > 4000) nextScreen();
  } else if (now - lastStepMs >= 85) {
    lastStepMs = now;
    bool more = drawTempScreen(screen, step++);
    bool scrolls = textWidth - 1 > 8;
    if (!more || (!scrolls && now - screenStartMs > 3500)) nextScreen();
  }
  delay(5);
}
