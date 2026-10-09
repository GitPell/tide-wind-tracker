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
#include <climits>     // LONG_MIN -- fetchWind()'s "age unknown" marker
#include "esp_sntp.h"  // sntp_get_sync_status() -- see setup()'s NTP-wait comment

#include "config.h"
#include "render.h"  // Snapshot, palette constants, draw*() -- see src/render.h

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

// M5Unified has no Sht4x driver on this board -- confirmed by compile error,
// see CLAUDE.md. Read the SHT40 (I2C 0x44) directly, matching
// refs/M5PaperColor-UserDemo/main/hal/hal.cpp Hal::sht40Read().
// Based on M5Stack's factory demo Hal::sht40Read() (MIT, © M5Stack).
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
// timeoutMs bounds the TLS handshake and each gap between received bytes
// separately, not the request as a whole. TCP connect keeps HTTPClient's own
// 5s default, and DNS keeps lwip's ~14s. The handshake has to be set
// explicitly: WiFiClientSecure defaults it to 120s, and its socket is
// non-blocking, so a stalled handshake spins until that runs out
// (framework WiFiClientSecure.cpp / ssl_client.cpp).
static bool httpGetJson(const String& url, JsonDocument& doc,
                        const DeserializationOption::Filter* filter = nullptr,
                        uint32_t timeoutMs = HTTP_TIMEOUT_MS) {
  WiFiClientSecure client;
  client.setInsecure();          // no cert pinning; see CLAUDE.md
  client.setTimeout(timeoutMs / 1000);
  client.setHandshakeTimeout(timeoutMs / 1000);

  HTTPClient http;
  http.setTimeout(timeoutMs);
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
  bool levelOk = true;
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
  } else {
    levelOk = false;
  }

  // Failure flags for the render (see Snapshot::fail), set from what
  // actually arrived so they match what the draw functions check.
  if (s.nTide < 2)  s.fail |= FAIL_TIDE_CURVE;
  if (!s.nEvents)   s.fail |= FAIL_HILO;
  if (!levelOk)     s.fail |= FAIL_TIDE_NOW;
  return s.nTide > 0;
}

// Observed wind from the station's own sensor (C1, 24 ft above the site),
// preferred over Open-Meteo's modeled values per CLAUDE.md "Data sources".
// `s` is speed in knots under units=english, `d` degrees, `g` gust knots --
// all as strings, which .as<float>() parses (same as the tide `v` fields).
// Shorter timeout than the other fetches because fetchWind() already holds
// Open-Meteo's values as a fallback.
static constexpr uint32_t OBS_WIND_TIMEOUT_MS = 5000;
static constexpr long     OBS_WIND_MAX_AGE_S  = 30 * 60;

// Returns nullptr and fills s.windNow/windDir/gustNow on success; otherwise
// returns a short reason for the WINDSRC log line and leaves `s` untouched.
// *ageMin is set whenever the reading's timestamp parsed.
static const char* fetchObservedWind(Snapshot& s, long* ageMin) {
  JsonDocument doc;
  if (!httpGetJson(coopsUrl("wind", nullptr, "latest"), doc, nullptr,
                   OBS_WIND_TIMEOUT_MS)) {
    return "http";
  }
  if (doc["error"].is<JsonObject>()) {   // CO-OPS reports errors with HTTP 200
    Serial.printf("obs wind: %s\n", doc["error"]["message"] | "?");
    return "noaa-error";
  }

  JsonObject r = doc["data"][0];
  const char* t  = r["t"];
  const char* sp = r["s"];
  const char* d  = r["d"];
  const char* g  = r["g"];
  if (!t || !sp || !d || !g || !*sp || !*d || !*g) return "parse";

  time_t obsT = parseLocal(t);
  if (!obsT) return "parse";
  long age = long(time(nullptr) - obsT);
  *ageMin = age / 60;
  if (labs(age) > OBS_WIND_MAX_AGE_S) return "stale";

  s.windNow = r["s"].as<float>();
  s.windDir = int(lroundf(r["d"].as<float>())) % 360;
  s.gustNow = r["g"].as<float>();
  return nullptr;
}

static bool fetchWind(Snapshot& s) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(SITE_LAT, 4);
  url += "&longitude="; url += String(SITE_LON, 4);
  url += "&hourly=wind_speed_10m"
         "&current=wind_speed_10m,wind_direction_10m,wind_gusts_10m"
         "&wind_speed_unit=kn&timezone=auto&forecast_days=2";

  // Open-Meteo is fetched every cycle for the forecast bars. Its `current`
  // values are the fallback for current speed, direction and gust, and are
  // overwritten below when the station's observed reading is usable.
  JsonDocument doc;
  bool fcstOk = httpGetJson(url, doc);
  if (fcstOk) {
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
  }
  doc.clear();

  // One grep-able line per cycle (WINDSRC prefix, like BATLOG): which source
  // supplied current wind, and why the observed reading wasn't used if not.
  // src=none means both failed and the values stay at Snapshot's zeros.
  long ageMin = LONG_MIN;
  const char* why = fetchObservedWind(s, &ageMin);
  char age[16] = "?";
  if (ageMin != LONG_MIN) snprintf(age, sizeof age, "%ld", ageMin);
  Serial.printf("WINDSRC src=%s reason=%s age_min=%s\n",
                why ? (fcstOk ? "fcst" : "none") : "obs", why ? why : "ok", age);

  if (!s.nForecast)     s.fail |= FAIL_FORECAST;
  if (!fcstOk && why)   s.fail |= FAIL_WIND_NOW;
  return fcstOk || !why;
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

#ifdef TIER1_TEST
// ------------------------------------------------------------- tier1 test ---
// No Wi-Fi/NOAA/Open-Meteo fetch, no PM1 sleep, no panel refresh. setup()
// instead waits for the serial host to open, then services newline-
// terminated PING/RENDER/QUIT commands over USB CDC -- see tools/hil.py.
// This makes the render path (initCanvas() + the six unmodified draw*()
// functions above) verifiable from a fixture JSON with no network or
// hardware timing dependencies.

// Snapshot JSON mirrors the struct field-for-field; each array's length is
// implicit in its size (capped at the same 26/10/24 the struct arrays hold,
// same as the production fetch path). time_t fields are Unix epoch seconds.
// See test/fixtures/example.json for a worked example and tools/hil.py for
// the host-side encoder/decoder.
static void tier1ParseSnapshot(JsonDocument& doc, Snapshot& s) {
  // Defaults to STATION_LABEL (config.h) when a fixture omits the field --
  // every existing fixture predates this field, and that default is exactly
  // what those fixtures rendered with before stationLabel existed (drawHeader()
  // read the STATION_LABEL macro directly), so their goldens stay valid
  // unchanged. See render.h's Snapshot::stationLabel comment.
  const char* label = doc["stationLabel"] | STATION_LABEL;
  snprintf(s.stationLabel, sizeof s.stationLabel, "%s", label);

  s.tideNow  = doc["tideNow"]  | 0.0f;
  s.windNow  = doc["windNow"]  | 0.0f;
  s.gustNow  = doc["gustNow"]  | 0.0f;
  s.windDir  = doc["windDir"]  | 0;
  s.indoorC  = doc["indoorC"]  | 0.0f;
  s.indoorRh = doc["indoorRh"] | 0.0f;
  s.battery  = doc["battery"]  | 0;
  s.ok       = doc["ok"]       | false;
  s.now      = (time_t)(doc["now"] | (int64_t)0);

  // Optional failure fields (absent in fixtures that predate them = no
  // failure). Names map through render.cpp's shared table.
  s.fail = 0;
  for (JsonVariant v : doc["fail"].as<JsonArray>()) {
    s.fail |= failFlagFromName(v | "");
  }
  snprintf(s.wifiReason, sizeof s.wifiReason, "%s",
           doc["wifiReason"] | "");

  s.nTide = 0;
  for (JsonObject p : doc["tide"].as<JsonArray>()) {
    if (s.nTide >= 26) break;
    s.tide[s.nTide++] = { (time_t)(p["t"] | (int64_t)0), p["ft"] | 0.0f };
  }

  s.nEvents = 0;
  for (JsonObject p : doc["events"].as<JsonArray>()) {
    if (s.nEvents >= 10) break;
    const char* kind = p["kind"] | "?";
    s.events[s.nEvents++] = { (time_t)(p["t"] | (int64_t)0), p["ft"] | 0.0f, kind[0] };
  }

  s.nForecast = 0;
  for (JsonObject p : doc["forecast"].as<JsonArray>()) {
    if (s.nForecast >= 24) break;
    s.forecast[s.nForecast++] = { (time_t)(p["t"] | (int64_t)0), p["kt"] | 0.0f };
  }
}

// Composes the same off-screen canvas drawAll() does -- same initCanvas()
// and the same six draw*() functions, called unmodified -- but stops short
// of pushSprite()/M5.Display.display(). The canvas buffer is dumped
// directly instead, per CLAUDE.md's note that it, not the physical panel,
// is "the deterministic artifact for automated verification."
static bool tier1RenderToCanvas(M5Canvas& canvas, const Snapshot& s) {
  if (!initCanvas(canvas)) return false;
  canvas.fillScreen(C_WHITE);
  drawHeader(canvas, s);
  drawNowStrip(canvas, s);
  drawTide(canvas, s);
  drawWind(canvas, s);
  drawForecast(canvas, s);
  drawFooter(canvas, s);
  return true;
}

static const char TIER1_B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// base64-encodes buf/len straight to Serial, wrapped at 76 chars/line as the
// dump format requires. Hand-rolled rather than pulling in mbedtls for one
// call site.
//
// Batched into a 16KB static chunk buffer and flushed with Serial.write()
// only when the chunk fills, instead of one pair of small write() calls per
// 76-char line (~4200 calls for a 120000-byte canvas). That per-line
// pattern was the actual bottleneck behind the ~14s dump time, not USB
// speed or baud: HWCDC::write() (arduino-esp32's
// cores/esp32/HWCDC.cpp, the class backing Serial on this board's native
// USB_SERIAL_JTAG peripheral -- confirmed baud-independent, it never reads
// its own `baud` argument) takes a FreeRTOS mutex and re-checks connection
// state on every call, so thousands of tiny calls serialize against that
// per-call overhead instead of letting the ISR-driven 64-byte-at-a-time
// drain run continuously. Chunking to 16KB cuts ~4200 calls to ~11.
//
// HWCDC::write() can legitimately return fewer bytes than requested --
// e.g. its blocking retry loop bails out early if `connected` flips false
// mid-write (a real, observed live: a large write can straddle a brief
// USB_SERIAL_JTAG connection blip). Confirmed live, 2026-09-05: an
// unchecked Serial.write() return value here silently truncated a real
// render's dump to ~36000 of 120000 bytes with no error at all -- the host
// side saw a well-formed ---FB-END--- and only caught it via the
// BYTES= size check. tier1WriteAll() retries the remainder instead of
// dropping it, and reports failure instead of continuing silently.
static bool tier1WriteAll(const uint8_t* buf, size_t len) {
  uint32_t start = millis();
  size_t sent = 0;
  while (sent < len) {
    size_t n = Serial.write(buf + sent, len - sent);
    sent += n;
    if (sent >= len) break;
    if (millis() - start > 5000) return false;  // no full progress in 5s -- give up
    delay(1);
  }
  return true;
}

static bool tier1PrintBase64(const uint8_t* buf, size_t len) {
  static constexpr int LINE_CHARS = 76;
  static constexpr size_t CHUNK_BYTES = 16384;
  static char chunk[CHUNK_BYTES];
  size_t chunkLen = 0;
  int col = 0;
  bool ok = true;

  auto flushChunk = [&]() {
    if (chunkLen && ok) {
      ok = tier1WriteAll((const uint8_t*)chunk, chunkLen);
    }
    chunkLen = 0;
  };
  // Leaves headroom for the up-to-2 bytes (char + possible '\n') a single
  // emit() can add before the next capacity check.
  auto emit = [&](char c) {
    chunk[chunkLen++] = c;
    if (++col == LINE_CHARS) { chunk[chunkLen++] = '\n'; col = 0; }
    if (chunkLen >= CHUNK_BYTES - 2) flushChunk();
  };

  size_t i = 0;
  for (; i + 3 <= len && ok; i += 3) {
    uint32_t n = (uint32_t(buf[i]) << 16) | (uint32_t(buf[i + 1]) << 8) | buf[i + 2];
    emit(TIER1_B64_CHARS[(n >> 18) & 0x3F]);
    emit(TIER1_B64_CHARS[(n >> 12) & 0x3F]);
    emit(TIER1_B64_CHARS[(n >> 6) & 0x3F]);
    emit(TIER1_B64_CHARS[n & 0x3F]);
  }
  size_t rem = ok ? len - i : 0;
  if (rem) {
    uint32_t n = uint32_t(buf[i]) << 16;
    if (rem == 2) n |= uint32_t(buf[i + 1]) << 8;
    emit(TIER1_B64_CHARS[(n >> 18) & 0x3F]);
    emit(TIER1_B64_CHARS[(n >> 12) & 0x3F]);
    emit(rem == 2 ? TIER1_B64_CHARS[(n >> 6) & 0x3F] : '=');
    emit('=');
  }
  if (ok && col != 0) chunk[chunkLen++] = '\n';  // terminate a trailing partial line
  flushChunk();
  return ok;
}

static void tier1HandleRender(const String& json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("#ERR json %s\n", err.c_str());
    return;
  }

  Snapshot s;
  tier1ParseSnapshot(doc, s);

  M5Canvas canvas(&M5.Display);
  if (!tier1RenderToCanvas(canvas, s)) {
    Serial.println("#ERR canvas alloc failed");
    return;
  }

  const uint8_t* buf = (const uint8_t*)canvas.getBuffer();
  uint32_t len = canvas.bufferLength();

  Serial.println("---FB-BEGIN---");
  Serial.printf("W=%d H=%d DEPTH=4 BYTES=%lu\n", SCREEN_W, SCREEN_H, (unsigned long)len);
  uint32_t dumpStart = millis();
  bool dumpOk = tier1PrintBase64(buf, len);
  if (dumpOk) {
    Serial.println("---FB-END---");
    // Informational only, printed after FB-END so it can't be mistaken for
    // base64 payload -- tools/hil.py's Device.render() makes a best-effort
    // read for this one extra line (Device._read_dump_ms()) to report
    // actual device-side dump time; its absence (e.g. an older firmware
    // build) is not an error, just missing bonus instrumentation.
    Serial.printf("DUMP_MS=%lu\n", (unsigned long)(millis() - dumpStart));
  } else {
    // No ---FB-END--- in this case -- tier1WriteAll() couldn't push the
    // remaining bytes within its own budget (see its comment). Report it
    // as an explicit error rather than leaving hil.py to either hang
    // waiting for a FB-END that isn't coming, or worse, silently accept a
    // truncated dump as if BYTES= had matched. hil.py's Device checks for
    // a "#ERR"-prefixed line during the transfer phase too, not just before
    // ---FB-BEGIN---, specifically to catch this. The '#' is not in the
    // base64 alphabet, so it can never collide with a line of legitimate
    // payload (unlike a plain "ERR" prefix -- base64's alphabet includes
    // E, R -- see the matching note in hil.py).
    Serial.println("#ERR dump write failed");
  }

  canvas.deleteSprite();
}

void setup() {
  auto cfg = M5.config();
  // TIER1_TEST never reads real RTC/IMU hardware: Snapshot::now comes from
  // the JSON fixture (see tier1ParseSnapshot()), not M5.Rtc, and
  // M5PaperColor has no IMU chip on the bus at all -- M5.Imu.begin() would
  // just probe several chip addresses that can never answer. Both are
  // gated by M5Unified::config_t flags (M5Unified.hpp, both default true)
  // that skip the corresponding _begin_rtc_imu() calls entirely
  // (M5Unified.cpp) rather than merely running faster, so this is a real
  // elimination of I2C traffic, not a guess. Left enabled in the
  // production #else branch below, which does need the real RTC.
  cfg.internal_rtc = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;   // diagnostic: M5PaperColor has no mic either
  cfg.internal_spk = false;   // diagnostic: nor a speaker

  // Default HWCDC RX/TX ring buffers are 256 bytes each (HWCDC::begin(),
  // only applied if not already set) -- too small for a multi-KB RENDER
  // fixture in one host-side write() (RX), and far too small for the
  // ~166KB base64 framebuffer dump (TX): every Serial.write() call blocks
  // until the 256-byte ring buffer has room, so thousands of small writes
  // into a tiny buffer serialize against HWCDC's own per-call mutex/ISR
  // overhead (HWCDC.cpp) rather than USB link speed -- confirmed as the
  // actual bottleneck behind a slow dump, not baud (HWCDC ignores its
  // `baud` argument entirely; see the BAUD comment in tools/hil.py). Both
  // must be set before Serial.begin(). Moved ahead of M5.begin() (2026-09-05,
  // diagnostic session): M5.begin()'s own verbose logging during board
  // autodetection was overflowing the default 256-byte buffer and silently
  // dropping lines before this call used to run.
  Serial.setRxBufferSize(32768);
  Serial.setTxBufferSize(32768);
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 15000) delay(10);
  delay(500);
  Serial.setTimeout(5000);

  uint32_t beginStart = millis();
  M5.begin(cfg);
  uint32_t beginMs = millis() - beginStart;

  Serial.printf("M5.begin() took %lu ms (internal_rtc=%d internal_imu=%d)\n",
                (unsigned long)beginMs, cfg.internal_rtc, cfg.internal_imu);
  Serial.println("TIER1-READY");

  while (true) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (line == "PING") {
      Serial.println("PONG");
    } else if (line == "QUIT") {
      Serial.println("BYE");
      Serial.flush();
      while (true) delay(1000);
    } else if (line.startsWith("RENDER ")) {
      tier1HandleRender(line.substring(7));
    } else {
      Serial.printf("#ERR unknown command: %s\n", line.c_str());
    }
  }
}

void loop() {}   // never reached; setup() never returns

#else
// ------------------------------------------------------------------- main ---
// Last STA disconnect reason this cycle (0 = none yet), recorded by the
// event handler connectWifi() registers. The Wi-Fi event task writes it.
static volatile uint8_t s_lastDisconnectReason = 0;

static const char* wifiStatusName(wl_status_t st) {
  switch (st) {
    case WL_NO_SHIELD:       return "WL_NO_SHIELD";
    case WL_IDLE_STATUS:     return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:       return "WL_CONNECTED";
    case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:    return "WL_DISCONNECTED";
  }
  return "UNKNOWN";
}

// Failure path only. Grep-able lines like BATLOG/WINDSRC: one WIFIFAIL line
// (status + last disconnect reason), one WIFISCAN line per visible network,
// and a WIFISCAN summary. Together they separate the likely causes:
// NO_AP_FOUND with target_seen=0 (AP off, out of range, or 5 GHz-only --
// the ESP32-S3 is 2.4 GHz only), AUTH_FAIL / 4WAY_HANDSHAKE_TIMEOUT (wrong
// password), or reason=0(none) (joined, but no IP from DHCP). The active
// scan costs ~1.5-4s (up to 300ms per channel; the framework caps its wait
// at 10s) -- only ever on a cycle that already failed.
// Also writes a short reason name into reasonOut for the panel header:
// the disconnect reason, or NO_IP (joined, no DHCP answer) / TIMEOUT when
// no disconnect event explains the failure.
static void logWifiFailure(uint32_t elapsedMs, char* reasonOut, size_t size) {
  wl_status_t st = WiFi.status();
  uint8_t reason = s_lastDisconnectReason;  // before disconnect() overwrites it
  Serial.printf("WIFIFAIL status=%s(%d) reason=%u(%s) elapsed_ms=%lu\n",
                wifiStatusName(st), int(st), reason,
                reason ? WiFi.disconnectReasonName(wifi_err_reason_t(reason))
                       : "none",
                (unsigned long)elapsedMs);
  snprintf(reasonOut, size, "%s",
           reason ? WiFi.disconnectReasonName(wifi_err_reason_t(reason))
                  : (st == WL_IDLE_STATUS ? "NO_IP" : "TIMEOUT"));

  WiFi.disconnect();  // stop reconnect attempts: a scan can't start mid-connect
  int16_t n = WiFi.scanNetworks();
  if (n < 0) {
    Serial.printf("WIFISCAN failed code=%d\n", n);
    return;
  }
  bool seen = false;
  for (int i = 0; i < n; i++) {
    bool target = WiFi.SSID(i) == WIFI_SSID;
    seen |= target;
    Serial.printf("WIFISCAN ssid=\"%s\" ch=%ld rssi=%ld target=%d\n",
                  WiFi.SSID(i).c_str(), (long)WiFi.channel(i),
                  (long)WiFi.RSSI(i), int(target));
  }
  Serial.printf("WIFISCAN done count=%d target_seen=%d\n", n, int(seen));
  WiFi.scanDelete();
}

static bool connectWifi(char* reasonOut, size_t size) {
  // Registering the handler is all this adds to the success path; the
  // WIFIFAIL/WIFISCAN work below runs only after the timeout expires.
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    s_lastDisconnectReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  logWifiFailure(millis() - start, reasonOut, size);
  return false;
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
  // tm_isdst must be -1 (let mktime() resolve DST itself), not the 0 that
  // struct tm nextTm = {} leaves it at -- see CLAUDE.md Verified corrections
  // for how the 0 default cost a real hour here. Matches parseLocal()/
  // parseIso() above.
  nextTm.tm_isdst = -1;
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
  uint32_t beginStart = millis();
  M5.begin(cfg);
  // Timed the same way, at the same point (immediately after M5.begin()
  // returns, before anything else), as the TIER1_TEST build's setup() --
  // see its comment -- to check whether the multi-second M5.begin() delay
  // seen there is inherent to this board/library (present here too) or an
  // artifact of TIER1_TEST specifically.
  uint32_t beginMs = millis() - beginStart;
  Serial.begin(115200);
  // Wait for a terminal only when a USB host is actually present.
  // isPlugged() tracks USB start-of-frame packets, which a host sends every
  // 1ms; HWCDC.cpp's tick hook marks the port unplugged after ~5ms without
  // one. On battery there is no host, so `!Serial` stays true and this loop
  // used to sit out the full 15s on every wake. With a host attached,
  // behavior is unchanged. (TIER1_TEST's own wait above always runs with a
  // host, so it's left as is.)
  if (Serial.isPlugged()) {
    uint32_t serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 15000) delay(10);
    delay(500);
  }
  Serial.printf("M5.begin() took %lu ms\n", (unsigned long)beginMs);

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
  snprintf(s.stationLabel, sizeof s.stationLabel, "%s", STATION_LABEL);
  if (!readSht40(&s.indoorC, &s.indoorRh)) {
    s.indoorC = 0; s.indoorRh = 0;
    s.fail |= FAIL_INDOOR;
  }
  s.battery  = M5.Power.getBatteryLevel();

  // One grep-able line per cycle for battery-life tracking (BATLOG prefix).
  // vbat_mv is -1 if the PM1 I2C read itself failed, so a bad reading still
  // shows up as one line rather than a silently missing cycle.
  uint16_t battMv = 0;
  bool vbatOk = pm1.readVbat(&battMv) == M5PM1_OK;
  Serial.printf("BATLOG cycle=%lu level=%d%% vbat_mv=%d\n",
                (unsigned long)cycle, s.battery, vbatOk ? (int)battMv : -1);

  if (connectWifi(s.wifiReason, sizeof s.wifiReason)) {
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
    //
    // sntp_get_sync_status() is read-once: after returning COMPLETED it resets
    // itself to RESET (esp_sntp.h). So poll it exactly once per iteration and
    // keep the result -- a second call after the loop reads RESET even when the
    // sync just succeeded, which is what b9607a8 did (caught on hardware,
    // 2026-09-28: every good sync was taken for a failure).
    uint32_t sntpStart = millis();
    sntp_sync_status_t sntpStatus;
    while ((sntpStatus = sntp_get_sync_status()) != SNTP_SYNC_STATUS_COMPLETED &&
           millis() - sntpStart < 10000) {
      delay(50);
    }

    if (sntpStatus == SNTP_SYNC_STATUS_COMPLETED) {
      struct tm lt;
      if (getLocalTime(&lt, 1000)) {
        // The RX8130 alarm in sleepUntilNext() schedules by its own clock, so
        // keep it synced to NTP every cycle.
        M5.Rtc.setDateTime(&lt);
      } else {
        Serial.println("ntp failed");
      }
    } else {
      // No sync this cycle. The system clock is still the RTC seed, which
      // setSystemTimeFromRtc() builds by reading the RTC's local-time digits
      // as UTC (RTC_Class.cpp). Under TZ_STRING that seed reads back 7-8h
      // early, and writing it to the RTC above would corrupt the RTC by the
      // same amount. So: skip the write-back, stop SNTP so a late sync can't
      // move the clock mid-cycle, and switch to UTC0 so localtime() shows
      // the RTC's digits unchanged. NOAA (lst_ldt) and Open-Meteo
      // (timezone=auto) timestamps are local digits too, so parseLocal()/
      // parseIso() and s.now all stay in that one frame -- the same frame a
      // Wi-Fi-failure cycle already uses, since it never sets TZ at all.
      // Found by reading the code, 2026-09-24; not observed on hardware.
      sntp_stop();
      setenv("TZ", "UTC0", 1);
      tzset();
      Serial.println("ntp failed: not synced in 10s, using RTC time");
      s.fail |= FAIL_CLOCK;
    }

    bool tides = fetchTides(s);
    bool wind  = fetchWind(s);
    s.ok = tides || wind;
    // events counted separately from `tides`, which only reflects the hourly
    // curve fetch -- the hilo (next-event) fetch can silently fail on its own.
    Serial.printf("tides=%d wind=%d events=%d\n", tides, wind, s.nEvents);
  } else {
    Serial.println("wifi failed");
    s.fail |= FAIL_WIFI;  // s.wifiReason was filled in by connectWifi()
  }

  // Sampled once, after the Wi-Fi/SNTP attempt whether or not it succeeded,
  // so every draw function sees the same "now" instead of each calling
  // time(nullptr) independently (see Snapshot::now).
  s.now = time(nullptr);

  if (cycle % FULL_REFRESH_EVERY == 1) {
    clearScreenFull();           // ghost-clearing pass
  }

  drawAll(s);
  sleepUntilNext();
}

void loop() {}   // never reached; setup() ends in deep sleep

#endif  // TIER1_TEST
