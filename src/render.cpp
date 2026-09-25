#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr const uint32_t* PALETTE_RGB = layout::palette::RGB;

// render.cpp no longer includes Arduino.h itself, but on this ESP32 build
// M5GFX.h pulls it in transitively anyway (platforms/esp32/Bus_SPI.hpp needs
// Arduino's Stream class) -- so its DEG_TO_RAD macro is still live here even
// though this file doesn't ask for it. A constant of that exact name would
// get textually replaced by the macro before the compiler ever saw it, so
// this one is named kDegToRad to avoid the collision. Its value is copied
// verbatim from Arduino.h's own definition so the numeric output (which
// feeds cosf/sinf and gets truncated to pixel coordinates) stays bit-for-bit
// identical to the pre-refactor build. A future SDL host build wouldn't
// compile M5GFX's ESP32 backend at all, so Arduino.h (and this whole
// problem) wouldn't be in its include graph either.
constexpr float kDegToRad = 0.017453292519943295769236907684886f;
constexpr float toRadians(float deg) { return deg * kDegToRad; }

template <typename T>
constexpr T clampT(T amt, T lo, T hi) {
  return amt < lo ? lo : (amt > hi ? hi : amt);
}

// FAIL_* bit, its fixture JSON name, and its header line-2 token (nullptr =
// never listed there). Table order is the line-2 token order: CLOCK first,
// since it qualifies every time on screen, then top to bottom as the
// sections appear -- tide level and high/low (now strip), tide curve, wind,
// forecast.
struct FailInfo { uint16_t flag; const char* json; const char* token; };
constexpr FailInfo FAIL_INFO[] = {
  { FAIL_WIFI,       "wifi",       nullptr },
  { FAIL_CLOCK,      "clock",      "CLOCK" },
  { FAIL_TIDE_NOW,   "tide_now",   "LEVEL" },
  { FAIL_HILO,       "hilo",       "HILO"  },
  { FAIL_TIDE_CURVE, "tide_curve", "TIDE"  },
  { FAIL_WIND_NOW,   "wind_now",   "WIND"  },
  { FAIL_FORECAST,   "forecast",   "FCST"  },
  { FAIL_INDOOR,     "indoor",     nullptr },
};
constexpr uint16_t FAIL_ALL_DATA = FAIL_TIDE_NOW | FAIL_HILO | FAIL_TIDE_CURVE |
                                   FAIL_WIND_NOW | FAIL_FORECAST;

// True if this data's source failed. FAIL_WIFI means nothing was fetched.
bool missing(const Snapshot& s, uint16_t flag) {
  return s.fail & (flag | FAIL_WIFI);
}

// Header line 2 on a failed cycle: "<when>  <detail>", or "<when>  MISSING"
// followed by the first `keep` tokens and "+N" for any dropped.
void composeLine2(char* out, size_t size, const char* when, const char* detail,
                  const char* const* tokens, int nTokens, int keep) {
  int n = snprintf(out, size, "%s  %s", when, detail ? detail : "MISSING");
  for (int i = 0; i < keep; i++) {
    n += snprintf(out + n, size - n, " %s", tokens[i]);
  }
  if (keep < nTokens) snprintf(out + n, size - n, " +%d", nTokens - keep);
}

// Draws `text` centered in the given box, in the current font and color.
void drawBoxMessage(M5Canvas& gfx, const char* text,
                    int x0, int y0, int x1, int y1) {
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.drawString(text, (x0 + x1) / 2, (y0 + y1) / 2);
  gfx.setTextDatum(textdatum_t::top_left);
}

}  // namespace

uint16_t failFlagFromName(const char* name) {
  if (!name) return 0;
  for (const FailInfo& f : FAIL_INFO) {
    if (strcmp(name, f.json) == 0) return f.flag;
  }
  return 0;
}

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
//
// On a failed cycle the header is the glance cue: red when there is no
// current data at all (NO WIFI, or NO DATA when every fetch failed), yellow
// when some is missing (PARTIAL DATA, or NO TIME SYNC when only the clock
// sync failed). The title replaces the station name. Line 2 is the date/time
// plus a detail: the Wi-Fi reason, "ALL FETCHES FAILED", or "MISSING" and one
// token per missing source, in FAIL_INFO order. If line 2 is too wide, the
// date is dropped (time only); if still too wide, trailing tokens are
// replaced by "+N" (the number dropped).
void drawHeader(M5Canvas& gfx, const Snapshot& s) {
  using namespace layout::header;
  uint32_t bg = C_BLUE, fg = C_WHITE;
  const char* title = s.stationLabel;
  const char* detail = nullptr;             // fixed line-2 detail, or...
  const char* tokens[8]; int nTokens = 0;   // ...a "MISSING" token list
  if (s.fail & FAIL_WIFI) {
    bg = C_RED;
    title = "NO WIFI";
    detail = s.wifiReason[0] ? s.wifiReason : "TIMEOUT";
  } else if ((s.fail & FAIL_ALL_DATA) == FAIL_ALL_DATA) {
    bg = C_RED;
    title = "NO DATA";
    detail = "ALL FETCHES FAILED";
  } else if (s.fail & (FAIL_ALL_DATA | FAIL_CLOCK)) {
    bg = C_YELLOW; fg = C_BLACK;
    if (s.fail & FAIL_ALL_DATA) {
      title = "PARTIAL DATA";
      for (const FailInfo& f : FAIL_INFO) {
        if (f.token && (s.fail & f.flag)) tokens[nTokens++] = f.token;
      }
    } else {
      title = "NO TIME SYNC";
    }
  }

  gfx.fillRect(0, 0, SCREEN_W, HEIGHT, bg);
  gfx.setTextColor(fg, bg);
  gfx.setTextSize(1);

  gfx.setFont(&fonts::FreeSansBold12pt7b);  // F_BIG equivalent
  gfx.setCursor(STATION_X, STATION_Y);
  gfx.print(title);

  char buf[96];
  struct tm lt; localtime_r(&s.now, &lt);
  strftime(buf, sizeof buf, "%a %d %b  %H:%M", &lt);
  gfx.setFont(&fonts::DejaVu12);  // F_SMALL equivalent
  if (detail || nTokens) {
    const int maxW = SCREEN_W - 2 * DATETIME_X;
    char date[24], hm[8];
    snprintf(date, sizeof date, "%s", buf);
    strftime(hm, sizeof hm, "%H:%M", &lt);
    // Full date with every token, then time only with every token, then
    // time only with fewer tokens. If nothing fits, buf keeps the last
    // (shortest) form tried.
    bool fits = false;
    for (int keep = nTokens; keep >= 0 && !fits; keep--) {
      const char* whens[2] = { date, hm };
      for (int w = (keep == nTokens ? 0 : 1); w < 2 && !fits; w++) {
        composeLine2(buf, sizeof buf, whens[w], detail, tokens, nTokens, keep);
        fits = gfx.textWidth(buf) <= maxW;
      }
    }
  }
  gfx.setCursor(DATETIME_X, DATETIME_Y);
  gfx.print(buf);

  if (s.fail & FAIL_INDOOR) {
    snprintf(buf, sizeof buf, "--C --%%  BAT %d%%", s.battery);
  } else {
    snprintf(buf, sizeof buf, "%.0fC %.0f%%  BAT %d%%",
             s.indoorC, s.indoorRh, s.battery);
  }
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
  if (missing(s, FAIL_TIDE_NOW)) {
    snprintf(buf, sizeof buf, "--");
  } else {
    snprintf(buf, sizeof buf, "%.1f", s.tideNow);
  }
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
  if (s.nTide < 2) {
    gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
    drawBoxMessage(gfx, "NO TIDE DATA", x0, y0, x1, y1);
    return;
  }

  float lo = s.tide[0].ft, hi = s.tide[0].ft;
  for (int i = 1; i < s.nTide; i++) {
    lo = std::min(lo, s.tide[i].ft);
    hi = std::max(hi, s.tide[i].ft);
  }
  float pad = std::max(0.4f, (hi - lo) * 0.15f);
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

  int nx = px(clampT(s.now, t0, t1));
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
    float la = toRadians(i * 90.0f - 90.0f);
    gfx.drawString(COMPASS_LABELS[i], cx + int(cosf(la) * lr), cy + int(sinf(la) * lr));
  }
  gfx.setTextDatum(textdatum_t::top_left);

  // No current wind: the ring and N/E/S/W labels stay, but no arrow, "--"
  // for every value, and only the chip's outline -- nothing that reads as a
  // real reading.
  const bool noWind = missing(s, FAIL_WIND_NOW);
  char buf[48];
  using namespace layout::firmware_only::wind;
  if (noWind) {
    gfx.setFont(&fonts::FreeSansBold18pt7b);  // F_HUGE equivalent
    gfx.setCursor(READING_X, wy + READING_DY);
    gfx.print("--");
    int numW = gfx.textWidth("--");
    gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
    gfx.setCursor(READING_X + numW + KT_GAP, wy + KT_DY);
    gfx.print("kt");
    gfx.setFont(&fonts::DejaVu18);  // F_REG equivalent
    gfx.setCursor(GUST_X, wy + GUST_DY); gfx.print("GUST --");
    gfx.setCursor(FROM_X, wy + FROM_DY); gfx.print("FROM --");
    gfx.drawRect(SCREEN_W - CHIP_RIGHT_OFFSET, wy + CHIP_DY, CHIP_W, CHIP_H, C_BLACK);
    return;
  }

  float a = toRadians(s.windDir + 180 - 90);
  int tipx = cx + cosf(a) * (r - ARROW_TIP_INSET), tipy = cy + sinf(a) * (r - ARROW_TIP_INSET);
  int tlx  = cx - cosf(a) * (r - ARROW_TAIL_INSET), tly = cy - sinf(a) * (r - ARROW_TAIL_INSET);
  uint32_t ac = windColor(s.windNow);
  float barbAngle = toRadians(float(ARROW_BARB_ANGLE_DEG));
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

  if (!s.nForecast) {
    gfx.setFont(&fonts::FreeSansBold9pt7b);  // F_MED equivalent
    drawBoxMessage(gfx, "NO FORECAST", x0, y0, x1, y1);
    return;
  }

  float peak = 20.0f;
  for (int i = 0; i < s.nForecast; i++) peak = std::max(peak, s.forecast[i].kt);

  int bw = (x1 - x0) / s.nForecast;
  gfx.setFont(&fonts::DejaVu9);  // F_TINY equivalent
  for (int i = 0; i < s.nForecast; i++) {
    float v = std::max(0.0f, s.forecast[i].kt);
    int h = std::max(BAR_MIN_HEIGHT, int(v / peak * (y1 - y0 - BAR_HEIGHT_MARGIN)));
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
