// Host-side SDL preview -- see platformio.ini's [env:native] and CLAUDE.md's
// SDL-host-build entries. Links src/render.cpp (the same drawHeader()/
// drawNowStrip()/drawTide()/drawWind()/drawForecast()/drawFooter() the
// firmware uses) against M5GFX's own SDL backend, so layout iteration runs
// the real render path instead of tools/preview.py's separate PIL
// reimplementation of it. Modeled on M5GFX's examples/PlatformIO_SDL/.
//
// Usage: pio run -e native -t upload
//   Renders test/fixtures/example.json by default; set SDL_PREVIEW_FIXTURE
//   to render a different fixture (e.g. test/fixtures/high_wind.json).
//
//   Set SDL_PREVIEW_DUMP_RAW to a file path to additionally (or instead --
//   see below) write the raw canvas buffer (canvas.getBuffer(), the exact
//   same palette_4bit bytes main.cpp's tier1HandleRender() base64-encodes
//   over serial for hil.py) to that path and exit immediately, without
//   opening/waiting on the SDL window -- for scripted comparison against a
//   device dump via `python tools/hil.py decode-raw`, which decodes this
//   exact format with the exact same function hil.py already uses for the
//   real device's dump (decode_palette4()). See CLAUDE.md's SDL-host-build
//   entries for why byte-for-byte identity between the two is the point.

#include <M5GFX.h>
#if !defined(SDL_h_)
#error "src/sdl_main.cpp only builds against M5GFX's SDL backend -- see [env:native] in platformio.ini."
#endif

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "render.h"

namespace {

// Mirrors main.cpp's tier1ParseSnapshot() (TIER1_TEST build, same
// test/fixtures/*.json format) field-for-field. Duplicated here rather than
// shared because that function currently lives behind main.cpp's ESP32-only
// TIER1_TEST guard, alongside Arduino/USB-serial code this host build can't
// (and shouldn't need to) compile. If this preview grows beyond a
// single-shot render, pull both into one platform-independent translation
// unit instead of maintaining two copies of the parsing logic.
void parseSnapshot(const JsonDocument& doc, Snapshot& s) {
  // "GOLDEN GATE", not a generic placeholder -- matches config.h's
  // STATION_LABEL, which is what tier1ParseSnapshot() falls back to for
  // fixtures (like every one under test/fixtures/) that predate the
  // stationLabel field. This file intentionally doesn't include config.h
  // (keeps the host preview decoupled from real Wi-Fi credentials), so the
  // value is duplicated here rather than shared -- confirmed as a real
  // mismatch (rendered "STATION" against a golden showing "GOLDEN GATE")
  // via `hil.py decode-raw`, 2026-09-16.
  const char* label = doc["stationLabel"] | "GOLDEN GATE";
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
  for (JsonVariantConst v : doc["fail"].as<JsonArrayConst>()) {
    const char* name = v | "";
    uint16_t flag = failFlagFromName(name);
    if (!flag) fprintf(stderr, "SDLPREVIEW: unknown fail name '%s'\n", name);
    s.fail |= flag;
  }
  snprintf(s.wifiReason, sizeof s.wifiReason, "%s",
           doc["wifiReason"] | "");

  s.nTide = 0;
  for (JsonObjectConst p : doc["tide"].as<JsonArrayConst>()) {
    if (s.nTide >= 26) break;
    s.tide[s.nTide++] = { (time_t)(p["t"] | (int64_t)0), p["ft"] | 0.0f };
  }

  s.nEvents = 0;
  for (JsonObjectConst p : doc["events"].as<JsonArrayConst>()) {
    if (s.nEvents >= 10) break;
    const char* kind = p["kind"] | "?";
    s.events[s.nEvents++] = { (time_t)(p["t"] | (int64_t)0), p["ft"] | 0.0f, kind[0] };
  }

  s.nForecast = 0;
  for (JsonObjectConst p : doc["forecast"].as<JsonArrayConst>()) {
    if (s.nForecast >= 24) break;
    s.forecast[s.nForecast++] = { (time_t)(p["t"] | (int64_t)0), p["kt"] | 0.0f };
  }
}

bool loadFixture(const char* path, Snapshot& s) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    fprintf(stderr, "SDLPREVIEW: could not open fixture '%s'\n", path);
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, text);
  if (err) {
    fprintf(stderr, "SDLPREVIEW: JSON parse error in '%s': %s\n", path, err.c_str());
    return false;
  }
  parseSnapshot(doc, s);
  return true;
}

M5GFX gfx;
Snapshot snapshot;

}  // namespace

void setup() {
  // TIER1_TEST (the ESP32 firmware path test/golden/*.png was captured
  // against, see src/main.cpp) never calls configTzTime()/setenv() -- it
  // has no Wi-Fi/NTP path at all -- so render.cpp's localtime_r() calls run
  // there under ESP-IDF's unconfigured default, UTC. This host build's C
  // runtime instead defaults to whatever this machine's own OS timezone
  // is, which produced a render exactly 7h off from the golden (confirmed
  // via `hil.py decode-raw` on this Pacific-zoned dev machine: header
  // "05:00"/"08:38" here vs. golden's "12:00"/"15:38"). Force UTC
  // explicitly so both paths interpret Snapshot::now identically -- this
  // is NOT what production main.cpp does (it sets TZ_STRING, Pacific, once
  // NTP succeeds); it specifically matches TIER1_TEST's unconfigured
  // state, since that's what the goldens this preview is compared against
  // were rendered under. Confirmed live, 2026-09-16.
#if defined(_WIN32)
  _putenv("TZ=UTC0");  // mingw-w64 doesn't declare POSIX setenv() by default
  _tzset();
#else
  setenv("TZ", "UTC0", 1);
  tzset();
#endif

  gfx.init();
  // M5GFX's SDL board table sets board_M5PaperColor's rotation to 1
  // (confirmed by reading M5GFX.cpp's autodetect()), which swaps gfx's
  // logical width/height to 600x400 even though the window itself is
  // 400x600 -- diagnosed 2026-09-16 via gfx.width()/height() logging that
  // showed exactly that swap, which is what produced a blank window
  // (pushSprite() blitting our fixed 400x600 portrait canvas onto a parent
  // that thought it was 600 wide). Force portrait explicitly rather than
  // trust that table, since render.cpp's SCREEN_W/SCREEN_H (400x600, from
  // layout.h) aren't rotation-aware.
  gfx.setRotation(0);

  const char* fixture = "test/fixtures/example.json";
  if (const char* env = getenv("SDL_PREVIEW_FIXTURE")) fixture = env;

  if (!loadFixture(fixture, snapshot)) {
    fprintf(stderr, "SDLPREVIEW: rendering an empty Snapshot instead\n");
  }

  M5Canvas canvas(&gfx);
  if (!initCanvas(canvas)) {
    fprintf(stderr, "SDLPREVIEW: canvas allocation failed\n");
    return;
  }

  canvas.fillScreen(C_WHITE);
  drawHeader(canvas, snapshot);
  drawNowStrip(canvas, snapshot);
  drawTide(canvas, snapshot);
  drawWind(canvas, snapshot);
  drawForecast(canvas, snapshot);
  drawFooter(canvas, snapshot);

  if (const char* dumpPath = getenv("SDL_PREVIEW_DUMP_RAW")) {
    const uint8_t* buf = (const uint8_t*)canvas.getBuffer();
    const size_t len = canvas.bufferLength();
    std::ofstream out(dumpPath, std::ios::binary);
    if (!out) {
      fprintf(stderr, "SDLPREVIEW: could not open '%s' for writing\n", dumpPath);
      exit(1);
    }
    out.write(reinterpret_cast<const char*>(buf), (std::streamsize)len);
    if (!out) {
      fprintf(stderr, "SDLPREVIEW: write to '%s' failed\n", dumpPath);
      exit(1);
    }
    fprintf(stderr, "SDLPREVIEW: wrote %zu raw bytes to '%s' (%dx%d @4bpp)\n",
            len, dumpPath, canvas.width(), canvas.height());
    canvas.deleteSprite();
    // Scripted/automated use -- don't open or wait on the SDL window.
    exit(0);
  }

  canvas.pushSprite(0, 0);
  canvas.deleteSprite();
  gfx.display();

  fprintf(stderr, "SDLPREVIEW: rendered '%s' -- close the window to exit.\n", fixture);
}

void loop() {
  // One static render per run -- nothing to animate. Panel_sdl's own event
  // pump (driven from user_func() below, via lgfx::Panel_sdl::main()) keeps
  // the window responsive to close/resize until the user closes it.
}

__attribute__((weak))
int user_func(bool* running) {
  setup();
  do {
    loop();
  } while (*running);
  return 0;
}

int main(int, char**) {
  return lgfx::Panel_sdl::main(user_func, 32);
}
