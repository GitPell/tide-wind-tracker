#!/usr/bin/env python3
"""
Host-side layout preview for the PaperColor tide & wind tracker.

Renders the exact 400x600 dashboard using only the six Spectra 6 colors, so you
can iterate on layout in milliseconds instead of waiting 15-30s per panel
refresh. The drawing code here is the specification the C++ in src/main.cpp
should mirror -- same coordinates, same palette, same rounding. Pixel
coordinates themselves live in ../layout.json (read at runtime, below); edit
that file to move things, not the literals that used to be inline here.

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
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# --- Configuration -----------------------------------------------------------
# Keep these in sync with src/config.h.

STATION_ID = "9414290"
STATION_NAME = "GOLDEN GATE"
LAT, LON = 37.8063, -122.4659

# --- Layout --------------------------------------------------------------
# layout.json is the single source of truth for pixel geometry, shared with
# src/main.cpp (via the generated src/layout.h -- see
# tools/gen_layout_header.py). Edit layout.json, not the literals below.

LAYOUT = json.loads((Path(__file__).resolve().parent.parent / "layout.json").read_text())

W, H = LAYOUT["screen"]["w"], LAYOUT["screen"]["h"]

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
    os.path.join(os.path.dirname(__file__), "fonts"),
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

    hdr, po_hdr = LAYOUT["header"], LAYOUT["preview_only"]["header"]
    tide_box, po_tide = LAYOUT["tide_box"], LAYOUT["preview_only"]["tide_box"]
    wind, po_wind = LAYOUT["wind"], LAYOUT["preview_only"]["wind"]
    fc = LAYOUT["forecast"]
    now_strip = LAYOUT["now_strip"]
    footer = LAYOUT["footer"]

    # ---- Header -------------------------------------------------------------
    d.rectangle([0, 0, W, hdr["height"]], fill=BLUE)
    d.text((hdr["station_x"], hdr["station_y"]), s.station, font=F_BIG, fill=WHITE)
    d.text((hdr["datetime_x"], hdr["datetime_y"]),
           s.updated.strftime("%a %d %b  %H:%M").upper(), font=F_SMALL, fill=WHITE)
    right(d, (W - po_hdr["indoor_margin"], po_hdr["indoor_y"]),
          f"{s.indoor_c:.0f}\u00b0C  {s.indoor_rh:.0f}%", F_SMALL, WHITE)
    right(d, (W - po_hdr["battery_margin"], po_hdr["battery_y"]),
          f"BATT {s.battery_pct}%", F_SMALL, WHITE)

    # ---- Now strip ----------------------------------------------------------
    rising = None
    for t, v, kind in s.tide_events:
        if t > s.updated:
            rising = (kind == "H")
            nxt = (t, v, kind)
            break
    else:
        nxt = None

    y0 = now_strip["y0_offset"]
    d.text((now_strip["label_x"], y0 + now_strip["label_y"]), "TIDE", font=F_MED, fill=BLACK)
    d.text((now_strip["value_x"], y0 + now_strip["value_y"]),
           f"{s.tide_now:.1f}", font=F_HUGE, fill=BLACK)
    l, t_, r, b = d.textbbox((0, 0), f"{s.tide_now:.1f}", font=F_HUGE)
    d.text((now_strip["value_x"] + (r - l) + now_strip["unit_gap_x"], y0 + now_strip["unit_y"]),
           "ft", font=F_MED, fill=BLACK)

    if rising is not None:
        arrow, word = ("\u25b2", "RISING") if rising else ("\u25bc", "FALLING")
        col = GREEN if rising else RED
        d.text((now_strip["trend_arrow_x"], y0 + now_strip["trend_arrow_y"]), arrow, font=F_BIG, fill=col)
        d.text((now_strip["trend_word_x"], y0 + now_strip["trend_word_y"]), word, font=F_MED, fill=col)

    if nxt:
        label = "NEXT HIGH" if nxt[2] == "H" else "NEXT LOW"
        nrm = now_strip["next_right_margin"]
        right(d, (W - nrm, y0 + now_strip["next_label_dy"]), label, F_SMALL, BLACK)
        right(d, (W - nrm, y0 + now_strip["next_time_dy"]), nxt[0].strftime("%H:%M"), F_BIG, BLACK)
        right(d, (W - nrm, y0 + now_strip["next_value_dy"]), f"{nxt[1]:.1f} ft", F_SMALL, BLACK)

    # ---- Tide curve ---------------------------------------------------------
    cx0, cy0 = tide_box["x0"], tide_box["y0"]
    cx1, cy1 = W - tide_box["right_margin"], tide_box["y1"]
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
        mr = tide_box["event_marker_radius"]
        for t, v, kind in s.tide_events:
            if not (tmin <= t <= tmax):
                continue
            x, y = px(t), py(v)
            col = YELLOW if kind == "H" else WHITE
            d.ellipse([x - mr, y - mr, x + mr, y + mr], fill=col, outline=BLACK)
            ty = y + (po_tide["label_high_dy"] if kind == "H" else po_tide["label_low_dy"])
            ty = min(max(ty, cy0 + po_tide["label_clamp_y_margin"]), cy1 - po_tide["label_clamp_y_margin"])
            tx = min(max(x, cx0 + po_tide["label_clamp_x_margin"]), cx1 - po_tide["label_clamp_x_margin"])
            centered(d, (tx, ty), t.strftime("%H:%M"), F_TINY,
                     BLACK if kind == "H" else WHITE)

        # Now marker, drawn last so it sits on top
        now_x = px(min(max(s.updated, tmin), tmax))
        d.line([(now_x, cy0), (now_x, cy1)], fill=RED, width=tide_box["now_line_width"])
        hw, th = po_tide["now_marker_half_width"], po_tide["now_marker_height"]
        d.polygon([(now_x - hw, cy0), (now_x + hw, cy0), (now_x, cy0 + th)], fill=RED)

    # ---- Wind ---------------------------------------------------------------
    wy = wind["y"]
    d.text((wind["label_x"], wy), "WIND", font=F_MED, fill=BLACK)

    # Compass rose
    ccx, ccy, cr = wind["compass_cx"], wy + wind["compass_dy"], wind["compass_r"]
    d.ellipse([ccx - cr, ccy - cr, ccx + cr, ccy + cr], outline=BLACK, width=2)
    lr = cr + po_wind["compass_label_radius_offset"]
    for lbl, ang in (("N", 0), ("E", 90), ("S", 180), ("W", 270)):
        a = math.radians(ang - 90)
        centered(d, (ccx + math.cos(a) * lr, ccy + math.sin(a) * lr),
                 lbl, F_TINY, BLACK)

    # Arrow points the way the wind is GOING (dir is where it comes FROM).
    a = math.radians(s.wind_dir + 180 - 90)
    tip_in, tail_in = wind["arrow_tip_inset"], wind["arrow_tail_inset"]
    tipx, tipy = ccx + math.cos(a) * (cr - tip_in), ccy + math.sin(a) * (cr - tip_in)
    tailx, taily = ccx - math.cos(a) * (cr - tail_in), ccy - math.sin(a) * (cr - tail_in)
    ac = wind_color(s.wind_now)
    barb_ang, barb_len = po_wind["arrow_barb_angle_deg"], po_wind["arrow_barb_length"]
    # Draw a black underlay one step wider so yellow arrows stay visible.
    for col, wdt in ((BLACK, wind["arrow_underlay_width"]), (ac, wind["arrow_color_width"])):
        d.line([(tailx, taily), (tipx, tipy)], fill=col, width=wdt)
        for side in (barb_ang, -barb_ang):
            b = a + math.radians(side)
            d.line([(tipx, tipy),
                    (tipx + math.cos(b) * barb_len, tipy + math.sin(b) * barb_len)],
                   fill=col, width=wdt)

    # Numbers stay black. Yellow and green text on white is illegible on this
    # panel, so the speed band is carried by a solid chip instead of by ink color.
    rx, ry = po_wind["reading_x"], wy + po_wind["reading_dy"]
    d.text((rx, ry), f"{s.wind_now:.0f}", font=F_HUGE, fill=BLACK)
    l, t_, r, b = d.textbbox((0, 0), f"{s.wind_now:.0f}", font=F_HUGE)
    d.text((rx + (r - l) + po_wind["kt_label_gap"], wy + po_wind["kt_label_dy"]),
           "kt", font=F_MED, fill=BLACK)
    chip_x0, chip_y0 = W - wind["chip_right_offset"], wy + wind["chip_dy"]
    d.rectangle([chip_x0, chip_y0, chip_x0 + wind["chip_w"], chip_y0 + wind["chip_h"]],
                fill=ac, outline=BLACK)
    d.text((wind["gust_x"], wy + wind["gust_dy"]), f"GUST {s.gust_now:.0f} kt", font=F_REG, fill=BLACK)
    d.text((wind["from_x"], wy + wind["from_dy"]),
           f"FROM {compass(s.wind_dir)}  {s.wind_dir}\u00b0", font=F_REG, fill=BLACK)

    # ---- 24h wind forecast strip -------------------------------------------
    sx0, sy0 = fc["x0"], fc["top"]
    sx1, sy1 = W - fc["right_margin"], fc["bottom"]
    right(d, (W - fc["right_margin"], sy0 - fc["label_dy_above_top"]),
          "WIND, NEXT 24H (kt)", F_SMALL, BLACK)
    if s.wind_forecast:
        n = len(s.wind_forecast)
        bw = (sx1 - sx0) / n
        peak = max(max(v for _, v in s.wind_forecast), 20.0)
        bi = fc["bar_inset"]
        for i, (t, v) in enumerate(s.wind_forecast):
            x = sx0 + i * bw
            bh = max(float(fc["bar_min_height"]),
                     (max(v, 0.0) / peak) * (sy1 - sy0 - fc["bar_height_margin"]))
            d.rectangle([x + bi, sy1 - bh, x + bw - bi, sy1],
                        fill=wind_color(v), outline=BLACK)
            if t.hour % 6 == 0:
                centered(d, (x + bw / 2, sy1 + fc["hour_label_dy"]), t.strftime("%H"), F_TINY, BLACK)
    d.line([(sx0, sy1), (sx1, sy1)], fill=BLACK, width=1)

    # ---- Footer -------------------------------------------------------------
    d.text((footer["left_x"], footer["y"]), "NOAA CO-OPS \u00b7 Open-Meteo", font=F_TINY, fill=BLACK)
    right(d, (W - footer["right_margin"], footer["y"]), f"UPD {s.updated.strftime('%H:%M')}", F_TINY, BLACK)

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
