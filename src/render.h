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
  // drawHeader()'s station name. Was the STATION_LABEL macro from config.h
  // read directly by drawHeader() before the render.h/render.cpp split
  // (2026-09-13) -- moved onto Snapshot so drawHeader() stays a pure
  // function of Snapshot (matching the `now` field below) instead of
  // render.cpp needing config.h, which also holds live Wi-Fi credentials.
  // Populated from STATION_LABEL by main.cpp's setup() and, with the same
  // fallback, by tier1ParseSnapshot() when a fixture JSON omits it.
  char  stationLabel[32] = "";
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
