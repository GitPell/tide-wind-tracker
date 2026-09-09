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
import os
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
# default --timeout: see Device's two-phase read model below. The
# 2026-09-08 measurement that justified raising this to 30s (~10.2s
# "transfer time") was itself a host-side bug -- see CLAUDE.md's
# 2026-09-10 entry -- not a real number, so it couldn't justify a real
# margin. Real, host-measured transfer time for a ~400x600 dump is
# ~220-235ms. 10s is ~40x that, and 2x `tier1WriteAll()`'s own 5000ms
# give-up threshold (src/main.cpp) with room left over for its `#ERR` line
# to actually arrive after it gives up, rather than this script timing out
# first and reporting a generic timeout instead of the device's real error.
SERIAL_TIMEOUT_S = 10

# pyserial's Serial class (serialutil.py's SerialBase(io.RawIOBase)) never
# overrides readline() and implements no peek(), so the stdlib's generic
# io.RawIOBase.readline() fallback is what actually runs -- and that reads
# exactly one byte per call to self.read(1). On Windows, serialwin32.py's
# read(size=1) turns each of those into its own blocking overlapped
# ReadFile() syscall. For a ~400x600 dump (~2100 base64 lines, ~162KB) that
# is on the order of 160,000 individual syscalls -- slow enough that the
# device's tier1WriteAll() retry loop (src/main.cpp), which has no way to
# tell "genuinely hung" apart from "host draining too slowly to keep up",
# can stall past even this script's own per-line timeout with no error
# line ever printed. _LineReader below reads in RX_BULK_READ-sized bulk
# chunks via Serial.read(n) and splits lines in Python instead, cutting the
# syscall count from ~1/byte to a handful for the whole dump. Confirmed by
# reading serialutil.py and serialwin32.py, 2026-09-08.
RX_BULK_READ = 65536

# serialwin32.py's Serial.open() hardcodes a 4096-byte Windows driver-level
# receive buffer (`# Setup a 4k buffer` / win32.SetupComm(handle, 4096,
# 4096)) unless the app raises it afterward via set_buffer_size(). 4096 is
# smaller than a single device-side 16KB tier1PrintBase64() write chunk, so
# without this the OS-level buffer -- not RX_BULK_READ reads -- would still
# be the bottleneck: the driver stops accepting more from the device (USB
# CDC flow control) the instant it fills, regardless of how fast this
# script asks for data afterward. Sized comfortably above one device-side
# chunk. Windows-only API (serialwin32.Serial only) -- guarded with
# hasattr() so this stays a no-op on other platforms' pyserial backends.
RX_DRIVER_BUFFER = 131072

SCREEN_W = 400
SCREEN_H = 600
DEPTH = 4  # bits/pixel, palette_4bit

# palette.json is the single source of truth for these six colors, shared
# with src/main.cpp (baked into generated src/layout.h by
# tools/gen_layout_header.py) and tools/preview.py. Read at runtime rather
# than hardcoded here so a golden PNG can't silently go stale against a
# palette change -- see palette.json for why array order is load-bearing.
_PALETTE_JSON_PATH = Path(__file__).resolve().parent.parent / "palette.json"

# `test` --all's fixture/golden pairing. Script-relative (like
# _PALETTE_JSON_PATH above), not CWD-relative like the single-fixture
# `fixture`/`--golden` arguments, since --all has no positional argument to
# anchor a relative path off of and shouldn't silently find nothing just
# because it was run from a different directory.
_FIXTURES_DIR = Path(__file__).resolve().parent.parent / "test" / "fixtures"
_GOLDEN_DIR = Path(__file__).resolve().parent.parent / "test" / "golden"


def _hex_to_rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def _load_palette():
    data = json.loads(_PALETTE_JSON_PATH.read_text())
    return [_hex_to_rgb(c["hex"]) for c in data["colors"]]


PALETTE_RGB = _load_palette()

FB_BEGIN = "---FB-BEGIN---"
FB_END = "---FB-END---"

# '#' is not in the base64 alphabet (A-Z, a-z, 0-9, +, /), so a line of
# legitimate base64 payload can never start with it -- unlike the "ERR"
# prefix this used to be, which collided with valid base64 (e.g. a payload
# line beginning "ERRxyz...=" is perfectly valid base64 and was previously
# misdiagnosed as "device reported an error mid-transfer"). Must match
# src/main.cpp's tier1HandleRender()/setup() error prefixes exactly.
ERR_PREFIX = "#ERR"


class DeviceError(RuntimeError):
    pass


# Distinct exit codes so a script driving `hil.py test` in a loop can tell
# "the render regressed" (EXIT_MISMATCH -- act on it, it's a real finding)
# apart from "couldn't even complete the comparison" (EXIT_DEVICE_ERROR --
# board unplugged, wrong firmware, protocol hiccup, missing golden file;
# retry or fix the environment, not the render). Both used to be a bare
# sys.exit(1), indistinguishable without parsing stderr text.
EXIT_MISMATCH = 1
EXIT_DEVICE_ERROR = 2


# --- Temporary instrumentation for the "transfer time tracks the timeout
# exactly" investigation (2026-09-10, serial-timing branch) -- not meant to
# ship. Enable with HIL_DEBUG_READS=1. Logs every underlying
# self.ser.read(n) call _LineReader makes (requested size, the ser.timeout
# it was given, how long the call actually took, and how many bytes came
# back), plus a few named markers (RENDER sent, FB_BEGIN/FB_END seen), all
# on one shared clock -- so the exact call that eats the missing time shows
# up directly instead of being inferred from aggregate numbers.
_DEBUG_READS = bool(os.environ.get("HIL_DEBUG_READS"))
_DEBUG_T0 = time.monotonic()


def _dbg(msg):
    if _DEBUG_READS:
        print(f"[{time.monotonic() - _DEBUG_T0:8.3f}s] {msg}", file=sys.stderr)


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
    sys.exit(EXIT_DEVICE_ERROR)


class _LineReader:
    """Splits '\\n'-terminated lines out of a pyserial port using bulk
    `ser.read(n)` calls into a local buffer, instead of relying on
    pyserial's own readline() -- see the RX_BULK_READ comment above for why
    that matters. Multiple lines delivered in one underlying burst are
    served from the buffer with no further I/O at all.

    Each underlying read asks for exactly what's currently available
    (`ser.in_waiting`, capped at `chunk_size`), not a fixed `chunk_size`
    every time -- see the comment in readline() for why a fixed request
    size turns any undersized final read into a multi-second stall on this
    port's Windows COMMTIMEOUTS configuration, confirmed live 2026-09-10.
    """

    def __init__(self, ser, chunk_size=RX_BULK_READ):
        self.ser = ser
        self.chunk_size = chunk_size
        self._buf = bytearray()

    def readline(self, deadline=None, timeout=None):
        """Returns the next line as bytes (no trailing '\\r\\n'), or None on
        timeout. Exactly one of:
        - `deadline`: a single absolute time.monotonic() value shared
          across however many underlying reads it takes to find a line --
          used when waiting for a response to *start*.
        - `timeout`: a fresh window in seconds, restarted on every
          underlying read this call has to make -- used when a multi-line
          transfer is already underway, so a slow-but-steady transfer isn't
          capped by a shared deadline (only a real gap counts as a stall).
        """
        while True:
            nl = self._buf.find(b"\n")
            if nl != -1:
                line = bytes(self._buf[:nl])
                del self._buf[:nl + 1]
                return line[:-1] if line[-1:] == b"\r" else line

            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self.ser.timeout = remaining
            else:
                self.ser.timeout = timeout

            # Request only as much as is actually sitting in the driver's
            # receive buffer right now (or, if nothing is, exactly 1 byte)
            # -- never a fixed self.chunk_size regardless of what's really
            # coming. Confirmed live, 2026-09-10 (see CLAUDE.md): on
            # Windows, this port's COMMTIMEOUTS configuration
            # (ReadIntervalTimeout=0, ReadTotalTimeoutMultiplier=0,
            # ReadTotalTimeoutConstant=timeout*1000) makes ReadFile block
            # for the *entire* configured timeout waiting for the full
            # requested count, not just until some data arrives -- it does
            # NOT return early with partial data. Asking for a fixed 64KB
            # on every call turned the final, necessarily-undersized read
            # of almost every transfer into a multi-second stall for data
            # that had, in the case that exposed this, already arrived
            # within the first ~200ms. Requesting in_waiting bytes lets a
            # call that already has data return immediately; requesting 1
            # byte when nothing is buffered yet means that call returns
            # the instant *any* new data shows up instead of waiting for a
            # specific count that may never come -- so this never spins on
            # a zero-byte read (self.ser.read(1) genuinely blocks, up to
            # `remaining`/`timeout`, until at least one byte exists or a
            # real stall is confirmed by hitting that timeout).
            request = min(self.chunk_size, self.ser.in_waiting or 1)
            wait_mode = f"deadline(remaining={remaining:.3f}s)" if deadline is not None else f"timeout={timeout:.3f}s"
            t_before = time.monotonic()
            chunk = self.ser.read(request)
            call_elapsed = time.monotonic() - t_before
            if _DEBUG_READS:
                preview = chunk[:32].decode("ascii", errors="replace")
                _dbg(f"ser.read({request}) {wait_mode} ser.timeout={self.ser.timeout:.3f}s "
                     f"-> {len(chunk)}B in {call_elapsed*1000:8.1f}ms  head={preview!r}")
            if not chunk:
                return None
            self._buf.extend(chunk)


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
        self.last_transfer_wall_s = None
        try:
            self.ser = serial.Serial(port, BAUD, timeout=self.timeout)
        except serial.SerialException as e:
            # Wrapped as DeviceError, not left to propagate raw: a bad/busy/
            # missing port (e.g. the board unplugged, or --port pointing at
            # nothing) is exactly the "couldn't reach the device" case every
            # cmd_* function's `except DeviceError` -> EXIT_DEVICE_ERROR path
            # exists for. Left unwrapped, this used to escape as an uncaught
            # SerialException -- a raw traceback, and Python's default exit
            # code 1 for an uncaught exception, indistinguishable from
            # EXIT_MISMATCH. Confirmed live, 2026-09-10.
            raise DeviceError(str(e)) from e
        # See the RX_DRIVER_BUFFER comment: Windows-only (serialwin32.Serial),
        # hence the hasattr guard rather than a platform check -- a no-op
        # everywhere else, where the backend's own buffering differs.
        if hasattr(self.ser, "set_buffer_size"):
            self.ser.set_buffer_size(RX_DRIVER_BUFFER)
        self._lines = _LineReader(self.ser)
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
        line = self._lines.readline(deadline=deadline)
        if line is None:
            raise DeviceError(f"timed out waiting for device response ({self.timeout}s)")
        return line.decode("ascii", errors="replace")

    def _readline_chunked(self):
        """Reads one line with its own fresh `self.timeout`-second window --
        used for the "waiting to continue" phase of an already-started
        multi-line transfer, so total transfer time isn't capped."""
        line = self._lines.readline(timeout=self.timeout)
        if line is None:
            raise DeviceError(
                f"timed out waiting for the next line mid-transfer ({self.timeout}s "
                "since the last one)"
            )
        return line.decode("ascii", errors="replace")

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
                raw = self._lines.readline(deadline=attempt_deadline)
                if raw is None:
                    break
                line = raw.decode("ascii", errors="replace")
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

        Also sets:
        - self.last_transfer_wall_s: real host-observed wall-clock time from
          when RENDER was sent to when ---FB-END--- was actually read off
          the wire (measured in _read_framebuffer(), before base64
          decoding). This is the honest transfer measurement -- use this,
          not last_dump_ms, to judge whether a real transfer is fast.
        - self.last_dump_ms, from the firmware's own DUMP_MS=<ms> line (see
          tier1HandleRender() in src/main.cpp). This is device-side
          Serial.write() *enqueue* time only, not a transfer measurement:
          HWCDC's write() queues bytes into a ring buffer that the ISR
          drains onto the wire asynchronously, so returning from
          Serial.write() does not mean the bytes have reached the host (see
          the tools/hil.py investigation notes in CLAUDE.md, 2026-09-08).
          None if the line doesn't show up (e.g. an older firmware build);
          that's not an error, just missing bonus instrumentation.
        """
        payload = json.dumps(snapshot, separators=(",", ":"))
        if "\n" in payload:
            raise ValueError("snapshot JSON must not contain embedded newlines")
        send_time = time.monotonic()
        deadline = send_time + self.timeout
        _dbg(f"RENDER sent ({len(payload)}B payload)")
        self._send_line("RENDER " + payload)
        result = self._read_framebuffer(deadline, send_time)
        _dbg("_read_framebuffer() returned, reading DUMP_MS...")
        self.last_dump_ms = self._read_dump_ms()
        _dbg(f"DUMP_MS read -> {self.last_dump_ms}")
        return result

    def _read_dump_ms(self):
        """Best-effort peek for the firmware's DUMP_MS=<ms> line. Purely
        informational, so this uses a short fixed timeout rather than
        self.timeout -- a device not printing it (older firmware) shouldn't
        make every render() wait out the full configured timeout."""
        raw = self._lines.readline(timeout=min(self.timeout, 1.0))
        if raw is None:
            return None
        line = raw.decode("ascii", errors="replace")
        if not line.startswith("DUMP_MS="):
            return None
        try:
            return int(line.split("=", 1)[1])
        except ValueError:
            return None

    def _read_framebuffer(self, deadline, send_time):
        # Phase 1: waiting for the response to start. Bounded by the single
        # absolute `deadline` computed when the command was sent -- a device
        # that never begins responding (hung, crashed, wrong firmware) still
        # fails within self.timeout seconds.
        while True:
            line = self._readline(deadline)
            if line.startswith(ERR_PREFIX):
                raise DeviceError(f"device reported an error: {line}")
            if line == FB_BEGIN:
                _dbg("FB_BEGIN seen")
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
        _dbg(f"header parsed: W={w} H={h} DEPTH={depth} BYTES={nbytes}")

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
                # Measured here, not after render() returns -- this is the
                # real transfer completing, before the (host-CPU-bound,
                # transfer-irrelevant) base64 decode below.
                self.last_transfer_wall_s = time.monotonic() - send_time
                _dbg(f"FB_END seen, last_transfer_wall_s={self.last_transfer_wall_s:.3f}s")
                break
            if line.startswith(ERR_PREFIX):
                # Firmware can abort mid-transfer instead of finishing with
                # FB_END -- e.g. tier1WriteAll() giving up on a stuck write
                # (see its comment in src/main.cpp). Must be treated as fatal,
                # not appended as base64 payload, or a truncated dump could
                # silently decode into a wrong-but-plausible-looking image.
                # Checking against ERR_PREFIX ("#ERR"), not a plain "ERR"
                # prefix, matters here specifically: this branch runs against
                # lines that are otherwise expected to be base64 payload, and
                # "ERR" alone is a valid base64 prefix (E, R are both in the
                # alphabet) -- a real payload line starting with those three
                # letters would otherwise be misdiagnosed as a device error.
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
    # Device(...) itself -- not just the calls after it -- must be inside
    # this try: it's what actually opens the serial port, so a bad/busy/
    # missing port raises DeviceError right here. Left outside the try (as
    # this used to be, in all four cmd_* functions), that exception skips
    # the `except DeviceError` handler entirely and escapes as an uncaught
    # traceback. `dev = None` first so `finally` can tell whether there's
    # anything to close. Confirmed live, 2026-09-10.
    dev = None
    try:
        dev = Device(port, timeout=args.timeout)
        dev.ping(timeout=args.connect_timeout)
        print("PONG")
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(EXIT_DEVICE_ERROR)
    finally:
        if dev:
            dev.close()
    return 0


def cmd_quit(args):
    port = find_port(args.port)
    dev = None  # see cmd_ping's comment on why Device(...) must be inside the try
    try:
        dev = Device(port, timeout=args.timeout)
        dev.quit()
        print("BYE")
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(EXIT_DEVICE_ERROR)
    finally:
        if dev:
            dev.close()
    return 0


def cmd_render(args):
    port = find_port(args.port)
    dev = None  # see cmd_ping's comment on why Device(...) must be inside the try
    try:
        dev = Device(port, timeout=args.timeout)
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
    except (DeviceError, FileNotFoundError, json.JSONDecodeError) as e:
        # The latter two are load_fixture() failing on a bad --fixture path
        # or malformed JSON -- a setup problem, same bucket as
        # EXIT_DEVICE_ERROR (couldn't reach a verdict), definitely not
        # EXIT_MISMATCH (compared and found a real difference). Left
        # uncaught, either used to escape as a raw traceback with Python's
        # default exit code 1, indistinguishable from a genuine mismatch.
        print(f"error: {e}", file=sys.stderr)
        sys.exit(EXIT_DEVICE_ERROR)
    finally:
        if dev:
            dev.close()
    return 0


def _print_timing(dev, host_elapsed_s):
    """Prints the real transfer time plus (if the firmware reported it) the
    device's own enqueue-time figure, so before/after comparisons across
    firmware changes don't require separately instrumenting each run by
    hand. See the caveat below DUMP_MS before trusting it as a transfer
    number -- it isn't one."""
    if dev.last_transfer_wall_s is not None:
        print(
            f"transfer time:    {dev.last_transfer_wall_s * 1000:.0f} ms  "
            "(host wall-clock, RENDER sent -> ---FB-END--- read off the wire -- "
            "the real transfer number)"
        )
    if dev.last_dump_ms is not None:
        print(
            f"device DUMP_MS:   {dev.last_dump_ms} ms  "
            "(Serial.write() ENQUEUE time only, on-device -- HWCDC's ring buffer "
            "drains to the wire asynchronously, so this is NOT how long the "
            "transfer actually took; compare against transfer time above, don't "
            "use this alone)"
        )
    print(f"host round-trip:  {host_elapsed_s * 1000:.0f} ms  (RENDER sent -> framebuffer fully decoded, incl. base64 decode)")


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


def _test_one_in_session(dev, fixture_path, golden_path, out_dir):
    """Renders `fixture_path` on the already-connected `dev` and compares
    against `golden_path`. Used by --all, where one Device is shared across
    every fixture instead of reconnecting (and re-eating the board's
    up-to-~50s USB reset/reboot) per fixture.

    Returns True on match, False for anything else that isn't a real match
    -- a pixel mismatch, a missing golden, or a bad/unreadable fixture file
    -- none of which should stop the rest of an --all run; they're just
    that one fixture's result. A missing golden is reported as a failure
    here, not skipped, same as a real mismatch.

    The one thing that's allowed to propagate uncaught is DeviceError, from
    dev.render() -- a real connection/protocol failure. Continuing to the
    next fixture over a session that just failed like that isn't safe (the
    same problem will likely just repeat), so the caller must stop the
    whole --all run instead of only failing this one fixture.
    """
    try:
        snapshot = load_fixture(fixture_path)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"FAIL (bad fixture): {fixture_path}: {e}", file=sys.stderr)
        return False

    t0 = time.monotonic()
    w, h, depth, raw = dev.render(snapshot)  # DeviceError intentionally propagates
    elapsed = time.monotonic() - t0
    actual = decode_palette4(raw, w, h)
    _print_timing(dev, elapsed)

    golden_path = Path(golden_path)
    if not golden_path.exists():
        print(f"FAIL (missing golden): {fixture_path} -> {golden_path}", file=sys.stderr)
        return False

    expected = Image.open(golden_path)
    if images_equal(expected, actual):
        print(f"PASS: {Path(fixture_path).name}")
        return True

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    expected_path = out_dir / "expected.png"
    actual_path = out_dir / "actual.png"
    diff_path = out_dir / "diff.png"
    expected.convert("RGB").save(expected_path)
    actual.save(actual_path)
    make_diff_image(expected, actual).save(diff_path)

    print(f"FAIL (mismatch): {Path(fixture_path).name}", file=sys.stderr)
    print(f"  expected: {expected_path}", file=sys.stderr)
    print(f"  actual:   {actual_path}", file=sys.stderr)
    print(f"  diff:     {diff_path}", file=sys.stderr)
    return False


def _cmd_test_all(args):
    fixtures = sorted(_FIXTURES_DIR.glob("*.json"))
    if not fixtures:
        print(f"error: no fixtures found in {_FIXTURES_DIR}", file=sys.stderr)
        sys.exit(EXIT_DEVICE_ERROR)

    out_root = Path(args.out_dir) if args.out_dir else Path(".")

    port = find_port(args.port)
    dev = None  # see cmd_ping's comment on why Device(...) must be inside the try
    results = []
    try:
        dev = Device(port, timeout=args.timeout)
        dev.ping(timeout=args.connect_timeout)
        for fixture_path in fixtures:
            name = fixture_path.stem
            golden_path = _GOLDEN_DIR / f"{name}.png"
            ok = _test_one_in_session(dev, fixture_path, golden_path, out_root / name)
            results.append((name, ok))
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        print(f"aborting --all after {len(results)}/{len(fixtures)} fixtures -- "
              "the shared session is likely no longer usable", file=sys.stderr)
        sys.exit(EXIT_DEVICE_ERROR)
    finally:
        if dev:
            dev.close()

    passed = sum(1 for _, ok in results if ok)
    total = len(results)
    print(f"\n{passed}/{total} fixtures passed")
    for name, ok in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")

    sys.exit(0 if passed == total else EXIT_MISMATCH)


def cmd_test(args):
    if args.all:
        if args.fixture or args.golden:
            print(
                "error: --all doesn't take a fixture positional or --golden -- it pairs every "
                f"{_FIXTURES_DIR}/*.json with {_GOLDEN_DIR}/<name>.png itself",
                file=sys.stderr,
            )
            sys.exit(2)
        _cmd_test_all(args)
        return

    if not args.fixture or not args.golden:
        print("error: 'fixture' and --golden are required (or pass --all to test every fixture)",
              file=sys.stderr)
        sys.exit(2)

    port = find_port(args.port)
    dev = None  # see cmd_ping's comment on why Device(...) must be inside the try
    try:
        dev = Device(port, timeout=args.timeout)
        dev.ping(timeout=args.connect_timeout)
        snapshot = load_fixture(args.fixture)
        t0 = time.monotonic()
        w, h, depth, raw = dev.render(snapshot)
        elapsed = time.monotonic() - t0
        actual = decode_palette4(raw, w, h)
        _print_timing(dev, elapsed)
    except (DeviceError, FileNotFoundError, json.JSONDecodeError) as e:
        # See cmd_render's matching except clause for why these two extra
        # exception types belong in the same bucket as DeviceError here.
        print(f"error: {e}", file=sys.stderr)
        sys.exit(EXIT_DEVICE_ERROR)
    finally:
        if dev:
            dev.close()

    golden_path = Path(args.golden)
    if not golden_path.exists():
        # Couldn't even reach a verdict -- not the same failure as a
        # confirmed pixel mismatch below, so it must not share that exit
        # code (see EXIT_MISMATCH/EXIT_DEVICE_ERROR).
        print(f"error: golden image not found: {golden_path}", file=sys.stderr)
        sys.exit(EXIT_DEVICE_ERROR)
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
    sys.exit(EXIT_MISMATCH)


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
    p_test.add_argument("fixture", nargs="?", help="Path to a Snapshot JSON fixture (omit with --all)")
    p_test.add_argument("--golden", help="Path to the golden PNG to compare against (omit with --all)")
    p_test.add_argument(
        "--all", action="store_true",
        help=f"Test every {_FIXTURES_DIR}/*.json against {_GOLDEN_DIR}/<name>.png in one shared "
             "serial session instead of reconnecting per fixture. Exit 0 only if every fixture "
             "matches, 1 if any mismatch (including a missing golden), 2 on a device error.",
    )
    p_test.add_argument(
        "--out-dir", default=None,
        help="Directory to write expected/actual/diff PNGs on mismatch -- with --all, each failing "
             "fixture gets its own <out-dir>/<name>/ subdirectory so failures don't overwrite each "
             "other (default: cwd)",
    )
    p_test.set_defaults(func=cmd_test)

    args = parser.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
