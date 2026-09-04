// Tide & wind tracker for M5Stack PaperColor (C151).
//
// Cycle: cold boot -> Wi-Fi -> NTP -> fetch NOAA + Open-Meteo -> draw -> PM1
// shutdown with an RX8130 wake alarm armed. There is no ESP32 deep sleep here
// -- the PM1 cuts power between cycles (see sleepUntilNext()), so every boot
// starts main.cpp from scratch. Confirmed against refs/M5PaperColor-UserDemo
// and .pio/libdeps/m5stack-papercolor/M5PM1; do not assume the M5Paper (mono)
// API applies.

#include <Arduino.h>
#include <M5Unified.h>
#include <M5PM1.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_sntp.h"  // sntp_get_sync_status() -- see setup()'s NTP-wait comment

#include "config.h"
#include "layout.h"  // GENERATED from layout.json -- see tools/gen_layout_header.py

// ---------------------------------------------------------------- palette ---
// The render target is now an M5Canvas at color_depth_t::palette_4bit (see
// drawAll()) -- the smallest M5GFX depth that can hold six distinct colors
// with an exact palette table and no dithering (palette_2bit only offers 4).
// LGFX_Sprite's palette-mode draw calls take a palette INDEX as the "color"
// argument, not an RGB triplet (confirmed by reading
// misc/colortype.hpp's convert_uint32_to_palette4()), so these constants are
// now indices 0-5 into PALETTE_RGB below rather than raw RGB values. The
// panel driver (Panel_ED2208.cpp) still nearest-matches/dithers RGB888
// against its own native 6-color table on display() -- pixel-identical
// output only requires that PALETTE_RGB carry the exact same RGB888 bytes
// that were previously passed straight to M5.Display.
static constexpr uint32_t C_BLACK  = 0;
static constexpr uint32_t C_WHITE  = 1;
static constexpr uint32_t C_RED    = 2;
static constexpr uint32_t C_YELLOW = 3;
static constexpr uint32_t C_BLUE   = 4;
static constexpr uint32_t C_GREEN  = 5;

// RGB888, indexed by the C_* constants above -- keeps the C++ and
// preview.py's colors in sync.
static constexpr uint32_t PALETTE_RGB[6] = {
  0x000000, 0xFFFFFF, 0xBF0000, 0xFFF338, 0x0000BF, 0x007C00,
};

static constexpr int SCREEN_W = layout::screen::W;
static constexpr int SCREEN_H = layout::screen::H;

// ------------------------------------------------------------------- data ---
struct TidePoint { time_t t; float ft; };
struct TideEvent { time_t t; float ft; char kind; };  // 'H' or 'L'
struct WindPoint { time_t t; float kt; };

struct Snapshot {
  TidePoint tide[26];      int nTide = 0;
  TideEvent events[10];    int nEvents = 0;
  WindPoint forecast[24];  int nForecast = 0;
  float tideNow = 0, windNow = 0, gustNow = 0;
  int   windDir = 0;
  float indoorC = 0, indoorRh = 0;
  int   battery = 0;
  bool  ok = false;
};

// The board fully cuts power between cycles (see sleepUntilNext()), so
// RTC_DATA_ATTR does not survive a cycle -- only the PM1's own RTC RAM does.
static M5PM1 pm1;
static constexpr m5pm1_gpio_num_t PM1_EPD_EN   = M5PM1_GPIO_NUM_0;  // PY_EPD_EN
static constexpr m5pm1_gpio_num_t PM1_RTC_WAKE = M5PM1_GPIO_NUM_2;  // RX8130 nIRQ

static uint32_t nextCycleCount() {
  uint32_t n = 0;
  pm1.readRtcRAM(0, reinterpret_cast<uint8_t*>(&n), sizeof(n));
  n++;
  pm1.writeRtcRAM(0, reinterpret_cast<uint8_t*>(&n), sizeof(n));
  return n;
}

// -------------------------------------------------------------- utilities ---
static time_t parseLocal(const char* s) {          // "2026-08-28 14:00"
  struct tm tmv = {};
  if (strptime(s, "%Y-%m-%d %H:%M", &tmv) == nullptr) return 0;
  tmv.tm_isdst = -1;
  return mktime(&tmv);
}

static time_t parseIso(const char* s) {            // "2026-08-28T14:00"
  struct tm tmv = {};
  if (strptime(s, "%Y-%m-%dT%H:%M", &tmv) == nullptr) return 0;
  tmv.tm_isdst = -1;
  return mktime(&tmv);
}

static const char* compass(int deg) {
  static const char* p[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                              "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  return p[int((deg % 360) / 22.5f + 0.5f) % 16];
}

static uint32_t windColor(float kt) {
  if (kt < 10) return C_GREEN;
  if (kt < 20) return C_YELLOW;
  return C_RED;
}

// M5Unified has no Sht4x driver on this board -- confirmed by compile error,
// see CLAUDE.md. Read the SHT40 (I2C 0x44) directly, matching
// refs/M5PaperColor-UserDemo/main/hal/hal.cpp Hal::sht40Read().
static bool readSht40(float* tempC, float* rh) {
  static constexpr uint8_t SHT4X_ADDR    = 0x44;
  static constexpr uint8_t SHT4X_CMD_HI_PRE = 0xFD;

  if (!M5.In_I2C.start(SHT4X_ADDR, false, 400000)) return false;
  M5.In_I2C.write(SHT4X_CMD_HI_PRE);
  M5.In_I2C.stop();

  delay(10);  // high-precision mode needs ~8.3ms

  uint8_t buf[6];
  if (!M5.In_I2C.start(SHT4X_ADDR, true, 400000)) return false;
  M5.In_I2C.read(buf, 6);
  M5.In_I2C.stop();

  uint16_t rawT = (buf[0] << 8) | buf[1];
  uint16_t rawH = (buf[3] << 8) | buf[4];
  *tempC = -45.0f + 175.0f * rawT / 65535.0f;
  *rh    = -6.0f  + 125.0f * rawH / 65535.0f;
  if (*rh > 100.0f) *rh = 100.0f;
  if (*rh < 0.0f)   *rh = 0.0f;
  return true;
}

// ------------------------------------------------------------------- http ---
static bool httpGetJson(const String& url, JsonDocument& doc,
                        const DeserializationOption::Filter* filter = nullptr) {
  WiFiClientSecure client;
  client.setInsecure();          // no cert pinning; see CLAUDE.md
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) { Serial.println("http begin failed"); return false; }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d for %s\n", code, url.c_str());
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DeserializationError err = filter
      ? deserializeJson(doc, body, *filter)
      : deserializeJson(doc, body);

  if (err) {
    Serial.printf("json: %s for %s\n", err.c_str(), url.c_str());
    Serial.printf("body (%u bytes): %s\n", body.length(), body.c_str());
    return false;
  }
  return true;
}

static String coopsUrl(const char* product, const char* interval,
                       const char* date) {
  String u = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter"
             "?station=" NOAA_STATION
             "&datum=MLLW&units=english&time_zone=lst_ldt&format=json"
             "&application=papercolor";
  u += "&product="; u += product;
  u += "&date=";    u += date;
  if (interval) { u += "&interval="; u += interval; }
  return u;
}

// hilo needs to look past midnight: a "date=today" fetch stops returning
// events once today's last high/low has already occurred (e.g. evening,
// after the day's final low), which starves drawNowStrip()'s "next event"
// scan and silently drops the trend arrow + NEXT HIGH/LOW block. Widen with
// begin_date+range so tomorrow's first event is always in view.
static String coopsRangeUrl(const char* product, const char* interval,
                            const char* beginDate, int rangeHours) {
  String u = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter"
             "?station=" NOAA_STATION
             "&datum=MLLW&units=english&time_zone=lst_ldt&format=json"
             "&application=papercolor";
  u += "&product=";    u += product;
  u += "&begin_date="; u += beginDate;
  u += "&range=";      u += rangeHours;
  if (interval) { u += "&interval="; u += interval; }
  return u;
}

// ------------------------------------------------------------------ fetch ---
static bool fetchTides(Snapshot& s) {
  JsonDocument doc;

  // begin_date only accepts an actual yyyyMMdd date, not the "today" keyword
  // that the date= param supports -- NOAA returns a "Wrong Date" error
  // otherwise. NTP has already synced by this point in the cycle.
  char todayStr[9];
  struct tm nowTm; time_t now = time(nullptr); localtime_r(&now, &nowTm);
  strftime(todayStr, sizeof todayStr, "%Y%m%d", &nowTm);

  if (httpGetJson(coopsUrl("predictions", "h", "today"), doc)) {
    for (JsonObject p : doc["predictions"].as<JsonArray>()) {
      if (s.nTide >= 26) break;
      s.tide[s.nTide++] = { parseLocal(p["t"]), p["v"].as<float>() };
    }
  }
  doc.clear();

  // hilo has turned out to be the flakiest of the CO-OPS calls in practice --
  // an occasional bare HTTPS failure with no pattern tied to the range widening
  // itself (see CLAUDE.md). Each attempt is already bounded by HTTP_TIMEOUT_MS
  // (in httpGetJson), so retrying a fixed number of times can't hang forever.
  // Retry the same request once, then fall back to the narrower "today" query
  // (smaller payload, more likely to succeed) rather than leaving events empty.
  static constexpr int HILO_RETRIES = 1;
  bool hiloOk = false;
  for (int attempt = 0; attempt <= HILO_RETRIES && !hiloOk; attempt++) {
    if (attempt) { delay(500); Serial.printf("hilo retry %d\n", attempt); }
    hiloOk = httpGetJson(coopsRangeUrl("predictions", "hilo", todayStr, 48), doc);
  }
  if (!hiloOk) {
    doc.clear();
    Serial.println("hilo range fetch failed, falling back to date=today");
    hiloOk = httpGetJson(coopsUrl("predictions", "hilo", "today"), doc);
  }
  if (hiloOk) {
    for (JsonObject p : doc["predictions"].as<JsonArray>()) {
      if (s.nEvents >= 10) break;
      const char* ty = p["type"];
      s.events[s.nEvents++] = { parseLocal(p["t"]), p["v"].as<float>(),
                                ty ? ty[0] : '?' };
    }
  } else {
    Serial.println("hilo fallback failed too, events will be empty");
  }
  doc.clear();

  // Observed level. Falls back to the prediction nearest now if the station
  // is offline, which happens more often than you would hope.
  if (httpGetJson(coopsUrl("water_level", nullptr, "latest"), doc) &&
      doc["data"][0]["v"].is<const char*>()) {
    s.tideNow = doc["data"][0]["v"].as<float>();
  } else if (s.nTide) {
    time_t now = time(nullptr);
    float best = 1e9; 
    for (int i = 0; i < s.nTide; i++) {
      float d = fabsf(float(s.tide[i].t - now));
      if (d < best) { best = d; s.tideNow = s.tide[i].ft; }
    }
  }
  return s.nTide > 0;
}

static bool fetchWind(Snapshot& s) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(SITE_LAT, 4);
  url += "&longitude="; url += String(SITE_LON, 4);
  url += "&hourly=wind_speed_10m"
         "&current=wind_speed_10m,wind_direction_10m,wind_gusts_10m"
         "&wind_speed_unit=kn&timezone=auto&forecast_days=2";

  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;

  s.windNow = doc["current"]["wind_speed_10m"] | 0.0f;
  s.windDir = doc["current"]["wind_direction_10m"] | 0;
  s.gustNow = doc["current"]["wind_gusts_10m"] | 0.0f;

  JsonArray times = doc["hourly"]["time"];
  JsonArray speeds = doc["hourly"]["wind_speed_10m"];
  time_t now = time(nullptr);
  for (size_t i = 0; i < times.size() && s.nForecast < 24; i++) {
    time_t t = parseIso(times[i]);
    if (t < now - 1800) continue;
    s.forecast[s.nForecast++] = { t, speeds[i].as<float>() };
  }
  return true;
}

// ------------------------------------------------------------------- draw ---
// Pixel coordinates come from layout::* (generated from ../layout.json --
// see tools/gen_layout_header.py). Iterate on layout with tools/preview.py
// first, then edit layout.json; both consumers pick it up from there.

// Font mapping from tools/preview.py's PIL fonts to bundled M5GFX fonts (see
// .pio/libdeps/m5stack-papercolor/M5GFX/src/lgfx/Fonts). M5GFX ships no bold
// DejaVu, so the bold sizes (F_HUGE/F_BIG/F_MED) use FreeSansBold instead;
// the regular sizes (F_REG/F_SMALL/F_TINY) use the bundled DejaVu, converted
// from the same DejaVuSans.ttf preview.py uses. Picked one size down from a
// naive line-height match so on-device text doesn't dwarf preview.py's.
//   F_HUGE (Bold 46) -> FreeSansBold18pt7b (42px)
//   F_BIG  (Bold 30) -> FreeSansBold12pt7b (29px)
//   F_MED  (Bold 18) -> FreeSansBold9pt7b  (22px)
//   F_REG  (Reg  15) -> DejaVu18           (18px)
//   F_SMALL(Reg  12) -> DejaVu12           (13px)
//   F_TINY (Reg  10) -> DejaVu9            (10px)

static void drawHeader(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::header;
  gfx.fillRect(0, 0, SCREEN_W, HEIGHT, C_BLUE);
  gfx.setTextColor(C_WHITE, C_BLUE);
  gfx.setTextSize(1);

  gfx.setFont(&fonts::FreeSansBold12pt7b);  // F_BIG equivalent
  gfx.setCursor(STATION_X, STATION_Y);
  gfx.print(STATION_LABEL);

  char buf[40];
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  strftime(buf, sizeof buf, "%a %d %b  %H:%M", &lt);
  gfx.setFont(&fonts::DejaVu12);  // F_SMALL equivalent
  gfx.setCursor(DATETIME_X, DATETIME_Y);
  gfx.print(buf);

  snprintf(buf, sizeof buf, "%.0fC %.0f%%  BATT %d%%",
           s.indoorC, s.indoorRh, s.battery);
  using namespace layout::firmware_only::header;
  gfx.setCursor(SCREEN_W - READOUT_RIGHT_OFFSET, READOUT_Y);
  gfx.print(buf);
}

// Side of the trend triangle in drawNowStrip(). Hand-drawn rather than a text
// glyph because the bundled GFXfont charsets are ASCII-only (0x20-0x7E) --
// no unicode triangle characters, unlike preview.py's PIL-rendered TTF.
static constexpr int TREND_ARROW_SIZE = 20;

static void drawNowStrip(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::now_strip;
  const int y0 = Y0_OFFSET;
  gfx.setTextColor(C_BLACK, C_WHITE);

  gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
  gfx.setCursor(LABEL_X, y0 + LABEL_Y);
  gfx.print("TIDE");

  char buf[16];
  snprintf(buf, sizeof buf, "%.1f", s.tideNow);
  gfx.setFont(&fonts::FreeSansBold18pt7b);  // F_HUGE equivalent
  gfx.setCursor(VALUE_X, y0 + VALUE_Y);
  gfx.print(buf);
  int valW = gfx.textWidth(buf);
  gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
  gfx.setCursor(VALUE_X + valW + UNIT_GAP_X, y0 + UNIT_Y);
  gfx.print("ft");

  // Next tide event strictly after now, mirroring preview.py's "rising"/"nxt"
  // scan over the hilo events.
  time_t now = time(nullptr);
  const TideEvent* nxt = nullptr;
  for (int i = 0; i < s.nEvents; i++) {
    if (s.events[i].t > now) { nxt = &s.events[i]; break; }
  }
  if (!nxt) return;

  bool rising = nxt->kind == 'H';
  uint32_t col = rising ? C_GREEN : C_RED;
  int tx = TREND_ARROW_X, ty = y0 + TREND_ARROW_Y, tw = TREND_ARROW_SIZE;
  if (rising) {
    gfx.fillTriangle(tx, ty + tw, tx + tw, ty + tw, tx + tw / 2, ty, col);
  } else {
    gfx.fillTriangle(tx, ty, tx + tw, ty, tx + tw / 2, ty + tw, col);
  }
  gfx.setTextColor(col, C_WHITE);
  gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
  gfx.setCursor(TREND_WORD_X, y0 + TREND_WORD_Y);
  gfx.print(rising ? "RISING" : "FALLING");
  gfx.setTextColor(C_BLACK, C_WHITE);

  gfx.setFont(&fonts::DejaVu12);  // F_SMALL equivalent
  gfx.drawRightString(nxt->kind == 'H' ? "NEXT HIGH" : "NEXT LOW",
                      SCREEN_W - NEXT_RIGHT_MARGIN, y0 + NEXT_LABEL_DY);

  struct tm nt; localtime_r(&nxt->t, &nt);
  strftime(buf, sizeof buf, "%H:%M", &nt);
  gfx.setFont(&fonts::FreeSansBold12pt7b);  // F_BIG equivalent
  gfx.drawRightString(buf, SCREEN_W - NEXT_RIGHT_MARGIN, y0 + NEXT_TIME_DY);

  snprintf(buf, sizeof buf, "%.1f ft", nxt->ft);
  gfx.setFont(&fonts::DejaVu12);  // F_SMALL equivalent
  gfx.drawRightString(buf, SCREEN_W - NEXT_RIGHT_MARGIN, y0 + NEXT_VALUE_DY);
}

static void drawTide(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::tide_box;
  const int x0 = X0, y0 = Y0, x1 = SCREEN_W - RIGHT_MARGIN, y1 = Y1;
  gfx.setTextColor(C_BLACK, C_WHITE);
  gfx.drawRect(x0, y0, x1 - x0, y1 - y0, C_BLACK);
  if (s.nTide < 2) return;

  float lo = s.tide[0].ft, hi = s.tide[0].ft;
  for (int i = 1; i < s.nTide; i++) {
    lo = min(lo, s.tide[i].ft);
    hi = max(hi, s.tide[i].ft);
  }
  float pad = max(0.4f, (hi - lo) * 0.15f);
  lo -= pad; hi += pad;

  time_t t0 = s.tide[0].t, t1 = s.tide[s.nTide - 1].t;
  auto px = [&](time_t t) {
    return x0 + int(float(t - t0) / float(t1 - t0) * (x1 - x0));
  };
  auto py = [&](float v) {
    return y1 - int((v - lo) / (hi - lo) * (y1 - y0));
  };

  // Vertical fill under the curve. One column at a time is simple and fast
  // enough; the panel refresh dwarfs the draw time regardless.
  for (int i = 0; i < s.nTide - 1; i++) {
    int ax = px(s.tide[i].t),   ay = py(s.tide[i].ft);
    int bx = px(s.tide[i+1].t), by = py(s.tide[i+1].ft);
    for (int x = ax; x <= bx; x++) {
      int y = ay + (bx > ax ? (by - ay) * (x - ax) / (bx - ax) : 0);
      gfx.drawFastVLine(x, y, y1 - y, C_BLUE);
    }
  }

  for (int i = 0; i < s.nEvents; i++) {
    if (s.events[i].t < t0 || s.events[i].t > t1) continue;
    int x = px(s.events[i].t), y = py(s.events[i].ft);
    bool high = s.events[i].kind == 'H';
    gfx.fillCircle(x, y, EVENT_MARKER_RADIUS, high ? C_YELLOW : C_WHITE);
    gfx.drawCircle(x, y, EVENT_MARKER_RADIUS, C_BLACK);
  }

  int nx = px(constrain(time(nullptr), t0, t1));
  for (int i = 0; i < NOW_LINE_WIDTH; i++) {
    gfx.drawFastVLine(nx + i, y0, y1 - y0, C_RED);
  }
}

static void drawWind(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::wind;
  const int wy = Y;
  gfx.setTextColor(C_BLACK, C_WHITE);
  gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
  gfx.setCursor(LABEL_X, wy);
  gfx.print("WIND");

  const int cx = COMPASS_CX, cy = wy + COMPASS_DY, r = COMPASS_R;
  gfx.drawCircle(cx, cy, r, C_BLACK);

  // N/E/S/W labels around the rose, matching preview.py's compass_label_*.
  static const char* COMPASS_LABELS[4] = {"N", "E", "S", "W"};
  int lr = r + COMPASS_LABEL_RADIUS_OFFSET;
  gfx.setFont(&fonts::DejaVu9);  // F_TINY equivalent
  gfx.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < 4; i++) {
    float la = radians(i * 90.0f - 90.0f);
    gfx.drawString(COMPASS_LABELS[i], cx + int(cosf(la) * lr), cy + int(sinf(la) * lr));
  }
  gfx.setTextDatum(textdatum_t::top_left);

  float a = radians(s.windDir + 180 - 90);
  int tipx = cx + cosf(a) * (r - ARROW_TIP_INSET), tipy = cy + sinf(a) * (r - ARROW_TIP_INSET);
  int tlx  = cx - cosf(a) * (r - ARROW_TAIL_INSET), tly = cy - sinf(a) * (r - ARROW_TAIL_INSET);
  uint32_t ac = windColor(s.windNow);
  // Without a barb, the shaft is the same width at both ends, so nothing on
  // the panel actually marks which end is the tip -- matches preview.py's
  // two angled strokes back from the tip (arrow_barb_angle_deg/_length).
  float barbAngle = radians(float(ARROW_BARB_ANGLE_DEG));
  int barbAx = tipx + int(cosf(a + barbAngle) * ARROW_BARB_LENGTH);
  int barbAy = tipy + int(sinf(a + barbAngle) * ARROW_BARB_LENGTH);
  int barbBx = tipx + int(cosf(a - barbAngle) * ARROW_BARB_LENGTH);
  int barbBy = tipy + int(sinf(a - barbAngle) * ARROW_BARB_LENGTH);
  // Black underlay first, so a yellow arrow still reads against white.
  for (int pass = 0; pass < 2; pass++) {
    uint32_t col = pass ? ac : C_BLACK;
    int w = pass ? ARROW_COLOR_WIDTH : ARROW_UNDERLAY_WIDTH;
    for (int o = -w / 2; o <= w / 2; o++) {
      gfx.drawLine(tlx + o, tly, tipx + o, tipy, col);
      gfx.drawLine(tlx, tly + o, tipx, tipy + o, col);
      gfx.drawLine(tipx + o, tipy, barbAx + o, barbAy, col);
      gfx.drawLine(tipx, tipy + o, barbAx, barbAy + o, col);
      gfx.drawLine(tipx + o, tipy, barbBx + o, barbBy, col);
      gfx.drawLine(tipx, tipy + o, barbBx, barbBy + o, col);
    }
  }

  char buf[48];
  using namespace layout::firmware_only::wind;
  snprintf(buf, sizeof buf, "%.0f", s.windNow);
  gfx.setFont(&fonts::FreeSansBold18pt7b);  // F_HUGE equivalent
  gfx.setCursor(READING_X, wy + READING_DY);
  gfx.print(buf);
  int numW = gfx.textWidth(buf);
  gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
  gfx.setCursor(READING_X + numW + KT_GAP, wy + KT_DY);
  gfx.print("kt");

  gfx.setFont(&fonts::DejaVu18);  // F_REG equivalent
  snprintf(buf, sizeof buf, "GUST %.0f kt", s.gustNow);
  gfx.setCursor(GUST_X, wy + GUST_DY); gfx.print(buf);
  snprintf(buf, sizeof buf, "FROM %s %d", compass(s.windDir), s.windDir);
  gfx.setCursor(FROM_X, wy + FROM_DY); gfx.print(buf);
  // DejaVu18's charset is ASCII-only (0x20-0x7E), no degree sign -- draw a
  // small ring instead, matching preview.py's trailing "°".
  int fromW = gfx.textWidth(buf);
  static constexpr int DEG_RADIUS = 2, DEG_GAP = 2, DEG_Y_OFFSET = 3;
  gfx.drawCircle(FROM_X + fromW + DEG_GAP + DEG_RADIUS,
                 wy + FROM_DY + DEG_Y_OFFSET + DEG_RADIUS, DEG_RADIUS, C_BLACK);

  // Speed band as a solid chip -- coloured text is unreadable on this panel.
  gfx.fillRect(SCREEN_W - CHIP_RIGHT_OFFSET, wy + CHIP_DY, CHIP_W, CHIP_H, ac);
  gfx.drawRect(SCREEN_W - CHIP_RIGHT_OFFSET, wy + CHIP_DY, CHIP_W, CHIP_H, C_BLACK);
}

static void drawForecast(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::forecast;
  const int x0 = X0, y1 = BOTTOM, x1 = SCREEN_W - RIGHT_MARGIN, y0 = TOP;
  gfx.drawFastHLine(x0, y1, x1 - x0, C_BLACK);

  gfx.setTextColor(C_BLACK, C_WHITE);
  gfx.setFont(&fonts::DejaVu12);  // F_SMALL equivalent
  gfx.drawRightString("WIND, NEXT 24H (kt)", SCREEN_W - RIGHT_MARGIN, y0 - LABEL_DY_ABOVE_TOP);

  if (!s.nForecast) return;

  float peak = 20.0f;
  for (int i = 0; i < s.nForecast; i++) peak = max(peak, s.forecast[i].kt);

  int bw = (x1 - x0) / s.nForecast;
  gfx.setFont(&fonts::DejaVu9);  // F_TINY equivalent
  for (int i = 0; i < s.nForecast; i++) {
    float v = max(0.0f, s.forecast[i].kt);
    int h = max(BAR_MIN_HEIGHT, int(v / peak * (y1 - y0 - BAR_HEIGHT_MARGIN)));
    int x = x0 + i * bw;
    gfx.fillRect(x + BAR_INSET, y1 - h, bw - 2 * BAR_INSET, h, windColor(v));
    gfx.drawRect(x + BAR_INSET, y1 - h, bw - 2 * BAR_INSET, h, C_BLACK);

    struct tm ft; localtime_r(&s.forecast[i].t, &ft);
    if (ft.tm_hour % 6 == 0) {
      char hbuf[4];
      snprintf(hbuf, sizeof hbuf, "%02d", ft.tm_hour);
      gfx.drawCenterString(hbuf, x + bw / 2, y1 + HOUR_LABEL_DY);
    }
  }
}

static void drawFooter(M5Canvas& gfx, const Snapshot& s) {
  (void)s;
  using namespace layout::footer;
  gfx.setTextColor(C_BLACK, C_WHITE);
  gfx.setFont(&fonts::DejaVu9);  // F_TINY equivalent
  gfx.setCursor(LEFT_X, Y);
  // DejaVu9's charset is ASCII-only (0x20-0x7E), no middle dot -- use a
  // hyphen in place of preview.py's "·".
  gfx.print("NOAA CO-OPS - Open-Meteo");

  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  char buf[16];
  strftime(buf, sizeof buf, "UPD %H:%M", &lt);
  gfx.drawRightString(buf, SCREEN_W - RIGHT_MARGIN, Y);
}

// Allocates and palettes the off-screen canvas shared by drawAll() and the
// ghost-clearing pass. M5Canvas(&M5.Display) defaults _psram = true (see
// M5GFX.h), so this comes out of the 8MB PSRAM, not the ~320KB internal
// heap -- but ESP.getFreeHeap() is still sampled around allocation in
// drawAll() per the internal-heap headroom this was asked to confirm.
static bool initCanvas(M5Canvas& canvas) {
  canvas.setColorDepth(lgfx::color_depth_t::palette_4bit);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) return false;
  canvas.createPalette(PALETTE_RGB, 6);
  return true;
}

// Ghost-clearing pass (see setup()) draws directly to M5.Display previously;
// now routed through the same canvas path as drawAll() so it never touches
// M5.Display with a raw palette-index "color" value by mistake.
static void clearScreenFull() {
  M5Canvas canvas(&M5.Display);
  if (!initCanvas(canvas)) {
    Serial.println("CANVAS ALLOC FAILED for ghost-clear pass");
    return;
  }
  canvas.fillScreen(C_WHITE);
  canvas.pushSprite(0, 0);
  canvas.deleteSprite();
  M5.Display.display();
}

static void drawAll(const Snapshot& s) {
  uint32_t heapBefore = ESP.getFreeHeap();

  M5Canvas canvas(&M5.Display);
  if (!initCanvas(canvas)) {
    uint32_t heapAfter = ESP.getFreeHeap();
    Serial.printf("CANVAS ALLOC FAILED: %dx%d @ palette_4bit, heap before=%lu after=%lu\n",
                  SCREEN_W, SCREEN_H, (unsigned long)heapBefore, (unsigned long)heapAfter);
    return;
  }

  uint32_t heapAfter = ESP.getFreeHeap();
  Serial.printf("canvas: %dx%d @ palette_4bit = %lu bytes, heap before=%lu after=%lu (used=%ld)\n",
                SCREEN_W, SCREEN_H, (unsigned long)canvas.bufferLength(),
                (unsigned long)heapBefore, (unsigned long)heapAfter,
                (long)heapBefore - (long)heapAfter);

  canvas.fillScreen(C_WHITE);
  drawHeader(canvas, s);
  drawNowStrip(canvas, s);
  drawTide(canvas, s);
  drawWind(canvas, s);
  drawForecast(canvas, s);
  drawFooter(canvas, s);

  canvas.pushSprite(0, 0);
  canvas.deleteSprite();

  // Confirmed against M5GFX's Panel_ED2208::display(): it calls
  // _turn_on_display() -> _wait_busy(), so this blocks for the full
  // 15-30s refresh (see CLAUDE.md hardware facts).
  M5.Display.display();
}

// ------------------------------------------------------------------- main ---
static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void sleepUntilNext() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Confirmed against refs/M5PaperColor-UserDemo (hal.cpp + app_manager.cpp):
  // the 92.53uA standby figure is the PM1 fully shut down, not an ESP32
  // timer-only deep sleep -- that leaves the PM1's LDO/DCDC rails running.
  // The RX8130 alarm pulls its nIRQ line, which PM1 GPIO2 watches as an
  // external wake source; that re-powers the whole board from cold, so
  // nothing in RAM survives except the PM1's own RTC RAM.
  pm1.gpioSetFunc(PM1_RTC_WAKE, M5PM1_GPIO_FUNC_WAKE);
  pm1.gpioSetPull(PM1_RTC_WAKE, M5PM1_GPIO_PULL_UP);
  pm1.gpioSetWakeEdge(PM1_RTC_WAKE, M5PM1_GPIO_WAKE_FALLING);
  pm1.gpioSetWakeEnable(PM1_RTC_WAKE, true);

  // Schedule off the RX8130's own battery-backed clock, not the system
  // clock -- that keeps the wake alarm correct even on a cycle where NTP
  // failed. Matches Hal::scheduleNextWakeMinutes() in the reference demo.
  m5::rtc_date_t rDate;
  m5::rtc_time_t rTime;
  struct tm nextTm = {};
  if (M5.Rtc.getDateTime(&rDate, &rTime)) {
    nextTm.tm_year = rDate.year - 1900;
    nextTm.tm_mon  = rDate.month - 1;
    nextTm.tm_mday = rDate.date;
    nextTm.tm_hour = rTime.hours;
    nextTm.tm_min  = rTime.minutes + UPDATE_MINUTES;
    nextTm.tm_sec  = rTime.seconds;
  } else {
    time_t next = time(nullptr) + time_t(UPDATE_MINUTES) * 60;
    localtime_r(&next, &nextTm);
  }
  mktime(&nextTm);  // normalize tm_min overflow into tm_hour/tm_mday
  M5.Rtc.setAlarmIRQ(&nextTm);

  uint8_t wakeSrc = 0;
  pm1.getWakeSource(&wakeSrc, M5PM1_CLEAN_ALL);
  M5.Rtc.clearIRQ();
  pm1.timerClear();

  Serial.flush();
  pm1.shutdown();
  while (true) delay(1000);   // power drops within ms; this is a safety net
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 15000) delay(10);
  delay(500);

  // Confirmed against refs/M5PaperColor-UserDemo/main/hal/hal.cpp Hal::init():
  // M5.begin() does not drive PM1 GPIO0 (PY_EPD_EN) for this board -- it only
  // enables the RGB LDO and the SD card rail (GPIO3). Without this the panel
  // stays blank.
  pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K);
  pm1.pinMode(PM1_EPD_EN, OUTPUT);
  pm1.digitalWrite(PM1_EPD_EN, HIGH);

  uint32_t cycle = nextCycleCount();
  Serial.printf("\n=== cycle %lu ===\n", (unsigned long)cycle);

  Snapshot s;
  if (!readSht40(&s.indoorC, &s.indoorRh)) {
    s.indoorC = 0; s.indoorRh = 0;
  }
  s.battery  = M5.Power.getBatteryLevel();

  if (connectWifi()) {
    configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");

    // getLocalTime() alone can't tell a genuine NTP sync from the RTC-seeded
    // system clock M5.begin() already set via M5.Rtc.setSystemTimeFromRtc()
    // (M5Unified.cpp _begin_rtc_imu(), runs before Wi-Fi/NTP even starts) --
    // Arduino's getLocalTime() (esp32-hal-time.c) just checks
    // tm_year > 2016, which a stale/wrong RTC value already satisfies before
    // any SNTP round-trip happens. That let it report "success" off the
    // RTC's old time and write that straight back via M5.Rtc.setDateTime()
    // below every cycle, never actually correcting it -- root cause of the
    // hilo next-event block silently going blank (fetched against a
    // begin_date computed from that stale clock, then compared against the
    // real NTP-corrected "now" once it landed later in the same cycle, so
    // every event looked like it was already in the past). Poll the real
    // ESP-IDF SNTP sync status instead. Confirmed by reading
    // esp32-hal-time.c, M5Unified.cpp, RTC_Class.cpp and esp_sntp.h,
    // 2026-08-30.
    uint32_t sntpStart = millis();
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED &&
           millis() - sntpStart < 10000) {
      delay(50);
    }

    struct tm lt;
    if (getLocalTime(&lt, 1000)) {
      // The RX8130 alarm in sleepUntilNext() schedules by its own clock, so
      // keep it synced to NTP every cycle.
      M5.Rtc.setDateTime(&lt);
    } else {
      Serial.println("ntp failed");
    }

    bool tides = fetchTides(s);
    bool wind  = fetchWind(s);
    s.ok = tides || wind;
    // events counted separately from `tides`, which only reflects the hourly
    // curve fetch -- the hilo (next-event) fetch can silently fail on its own.
    Serial.printf("tides=%d wind=%d events=%d\n", tides, wind, s.nEvents);
  } else {
    Serial.println("wifi failed");
  }

  if (cycle % FULL_REFRESH_EVERY == 1) {
    clearScreenFull();           // ghost-clearing pass
  }

  drawAll(s);
  sleepUntilNext();
}

void loop() {}   // never reached; setup() ends in deep sleep
