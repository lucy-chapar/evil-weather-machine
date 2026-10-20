// Weather display for the Digital Sand toy.
//
// Flips through today's high and low temperature (arrow + number), an animated
// weather icon and the chance of rain (a raindrop that fills up).  On shot days
// it adds a beating trans-flag heart with a green dot in the bottom corner on
// the side to use; the injection schedule comes from config.h.
// Forecasts come from Open-Meteo (no API key).  WiFi is set up from a phone
// through the "DigitalSand-Weather" access point on first boot.
//
// Firmware updates over WiFi: pio run -e ota -t upload (see platformio.ini).
//
// Button (D4): short press = next screen, hold = next brightness step, hold while powering on = forget the WiFi network.

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
  int code = 0;          // WMO weather code
  bool isDay = true;
  float highC = 0;
  float lowC = 0;
  int precipPct = 0;
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

static void drawIcon(const Icon& icon, uint32_t frame) {
  strip.clear();
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

static const Icon& iconFor(int code, bool day) {
  switch (code) {
    case 0:
    case 1: return day ? ICON_SUN : ICON_MOON;
    case 2: return day ? ICON_PARTLY : ICON_CLOUDY;
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
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 3) +
               "&longitude=" + String(longitude, 3) +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
               "&current=is_day&timezone=auto&forecast_days=1";
  String body;
  if (!httpGet(url, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject daily = doc["daily"];
  if (daily["weather_code"][0].isNull()) return false;
  forecast.code = daily["weather_code"][0];
  forecast.highC = daily["temperature_2m_max"][0];
  forecast.lowC = daily["temperature_2m_min"][0];
  forecast.precipPct = daily["precipitation_probability_max"][0] | 0;
  forecast.isDay = (doc["current"]["is_day"] | 1) == 1;
  forecast.valid = true;
  int32_t offset = doc["utc_offset_seconds"] | 0;
  if (offset != prefs.getInt("utcoff", INT32_MIN)) prefs.putInt("utcoff", offset);
  Serial.printf("Today: code %d, high %.1fC, low %.1fC, rain %d%%, %s\n", forecast.code,
                forecast.highC, forecast.lowC, forecast.precipPct, forecast.isDay ? "day" : "night");
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

// Side for the most recent scheduled shot (today's, on shot day).
static char demoSide = 0;  // demo build: force the shot-day side

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

enum Screen { SCREEN_HIGH, SCREEN_LOW, SCREEN_ICON, SCREEN_RAIN, SCREEN_HEART };
static Screen screen = SCREEN_HIGH;
static uint32_t screenStartMs = 0;
static uint32_t lastStepMs = 0;
static uint32_t step = 0;  // animation frame, scroll offset or fill level

static void showScreen(Screen s) {
  screen = s;
  screenStartMs = millis();
  lastStepMs = 0;
  step = 0;
  textClear();
  if (s == SCREEN_HIGH) textAdd(displayTemp(forecast.highC), tempColor(forecast.highC));
  if (s == SCREEN_LOW) textAdd(displayTemp(forecast.lowC), tempColor(forecast.lowC));
}

// High, low, icon, rain, then the heart on shot days.
static void nextScreen() {
  Screen s = screen == SCREEN_RAIN && !isShotDay() ? SCREEN_HEART : screen;
  showScreen(s >= SCREEN_HEART ? SCREEN_HIGH : (Screen)(s + 1));
}

// High or low temperature: arrow on rows 0-1, number on rows 3-7.
static bool drawTempScreen(bool high, uint32_t step) {
  float c = high ? forecast.highC : forecast.lowC;
  strip.clear();
  const char* const* arrow = high ? ART_ARROW_UP : ART_ARROW_DOWN;
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 8; x++)
      if (arrow[y][x] == 'X') setPixel(x, y, scale(tempColor(c), 1, 2));
  bool more = drawText(3, step);
  strip.show();
  return more;
}

static bool inDrop(int x, int y) { return x >= 0 && x < 8 && y >= 0 && y < 8 && ART_DROP[y][x] == 'B'; }

// Rain chance: a drop that fills from the bottom, one row per 12.5%.  The empty
// part shows only the drop's outline, so 0% reads as an empty drop.
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

static void applyBrightness() {
  strip.setBrightness(BRIGHTNESS[brightnessStep]);
  prefs.putUChar("bright", brightnessStep);
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
struct DemoItem {
  const char* name;
  uint16_t ms;      // how long to show it
  uint16_t frameMs; // time between animation frames
};

static const Icon* const DEMO_ICONS[] = {&ICON_SUN, &ICON_MOON, &ICON_PARTLY, &ICON_CLOUDY, &ICON_FOG,
                                         &ICON_DRIZZLE, &ICON_RAIN, &ICON_SNOW, &ICON_STORM};
static const DemoItem DEMO[] = {
    {"sun", 4000, 0}, {"moon", 4000, 0}, {"partly cloudy", 4000, 0}, {"cloudy", 4000, 0},
    {"fog", 4000, 0}, {"drizzle", 4000, 0}, {"rain", 4000, 0}, {"snow", 4000, 0},
    {"thunderstorm", 5000, 0},
    {"high 62F", 3000, 85}, {"low 45F", 3000, 85}, {"high 101F (scrolls)", 5500, 85},
    {"low -12F (scrolls)", 5500, 85},
    {"rain 0%", 2500, 120}, {"rain 25%", 2500, 120}, {"rain 50%", 2500, 120}, {"rain 100%", 3000, 120},
    {"shot day, left", 5000, 150}, {"shot day, right", 5000, 150},
    {"wifi setup", 2000, 1000}, {"loading", 2500, 80}, {"error", 2000, 1000},
};
static const int DEMO_COUNT = sizeof(DEMO) / sizeof(DEMO[0]);

static void demoStart(int i) {
  Serial.printf("demo: %s\n", DEMO[i].name);
  static const float temps[] = {16.7, 7.2, 38.3, -24.4};  // 62, 45, 101, -12 F
  demoSide = i == 18 ? 'R' : 'L';
  if (i >= 9 && i <= 12) {
    forecast.highC = forecast.lowC = temps[i - 9];
    showScreen(i % 2 ? SCREEN_HIGH : SCREEN_LOW);
  }
  static const int rain[] = {0, 25, 50, 100};
  if (i >= 13 && i <= 16) forecast.precipPct = rain[i - 13];
}

static void demoFrame(int i, uint32_t f) {
  if (i < 9) drawIcon(*DEMO_ICONS[i], f);
  else if (i <= 12) drawTempScreen(i % 2, f);
  else if (i <= 16) drawRainScreen(f);
  else if (i <= 18) drawHeartScreen(f);
  else if (i == 19) drawIcon(ICON_WIFI_SETUP, 0);
  else if (i == 20) drawSpinner(f);
  else drawIcon(ICON_ERROR, 0);
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
    demoStart(i);
  }
  uint16_t period = i < 9 ? DEMO_ICONS[i]->frameMs : DEMO[i].frameMs;
  if (lastMs == 0 || now - lastMs >= period) {
    lastMs = now;
    demoFrame(i, frame++);
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
  showScreen(SCREEN_HIGH);
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

  if ((int32_t)(now - nextFetchMs) >= 0) {
    drawSpinner(now / 80);
    bool ok = fetchForecast();
    fetchFailed = !ok;
    nextFetchMs = millis() + (ok ? REFRESH_MINUTES : RETRY_MINUTES) * 60000UL;
    if (ok) showScreen(screen);  // redraw with the new numbers
  }

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
    bool more = drawTempScreen(screen == SCREEN_HIGH, step++);
    bool scrolls = textWidth - 1 > 8;
    if (!more || (!scrolls && now - screenStartMs > 3500)) nextScreen();
  }
  delay(5);
}
