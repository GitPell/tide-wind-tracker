# Render Module Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `Snapshot` (and its `TidePoint`/`TideEvent`/`WindPoint` field types), the six palette constants, `compass()`, `windColor()`, `initCanvas()`, and the six `draw*()` functions out of `src/main.cpp` into a new `src/render.h` / `src/render.cpp` pair that includes no `Arduino.h`, `M5Unified.h`, or network headers — so the same render code can eventually compile into both the ESP32 firmware and a future SDL host build.

**Architecture:** Two-stage extraction. Task 1 moves the code mechanically (bodies untouched, `Arduino.h` temporarily still included in `render.cpp`) so a reviewer can confirm the move itself introduced no logic change. Task 2 removes `Arduino.h` and replaces the four Arduino macros the moved code actually depends on (`radians()`, `constrain()`, `min()`, `max()`) with portable equivalents that reproduce Arduino's exact numeric behavior — this is the only place semantics could shift, so it's isolated in its own reviewable diff. Task 3 records status and open verification gaps in `CLAUDE.md`, matching this project's existing documentation convention.

**Tech Stack:** C++ (Arduino framework / ESP32-S3, `espressif32` platform), M5GFX (`M5Canvas`), PlatformIO.

**Spec:** This plan's spec is the user's request in-conversation (no separate spec doc): pure refactor, pixel-identical output, host-side compilation checks only, both PlatformIO envs must still build. The known portability obstacles are pre-recorded in `CLAUDE.md`'s "A host-side SDL renderer..." entry (2026-09-13) — this plan fixes exactly the four macros that entry names and nothing more.

## Amendments (recorded during execution)

- **2026-09-13, during Task 2: renamed the `DEG_TO_RAD` constant to `kDegToRad`.** `render.cpp` dropping its own `#include <Arduino.h>` doesn't actually remove `Arduino.h` from this file's translation unit on the ESP32 build: `M5GFX.h` → `lgfx/v1/platforms/esp32/Bus_SPI.hpp` → `misc/datawrapper.hpp` transitively includes it (M5GFX's ESP32 backend needs Arduino's `Stream` class), so its macros — confirmed by reading `Arduino.h`: `constrain`, `radians`, and `DEG_TO_RAD` are `#define`s; `min`/`max` deliberately are not ("can't define max() / min() because of conflicts with C++") — are still live in this file regardless. A `constexpr float DEG_TO_RAD = ...` of the same name got textually replaced by the macro before the compiler parsed it (`error: expected unqualified-id before numeric constant`). Renamed to `kDegToRad`; value unchanged. This doesn't change what Task 2 accomplishes (`render.cpp`'s own code no longer *calls* any Arduino macro, which is what makes it portable to a build where `Arduino.h` genuinely isn't present, like a future SDL host build that never compiles M5GFX's ESP32 backend at all) — it just means "no Arduino.h in the include graph" isn't literally true for the ESP32 target specifically, only for the code this module itself writes.
- **2026-09-13, during Task 1: `Snapshot` gained a `stationLabel` field — not part of the original "mechanical move only" scope.** `drawHeader()` turned out to read `STATION_LABEL`, a `#define` in `src/config.h` (the real, gitignored local config file, which also holds the live `WIFI_SSID`/`WIFI_PASS`) — not, as assumed when this plan was written, a `layout::header` constant. Including `config.h` from `render.cpp` to fix the resulting compile error would have made the "pure, portable render module" transitively depend on a file containing Wi-Fi credentials just to draw a text label, defeating the point of the extraction. Per the user's decision: added `char stationLabel[32] = "";` to `Snapshot` (`render.h`), populated it in `main.cpp`'s production `setup()` from `STATION_LABEL` (same as every other `Snapshot` field main.cpp already fills in), and in `tier1ParseSnapshot()` from an optional `"stationLabel"` JSON key that **defaults to `STATION_LABEL`** when absent — deliberately so the 5 existing fixtures (which predate this field and were never updated to include it) parse to the exact value they always rendered with, keeping their committed goldens valid without regenerating anything. This makes `drawHeader()` a true pure function of `Snapshot`, consistent with `CLAUDE.md`'s existing `Snapshot::now` precedent, but it is a real, if small, semantic addition — not a pure code relocation like the rest of Task 1 — so it's called out here rather than folded silently into "mechanical move only."

## Global Constraints

- **Pure refactor.** No behavior change. Every moved function body must be textually identical to its pre-move source except for the macro replacements in Task 2, which must reproduce Arduino's exact numeric output (see Task 2's `DEG_TO_RAD` note).
- **Device is off-limits.** A battery soak test is running (cycle 289+, started 2026-09-11). No `pio run -t upload`, no `tools/hil.py`, no `pio device monitor`, no opening the serial port, for the whole plan.
- **"Verify" means `pio run` only** (no `-t upload`) — compiles for the ESP32 target on this machine without touching the board, per `CLAUDE.md`'s current-state note.
- **Both PlatformIO envs must build**: `pio run -e m5stack-papercolor` and `pio run -e m5stack-papercolor-test`.
- **`src/render.h` and `src/render.cpp`'s final state must contain no `Arduino.h`, `M5Unified.h`, `WiFi.h`, `WiFiClientSecure.h`, or `HTTPClient.h` includes.** `M5GFX.h` is required and fine — it's the library the SDL backend already vendors (per `CLAUDE.md`), not one of the excluded ones.
- **`std::min`/`std::max` are typed; Arduino's `min()`/`max()` macros are not.** A macro call site with mixed `int`/`float` arguments compiles silently (with C's usual arithmetic conversion); the equivalent `std::min<T>`/`std::max<T>` call does not compile unless both arguments are the same `T`. If Task 2 finds a call site like that, **do not add a cast to make it compile** — a cast changes which value gets rounded and when, which can shift a truncated pixel coordinate by one, which is exactly the kind of change this pure refactor must not make. Stop and report the call site to the user instead; resolving it is a design decision (which side to widen, if any), not a mechanical one.
- **Don't invent new portability fixes.** `CLAUDE.md` documents exactly one class of obstacle for this code (`radians()`/`constrain()`/`min()`/`max()` are Arduino.h macros). Fix only that. Anything else the eventual SDL build turns up (e.g. `localtime_r` availability under MSYS2/mingw) is undiscovered without a host toolchain — this machine has none installed (confirmed 2026-09-13: no `g++`/`gcc`/`cl`/`clang++` on PATH), so don't guess at fixes for problems that haven't been observed.
- **Golden-image verification is out of scope for this plan's own completion.** `test/golden/*.png` (5 fixtures: `calm`, `example`, `high_wind`, `long_station`, `no_events`) can only be checked with `tools/hil.py test --all` against real hardware, which is frozen for the soak test. This plan's own exit criterion is "compiles identically on both envs, code review confirms no logic changed" — the golden diff is a follow-up the user runs once the board is available again.

---

## File Structure

```
src/render.h     NEW — Snapshot/TidePoint/TideEvent/WindPoint, palette
                 constants, SCREEN_W/SCREEN_H, and declarations for
                 compass(), windColor(), initCanvas(), and the six
                 draw*() functions. Includes only <M5GFX.h>, "layout.h",
                 and <ctime>.
src/render.cpp   NEW — definitions of everything render.h declares.
                 Task 1: still includes <Arduino.h> for the four macros.
                 Task 2: Arduino.h removed, macros replaced locally.
src/main.cpp     MODIFIED — the moved code deleted; #include "render.h"
                 added; drawAll(), clearScreenFull(), the TIER1_TEST
                 harness, fetch*(), connectWifi(), sleepUntilNext(),
                 setup()/loop() all stay here unchanged (they need
                 M5Unified/network and are out of this plan's scope).
CLAUDE.md        MODIFIED (Task 3) — status note on what's verified vs.
                 still pending.
```

---

### Task 1: Mechanical extraction (no logic changes)

**Files:**
- Create: `src/render.h`
- Create: `src/render.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Produces (for Task 2 and for `main.cpp`): `struct TidePoint { time_t t; float ft; };`, `struct TideEvent { time_t t; float ft; char kind; };`, `struct WindPoint { time_t t; float kt; };`, `struct Snapshot { ... }` (fields unchanged from current `main.cpp`), `constexpr uint32_t C_BLACK/C_WHITE/C_RED/C_YELLOW/C_BLUE/C_GREEN`, `constexpr int SCREEN_W/SCREEN_H`, `const char* compass(int deg)`, `uint32_t windColor(float kt)`, `bool initCanvas(M5Canvas& canvas)`, `void drawHeader/drawNowStrip/drawTide/drawWind/drawForecast/drawFooter(M5Canvas& gfx, const Snapshot& s)`.

- [x] **Step 1: Create `src/render.h`**

```cpp
#pragma once

// Render module: the pure drawing surface shared by the firmware build and
// a future SDL host build (see CLAUDE.md's SDL-host-build entry). No
// Arduino.h, M5Unified.h, or network includes here or in render.cpp --
// M5GFX itself already vendors a working SDL backend and doesn't need
// either of those to draw.
#include <M5GFX.h>

#include <ctime>

#include "layout.h"

// ---------------------------------------------------------------- palette ---
// M5Canvas palette INDICES (0-5), not RGB values -- the render target is an
// M5Canvas at color_depth_t::palette_4bit (see initCanvas()). Index order is
// fixed by palette.json; the actual RGB888 bytes live in
// layout::palette::RGB, consumed only inside initCanvas().
static constexpr uint32_t C_BLACK  = 0;
static constexpr uint32_t C_WHITE  = 1;
static constexpr uint32_t C_RED    = 2;
static constexpr uint32_t C_YELLOW = 3;
static constexpr uint32_t C_BLUE   = 4;
static constexpr uint32_t C_GREEN  = 5;

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
  // Sampled once in setup(), after the Wi-Fi/SNTP attempt (whether or not it
  // succeeded) and before any drawing -- every draw function reads this
  // instead of calling time(nullptr) itself, so the render is a pure
  // function of Snapshot. See CLAUDE.md's "Snapshot carries a time_t now
  // field" entry.
  time_t now = 0;
};

// ------------------------------------------------------------- render API ---
const char* compass(int deg);
uint32_t windColor(float kt);

bool initCanvas(M5Canvas& canvas);

void drawHeader(M5Canvas& gfx, const Snapshot& s);
void drawNowStrip(M5Canvas& gfx, const Snapshot& s);
void drawTide(M5Canvas& gfx, const Snapshot& s);
void drawWind(M5Canvas& gfx, const Snapshot& s);
void drawForecast(M5Canvas& gfx, const Snapshot& s);
void drawFooter(M5Canvas& gfx, const Snapshot& s);
```

- [x] **Step 2: Create `src/render.cpp`** (bodies copied verbatim from the current `src/main.cpp`; `Arduino.h` kept for now so `radians()`/`constrain()`/`min()`/`max()` still resolve — removed in Task 2)

```cpp
#include "render.h"

// TEMPORARY (removed in Task 2): still needed for radians()/constrain()/
// min()/max(), which the moved draw functions use. Task 2 replaces all four
// with portable equivalents and drops this include.
#include <Arduino.h>

namespace {
constexpr const uint32_t* PALETTE_RGB = layout::palette::RGB;
}  // namespace

const char* compass(int deg) {
  static const char* p[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                              "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  return p[int((deg % 360) / 22.5f + 0.5f) % 16];
}

uint32_t windColor(float kt) {
  if (kt < 10) return C_GREEN;
  if (kt < 20) return C_YELLOW;
  return C_RED;
}

// Allocates and palettes the off-screen canvas shared by drawAll() and the
// ghost-clearing pass. M5Canvas(&M5.Display) defaults _psram = true (see
// M5GFX.h), so this comes out of the 8MB PSRAM, not the ~320KB internal
// heap.
bool initCanvas(M5Canvas& canvas) {
  canvas.setColorDepth(lgfx::color_depth_t::palette_4bit);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) return false;
  canvas.createPalette(PALETTE_RGB, 6);
  return true;
}

// Font mapping from tools/preview.py's PIL fonts to bundled M5GFX fonts --
// see CLAUDE.md's "M5GFX ships no bold DejaVu" entry for the full table.
void drawHeader(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::header;
  gfx.fillRect(0, 0, SCREEN_W, HEIGHT, C_BLUE);
  gfx.setTextColor(C_WHITE, C_BLUE);
  gfx.setTextSize(1);

  gfx.setFont(&fonts::FreeSansBold12pt7b);  // F_BIG equivalent
  gfx.setCursor(STATION_X, STATION_Y);
  gfx.print(STATION_LABEL);

  char buf[40];
  struct tm lt; localtime_r(&s.now, &lt);
  strftime(buf, sizeof buf, "%a %d %b  %H:%M", &lt);
  gfx.setFont(&fonts::DejaVu12);  // F_SMALL equivalent
  gfx.setCursor(DATETIME_X, DATETIME_Y);
  gfx.print(buf);

  snprintf(buf, sizeof buf, "%.0fC %.0f%%  BAT %d%%",
           s.indoorC, s.indoorRh, s.battery);
  using namespace layout::firmware_only::header;
  gfx.setCursor(SCREEN_W - READOUT_RIGHT_OFFSET, READOUT_Y);
  gfx.print(buf);
}

// Side of the trend triangle in drawNowStrip(). Hand-drawn rather than a
// text glyph because the bundled GFXfont charsets are ASCII-only.
static constexpr int TREND_ARROW_SIZE = 20;

void drawNowStrip(M5Canvas& gfx, const Snapshot& s) {
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

  const TideEvent* nxt = nullptr;
  for (int i = 0; i < s.nEvents; i++) {
    if (s.events[i].t > s.now) { nxt = &s.events[i]; break; }
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

void drawTide(M5Canvas& gfx, const Snapshot& s) {
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

  int nx = px(constrain(s.now, t0, t1));
  for (int i = 0; i < NOW_LINE_WIDTH; i++) {
    gfx.drawFastVLine(nx + i, y0, y1 - y0, C_RED);
  }
}

void drawWind(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::wind;
  const int wy = Y;
  gfx.setTextColor(C_BLACK, C_WHITE);
  gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
  gfx.setCursor(LABEL_X, wy);
  gfx.print("WIND");

  const int cx = COMPASS_CX, cy = wy + COMPASS_DY, r = COMPASS_R;
  gfx.drawCircle(cx, cy, r, C_BLACK);

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
  float barbAngle = radians(float(ARROW_BARB_ANGLE_DEG));
  int barbAx = tipx + int(cosf(a + barbAngle) * ARROW_BARB_LENGTH);
  int barbAy = tipy + int(sinf(a + barbAngle) * ARROW_BARB_LENGTH);
  int barbBx = tipx + int(cosf(a - barbAngle) * ARROW_BARB_LENGTH);
  int barbBy = tipy + int(sinf(a - barbAngle) * ARROW_BARB_LENGTH);
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
  int fromW = gfx.textWidth(buf);
  static constexpr int DEG_RADIUS = 2, DEG_GAP = 2, DEG_Y_OFFSET = 3;
  gfx.drawCircle(FROM_X + fromW + DEG_GAP + DEG_RADIUS,
                 wy + FROM_DY + DEG_Y_OFFSET + DEG_RADIUS, DEG_RADIUS, C_BLACK);

  gfx.fillRect(SCREEN_W - CHIP_RIGHT_OFFSET, wy + CHIP_DY, CHIP_W, CHIP_H, ac);
  gfx.drawRect(SCREEN_W - CHIP_RIGHT_OFFSET, wy + CHIP_DY, CHIP_W, CHIP_H, C_BLACK);
}

void drawForecast(M5Canvas& gfx, const Snapshot& s) {
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

void drawFooter(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::footer;
  gfx.setTextColor(C_BLACK, C_WHITE);
  gfx.setFont(&fonts::DejaVu9);  // F_TINY equivalent
  gfx.setCursor(LEFT_X, Y);
  gfx.print("NOAA CO-OPS - Open-Meteo");

  struct tm lt; localtime_r(&s.now, &lt);
  char buf[16];
  strftime(buf, sizeof buf, "UPD %H:%M", &lt);
  gfx.drawRightString(buf, SCREEN_W - RIGHT_MARGIN, Y);
}
```

- [x] **Step 3: Edit `src/main.cpp`**

Remove the `#include "layout.h"` line (now pulled in transitively via `render.h`; safe to drop since nothing else in `main.cpp` references `layout::` directly outside the moved code) and add `#include "render.h"` right after it:

```cpp
#include "config.h"
#include "render.h"
```

Delete these now-duplicated blocks entirely (they moved to `render.h`/`render.cpp`):
- The `// ---------------------------------------------------------------- palette ---` comment block through `static constexpr int SCREEN_H = layout::screen::H;` (the six `C_*` constants, `PALETTE_RGB`, `SCREEN_W`, `SCREEN_H`).
- The `// ------------------------------------------------------------------- data ---` comment through the closing `};` of `struct Snapshot` (i.e. `TidePoint`, `TideEvent`, `WindPoint`, `Snapshot`).
- `compass()` and `windColor()`.
- Everything from the `// ------------------------------------------------------------------- draw ---` comment through the end of `initCanvas()` (i.e. `drawHeader`, the `TREND_ARROW_SIZE` constant, `drawNowStrip`, `drawTide`, `drawWind`, `drawForecast`, `drawFooter`, `initCanvas`).

Everything else in `main.cpp` — `nextCycleCount()`, `parseLocal()`/`parseIso()`, `readSht40()`, the HTTP/fetch functions, `clearScreenFull()`, `drawAll()`, the whole `TIER1_TEST` block, `connectWifi()`, `sleepUntilNext()`, `setup()`/`loop()` — is unchanged; they already only call into the moved symbols by name, which now resolve via `render.h`.

- [x] **Step 4: Build both PlatformIO envs**

Run: `pio run -e m5stack-papercolor`
Expected: `SUCCESS`, no errors or warnings about undefined symbols.

Run: `pio run -e m5stack-papercolor-test`
Expected: `SUCCESS`.

- [x] **Step 5: Confirm the move introduced no logic change**

Run: `git diff HEAD -- src/main.cpp` and read it top to bottom — every hunk should be a pure deletion (the code that moved), with no lines inside `main.cpp`'s surviving functions changed.

Then compare each function in `src/render.cpp` against the deleted `main.cpp` version (e.g. `git show HEAD:src/main.cpp | sed -n '318,583p'` alongside the new file) to confirm the bodies are character-for-character identical apart from the temporary `Arduino.h` include and dropping the `static` on `compass()`/`windColor()`/`initCanvas()` (they need external linkage now that they're declared in `render.h` and called from `main.cpp`).

- [x] **Step 6: Commit**

```bash
git add src/render.h src/render.cpp src/main.cpp
git commit -m "$(cat <<'EOF'
Extract render module (render.h/render.cpp) from main.cpp

Mechanical move only -- Snapshot, the palette constants, compass(),
windColor(), initCanvas(), and the six draw*() functions now live in
render.h/render.cpp. Arduino.h is still included in render.cpp for now
(next commit removes it); no drawing logic changed.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Drop Arduino.h; replace radians()/constrain()/min()/max()

**Files:**
- Modify: `src/render.cpp`

**Interfaces:**
- Consumes: everything Task 1 put in `render.h` (unchanged).
- Produces: `render.cpp` with zero Arduino/M5Unified/network includes — the state Task 3 documents as done.

- [x] **Step 1: Audit every `min()`/`max()`/`constrain()` call site for type mismatches before touching any of them**

Arduino's `min()`/`max()`/`constrain()` are untyped macros — they compile regardless of whether the arguments match. `std::min`/`std::max`/the `clampT<T>` template this task introduces all require both arguments to already be the same type. Casting one side to make it compile is exactly the kind of change this refactor must not make (a cast can move a rounding boundary, which can move a pixel) — so this step's job is to confirm, for each site, that both arguments are *already* the same type in the moved code, using the actual declared types, not by inspection alone.

There are 7 sites in `render.cpp` after Task 1 (`drawTide`: 3x `min`/`max` + 1x `constrain`; `drawForecast`: 3x `max`). For each, check the declared type of both arguments:

| Site | Arg A | Arg A type | Arg B | Arg B type |
|---|---|---|---|---|
| `drawTide`: `min(lo, s.tide[i].ft)` | `lo` | `float` (`float lo = s.tide[0].ft;`) | `s.tide[i].ft` | `float` (`TidePoint::ft`) |
| `drawTide`: `max(hi, s.tide[i].ft)` | `hi` | `float` | `s.tide[i].ft` | `float` |
| `drawTide`: `max(0.4f, (hi - lo) * 0.15f)` | `0.4f` | `float` literal | `(hi - lo) * 0.15f` | `float` expression |
| `drawTide`: `constrain(s.now, t0, t1)` | `s.now`, `t0`, `t1` | all `time_t` | — | — |
| `drawForecast`: `max(peak, s.forecast[i].kt)` | `peak` | `float` (`float peak = 20.0f;`) | `s.forecast[i].kt` | `float` (`WindPoint::kt`) |
| `drawForecast`: `max(0.0f, s.forecast[i].kt)` | `0.0f` | `float` literal | `s.forecast[i].kt` | `float` |
| `drawForecast`: `max(BAR_MIN_HEIGHT, int(...))` | `BAR_MIN_HEIGHT` | `int` | `int(v / peak * ...)` | `int` (explicit cast already present in the pre-refactor code, not one this task adds) |

`BAR_MIN_HEIGHT`'s type isn't a guess: `tools/gen_layout_header.py` (the script that generates `layout.h`) only ever emits `constexpr int` for layout values and raises `TypeError: layout.json: ... is non-integer; layout.h only emits ints` if a value isn't a whole number — confirm this still holds by checking the generator hasn't changed: `grep -n "layout.h only emits ints" tools/gen_layout_header.py` should still find that line. Combined with `TidePoint::ft`, `WindPoint::kt`, and `time_t` fields being fixed by `render.h`'s struct definitions (Task 1, unchanged), all 7 sites are same-typed on both sides.

If re-reading `render.cpp` at execution time shows a site whose two arguments are **not** the same type (e.g. a future layout or struct change introduced one), **stop before writing any substitution for that site and report it** — name the file, line, both arguments, and both their types — rather than adding a cast to make `std::min`/`std::max`/`clampT` compile.

- [x] **Step 2: Replace the include block**

Before:
```cpp
#include "render.h"

// TEMPORARY (removed in Task 2): still needed for radians()/constrain()/
// min()/max(), which the moved draw functions use. Task 2 replaces all four
// with portable equivalents and drops this include.
#include <Arduino.h>

namespace {
constexpr const uint32_t* PALETTE_RGB = layout::palette::RGB;
}  // namespace
```

After:
```cpp
#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr const uint32_t* PALETTE_RGB = layout::palette::RGB;

// Arduino.h's radians()/constrain()/min()/max() macros aren't available
// here -- render.h/.cpp intentionally exclude Arduino.h so this module can
// also compile for a future SDL host build (see CLAUDE.md's SDL entry).
// DEG_TO_RAD is copied verbatim from Arduino.h's own definition so the
// numeric output (which feeds cosf/sinf and gets truncated to pixel
// coordinates) stays bit-for-bit identical to the pre-refactor build.
constexpr float DEG_TO_RAD = 0.017453292519943295769236907684886f;
constexpr float toRadians(float deg) { return deg * DEG_TO_RAD; }

template <typename T>
constexpr T clampT(T amt, T lo, T hi) {
  return amt < lo ? lo : (amt > hi ? hi : amt);
}

}  // namespace
```

- [x] **Step 3: Replace the four call sites in `drawTide()`**

Before:
```cpp
  float lo = s.tide[0].ft, hi = s.tide[0].ft;
  for (int i = 1; i < s.nTide; i++) {
    lo = min(lo, s.tide[i].ft);
    hi = max(hi, s.tide[i].ft);
  }
  float pad = max(0.4f, (hi - lo) * 0.15f);
```
After:
```cpp
  float lo = s.tide[0].ft, hi = s.tide[0].ft;
  for (int i = 1; i < s.nTide; i++) {
    lo = std::min(lo, s.tide[i].ft);
    hi = std::max(hi, s.tide[i].ft);
  }
  float pad = std::max(0.4f, (hi - lo) * 0.15f);
```

Before:
```cpp
  int nx = px(constrain(s.now, t0, t1));
```
After:
```cpp
  int nx = px(clampT(s.now, t0, t1));
```

- [x] **Step 4: Replace the three `radians()` calls in `drawWind()`**

Before:
```cpp
    float la = radians(i * 90.0f - 90.0f);
```
After:
```cpp
    float la = toRadians(i * 90.0f - 90.0f);
```

Before:
```cpp
  float a = radians(s.windDir + 180 - 90);
```
After:
```cpp
  float a = toRadians(s.windDir + 180 - 90);
```

Before:
```cpp
  float barbAngle = radians(float(ARROW_BARB_ANGLE_DEG));
```
After:
```cpp
  float barbAngle = toRadians(float(ARROW_BARB_ANGLE_DEG));
```

- [x] **Step 5: Replace the three `max()` calls in `drawForecast()`**

Before:
```cpp
  for (int i = 0; i < s.nForecast; i++) peak = max(peak, s.forecast[i].kt);
```
After:
```cpp
  for (int i = 0; i < s.nForecast; i++) peak = std::max(peak, s.forecast[i].kt);
```

Before:
```cpp
    float v = max(0.0f, s.forecast[i].kt);
    int h = max(BAR_MIN_HEIGHT, int(v / peak * (y1 - y0 - BAR_HEIGHT_MARGIN)));
```
After:
```cpp
    float v = std::max(0.0f, s.forecast[i].kt);
    int h = std::max(BAR_MIN_HEIGHT, int(v / peak * (y1 - y0 - BAR_HEIGHT_MARGIN)));
```

- [x] **Step 6: Confirm no forbidden includes remain**

Run: `grep -n -E "Arduino\.h|M5Unified\.h|WiFi(ClientSecure)?\.h|HTTPClient\.h" src/render.h src/render.cpp`
Expected: no output (exit code 1 from grep).

- [x] **Step 7: Build both PlatformIO envs**

Run: `pio run -e m5stack-papercolor`
Expected: `SUCCESS`.

Run: `pio run -e m5stack-papercolor-test`
Expected: `SUCCESS`.

- [x] **Step 8: Commit**

```bash
git add src/render.cpp
git commit -m "$(cat <<'EOF'
Drop Arduino.h from render module

Replace the four Arduino.h macros the moved draw functions relied on
(radians(), constrain(), min(), max()) with local equivalents --
render.h/render.cpp now include only M5GFX.h, layout.h, and standard
headers, matching the no-Arduino/no-M5Unified/no-network constraint
needed for a future SDL host build. toRadians() copies Arduino's own
DEG_TO_RAD constant verbatim to keep numeric output unchanged.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Document status and open verification gaps

**Files:**
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: nothing code-level; this is a documentation-only task.

- [x] **Step 1: Add a bullet under "Not yet exercised"**

Insert this bullet into the "Not yet exercised" list in `CLAUDE.md` (after the existing battery-life bullet):

```markdown
- Pixel-identical output from the `src/render.h`/`src/render.cpp` extraction
  (2026-09-13): `Snapshot`, the palette constants, `compass()`,
  `windColor()`, `initCanvas()`, and the six `draw*()` functions now live in
  their own header/source pair with no `Arduino.h`/`M5Unified.h`/network
  includes -- both PlatformIO envs (`pio run`) build clean, and the move was
  reviewed line-for-line as a pure relocation (the only logic change, the
  `radians()`/`constrain()`/`min()`/`max()` macro replacements, reproduces
  Arduino's exact `DEG_TO_RAD` constant). **Not yet confirmed against real
  hardware**: `tools/hil.py test --all`'s 5 goldens (`calm`, `example`,
  `high_wind`, `long_station`, `no_events`) haven't been re-run -- blocked by
  the battery soak test (see top of this file). Also not yet attempted:
  actually compiling `render.cpp` into an SDL host build -- the toolchain it
  needs (MSYS2, gcc/g++, SDL2 dev headers, PlatformIO's `native` platform)
  is still not installed on this machine (confirmed absent again
  2026-09-13). Re-run the golden diff as the first thing once the soak test
  ends and the board is available.
```

- [x] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "$(cat <<'EOF'
Note render-module extraction status and pending verification

Both PlatformIO envs build after the render.h/render.cpp extraction, but
the golden-image diff (blocked by the battery soak test) and an actual
SDL host compile (blocked by the still-missing toolchain) are both still
unverified -- flag both explicitly so they aren't mistaken for done.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Verification Summary

**Can verify now, this session:**
- `pio run -e m5stack-papercolor` and `pio run -e m5stack-papercolor-test` both succeed after every task (compile-only, no upload — safe under the soak-test freeze).
- `grep` confirms `render.h`/`render.cpp` contain no `Arduino.h`/`M5Unified.h`/`WiFi*.h`/`HTTPClient.h` includes after Task 2.
- Manual/`git diff` review confirms Task 1 is a pure code move and Task 2's only semantic changes are the four documented macro replacements.

**Must wait for hardware (battery soak test to end):**
- `python tools/hil.py test --all` against all 5 committed goldens — the actual proof that output is pixel-identical, not just "compiles and looks the same on inspection."

**Must wait for toolchain installation (separate from the soak freeze — `CLAUDE.md`'s SDL "Next steps" item):**
- Actually compiling `render.cpp` as part of an SDL host build. No C or C++ compiler of any kind is currently on this machine's PATH (checked: `g++`, `gcc`, `cl`, `clang++` — none found), so this plan cannot attempt it, only clear the way for it.
