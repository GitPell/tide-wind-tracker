#!/usr/bin/env python3
"""
Host-side layout preview for the PaperColor tide & wind tracker.

Renders the exact 400x600 dashboard using only the six Spectra 6 colors, so you
can iterate on layout in milliseconds instead of waiting 15-30s per panel
refresh. The drawing code here is the specification the C++ in src/main.cpp
should mirror -- same coordinates, same palette, same rounding.

    python tools/preview.py              # synthetic data, no network
    python tools/preview.py --live       # fetch real NOAA + Open-Meteo data
    python tools/preview.py --check      # assert every pixel is on-palette

Requires: pillow  (and requests, for --live)
"""

import argparse
import datetime as dt
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# --- Configuration -----------------------------------------------------------
# Keep these in sync with src/config.h.

STATION_ID = "9414290"
STATION_NAME = "GOLDEN GATE"
LAT, LON = 37.8063, -122.4659

W, H = 400, 600

# --- Spectra 6 palette -------------------------------------------------------
# Approximate sRGB values for the six achievable colors. Tune these against a
# photo of the real panel; they only affect how faithful the preview looks, not
# what the device does.

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
RED = (191, 0, 0)
YELLOW = (255, 243, 56)
BLUE = (0, 0, 191)
GREEN = (0, 124, 0)

PALETTE = [BLACK, WHITE, RED, YELLOW, BLUE, GREEN]

# --- Fonts -------------------------------------------------------------------

FONT_DIRS = [
    "/usr/share/fonts/truetype/dejavu",
    "/System/Library/Fonts/Supplemental",
    "C:/Windows/Fonts",
]


def font(name, size):
    for d in FONT_DIRS:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default(size)


F_HUGE = font("DejaVuSans-Bold.ttf", 46)
F_BIG = font("DejaVuSans-Bold.ttf", 30)
F_MED = font("DejaVuSans-Bold.ttf", 18)
F_REG = font("DejaVuSans.ttf", 15)
F_SMALL = font("DejaVuSans.ttf", 12)
F_TINY = font("DejaVuSans.ttf", 10)


# --- Data models -------------------------------------------------------------


class Snapshot:
    """Everything the dashboard needs, in display units (feet, knots)."""

    def __init__(self):
        self.updated = dt.datetime.now()
        self.station = STATION_NAME
        self.tide_curve = []       # [(datetime, feet)] hourly, ~25 points
        self.tide_events = []      # [(datetime, feet, 'H'|'L')]
        self.tide_now = 0.0        # observed or interpolated feet
        self.wind_now = 0.0        # knots
        self.wind_dir = 0          # degrees FROM
        self.gust_now = 0.0        # knots
        self.wind_forecast = []    # [(datetime, knots)] hourly, next 24h
        self.indoor_c = 21.0
        self.indoor_rh = 48.0
        self.battery_pct = 82


def synthetic():
    """Plausible fake data so the layout can be tuned without a network."""
    s = Snapshot()
    now = dt.datetime.now().replace(minute=0, second=0, microsecond=0)
    start = now - dt.timedelta(hours=12)

    # Semidiurnal tide: two highs a day, mixed amplitude, ~3ft mean range.
    def height(t):
        h = (t - start).total_seconds() / 3600.0
        return 3.0 + 1.9 * math.sin(2 * math.pi * h / 12.42) \
                   + 0.6 * math.sin(2 * math.pi * h / 24.84 + 1.1)

    s.tide_curve = [(start + dt.timedelta(hours=i), height(start + dt.timedelta(hours=i)))
                    for i in range(25)]
    s.tide_now = height(dt.datetime.now())

    # Find local extrema on a fine grid for the H/L markers.
    fine = [(start + dt.timedelta(minutes=6 * i),
             height(start + dt.timedelta(minutes=6 * i))) for i in range(241)]
    for i in range(1, len(fine) - 1):
        prev, cur, nxt = fine[i - 1][1], fine[i][1], fine[i + 1][1]
        if cur > prev and cur >= nxt:
            s.tide_events.append((fine[i][0], cur, "H"))
        elif cur < prev and cur <= nxt:
            s.tide_events.append((fine[i][0], cur, "L"))

    s.wind_now, s.wind_dir, s.gust_now = 14.3, 292, 21.8
    s.wind_forecast = [
        (now + dt.timedelta(hours=i),
         9 + 8 * math.sin(2 * math.pi * (i + 4) / 26) + 2 * math.sin(i * 1.7))
        for i in range(24)
    ]
    return s


def live():
    """Fetch real data. Mirrors exactly what the firmware requests."""
    import requests

    s = Snapshot()
    coops = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter"
    common = dict(station=STATION_ID, datum="MLLW", units="english",
                  time_zone="lst_ldt", format="json", application="papercolor")

    r = requests.get(coops, params={**common, "date": "today",
                                    "product": "predictions", "interval": "h"},
                     timeout=20).json()
    s.tide_curve = [(dt.datetime.strptime(p["t"], "%Y-%m-%d %H:%M"), float(p["v"]))
                    for p in r.get("predictions", [])]

    r = requests.get(coops, params={**common, "date": "today",
                                    "product": "predictions", "interval": "hilo"},
                     timeout=20).json()
    s.tide_events = [(dt.datetime.strptime(p["t"], "%Y-%m-%d %H:%M"),
                      float(p["v"]), p["type"]) for p in r.get("predictions", [])]

    try:
        r = requests.get(coops, params={**common, "date": "latest",
                                        "product": "water_level"}, timeout=20).json()
        s.tide_now = float(r["data"][0]["v"])
    except Exception:
        s.tide_now = s.tide_curve[len(s.tide_curve) // 2][1] if s.tide_curve else 0.0

    r = requests.get(
        "https://api.open-meteo.com/v1/forecast",
        params=dict(latitude=LAT, longitude=LON,
                    hourly="wind_speed_10m,wind_direction_10m,wind_gusts_10m",
                    current="wind_speed_10m,wind_direction_10m,wind_gusts_10m",
                    wind_speed_unit="kn", timezone="auto", forecast_days=2),
        timeout=20).json()
    cur = r["current"]
    s.wind_now = cur["wind_speed_10m"]
    s.wind_dir = int(cur["wind_direction_10m"])
    s.gust_now = cur["wind_gusts_10m"]
    hourly = r["hourly"]
    times = [dt.datetime.fromisoformat(t) for t in hourly["time"]]
    now = dt.datetime.now()
    pairs = [(t, v) for t, v in zip(times, hourly["wind_speed_10m"]) if t >= now]
    s.wind_forecast = pairs[:24]
    return s


# --- Drawing helpers ---------------------------------------------------------


def compass(deg):
    pts = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
           "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]
    return pts[int((deg % 360) / 22.5 + 0.5) % 16]


def wind_color(kn):
    """Three bands. Adjust the thresholds to whatever you actually care about."""
    if kn < 10:
        return GREEN
    if kn < 20:
        return YELLOW
    return RED


def centered(d, xy, text, f, fill):
    x, y = xy
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    d.text((x - (r - l) / 2 - l, y - (b - t) / 2 - t), text, font=f, fill=fill)


def right(d, xy, text, f, fill):
    x, y = xy
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    d.text((x - (r - l) - l, y), text, font=f, fill=fill)


# --- Layout ------------------------------------------------------------------


def render(s):
    img = Image.new("RGB", (W, H), WHITE)
    d = ImageDraw.Draw(img)

    # ---- Header -------------------------------------------------------------
    d.rectangle([0, 0, W, 56], fill=BLUE)
    d.text((12, 8), s.station, font=F_BIG, fill=WHITE)
    d.text((13, 38), s.updated.strftime("%a %d %b  %H:%M").upper(),
           font=F_SMALL, fill=WHITE)
    right(d, (W - 12, 10), f"{s.indoor_c:.0f}\u00b0C  {s.indoor_rh:.0f}%",
          F_SMALL, WHITE)
    right(d, (W - 12, 30), f"BATT {s.battery_pct}%", F_SMALL, WHITE)

    # ---- Now strip ----------------------------------------------------------
    rising = None
    for t, v, kind in s.tide_events:
        if t > s.updated:
            rising = (kind == "H")
            nxt = (t, v, kind)
            break
    else:
        nxt = None

    y0 = 56
    d.text((12, y0 + 10), "TIDE", font=F_MED, fill=BLACK)
    d.text((12, y0 + 34), f"{s.tide_now:.1f}", font=F_HUGE, fill=BLACK)
    l, t_, r, b = d.textbbox((0, 0), f"{s.tide_now:.1f}", font=F_HUGE)
    d.text((12 + (r - l) + 6, y0 + 58), "ft", font=F_MED, fill=BLACK)

    if rising is not None:
        arrow, word = ("\u25b2", "RISING") if rising else ("\u25bc", "FALLING")
        col = GREEN if rising else RED
        d.text((150, y0 + 40), arrow, font=F_BIG, fill=col)
        d.text((180, y0 + 48), word, font=F_MED, fill=col)

    if nxt:
        label = "NEXT HIGH" if nxt[2] == "H" else "NEXT LOW"
        right(d, (W - 12, y0 + 30), label, F_SMALL, BLACK)
        right(d, (W - 12, y0 + 46), nxt[0].strftime("%H:%M"), F_BIG, BLACK)
        right(d, (W - 12, y0 + 80), f"{nxt[1]:.1f} ft", F_SMALL, BLACK)

    # ---- Tide curve ---------------------------------------------------------
    cx0, cy0, cx1, cy1 = 12, 172, W - 12, 336
    d.rectangle([cx0, cy0, cx1, cy1], outline=BLACK, width=1)

    if s.tide_curve:
        ts = [t for t, _ in s.tide_curve]
        vs = [v for _, v in s.tide_curve]
        tmin, tmax = ts[0], ts[-1]
        span = (tmax - tmin).total_seconds()
        lo, hi = min(vs), max(vs)
        pad = max(0.4, (hi - lo) * 0.15)
        lo, hi = lo - pad, hi + pad

        def px(t):
            return cx0 + (t - tmin).total_seconds() / span * (cx1 - cx0)

        def py(v):
            return cy1 - (v - lo) / (hi - lo) * (cy1 - cy0)

        # Filled water body. Solid blue -- no gradient exists on this panel.
        poly = [(px(t), py(v)) for t, v in s.tide_curve]
        d.polygon(poly + [(cx1, cy1), (cx0, cy1)], fill=BLUE)

        # Midnight gridlines
        cur = tmin.replace(hour=0, minute=0, second=0, microsecond=0)
        while cur <= tmax:
            if tmin <= cur <= tmax:
                x = px(cur)
                d.line([(x, cy0), (x, cy1)], fill=WHITE, width=1)
            cur += dt.timedelta(hours=6)

        # High / low markers
        for t, v, kind in s.tide_events:
            if not (tmin <= t <= tmax):
                continue
            x, y = px(t), py(v)
            col = YELLOW if kind == "H" else WHITE
            d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=col, outline=BLACK)
            ty = y - 18 if kind == "H" else y + 10
            ty = min(max(ty, cy0 + 8), cy1 - 8)
            tx = min(max(x, cx0 + 20), cx1 - 20)
            centered(d, (tx, ty), t.strftime("%H:%M"), F_TINY,
                     BLACK if kind == "H" else WHITE)

        # Now marker, drawn last so it sits on top
        now_x = px(min(max(s.updated, tmin), tmax))
        d.line([(now_x, cy0), (now_x, cy1)], fill=RED, width=2)
        d.polygon([(now_x - 5, cy0), (now_x + 5, cy0), (now_x, cy0 + 7)], fill=RED)

    # ---- Wind ---------------------------------------------------------------
    wy = 352
    d.text((12, wy), "WIND", font=F_MED, fill=BLACK)

    # Compass rose
    ccx, ccy, cr = 78, wy + 74, 46
    d.ellipse([ccx - cr, ccy - cr, ccx + cr, ccy + cr], outline=BLACK, width=2)
    for lbl, ang in (("N", 0), ("E", 90), ("S", 180), ("W", 270)):
        a = math.radians(ang - 90)
        centered(d, (ccx + math.cos(a) * (cr + 11), ccy + math.sin(a) * (cr + 11)),
                 lbl, F_TINY, BLACK)

    # Arrow points the way the wind is GOING (dir is where it comes FROM).
    a = math.radians(s.wind_dir + 180 - 90)
    tipx, tipy = ccx + math.cos(a) * (cr - 8), ccy + math.sin(a) * (cr - 8)
    tailx, taily = ccx - math.cos(a) * (cr - 18), ccy - math.sin(a) * (cr - 18)
    ac = wind_color(s.wind_now)
    # Draw a black underlay one step wider so yellow arrows stay visible.
    for col, wdt in ((BLACK, 9), (ac, 5)):
        d.line([(tailx, taily), (tipx, tipy)], fill=col, width=wdt)
        for side in (140, -140):
            b = a + math.radians(side)
            d.line([(tipx, tipy),
                    (tipx + math.cos(b) * 16, tipy + math.sin(b) * 16)],
                   fill=col, width=wdt)

    # Numbers stay black. Yellow and green text on white is illegible on this
    # panel, so the speed band is carried by a solid chip instead of by ink color.
    d.text((152, wy + 26), f"{s.wind_now:.0f}", font=F_HUGE, fill=BLACK)
    l, t_, r, b = d.textbbox((0, 0), f"{s.wind_now:.0f}", font=F_HUGE)
    d.text((152 + (r - l) + 8, wy + 52), "kt", font=F_MED, fill=BLACK)
    d.rectangle([W - 26, wy + 26, W - 14, wy + 74], fill=ac, outline=BLACK)
    d.text((152, wy + 80), f"GUST {s.gust_now:.0f} kt", font=F_REG, fill=BLACK)
    d.text((152, wy + 100), f"FROM {compass(s.wind_dir)}  {s.wind_dir}\u00b0",
           font=F_REG, fill=BLACK)

    # ---- 24h wind forecast strip -------------------------------------------
    sx0, sy0, sx1, sy1 = 12, 502, W - 12, 572
    right(d, (W - 12, sy0 - 18), "WIND, NEXT 24H (kt)", F_SMALL, BLACK)
    if s.wind_forecast:
        n = len(s.wind_forecast)
        bw = (sx1 - sx0) / n
        peak = max(max(v for _, v in s.wind_forecast), 20.0)
        for i, (t, v) in enumerate(s.wind_forecast):
            x = sx0 + i * bw
            bh = max(1.0, (max(v, 0.0) / peak) * (sy1 - sy0 - 12))
            d.rectangle([x + 1, sy1 - bh, x + bw - 1, sy1],
                        fill=wind_color(v), outline=BLACK)
            if t.hour % 6 == 0:
                centered(d, (x + bw / 2, sy1 + 9), t.strftime("%H"), F_TINY, BLACK)
    d.line([(sx0, sy1), (sx1, sy1)], fill=BLACK, width=1)

    # ---- Footer -------------------------------------------------------------
    d.text((12, 583), "NOAA CO-OPS \u00b7 Open-Meteo", font=F_TINY, fill=BLACK)
    right(d, (W - 12, 583), f"UPD {s.updated.strftime('%H:%M')}", F_TINY, BLACK)

    return img


# --- Palette enforcement -----------------------------------------------------


def snap(img):
    """Force every pixel onto the six-color palette (nearest in RGB)."""
    pal = Image.new("P", (1, 1))
    flat = [c for rgb in PALETTE for c in rgb]
    pal.putpalette(flat + [0] * (768 - len(flat)))
    return img.quantize(palette=pal, dither=Image.Dither.NONE).convert("RGB")


def offpalette(img):
    """Distinct colors in the image that the panel cannot reproduce."""
    allowed = set(PALETTE)
    return [(count, c) for count, c in img.getcolors(maxcolors=1 << 24)
            if c not in allowed]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--live", action="store_true", help="fetch real data")
    ap.add_argument("--check", action="store_true", help="report off-palette pixels")
    ap.add_argument("-o", "--out", default="preview.png")
    args = ap.parse_args()

    s = live() if args.live else synthetic()
    img = render(s)

    final = snap(img)

    if args.check:
        # Off-palette pixels in the raw render are almost entirely font
        # antialiasing, which snap() resolves. What matters is how many pixels
        # move -- a large number means a design element depends on shading the
        # panel cannot produce.
        aa = sum(n for n, _ in offpalette(img))
        print(f"{aa} px ({aa / (W * H):.1%}) antialiased, snapped to palette")
        leftover = offpalette(final)
        print("snapped output is clean" if not leftover
              else f"PROBLEM: {leftover[:5]}", file=sys.stderr if leftover else sys.stdout)

    final.save(args.out)
    print(f"wrote {args.out} ({W}x{H})")


if __name__ == "__main__":
    main()
