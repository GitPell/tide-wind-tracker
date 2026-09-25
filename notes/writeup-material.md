# Write-up material

Raw material for the project write-up: the discovery stories that used to
live in CLAUDE.md. CLAUDE.md now keeps only the rules and the reasoning
behind them; the how-we-found-it detail is here. Each section names the
commits and notes files involved.

---

## The "flaky hilo fetch" that was a clock bug

Commits: `9a5dc42` (first treated as fetch reliability), `bebec80` (real fix).

For days the NEXT HIGH/LOW block and the tide-curve dots stayed blank across
many consecutive cycles. It looked like an occasional NOAA hilo HTTPS
failure, and `9a5dc42` added retries and a fallback. The real cause was the
clock: Arduino's `getLocalTime()` only checks `tm_year > 2016`, and
`M5.begin()` had already seeded the system clock from the RX8130 via
`setSystemTimeFromRtc()`. So `getLocalTime()` "succeeded" instantly on a
stale clock, `setup()` wrote that stale time back to the RTC, and SNTP
corrected the clock asynchronously later in the cycle. `fetchTides()` built
the hilo `begin_date` from the stale clock, while `drawNowStrip()` compared
events against the corrected clock -- so every fetched event compared as
already past. A temporary Serial dump of parsed event epochs vs `now` showed
events ~9.5 months before `now` despite a successful, non-retried fetch.
Fixed 2026-08-30 by polling `sntp_get_sync_status()`.

## The upload-button myth

Commits: `2e12a16` (documented as always required), corrected 2026-09-05.

Uploads were first documented as needing a physical power-button press.
During the boot-delay investigation, `pio run -e <env> -t upload
--upload-port COMx` succeeded 10+ times with no button: esptool's RTS reset enters the
bootloader whenever the board is powered (awake mid-cycle, or idling in
`TIER1_TEST`'s command loop). The button is only needed mid-sleep, when the
PM1 has cut power and the port doesn't exist -- a different error
(`Could not open COMx ...` / `FileNotFoundError`).

## Canvas rewrite and the palette-index hazard

Commits: `caa89b5`, `2e12a16`, `f9da748`.

Moving the render into an off-screen `palette_4bit` `M5Canvas` turned the
six `C_*` color constants into palette indices 0-5. Passing one straight to
`M5.Display` (still RGB888) silently draws near-black `0x000001` instead of
white -- the ghost-clear pass had to be routed through the canvas. The same
week, sampling `now` once into `Snapshot` (`f9da748`) fixed a race where the
header clock, next-event label, now-line and footer could disagree across a
minute boundary, and made the render a pure function of `Snapshot` --
the property every later test depends on.

## Fifty seconds in M5.begin()

Measured 2026-09-05 (Tier 1 work, around `cba9b85`).

`M5.begin()` takes ~50.3-50.5 s on every boot, both envs, across ~10 boots:
~17 s in `Display.init()` (board/panel autodetect), ~33 s in `_begin(cfg)`.
Disabling `internal_rtc`, `internal_imu`, `internal_mic` and `internal_spk`
together changed neither number by a millisecond. PMIC auto-probing in
`Power_Class` is the untested next candidate. Getting even this split
required a detour, because vendored-library logging turned out to be dead
(next section).

## Five ways ESP_LOG didn't reach the console

2026-09-05.

Raw `ESP_LOG*` calls in vendored `.cpp` files never reached the USB CDC
console. Five attempts, all negative:
1. `-DCORE_DEBUG_LEVEL=5` globally -- worse: unlocked framework-wide
   `log_v()`/`log_d()` (I2C/SPI/PSRAM internals), flooded even a 32 KB TX
   buffer during the ~50 s `M5.begin()` window, and evicted lines that had
   shown reliably (e.g. `esp32-hal-i2c.c`'s `i2cInit()`).
2. `esp_log_level_set("M5GFX", ESP_LOG_VERBOSE)` -- the runtime gate is
   `esp32-hal-misc.c`'s unconditional `esp_log_level_set("*",
   CONFIG_LOG_DEFAULT_LEVEL)`, with `CONFIG_LOG_DEFAULT_LEVEL` baked into the
   precompiled `sdkconfig.h` as 1 (ERROR only).
3. (2) plus `Serial.setDebugOutput(true)` (`HWCDC::setDebugOutput()` ->
   `ets_install_putc2()`).
4. A manual `ESP_LOGI("M5GFX", ...)` in `main.cpp` itself with (2)+(3).
5. (4) plus `esp_log_set_vprintf()` pointed at a `Serial` writer.
Root cause not found. What did work: Arduino's `log_i()`/`log_e()`, and
plain `printf()` bracketing inside functions header-inlined into
`main.cpp.o` (e.g. `M5Unified::begin()` in `M5Unified.hpp`).

## The transfer time that tracked the timeout

Commits: `2cc39f2` (2026-09-08), `caf547c` (fix, 2026-09-09/10).

`hil.py` framebuffer reads first used pyserial's `readline()` (one blocking
syscall per byte on Windows) with the default 4096-byte driver buffer; the
2026-09-08 fix moved to bulk reads and a 128 KB buffer. The measured
"transfer time" then read ~10.2 s, ~16 KB/s, apparently invariant across
`CHUNK_BYTES` of 4/8/16/32 KB, and was blamed on a `USB_SERIAL_JTAG`
ceiling; `--timeout` was raised to 30 s on that basis. All wrong: raising
`--timeout` from 10 s to 30 s to 60 s moved "transfer time" in lockstep
(10230 ms, 30212 ms, 60203 ms) -- a real measurement doesn't move with an
unrelated timeout. `_LineReader` requested a fixed 64 KB (`RX_BULK_READ`) on every underlying
`Serial.read(n)`, and
Windows' COMMTIMEOUTS (`ReadIntervalTimeout=0`,
`ReadTotalTimeoutMultiplier=0`, `ReadTotalTimeoutConstant=timeout*1000`)
make `ReadFile` wait the whole timeout for the full count; the last
`Serial.read(65536)` (~31 KB left) sat out the timeout on data that had arrived in ~200 ms. The
`CHUNK_BYTES` invariance was the same artifact (the host request size never
changed), so whether `CHUNK_BYTES` matters is still unproven. Real transfer:
~220-235 ms, ~700 KB/s, consistent with the device's own `DUMP_MS` enqueue
figure (~160 ms). The earlier 16 KB device-side write batching was a
separate, real fix (thousands of tiny `Serial.write()` calls serializing on
per-call overhead).

## Fixtures that drifted, and exit codes that lied

Commits: `38cb053`, `44b9047`, `6e7988c` (2026-09-09).

The hand-typed `events[]` in `example.json` had drifted from `tide[]`: two
of four events sat 45-90 minutes from the curve's real peaks/troughs, one
with a height the curve never reached -- and nothing in the JSON looked
wrong until a render. Fixtures have been generated from one analytic model
since. Separately, `hil.py` used a bare `sys.exit(1)` for both "render
differs" and "board unplugged", and connection failures during `Device()`
construction (outside the `try` in all four commands) escaped as raw
tracebacks. Fixed 2026-09-10: `Device.__init__` wraps
`serial.SerialException` as `DeviceError`, `cmd_render`/`cmd_test` catch
fixture-loading failures, and `Device(...)` moved inside each `try`. Exit
codes 1 and 2 now mean different things.

## Extracting the render module, and a macro that ate a constant

Commits: `3e24d5c`, `7391af5`, `34bf24b`; plan in
`docs/superpowers/plans/2026-09-13-render-module-extraction.md`.

The draw code moved from `main.cpp` into `render.h`/`render.cpp` without
`Arduino.h`, to be portable to a host build. A local `constexpr float
DEG_TO_RAD` was silently replaced by Arduino.h's `#define DEG_TO_RAD` --
which is still in the translation unit via `M5GFX.h` -> `Bus_SPI.hpp` ->
`datawrapper.hpp` -- giving `expected unqualified-id before numeric
constant`. Renamed `kDegToRad`. `Snapshot` also gained `stationLabel` so
`render.cpp` wouldn't need `config.h` (which holds Wi-Fi credentials). The
extraction was confirmed 5/5 PASS on hardware 2026-09-14, briefly pausing
the battery soak.

## Building the SDL host preview

Commits: `d20dd59` (feasibility, 2026-09-13), `bd381d3` (done, 2026-09-16).

As of 2026-09-13 the machine had no MSYS2, no gcc/g++, no SDL2, no
`native` platform. M5GFX already vendored LovyanGFX's SDL backend
(`lgfx/v1/platforms/sdl/`, an empty TU on ESP32 behind `#if
defined(SDL_h_)`), selected automatically by `device.hpp` when neither
`ESP_PLATFORM` nor `ARDUINO` is defined, with a working example in
`examples/PlatformIO_SDL/`. Getting it to render hit three bugs, none in
`render.cpp`: mingw hiding `localtime_r()`; `sdl_main.cpp` compiled into the
ESP32 envs (its `#error` guard fired but GCC kept going, burying the real
error under `'lgfx::Panel_sdl' has not been declared`); and a blank window
because M5GFX's SDL board table sets rotation 1 (600x400) while the canvas
is 400x600, so `canvas.pushSprite(0, 0)` blitted onto a parent it didn't
fit. Making it byte-identical to the device hit two more: a
`"STATION"` fallback label instead of `"GOLDEN GATE"`, and every timestamp
7 hours off because the host used the PC's Pacific timezone while
`TIER1_TEST` renders under unconfigured UTC.

## The hour-late wake alarm

Commits: `65463bc`, `237ef2f`, `96bcadc`; notes
`notes/interval-experiment-2026-09-15.md`, `notes/soak-2026-09-11.md`.

Soak 1 cycled every 89.3 min against `UPDATE_MINUTES=30`. First read as
"almost exactly 3x", but against the right baseline (~78 s awake + 30 min =
~31.3 min) the excess was ~58 min and the ratio ~2.85x. Three outcomes were
pre-registered before a rerun at `UPDATE_MINUTES=2`: A no bug (~3.3 min), B
multiplicative (~10 min), C fixed additive (~61 min). Two runs (cycles
355-360 and 362-368) landed on C. The cause: `struct tm nextTm = {}` leaves
`tm_isdst = 0`, so during PDT `mktime()` read the wake time as PST digits,
one hour late; dormant outside DST. A diagnostic that compared `mktime()`'s
output with a `localtime_r()` round trip first seemed to rule this out --
the two agreed, because both were correct about an already-wrong epoch.
Decoding the epoch to UTC and comparing both interpretations settled it.

## The battery conclusion that flipped

Commits: `65463bc`, `7b52c2d`, `5c5c8ba`.

Soak 1 (100% -> 98% over 45 cycles, 66h57m) was read as "month-shaped, not
week-shaped" drain, suggesting the ~50 s `M5.begin()` cost didn't matter.
Soak 2 (100% -> 46% over 375 cycles, 8.24 days) drained 0.144%/cycle vs soak
1's 0.044%/cycle -- ~3.3x per cycle -- projecting ~15 days per charge. The
gauge may read flat near full charge; unconfirmed. A 12-cycle segment of
soak 2 ran at ~29.2 min/cycle, which the minute-resolution RX8130 alarm
can't produce; unexplained. Reading `HWCDC.cpp` then showed every battery
wake had waited the full 15 s for a USB host that wasn't there, so the real
awake time on battery was ~93 s, not the ~78 s measured with USB attached.

## Bugs found by reading, not by failure

Commits: `6d5d443`, `b9607a8`.

Two defects were found by reading framework source rather than from a
symptom. `WiFiClientSecure`'s TLS handshake timeout defaults to 120 s on a
non-blocking socket, so the "15 s HTTP timeout" never bounded the handshake.
And after a failed SNTP sync, `setup()` still wrote the RTC-seeded clock --
which reads 7-8 h early under `TZ_STRING`, because the seed treats the RTC's
local digits as UTC -- back into the RTC.

## Proving the pins changed nothing

Commit: `81f0813` (full byte-level analysis in its message).

Pinning libraries changed `firmware.bin`. Every differing byte was traced:
four `__FILE__` strings containing the libdeps folder name, the
`app_elf_sha256` field, and the image trailer. The test env, 144 bytes
shorter, was rebuilt with `-fmacro-prefix-map` restoring the old folder
name and then differed only in the hash field and trailer. Along the way,
the VS Code PlatformIO extension was caught installing the unpinned
`lib_deps` into the default env mid-build, and `pio run -v` was found to
crash while printing the `firmware.bin` command. Unpinned, a fresh clone
would already have fetched newer untested commits (M5GFX `b863d50`,
M5Unified `8b63555`, registry M5GFX 0.2.30).
