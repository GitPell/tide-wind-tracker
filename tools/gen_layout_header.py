#!/usr/bin/env python3
"""
Generate src/layout.h from layout.json and palette.json.

layout.json is the single source of truth for pixel geometry shared by
tools/preview.py and src/main.cpp; palette.json is the same for the six
Spectra 6 panel colors, also shared with tools/hil.py. This script turns both
into a single C++ header of nested namespaces and constexpr values. It runs
automatically before every `pio run` (see the extra_scripts hook in
platformio.ini); it can also be run standalone:

    python tools/gen_layout_header.py

Only the shared sections and "firmware_only" are emitted -- "preview_only"
holds elements src/main.cpp doesn't draw yet, so there's nothing there for it
to reference. Keys starting with "_" are comments and are skipped.
"""

import json
from pathlib import Path

try:
    # PlatformIO's extra_scripts run inside a SCons exec() with no __file__,
    # but Import/env are injected -- use the project dir it already knows.
    Import("env")  # noqa: F821
    ROOT = Path(env["PROJECT_DIR"])  # noqa: F821
except NameError:
    # Standalone: `python tools/gen_layout_header.py`.
    ROOT = Path(__file__).resolve().parent.parent

LAYOUT_JSON = ROOT / "layout.json"
PALETTE_JSON = ROOT / "palette.json"
OUT_HEADER = ROOT / "src" / "layout.h"

SKIP_TOP_LEVEL = {"preview_only"}


def emit(name, value, indent):
    pad = "  " * indent
    if isinstance(value, str):
        return  # comment key, e.g. "_comment"
    if isinstance(value, dict):
        lines = [f"{pad}namespace {name} {{"]
        for k, v in value.items():
            if k.startswith("_"):
                continue
            lines.append(emit(k, v, indent + 1))
        lines.append(f"{pad}}}")
        return "\n".join(lines)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"layout.json: {name!r} must be a number, got {value!r}")
    if isinstance(value, float) and not value.is_integer():
        raise TypeError(f"layout.json: {name!r} = {value} is non-integer; "
                        "layout.h only emits ints")
    return f"{pad}constexpr int {name.upper()} = {int(value)};"


def emit_palette(indent):
    pad = "  " * indent
    colors = json.loads(PALETTE_JSON.read_text())["colors"]
    values = []
    for c in colors:
        hexstr = c["hex"].lstrip("#")
        if len(hexstr) != 6:
            raise ValueError(f"palette.json: color {c!r} hex must be 6 hex digits")
        values.append(f"0x{int(hexstr, 16):06X}")
    lines = [f"{pad}namespace palette {{"]
    lines.append(f"{pad}  constexpr uint32_t RGB[{len(values)}] = {{ {', '.join(values)} }};")
    lines.append(f"{pad}}}")
    return "\n".join(lines)


def generate():
    layout = json.loads(LAYOUT_JSON.read_text())

    body = [emit_palette(1)]
    for key, value in layout.items():
        if key.startswith("_") or key in SKIP_TOP_LEVEL:
            continue
        body.append(emit(key, value, 1))

    header = f"""\
// GENERATED FILE -- do not edit by hand.
// Regenerated from {LAYOUT_JSON.relative_to(ROOT).as_posix()} and
// {PALETTE_JSON.relative_to(ROOT).as_posix()} by tools/gen_layout_header.py,
// run automatically by `pio run` (see extra_scripts in platformio.ini).
// Edit those files instead.
#pragma once

#include <cstdint>

namespace layout {{
{chr(10).join(body)}
}}  // namespace layout
"""

    OUT_HEADER.write_text(header, newline="\n")
    print(f"generated {OUT_HEADER.relative_to(ROOT).as_posix()} "
          f"from {LAYOUT_JSON.relative_to(ROOT).as_posix()} and "
          f"{PALETTE_JSON.relative_to(ROOT).as_posix()}")


generate()
