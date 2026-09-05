#!/usr/bin/env python3
"""
Tier 1 hardware-in-the-loop harness for the PaperColor tide & wind tracker.

Talks to a board flashed with the `m5stack-papercolor-test` PlatformIO env
(build with `-D TIER1_TEST`, see platformio.ini and src/main.cpp) over its
native USB CDC serial port. That firmware does no Wi-Fi/NOAA/Open-Meteo
fetch, no PM1 sleep, and no panel refresh -- it just waits for a Snapshot as
JSON, renders it into the same off-screen canvas the real render path uses,
and dumps the raw canvas buffer back over serial. This script sends the
fixture, decodes the dump into a PNG (using the same palette.json the
firmware's PALETTE_RGB is generated from), and optionally diffs it against a
golden image -- so the render path is verifiable with no network, no
NOAA/Open-Meteo flakiness, and no 15-30s panel refresh per iteration.

    python tools/hil.py render test/fixtures/example.json --out actual.png
    python tools/hil.py test test/fixtures/example.json --golden golden.png

Requires: see tools/requirements.txt (pip install -r tools/requirements.txt)
"""

import argparse
import base64
import json
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports
from PIL import Image, ImageChops

# Espressif's registered VID and the PID Arduino-ESP32 uses for a native USB
# CDC port when ARDUINO_USB_MODE=1 (confirmed against the esp32s3box board
# definition platformio.ini builds against --
# ~/.platformio/platforms/espressif32/boards/esp32s3box.json "hwids").
USB_VID = 0x303A
USB_PID = 0x1001

# The device's native USB CDC (ESP32-S3's dedicated USB_SERIAL_JTAG
# peripheral, not a real UART) ignores this entirely -- see the module
# docstring note in the "Serial speed" investigation, or HWCDC.cpp's
# begin(unsigned long baud), which never reads its own baud argument. Kept
# only because pyserial's constructor requires some value.
BAUD = 115200
SERIAL_TIMEOUT_S = 10  # default --timeout: see Device's two-phase read model below.

SCREEN_W = 400
SCREEN_H = 600
DEPTH = 4  # bits/pixel, palette_4bit

# palette.json is the single source of truth for these six colors, shared
# with src/main.cpp (baked into generated src/layout.h by
# tools/gen_layout_header.py) and tools/preview.py. Read at runtime rather
# than hardcoded here so a golden PNG can't silently go stale against a
# palette change -- see palette.json for why array order is load-bearing.
_PALETTE_JSON_PATH = Path(__file__).resolve().parent.parent / "palette.json"


def _hex_to_rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def _load_palette():
    data = json.loads(_PALETTE_JSON_PATH.read_text())
    return [_hex_to_rgb(c["hex"]) for c in data["colors"]]


PALETTE_RGB = _load_palette()

FB_BEGIN = "---FB-BEGIN---"
FB_END = "---FB-END---"


class DeviceError(RuntimeError):
    pass


def _format_vid_pid(p):
    vid = f"{p.vid:04X}" if p.vid is not None else "----"
    pid = f"{p.pid:04X}" if p.pid is not None else "----"
    return f"{vid}:{pid}"


def _print_all_ports(ports):
    """Full visibility into what was actually enumerated, so a mismatch (or
    a non-USB virtual port like Windows' "Intel(R) Active Management
    Technology - SOL" showing up with no VID:PID at all) is visible instead
    of silent."""
    print("ports found:", file=sys.stderr)
    if not ports:
        print("  (none)", file=sys.stderr)
        return
    for p in ports:
        print(f"  {p.device}: VID:PID {_format_vid_pid(p)}  {p.description}", file=sys.stderr)


def find_port(explicit_port=None):
    if explicit_port:
        print(f"using {explicit_port} (--port)", file=sys.stderr)
        return explicit_port

    ports = serial.tools.list_ports.comports()
    # pyserial only ever sets .vid/.pid when it could actually parse a
    # USB hardware ID (list_ports_windows.py's iterate_comports() gates
    # this on the hardware ID string starting with "USB" -- confirmed by
    # reading that source); anything else, including non-USB virtual ports
    # like Intel AMT's Serial-over-LAN, gets .vid = .pid = None. The
    # `is not None` guards below are belt-and-suspenders on top of that --
    # this must never match on a None/None pair, and there must be no path
    # that returns a port without both fields equal to the real VID:PID.
    matches = [
        p for p in ports
        if p.vid is not None and p.pid is not None
        and p.vid == USB_VID and p.pid == USB_PID
    ]

    if len(matches) == 1:
        p = matches[0]
        print(f"using {p.device} (VID:PID {USB_VID:04X}:{USB_PID:04X}, {p.description})", file=sys.stderr)
        return p.device

    if not matches:
        print(
            f"error: no serial device found matching USB VID:PID "
            f"{USB_VID:04X}:{USB_PID:04X} (the PaperColor's native USB CDC "
            f"port). Is it plugged in and running the m5stack-papercolor-test "
            f"build? Pass --port to bypass this search.",
            file=sys.stderr,
        )
    else:
        names = ", ".join(p.device for p in matches)
        print(
            f"error: multiple devices match USB VID:PID {USB_VID:04X}:{USB_PID:04X}: "
            f"{names}. Pass --port to pick one.",
            file=sys.stderr,
        )
    _print_all_ports(ports)
    sys.exit(1)


class Device:
    """Line-oriented command/response protocol over the test firmware's serial port.

    Two different timeout regimes apply, matched to two different failure
    modes:

    - Waiting for a response to *start* (immediately after sending a
      command, and while skipping any stray log lines before a multi-line
      reply's first marker) is bounded by a single absolute deadline of
      `self.timeout` seconds from when the command was sent. This is what
      catches a genuinely hung device -- one that never says anything back.
    - Waiting for a response already underway to *continue* (each line of a
      framebuffer dump, once ---FB-BEGIN--- has been seen) instead gives
      every line its own fresh `self.timeout`-second window. A ~400x600
      framebuffer is ~2100 base64 lines and has been observed taking well
      over 10s end to end on this board's native USB CDC (see the "Serial
      speed" note in tools/hil.py's history/CLAUDE.md) -- that's a slow but
      steady transfer, not a hang, and an absolute deadline shared across
      the whole dump would misdiagnose it as one. Only a gap of
      `self.timeout` seconds *between* lines counts as a real stall here.
    """

    def __init__(self, port, timeout=SERIAL_TIMEOUT_S):
        self.timeout = timeout
        self.last_dump_ms = None
        self.ser = serial.Serial(port, BAUD, timeout=self.timeout)
        # Native USB CDC on this board does not reset the MCU when the host
        # opens the port (unlike a UART-bridge chip toggling DTR), so we may
        # be connecting long after the board's one-time "TIER1-READY" boot
        # banner was printed and lost. Discard whatever is sitting in the
        # buffer and sync fresh with a PING instead of waiting for that banner.
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def _readline(self, deadline):
        """Reads one line against a shared absolute `deadline` (a
        time.monotonic() value) -- used for the "waiting to start" phase."""
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise DeviceError(f"timed out waiting for device response ({self.timeout}s)")
        self.ser.timeout = remaining
        raw = self.ser.readline()
        if not raw:
            raise DeviceError(f"timed out waiting for device response ({self.timeout}s)")
        return raw.decode("ascii", errors="replace").rstrip("\r\n")

    def _readline_chunked(self):
        """Reads one line with its own fresh `self.timeout`-second window --
        used for the "waiting to continue" phase of an already-started
        multi-line transfer, so total transfer time isn't capped."""
        self.ser.timeout = self.timeout
        raw = self.ser.readline()
        if not raw:
            raise DeviceError(
                f"timed out waiting for the next line mid-transfer ({self.timeout}s "
                "since the last one)"
            )
        return raw.decode("ascii", errors="replace").rstrip("\r\n")

    def _send_line(self, s):
        self.ser.write((s + "\n").encode("ascii"))
        self.ser.flush()

    def ping(self, timeout=None):
        """Sends PING and waits for PONG, resending every RETRY_S seconds
        across the whole window instead of once and hoping.

        Confirmed live on the actual board (not just from reading source):
        opening a *new* serial connection -- pio device monitor's or this
        script's, no difference between them -- resets this board via
        ESP32-S3's native USB_SERIAL_JTAG hardware auto-reset (a DTR-edge
        trigger implemented in silicon/ROM, entirely below and invisible to
        the HWCDC Arduino class, which has zero DTR/RTS handling of its own
        -- confirmed by reading HWCDC.cpp end to end). After that reset,
        TIER1_TEST's setup() has been observed taking anywhere from a few
        seconds up to 35-50+ seconds to reach the command loop and print
        TIER1-READY, apparently from I2C retries inside M5.begin() talking
        to the board's RTC/PM1 chips (not fully characterized, and possibly
        worsened by rapid repeated resets during this investigation rather
        than representative of one normal reset).

        A single PING sent once has a real chance of landing in that dead
        window: bytes arriving before setup() calls Serial.setRxBufferSize()
        are silently dropped when that call recreates the RX queue (see
        HWCDC::setRxBufferSize() -- it deletes the old queue outright). If
        that's the PING, no PONG ever comes even though the device is fine
        and would have answered a PING sent moments later. Resending
        periodically survives that race; a plain one-shot send does not.
        """
        timeout = self.timeout if timeout is None else timeout
        retry_every = 3.0
        deadline = time.monotonic() + timeout
        while True:
            self._send_line("PING")
            attempt_deadline = min(deadline, time.monotonic() + retry_every)
            while True:
                remaining = attempt_deadline - time.monotonic()
                if remaining <= 0:
                    break
                self.ser.timeout = remaining
                raw = self.ser.readline()
                if not raw:
                    break
                line = raw.decode("ascii", errors="replace").rstrip("\r\n")
                if line == "PONG":
                    return
                # Ignore stray boot/debug lines and keep reading this attempt out.
            if time.monotonic() >= deadline:
                raise DeviceError(f"device did not respond PONG within {timeout}s")

    def quit(self):
        deadline = time.monotonic() + self.timeout
        self._send_line("QUIT")
        while True:
            line = self._readline(deadline)
            if line == "BYE":
                return

    def render(self, snapshot):
        """Sends a RENDER command for `snapshot` (a dict) and returns the
        decoded (w, h, depth, raw_bytes) framebuffer dump.

        Also sets self.last_dump_ms from the firmware's own DUMP_MS=<ms>
        line (see tier1HandleRender() in src/main.cpp), printed right after
        ---FB-END--- -- the time the device itself spent base64-encoding
        and writing the dump to Serial, isolated from JSON parsing/drawing
        on the device side and from PNG decoding on this side. None if the
        line doesn't show up (e.g. an older firmware build); that's not an
        error, just missing bonus instrumentation.
        """
        payload = json.dumps(snapshot, separators=(",", ":"))
        if "\n" in payload:
            raise ValueError("snapshot JSON must not contain embedded newlines")
        deadline = time.monotonic() + self.timeout
        self._send_line("RENDER " + payload)
        result = self._read_framebuffer(deadline)
        self.last_dump_ms = self._read_dump_ms()
        return result

    def _read_dump_ms(self):
        """Best-effort peek for the firmware's DUMP_MS=<ms> line. Purely
        informational, so this uses a short fixed timeout rather than
        self.timeout -- a device not printing it (older firmware) shouldn't
        make every render() wait out the full configured timeout."""
        old_timeout = self.ser.timeout
        self.ser.timeout = min(self.timeout, 1.0)
        try:
            raw = self.ser.readline()
        finally:
            self.ser.timeout = old_timeout
        if not raw:
            return None
        line = raw.decode("ascii", errors="replace").rstrip("\r\n")
        if not line.startswith("DUMP_MS="):
            return None
        try:
            return int(line.split("=", 1)[1])
        except ValueError:
            return None

    def _read_framebuffer(self, deadline):
        # Phase 1: waiting for the response to start. Bounded by the single
        # absolute `deadline` computed when the command was sent -- a device
        # that never begins responding (hung, crashed, wrong firmware) still
        # fails within self.timeout seconds.
        while True:
            line = self._readline(deadline)
            if line.startswith("ERR"):
                raise DeviceError(f"device reported an error: {line}")
            if line == FB_BEGIN:
                break

        # Phase 2: the transfer itself, once it has demonstrably started.
        # Each line gets a fresh timeout window instead of sharing `deadline`
        # -- see the class docstring for why.
        header = self._readline_chunked()
        fields = dict(part.split("=", 1) for part in header.split())
        try:
            w, h, depth, nbytes = (
                int(fields["W"]), int(fields["H"]), int(fields["DEPTH"]), int(fields["BYTES"])
            )
        except (KeyError, ValueError) as e:
            raise DeviceError(f"malformed framebuffer header: {header!r}") from e

        # Per-line timeouts alone don't bound the *total* number of lines --
        # a firmware bug that never emits ---FB-END--- would otherwise loop
        # forever one well-formed line at a time. Cap it at the expected
        # base64 line count (+ margin) so that fails loudly instead.
        b64_len = 4 * ((nbytes + 2) // 3)  # base64 expansion, no line breaks
        max_lines = -(-b64_len // 76) + 8  # ceil(b64_len / 76) + margin

        b64_chunks = []
        for _ in range(max_lines):
            line = self._readline_chunked()
            if line == FB_END:
                break
            if line.startswith("ERR"):
                # Firmware can abort mid-transfer instead of finishing with
                # FB_END -- e.g. tier1WriteAll() giving up on a stuck write
                # (see its comment in src/main.cpp). Must be treated as fatal,
                # not appended as base64 payload, or a truncated dump could
                # silently decode into a wrong-but-plausible-looking image.
                raise DeviceError(f"device reported an error mid-transfer: {line}")
            b64_chunks.append(line)
        else:
            raise DeviceError(
                f"framebuffer transfer exceeded {max_lines} lines without seeing {FB_END}"
            )

        raw = base64.b64decode("".join(b64_chunks))
        if len(raw) != nbytes:
            raise DeviceError(
                f"framebuffer size mismatch: header said BYTES={nbytes}, decoded {len(raw)}"
            )
        return w, h, depth, raw


def decode_palette4(raw, w, h):
    """Decodes a palette_4bit LGFX_Sprite buffer into an RGB PIL Image.

    Packing (confirmed by reading M5GFX's LGFX_Sprite.cpp
    Panel_Sprite::drawPixelPreclipped() and colortype.hpp's
    convert_uint32_to_palette4(), which replicates a palette index into both
    nibbles of a byte before the per-pixel nibble mask is applied): each byte
    holds two horizontally-adjacent pixels, first (even x) in the high
    nibble, second (odd x) in the low nibble. Row stride is w/2 bytes (w=400
    is even, so no padding).
    """
    if w % 2 != 0:
        raise ValueError("decode_palette4 assumes an even width (no nibble padding)")
    row_bytes = w // 2
    if len(raw) != row_bytes * h:
        raise ValueError(f"buffer is {len(raw)} bytes, expected {row_bytes * h} for {w}x{h} @ 4bpp")

    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row_off = y * row_bytes
        for bx in range(row_bytes):
            byte = raw[row_off + bx]
            hi = (byte >> 4) & 0x0F
            lo = byte & 0x0F
            x = bx * 2
            px[x, y] = PALETTE_RGB[hi]
            px[x + 1, y] = PALETTE_RGB[lo]
    return img


def load_fixture(path):
    return json.loads(Path(path).read_text())


def cmd_ping(args):
    port = find_port(args.port)
    dev = Device(port, timeout=args.timeout)
    try:
        dev.ping(timeout=args.connect_timeout)
        print("PONG")
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        dev.close()
    return 0


def cmd_quit(args):
    port = find_port(args.port)
    dev = Device(port, timeout=args.timeout)
    try:
        dev.quit()
        print("BYE")
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        dev.close()
    return 0


def cmd_render(args):
    port = find_port(args.port)
    dev = Device(port, timeout=args.timeout)
    try:
        dev.ping(timeout=args.connect_timeout)
        snapshot = load_fixture(args.fixture)
        t0 = time.monotonic()
        w, h, depth, raw = dev.render(snapshot)
        elapsed = time.monotonic() - t0
        if (w, h, depth) != (SCREEN_W, SCREEN_H, DEPTH):
            print(
                f"warning: device reported {w}x{h}@{depth}bpp, expected "
                f"{SCREEN_W}x{SCREEN_H}@{DEPTH}bpp",
                file=sys.stderr,
            )
        img = decode_palette4(raw, w, h)
        img.save(args.out)
        print(f"wrote {args.out}")
        _print_timing(dev, elapsed)
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        dev.close()
    return 0


def _print_timing(dev, host_elapsed_s):
    """Prints round-trip and (if the firmware reported it) device-side dump
    timing, so before/after comparisons across firmware changes don't
    require separately instrumenting each run by hand."""
    if dev.last_dump_ms is not None:
        print(f"device dump time: {dev.last_dump_ms} ms  (encode + Serial.write, on-device)")
    print(f"host round-trip:  {host_elapsed_s * 1000:.0f} ms  (RENDER sent -> framebuffer fully decoded)")


def images_equal(a, b):
    return a.size == b.size and a.tobytes() == b.tobytes()


def make_diff_image(expected, actual):
    """expected/actual highlighted in magenta wherever they differ, on top of `expected`."""
    e = expected.convert("RGB")
    a = actual.convert("RGB")
    if e.size != a.size:
        # Can't overlay pixel-for-pixel at mismatched sizes; just show actual.
        return a
    diff = ImageChops.difference(e, a)
    mask = diff.convert("L").point(lambda p: 255 if p else 0)
    highlight = Image.new("RGB", e.size, (255, 0, 255))
    return Image.composite(highlight, e, mask)


def cmd_test(args):
    port = find_port(args.port)
    dev = Device(port, timeout=args.timeout)
    try:
        dev.ping(timeout=args.connect_timeout)
        snapshot = load_fixture(args.fixture)
        t0 = time.monotonic()
        w, h, depth, raw = dev.render(snapshot)
        elapsed = time.monotonic() - t0
        actual = decode_palette4(raw, w, h)
        _print_timing(dev, elapsed)
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        dev.close()

    golden_path = Path(args.golden)
    if not golden_path.exists():
        print(f"error: golden image not found: {golden_path}", file=sys.stderr)
        sys.exit(1)
    expected = Image.open(golden_path)

    if images_equal(expected, actual):
        print(f"MATCH: {args.fixture} matches {args.golden}")
        return 0

    out_dir = Path(args.out_dir) if args.out_dir else Path(".")
    out_dir.mkdir(parents=True, exist_ok=True)
    expected_path = out_dir / "expected.png"
    actual_path = out_dir / "actual.png"
    diff_path = out_dir / "diff.png"

    expected.convert("RGB").save(expected_path)
    actual.save(actual_path)
    make_diff_image(expected, actual).save(diff_path)

    print(f"MISMATCH: {args.fixture} does not match {args.golden}", file=sys.stderr)
    print(f"  expected: {expected_path}", file=sys.stderr)
    print(f"  actual:   {actual_path}", file=sys.stderr)
    print(f"  diff:     {diff_path}", file=sys.stderr)
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="Serial port to use instead of auto-detecting by USB VID/PID")
    parser.add_argument(
        "--timeout", type=float, default=SERIAL_TIMEOUT_S,
        help=f"Seconds to wait for a response to start, and separately, seconds of silence "
             f"between lines of an in-progress transfer before giving up on either "
             f"(default: {SERIAL_TIMEOUT_S})",
    )
    parser.add_argument(
        "--connect-timeout", type=float, default=60.0,
        help="Seconds to wait for the initial PING/PONG handshake after opening the port. "
             "Longer than --timeout because opening a new connection resets this board's "
             "native USB hardware (confirmed live, independent of firmware -- see Device.ping()), "
             "and the reboot has been observed taking up to ~50s (default: 60)",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_ping = sub.add_parser("ping", help="Check that the device is alive and idle at the command prompt")
    p_ping.set_defaults(func=cmd_ping)

    p_quit = sub.add_parser("quit", help="Tell the device to print BYE and halt")
    p_quit.set_defaults(func=cmd_quit)

    p_render = sub.add_parser("render", help="Render a fixture and save it as a PNG")
    p_render.add_argument("fixture", help="Path to a Snapshot JSON fixture")
    p_render.add_argument("--out", default="actual.png", help="Output PNG path (default: actual.png)")
    p_render.set_defaults(func=cmd_render)

    p_test = sub.add_parser("test", help="Render a fixture and compare it against a golden PNG")
    p_test.add_argument("fixture", help="Path to a Snapshot JSON fixture")
    p_test.add_argument("--golden", required=True, help="Path to the golden PNG to compare against")
    p_test.add_argument("--out-dir", default=None, help="Directory to write expected/actual/diff PNGs on mismatch (default: cwd)")
    p_test.set_defaults(func=cmd_test)

    args = parser.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
