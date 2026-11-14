// DeskBuddy: 8-bit style clock + weather (Open-Meteo, no API key) + calendar
// Board: ESP32-2432S028R "CYD" 2.8" (ILI9341 320x240, XPT2046 touch)
//
// Controls:
//   short tap        -> switch clock / calendar
//   hold ~2 s        -> Wi-Fi / city setup portal ("DeskBuddy-Setup")
//
// Time comes from NTP; the UTC offset (incl. DST) comes from Open-Meteo,
// so the only thing you configure is Wi-Fi and a city name.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

// ---------- settings you may want to tweak ----------
#define INVERT_COLORS false        // set true if colors look inverted on your board
#define WEATHER_REFRESH_MS (15UL * 60UL * 1000UL)
#define CAL_TIMEOUT_MS 30000UL     // calendar returns to clock after this
#define LONG_PRESS_MS 2000UL

// ---------- palette (RGB565) ----------
#define RGB(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t SKY = RGB(92, 148, 252);
static const uint16_t BRICK = RGB(200, 76, 12);
static const uint16_t BLACK = 0x0000;
static const uint16_t WHITE = 0xFFFF;
static const uint16_t YEL = RGB(248, 184, 0);
static const uint16_t GREEN = RGB(0, 168, 0);
static const uint16_t DGREEN = RGB(0, 88, 0);
static const uint16_t GRAY = RGB(188, 188, 188);
static const uint16_t NAVY = RGB(0, 40, 168);
static const uint16_t BARBG = RGB(60, 100, 200);
static const uint16_t RED = RGB(252, 100, 100);

// ---------- 12x12 pixel icons ----------
static const char* const SPR_SUN[12] = {
    ".....YY.....", ".Y...YY...Y.", "..Y......Y..", "....YYYY....",
    "...YYYYYY...", "YY.YYYYYY.YY", "YY.YYYYYY.YY", "...YYYYYY...",
    "....YYYY....", "..Y......Y..", ".Y...YY...Y.", ".....YY....."};
static const char* const SPR_CLOUD[12] = {
    "............", "............", "............", "....WWW.....",
    "...WWWWW.WW.", ".WWWWWWWWWWW", "WWWWWWWWWWWW", "WWWWWWWWWWWW",
    ".GGGGGGGGGG.", "............", "............", "............"};
static const char* const SPR_RAIN[12] = {
    "............", "....GGG.....", "...GGGGG.GG.", ".GGGGGGGGGGG",
    "GGGGGGGGGGGG", ".GGGGGGGGGG.", "............", "..B..B..B...",
    "..B..B..B...", ".B..B..B....", ".B..B..B....", "............"};
static const char* const SPR_SNOW[12] = {
    "............", "....GGG.....", "...GGGGG.GG.", ".GGGGGGGGGGG",
    "GGGGGGGGGGGG", ".GGGGGGGGGG.", "............", ".W...W...W..",
    "...W...W....", ".W...W...W..", "...W...W....", "............"};
static const char* const SPR_STORM[12] = {
    "............", "....GGG.....", "...GGGGG.GG.", ".GGGGGGGGGGG",
    "GGGGGGGGGGGG", ".GGGGGGGGGG.", "............", ".....YY.....",
    "....YY......", "...YYYY.....", ".....YY.....", "....Y......."};

struct Wx {
  const char* const* spr;
  const char* label;
};

// Conditions are short strings (clear, partly-cloudy, cloudy, overcast, light-rain,
// rain, showers, snow, thunderstorm, ...). Open-Meteo WMO codes are converted to them
// in wmoToCond(), so the renderer does not depend on the weather source.
static Wx wxFor(const String& c, const String& ph) {
  if (ph == "fog" || ph == "mist") return {SPR_CLOUD, "FOG"};
  if (c == "clear") return {SPR_SUN, "CLEAR"};
  if (c == "partly-cloudy") return {SPR_SUN, "FAIR"};
  if (c == "cloudy") return {SPR_CLOUD, "CLOUDY"};
  if (c == "overcast") return {SPR_CLOUD, "OVERCAST"};
  if (c.indexOf("thunder") >= 0) return {SPR_STORM, "STORM"};
  if (c == "hail") return {SPR_SNOW, "HAIL"};
  if (c.indexOf("snow") >= 0) return {SPR_SNOW, "SNOW"};
  if (c == "showers") return {SPR_RAIN, "SHOWER"};
  if (c.indexOf("rain") >= 0) return {SPR_RAIN, "RAIN"};
  return {SPR_CLOUD, "-"};
}

// ---------- globals ----------
TFT_eSPI tft;
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(33, 36);  // CS, IRQ
Preferences prefs;
WiFiManager wm;
WiFiManagerParameter* pCity = nullptr;
WiFiManagerParameter* pLat = nullptr;
WiFiManagerParameter* pLon = nullptr;
WiFiManagerParameter* pTz = nullptr;
WiFiManagerParameter* pFlip = nullptr;

String city, cityLabel;
float lat = 0, lon = 0;
long utcOffset = 0;
bool haveOffset = false;
bool manualCoords = false;  // true: lat/lon entered by hand, no geocoding
String tzName;               // IANA name, e.g. Europe/Berlin; empty = auto by coordinates
bool flipDisplay = false;    // true: screen rotated 180 degrees

bool haveWeather = false;
float temp = 0, tmax = 0, tmin = 0, wind = 0;
String wcond, wphen;
uint32_t lastWeatherTry = 0;

int screenMode = 0;  // 0 clock, 1 calendar
uint32_t calShownAt = 0;
int lastMin = -1, lastSec = -1, lastDay = -1;

// ---------- helpers ----------
static bool haveLoc() { return lat != 0 || lon != 0; }

static void applyRotation() {
  uint8_t r = flipDisplay ? 3 : 1;
  tft.setRotation(r);
  ts.setRotation(r);
}

static bool validTz(const String& v) {
  if (v.length() > 40) return false;
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (!(isalnum((uint8_t)c) || c == '/' || c == '_' || c == '+' || c == '-')) return false;
  }
  return true;
}

static bool parseFlag(String v, bool def) {
  v.trim();
  v.toLowerCase();
  if (v == "1" || v == "y" || v == "yes" || v == "true" || v == "on") return true;
  if (v == "0" || v == "n" || v == "no" || v == "false" || v == "off") return false;
  return def;
}

static String asciiUpper(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    o += (c < 128) ? (char)toupper(c) : '?';
  }
  return o;
}

static String urlEncode(const String& s) {
  String o;
  char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += (char)c;
    } else {
      snprintf(b, sizeof b, "%%%02X", c);
      o += b;
    }
  }
  return o;
}

static void shadowText(const String& s, int x, int y, uint8_t size, uint16_t col) {
  tft.setTextFont(1);
  tft.setTextSize(size);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(BLACK);
  tft.drawString(s, x + size, y + size);
  tft.setTextColor(col);
  tft.drawString(s, x, y);
}

static void drawSprite(const char* const* spr, int x, int y, int s) {
  for (int r = 0; r < 12; r++) {
    for (int c = 0; c < 12; c++) {
      char ch = spr[r][c];
      if (ch == '.') continue;
      uint16_t col = WHITE;
      switch (ch) {
        case 'Y': col = YEL; break;
        case 'G': col = GRAY; break;
        case 'B': col = NAVY; break;
        default: break;
      }
      tft.fillRect(x + c * s, y + r * s, s, s, col);
    }
  }
}

static void drawBricks(int y0, int rows) {
  for (int r = 0; r < rows; r++) {
    int y = y0 + r * 8;
    tft.fillRect(0, y, 320, 8, BRICK);
    tft.drawFastHLine(0, y, 320, BLACK);
    for (int x = (r % 2) ? 8 : 0; x < 320; x += 16) tft.drawFastVLine(x, y, 8, BLACK);
  }
}

static void outlinedBlock(int x, int y, int w, int h) {
  tft.fillRect(x - 2, y - 2, w + 4, h + 2, DGREEN);
  tft.fillRect(x, y, w, h, GREEN);
}

// ---------- network ----------
static bool httpGetJson(const String& url, JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();  // public weather data only; no secrets are sent
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getStream());
  http.end();
  return ok;
}

static bool geocode() {
  if (WiFi.status() != WL_CONNECTED || city.isEmpty()) return false;
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(city) +
               "&count=1&language=en&format=json";
  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;
  JsonVariant r = doc["results"][0];
  if (r.isNull()) return false;
  lat = r["latitude"] | 0.0f;
  lon = r["longitude"] | 0.0f;
  cityLabel = asciiUpper(String(r["name"] | city.c_str()));
  prefs.putFloat("lat", lat);
  prefs.putFloat("lon", lon);
  prefs.putString("label", cityLabel);
  return haveLoc();
}

static void applyUtcOffset(long off) {
  if (!haveOffset || off != utcOffset) {
    if (off != utcOffset) prefs.putLong("off", off);
    utcOffset = off;
    haveOffset = true;
    configTime(utcOffset, 0, "pool.ntp.org", "time.google.com");
  }
}

static const char* wmoToCond(int c, const char*& phen) {
  phen = "";
  if (c == 0) return "clear";
  if (c == 1) return "partly-cloudy";
  if (c == 2) return "cloudy";
  if (c == 3) return "overcast";
  if (c == 45 || c == 48) {
    phen = "fog";
    return "overcast";
  }
  if (c >= 51 && c <= 57) return "light-rain";
  if (c >= 61 && c <= 67) return "rain";
  if (c >= 80 && c <= 82) return "showers";
  if (c >= 71 && c <= 77) return "snow";
  if (c == 85 || c == 86) return "snow-showers";
  if (c >= 95) return "thunderstorm";
  return "cloudy";
}

static bool fetchWeatherOpenMeteo() {
  String base = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) +
                "&longitude=" + String(lon, 4) +
                "&current=temperature_2m,weather_code,wind_speed_10m"
                "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&wind_speed_unit=ms&timezone=";
  JsonDocument doc;
  bool ok = httpGetJson(base + (tzName.isEmpty() ? String("auto") : urlEncode(tzName)), doc);
  if (!ok && !tzName.isEmpty()) {  // unknown timezone name: fall back to automatic
    doc.clear();
    ok = httpGetJson(base + "auto", doc);
  }
  if (!ok) return false;
  JsonVariant cur = doc["current"];
  if (cur.isNull()) return false;
  temp = cur["temperature_2m"] | 0.0f;
  wind = cur["wind_speed_10m"] | 0.0f;
  const char* ph = "";
  wcond = wmoToCond(cur["weather_code"] | 0, ph);
  wphen = ph;
  tmax = doc["daily"]["temperature_2m_max"][0] | temp;
  tmin = doc["daily"]["temperature_2m_min"][0] | temp;
  applyUtcOffset(doc["utc_offset_seconds"] | 0L);
  return true;
}

static bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED || !haveLoc()) return false;
  if (!fetchWeatherOpenMeteo()) return false;
  haveWeather = true;
  return true;
}

static void ensureLocation() {
  if (!haveLoc() && !city.isEmpty()) geocode();
}

// ---------- drawing ----------
static void drawSetup() {
  tft.fillScreen(SKY);
  drawBricks(200, 5);
  shadowText("WIFI SETUP", 40, 20, 4, YEL);
  shadowText("1. Join Wi-Fi:", 20, 80, 2, WHITE);
  shadowText("   DeskBuddy-Setup", 20, 104, 2, YEL);
  shadowText("2. Open 192.168.4.1", 20, 136, 2, WHITE);
  shadowText("3. Wi-Fi, city or coords", 20, 160, 2, WHITE);
}

static void showMsg(const char* a, const char* b) {
  tft.fillScreen(SKY);
  drawBricks(200, 5);
  shadowText(a, 20, 90, 3, WHITE);
  shadowText(b, 20, 130, 2, YEL);
}

static void drawWeather() {
  tft.fillRect(24, 124, 296, 50, SKY);
  tft.fillRect(104, 174, 216, 10, SKY);
  if (!haveWeather) {
    shadowText("NO WEATHER DATA", 40, 140, 2, WHITE);
    return;
  }
  Wx w = wxFor(wcond, wphen);
  drawSprite(w.spr, 40, 124, 4);
  char b[24];
  snprintf(b, sizeof b, "%d\xF7"
                        "C",
           (int)lroundf(temp));
  shadowText(b, 104, 126, 4, WHITE);
  shadowText(w.label, 216, 134, 2, YEL);
  snprintf(b, sizeof b, "H:%d L:%d W:%d", (int)lroundf(tmax), (int)lroundf(tmin), (int)lroundf(wind));
  shadowText(b, 104, 166, 2, WHITE);
}

static void drawScene() {
  tft.fillScreen(SKY);
  drawBricks(200, 5);
  // hill (left) and bush (right)
  outlinedBlock(8, 192, 96, 8);
  outlinedBlock(24, 184, 64, 8);
  outlinedBlock(40, 176, 32, 8);
  outlinedBlock(232, 192, 64, 8);
  outlinedBlock(240, 184, 48, 8);
  // city name (top left)
  shadowText(cityLabel.isEmpty() ? String("DESKBUDDY") : cityLabel.substring(0, 12), 8, 6, 2, WHITE);
  drawWeather();
}

static void drawTimeDigits(const tm& t) {
  char hh[3], mm[3];
  snprintf(hh, sizeof hh, "%02d", t.tm_hour);
  snprintf(mm, sizeof mm, "%02d", t.tm_min);
  tft.fillRect(24, 32, 272, 76, SKY);
  shadowText(hh, 40, 34, 8, WHITE);
  shadowText(mm, 184, 34, 8, WHITE);
}

static void drawColon(bool on) {
  tft.fillRect(146, 32, 38, 76, SKY);
  if (on) shadowText(":", 136, 34, 8, WHITE);
}

static void drawBar(int sec) {
  int filled = sec / 3 + 1;
  for (int i = 0; i < 20; i++) tft.fillRect(40 + i * 12, 112, 10, 8, i < filled ? YEL : BARBG);
}

static void drawDate(const tm& t) {
  char b[16];
  strftime(b, sizeof b, "%a %d %b", &t);
  String s = asciiUpper(b);
  int w = s.length() * 12;
  tft.fillRect(156, 4, 164, 22, SKY);
  shadowText(s, 312 - w, 6, 2, WHITE);
}

static void updateClock(time_t now) {
  tm t;
  localtime_r(&now, &t);
  if (t.tm_min != lastMin) {
    drawTimeDigits(t);
    drawDate(t);
    lastMin = t.tm_min;
    lastSec = -1;
  }
  if (t.tm_sec != lastSec) {
    drawColon(t.tm_sec % 2 == 0);
    drawBar(t.tm_sec);
    lastSec = t.tm_sec;
  }
}

static int daysInMonth(int year, int mon) {
  static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return d[mon];
}

static void drawCalendar(const tm& now) {
  tft.fillScreen(SKY);
  char b[20];
  strftime(b, sizeof b, "%b %Y", &now);
  String title = asciiUpper(b);
  shadowText(title, (320 - (int)title.length() * 18) / 2, 6, 3, YEL);
  const char* wd[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
  for (int i = 0; i < 7; i++) shadowText(wd[i], i * 40 + 8, 34, 2, i >= 5 ? RED : WHITE);
  tm f = now;
  f.tm_mday = 1;
  f.tm_hour = 12;
  f.tm_isdst = -1;
  mktime(&f);
  int off = (f.tm_wday + 6) % 7;  // Monday first
  int dim = daysInMonth(now.tm_year + 1900, now.tm_mon);
  for (int d = 1; d <= dim; d++) {
    int idx = off + d - 1;
    int r = idx / 7, c = idx % 7;
    int x = c * 40, y = 56 + r * 28;
    String s = String(d);
    int w = s.length() * 12;
    if (d == now.tm_mday) {
      tft.fillRect(x + 2, y + 2, 36, 24, YEL);
      tft.drawRect(x + 2, y + 2, 36, 24, BLACK);
      tft.setTextFont(1);
      tft.setTextSize(2);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(BLACK);
      tft.drawString(s, x + (40 - w) / 2, y + 6);
    } else {
      shadowText(s, x + (40 - w) / 2, y + 6, 2, WHITE);
    }
  }
}

static void redrawAll() {
  lastMin = lastSec = lastDay = -1;
  if (screenMode == 0) drawScene();
  else calShownAt = millis();
}

// ---------- Wi-Fi / city setup ----------
static bool parseCoord(String v, float lo, float hi, float& out) {
  v.trim();
  v.replace(',', '.');
  if (v.isEmpty()) return false;
  char* end = nullptr;
  float x = strtof(v.c_str(), &end);
  if (end == v.c_str() || *end != '\0' || x < lo || x > hi) return false;
  out = x;
  return true;
}

static String coordLabel() {
  char b[24];
  snprintf(b, sizeof b, "%.1f%c %.1f%c", fabsf(lat), lat >= 0 ? 'N' : 'S', fabsf(lon), lon >= 0 ? 'E' : 'W');
  return String(b);
}

// Reads the portal fields. Valid latitude + longitude win over the city name.
static void applySettings() {
  // timezone (empty = auto)
  String tz = pTz->getValue();
  tz.trim();
  if (!validTz(tz)) tz = tzName;
  if (tz != tzName) {
    tzName = tz;
    prefs.putString("tz", tzName);
    haveOffset = false;  // force time re-config on next weather fetch
  }
  // display flip
  bool f = parseFlag(pFlip->getValue(), flipDisplay);
  if (f != flipDisplay) {
    flipDisplay = f;
    prefs.putBool("flip", flipDisplay);
    applyRotation();
  }

  String c = pCity->getValue();
  c.trim();
  float la, lo;
  bool okLat = parseCoord(pLat->getValue(), -90.0f, 90.0f, la);
  bool okLon = parseCoord(pLon->getValue(), -180.0f, 180.0f, lo);
  if (okLat && okLon) {
    bool changed = !manualCoords || fabsf(la - lat) > 0.00001f || fabsf(lo - lon) > 0.00001f || c != city;
    if (!changed) return;
    manualCoords = true;
    lat = la;
    lon = lo;
    city = c;
    cityLabel = c.isEmpty() ? coordLabel() : asciiUpper(c);
    prefs.putBool("manual", true);
    prefs.putString("city", city);
    prefs.putFloat("lat", lat);
    prefs.putFloat("lon", lon);
    prefs.putString("label", cityLabel);
  } else if (c.length() && (c != city || manualCoords)) {
    manualCoords = false;
    city = c;
    lat = lon = 0;
    cityLabel = "";
    prefs.putBool("manual", false);
    prefs.putString("city", city);
    prefs.putFloat("lat", 0);
    prefs.putFloat("lon", 0);
    prefs.putString("label", "");
  }
}

static void runPortal() {
  drawSetup();
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal("DeskBuddy-Setup");
  applySettings();
  ensureLocation();
  haveWeather = false;
  fetchWeather();
  lastWeatherTry = millis();
  redrawAll();
}

// ---------- touch ----------
static void handleTouch() {
  static bool pressed = false;
  static uint32_t start = 0;
  bool t = ts.tirqTouched() && ts.touched();
  if (t && !pressed) {
    pressed = true;
    start = millis();
  } else if (!t && pressed) {
    pressed = false;
    uint32_t d = millis() - start;
    if (d >= LONG_PRESS_MS) {
      runPortal();
    } else if (d > 50) {
      screenMode ^= 1;
      redrawAll();
    }
  }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.invertDisplay(INVERT_COLORS);
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  touchSPI.begin(25, 39, 32, 33);
  ts.begin(touchSPI);

  prefs.begin("deskbuddy", false);
  city = prefs.getString("city", "Saint Petersburg");
  cityLabel = prefs.getString("label", "PETERSBURG");
  lat = prefs.getFloat("lat", 59.9386f);
  lon = prefs.getFloat("lon", 30.3141f);
  manualCoords = prefs.getBool("manual", false);
  tzName = prefs.getString("tz", "Europe/Moscow");
  utcOffset = prefs.getLong("off", 10800);  // Moscow time until the first weather reply
  flipDisplay = prefs.getBool("flip", false);
  applyRotation();

  showMsg("DESKBUDDY", "Connecting...");
  WiFi.mode(WIFI_STA);
  static WiFiManagerParameter hint(
      "<p>Enter a city <b>or</b> latitude and longitude in decimal degrees "
      "(e.g. 52.52 and 13.40). Coordinates take priority.</p>");
  pCity = new WiFiManagerParameter("city", "City (optional with coordinates)", city.c_str(), 40);
  pLat = new WiFiManagerParameter("lat", "Latitude (-90..90)", manualCoords ? String(lat, 5).c_str() : "", 12);
  pLon = new WiFiManagerParameter("lon", "Longitude (-180..180)", manualCoords ? String(lon, 5).c_str() : "", 12);
  wm.addParameter(&hint);
  wm.addParameter(pCity);
  wm.addParameter(pLat);
  wm.addParameter(pLon);
  pTz = new WiFiManagerParameter("tz", "Timezone, empty = auto (e.g. Europe/Berlin)", tzName.c_str(), 40);
  pFlip = new WiFiManagerParameter("flip", "Flip display 180 deg (0 = no, 1 = yes)", flipDisplay ? "1" : "0", 5);
  wm.addParameter(pTz);
  wm.addParameter(pFlip);
  wm.setAPCallback([](WiFiManager*) { drawSetup(); });
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);
  if (!wm.autoConnect("DeskBuddy-Setup")) ESP.restart();
  applySettings();
  if (!haveLoc() && city.isEmpty()) runPortal();

  showMsg("DESKBUDDY", "Loading weather...");
  ensureLocation();
  if (!haveLoc()) {
    showMsg("CITY NOT FOUND", "Hold screen 2 s to change");
    delay(3000);
  }
  fetchWeather();
  lastWeatherTry = millis();
  if (!haveOffset) configTime(utcOffset, 0, "pool.ntp.org", "time.google.com");

  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 10000) delay(200);
  drawScene();
}

void loop() {
  handleTouch();

  time_t now = time(nullptr);
  if (now > 1700000000) {
    if (screenMode == 0) {
      updateClock(now);
    } else {
      tm t;
      localtime_r(&now, &t);
      if (t.tm_mday != lastDay) {
        drawCalendar(t);
        lastDay = t.tm_mday;
      }
      if (millis() - calShownAt > CAL_TIMEOUT_MS) {
        screenMode = 0;
        redrawAll();
      }
    }
  }

  uint32_t interval = haveWeather ? WEATHER_REFRESH_MS : 30000UL;
  if (millis() - lastWeatherTry > interval) {
    lastWeatherTry = millis();
    ensureLocation();
    if (fetchWeather() && screenMode == 0) drawWeather();
  }
  delay(20);
}
