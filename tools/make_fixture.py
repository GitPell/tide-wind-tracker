#!/usr/bin/env python3
"""
Generates a Snapshot JSON fixture (consumed by tools/hil.py and
test/fixtures/*.json) from a smooth analytic tide model, instead of
hand-typing tide[] and events[] as two separately-authored arrays.

Why: test/fixtures/example.json's hand-typed events[] silently drifted out
of sync with tide[] -- two of four events had timestamps 45-90 minutes from
where the sampled curve actually peaks/troughs, one with a magnitude the
curve never reaches at any sampled point (see CLAUDE.md, 2026-09-09
investigation). Hand-typing two arrays that are supposed to agree is exactly
the kind of thing that silently drifts. Deriving both from one continuous
function makes that class of bug structurally impossible: tide[] is hourly
samples of height(t), and events[] is height(t)'s own true extrema (found by
scanning a fine grid, not by picking the nearest hourly sample) -- whatever
tide[] says, events[] is *provably* consistent with, by construction.

Reuses the same two-constituent harmonic model tools/preview.py's
synthetic() already uses for network-free layout iteration (mean level +
~M2 semidiurnal ~12.42h term + ~K1 diurnal-inequality ~24.84h term) -- not a
new model, the existing one, now shared instead of duplicated. It isn't
meant to reproduce a real station; just a smooth, plausible curve with the
same qualitative shape (mixed semidiurnal, asymmetric highs/lows) real
Golden Gate tides have.

`--start` defaults to a fixed epoch (not "now") so the output is
reproducible across runs -- this feeds golden-image tests in tools/hil.py,
where a fixture that silently changed every time it was regenerated would
invalidate the golden PNG for no reason.

    python tools/make_fixture.py                                # to stdout
    python tools/make_fixture.py --out test/fixtures/example.json
    python tools/make_fixture.py --start 2026-09-05T00:00 --hours 25 --out test/fixtures/other.json
"""

import argparse
import datetime as dt
import json
import math
import sys
from pathlib import Path

# Same two-constituent model as tools/preview.py's synthetic(): mean level +
# M2-like semidiurnal (~12.42h) + K1-like diurnal-inequality (~24.84h,
# phase-shifted) term.
MEAN_FT = 3.0
SEMI_AMP_FT, SEMI_PERIOD_H = 1.9, 12.42
DIURNAL_AMP_FT, DIURNAL_PERIOD_H, DIURNAL_PHASE = 0.6, 24.84, 1.1

# Fixed so regenerating with default args is reproducible -- matches
# test/fixtures/example.json's original start time.
DEFAULT_START_EPOCH = 1788480000
DEFAULT_HOURS = 25
DEFAULT_NOW_OFFSET_H = 12


def height(hours_since_start):
    """The tide model, in feet, as a function of hours since `start`. The
    single source of truth for both tide[] (hourly samples of this) and
    events[] (this function's own true extrema) -- see module docstring."""
    h = hours_since_start
    return (MEAN_FT
            + SEMI_AMP_FT * math.sin(2 * math.pi * h / SEMI_PERIOD_H)
            + DIURNAL_AMP_FT * math.sin(2 * math.pi * h / DIURNAL_PERIOD_H + DIURNAL_PHASE))


def find_events(total_hours, grid_minutes=1):
    """Finds height()'s true local extrema by scanning a fine grid and
    comparing each point against its immediate neighbors. Returns a list of
    (hours_since_start, ft, "H"|"L"), sorted by time.

    This is the same technique tools/preview.py's synthetic() uses for its
    tide_events, just at finer (1-minute, vs. that function's 6-minute)
    resolution -- a one-time generator run, not a per-frame render, so the
    extra resolution costs nothing and narrows event times to within a
    minute of the model's real turning point instead of within six.
    """
    n = int(total_hours * 60 / grid_minutes) + 1
    fine = [(i * grid_minutes / 60.0, height(i * grid_minutes / 60.0)) for i in range(n)]
    events = []
    for i in range(1, len(fine) - 1):
        h_prev, h_cur, h_next = fine[i - 1][1], fine[i][1], fine[i + 1][1]
        if h_cur > h_prev and h_cur >= h_next:
            events.append((fine[i][0], h_cur, "H"))
        elif h_cur < h_prev and h_cur <= h_next:
            events.append((fine[i][0], h_cur, "L"))
    return events


def parse_start(s):
    """--start accepts a raw epoch integer or an ISO 8601 string. A naive
    ISO string is treated as UTC, not the host machine's local timezone --
    otherwise regenerating the same fixture on a different machine (or a
    machine with a different TZ setting) could silently shift every
    timestamp, defeating the point of a reproducible fixture."""
    try:
        return int(s)
    except ValueError:
        pass
    d = dt.datetime.fromisoformat(s)
    if d.tzinfo is None:
        d = d.replace(tzinfo=dt.timezone.utc)
    return int(d.timestamp())


def build_snapshot(start_epoch, hours, now_offset_h, wind_now, gust_now, wind_dir,
                    indoor_c, indoor_rh, battery, ok):
    if hours + 1 > 26:
        raise ValueError(f"hours={hours} would produce {hours + 1} tide[] points, exceeds Snapshot::tide[26]")

    tide = [
        {"t": start_epoch + i * 3600, "ft": round(height(i), 2)}
        for i in range(hours + 1)
    ]

    events = [
        {"t": start_epoch + round(h * 3600), "ft": round(v, 2), "kind": k}
        for h, v, k in find_events(hours)
    ]
    if len(events) > 10:
        raise ValueError(f"{len(events)} events found, exceeds Snapshot::events[10] -- shorten --hours")

    now_epoch = start_epoch + now_offset_h * 3600
    tide_now = round(height(now_offset_h), 2)

    # Wind forecast: same smooth shape and fixed 24-hour span tools/
    # preview.py's synthetic() uses, unconditionally from `now` -- a real
    # forecast covers the next day regardless of how far the tide table
    # happens to extend, so this isn't tied to --hours. Not derived from a
    # model with true extrema like tide[]/events[] -- forecast bars have no
    # cross-array consistency constraint to satisfy, so there's nothing
    # here for a generator to protect against drifting.
    forecast = [
        {"t": now_epoch + i * 3600,
         "kt": round(9 + 8 * math.sin(2 * math.pi * (i + 4) / 26) + 2 * math.sin(i * 1.7), 1)}
        for i in range(24)
    ]

    return {
        "now": now_epoch,
        "tideNow": tide_now,
        "windNow": wind_now,
        "gustNow": gust_now,
        "windDir": wind_dir,
        "indoorC": indoor_c,
        "indoorRh": indoor_rh,
        "battery": battery,
        "ok": ok,
        "tide": tide,
        "events": events,
        "forecast": forecast,
    }


_ARRAY_FIELDS = ("tide", "events", "forecast")


def format_snapshot(snapshot):
    """Serializes to match the hand-authored fixture style already in
    test/fixtures/ -- top-level 2-space indent, but each tide[]/events[]/
    forecast[] entry compact on one line (`{ "t": ..., "ft": ... }`)
    instead of json.dumps(indent=2)'s one-key-per-line default. Purely
    cosmetic, but keeps regenerated fixtures diffable against hand-authored
    ones instead of turning every entry into a multi-line block."""
    lines = ["{"]
    keys = list(snapshot.keys())
    for i, key in enumerate(keys):
        comma = "," if i < len(keys) - 1 else ""
        value = snapshot[key]
        if key in _ARRAY_FIELDS:
            lines.append(f'  "{key}": [')
            for j, entry in enumerate(value):
                entry_comma = "," if j < len(value) - 1 else ""
                fields = ", ".join(f'"{k}": {json.dumps(v)}' for k, v in entry.items())
                lines.append(f"    {{ {fields} }}{entry_comma}")
            lines.append(f"  ]{comma}")
        else:
            lines.append(f'  "{key}": {json.dumps(value)}{comma}')
    lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--start", default=str(DEFAULT_START_EPOCH),
                         help=f"Epoch seconds or ISO 8601 (UTC if naive) for tide[0].t (default: {DEFAULT_START_EPOCH})")
    parser.add_argument("--hours", type=int, default=DEFAULT_HOURS,
                         help=f"Hours of hourly tide[] samples past --start, producing hours+1 points, "
                              f"capped at 26 by Snapshot::tide[26] (default: {DEFAULT_HOURS})")
    parser.add_argument("--now-offset", type=float, default=DEFAULT_NOW_OFFSET_H, dest="now_offset",
                         help=f"Hours past --start to place \"now\" (default: {DEFAULT_NOW_OFFSET_H})")
    parser.add_argument("--wind-now", type=float, default=14.5, dest="wind_now")
    parser.add_argument("--gust-now", type=float, default=21.0, dest="gust_now")
    parser.add_argument("--wind-dir", type=int, default=270, dest="wind_dir")
    parser.add_argument("--indoor-c", type=float, default=21.5, dest="indoor_c")
    parser.add_argument("--indoor-rh", type=float, default=46.0, dest="indoor_rh")
    parser.add_argument("--battery", type=int, default=78)
    parser.add_argument("--not-ok", action="store_false", dest="ok",
                         help="Set ok=false (simulates a fetch failure) instead of the default true")
    parser.add_argument("--out", help="Output path (default: stdout)")
    args = parser.parse_args()

    start_epoch = parse_start(args.start)
    snapshot = build_snapshot(
        start_epoch, args.hours, args.now_offset,
        args.wind_now, args.gust_now, args.wind_dir,
        args.indoor_c, args.indoor_rh, args.battery, args.ok,
    )

    text = format_snapshot(snapshot)
    if args.out:
        Path(args.out).write_text(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
