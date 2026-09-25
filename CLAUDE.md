# Tide & Wind Tracker — M5Stack PaperColor (SKU C151)

Battery-powered e-paper dashboard showing tide state and wind conditions for a
coastal location. Wakes on a timer, fetches data over Wi-Fi, redraws, sleeps.

---

## Project status
The project's finish line is defined in DONE.md. Read it before planning work, and treat anything not listed there as out of scope.

---

## Current state (as of 2026-09-14)

**Works, confirmed on real hardware:**
- Build pipeline: `pio run` regenerates `src/layout.h` from `layout.json` and
  compiles clean.
- Full cycle: Wi-Fi connect -> NTP -> NOAA tide fetch -> Open-Meteo wind fetch,
  all succeeding.
- The complete draw: header, now-strip (tide value, trend arrow, NEXT
  HIGH/LOW), tide curve, wind section (compass rose with N/E/S/W labels and a
  barbed direction arrow, speed-color chip, hand-drawn degree ring), forecast
  bars/label/hour-ticks, and footer -- all render correctly and are legible on
  the panel, with no remaining known gaps vs. the `tools/preview.py` design
  (`preview_only` in `layout.json` now holds only preview's own PIL
  font-metric offsets, not missing features).
- Real fonts throughout, replacing the original tiny `Font0` bitmap font (see
  Verified corrections below). SHT40 indoor temp/humidity read.
- The real low-power sleep path: `setup()` ends in `sleepUntilNext()` (PM1
  shutdown + RX8130 wake alarm) rather than a software restart. Confirmed
  cycling correctly -- PM1 shutdown -> RX8130 wake -> cold boot -> full fetch
  -- at both a shortened test cadence and the production 30-minute cadence,
  including the PM1-RTC-RAM-backed cycle counter surviving real power-off.
- The NEXT HIGH/LOW block and tide-curve dots, which had gone persistently
  blank across many consecutive cycles -- previously misdiagnosed as an
  occasional NOAA hilo HTTP failure (see Verified corrections below for the
  real cause: a bad system clock, not the fetch). Root-caused and fixed
  2026-08-30 by polling `sntp_get_sync_status()` instead of trusting
  `getLocalTime()`; confirmed showing correctly on real hardware afterward.
- Rendering is canvas-buffered (`src/main.cpp`, 2026-09-04): `drawAll()`
  composes the full frame into an off-screen `M5Canvas` and pushes it with a
  single `pushSprite()` + one `display()` call, instead of each primitive
  drawing straight to `M5.Display`. Confirmed on real hardware: correct
  visual output, and canvas allocation shifts `ESP.getFreeHeap()` by only 64
  bytes (buffer lives in PSRAM -- see Verified corrections).
- The `src/render.h`/`src/render.cpp` extraction (2026-09-13, see Verified
  corrections) is pixel-identical on real hardware: `python tools/hil.py
  test --all` against all 5 goldens (`calm`, `example`, `high_wind`,
  `long_station`, `no_events`) is **5/5 PASS, exit 0** -- confirmed
  2026-09-14, briefly interrupting the battery soak test (flashed
  `m5stack-papercolor-test`, ran the suite, flashed `m5stack-papercolor`
  back to resume it; the PM1-RTC-RAM cycle counter is untouched by
  reflashing, so the soak's cycle count continued from where it left off).
- The host-side SDL preview (`pio run -e native -t upload`,
  `src/sdl_main.cpp`): links the real M5GFX/LovyanGFX drawing code in
  `src/render.cpp` against M5GFX's own SDL backend, instead of
  `tools/preview.py`'s separate PIL reimplementation of the same layout.
  Toolchain (MSYS2, MinGW-w64 GCC, SDL2 dev files, PlatformIO's `native`
  platform) installed and working, 2026-09-16. Confirmed **byte-for-byte
  identical** to the real device's output: `tools/hil.py decode-raw`
  against a `SDL_PREVIEW_DUMP_RAW` dump of `test/fixtures/example.json`
  reports `MATCH` against `test/golden/example.png` -- not just visually
  similar, the exact same bytes. Three real bugs found and fixed getting
  there (a rotation mismatch, a timezone mismatch, and a wrong fallback
  station label -- none in `render.cpp` itself) -- see "Verified
  corrections".
- **A run-to-empty battery test is in progress -- do not interrupt it.**
  Started 2026-09-16 (cycle 385, 100%), expected to end around 2026-10-01.
  Until it does: no flashing, no USB connection (USB charges the battery
  and invalidates the measurement), so no `pio run -t upload`, no
  `pio device monitor`, no `tools/hil.py`. Firmware and render changes can
  still be written, built with plain `pio run`, and checked with the SDL
  build (`pio run -e native`), then flashed together afterwards. See the
  sequencing rule in DONE.md.
- Battery soak 1 (pre-`tm_isdst`-fix firmware): started cycle 289 on
  2026-09-11 at 15:11 at 100% battery, ended cycle 334 on 2026-09-14 at
  10:08 at 98% -- 45 cycles over 66h57m. Two findings:
  - **Corrected 2026-09-24:** this soak's 100%->98% was originally read as
    "battery drain is month-shaped, not week-shaped", and taken to mean the
    ~50s `M5.begin()` cost wasn't a real battery problem. Soak 2 (below)
    overturns that: it drained ~3.3x faster *per cycle* (0.144%/cycle vs
    0.044%/cycle here), so the slower pre-fix cadence doesn't explain the
    gap. The cause is unconfirmed: the battery gauge may be nonlinear near
    full charge (e.g. voltage-based, or pinned at 100% above some
    threshold), which would make a short run starting at 100% under-report
    its drain.
    Don't use soak 1's figures for any battery-life estimate.
  - The observed interval was **89.3 minutes/cycle against a configured
    `UPDATE_MINUTES=30`** (`src/config.h`). Root-caused 2026-09-16: not a
    multiplicative bug (a follow-up rerun at `UPDATE_MINUTES=2` added the
    same ~58-60 min excess, not a scaled one) -- `nextTm.tm_isdst` in
    `sleepUntilNext()` defaulted to 0 instead of -1, so `mktime()` resolved
    the wake time under the wrong (standard, not daylight) UTC offset. See
    "Verified corrections" for the full mechanism. Fixed in source
    (`nextTm.tm_isdst = -1;` before the `mktime()` call, commit `237ef2f`)
    and **confirmed on real hardware, 2026-09-16**: a fresh soak segment
    (cycle 385, 14:32:48 -> cycle 397, 20:22:38, same day) ran at ~29.2
    min/cycle -- in line with the ~31.3 min expected baseline (~78s awake +
    30 min) and nowhere near the old ~89.3, confirming the fix.
    **Caveat (2026-09-24):** that ~29.2 figure is inconsistent with the
    RX8130 wake alarm, which has minute resolution (no seconds field --
    `RX8130_Class.cpp` `setAlarmIRQ()`). With the alarm set 30 min after
    each cycle ends, a ~78-93s awake time should give periods of at least
    31 min, never under 30. Unexplained and not investigated. The fix is
    confirmed instead by soak 2's full-run average, 31.7 min/cycle over
    375 cycles (below), which does fit. The
    100%->98% figure from the *original* (pre-fix) soak still shouldn't be
    used for a days-per-charge estimate (see "Design constraints"), but the
    cycles-per-day term itself is no longer in question.
- Battery soak 2 (post-fix firmware: the `tm_isdst` fix committed as
  `237ef2f`; no ESP32 firmware source has changed since): cycle 385 on
  2026-09-16 at 14:32 at 100% -> cycle 760 on 2026-09-24 at 20:23 at 46%
  -- 375 cycles over 197h51m (**8.24 days**), **31.7 min/cycle** (~45.5
  cycles/day, matching the ~31.3 min expected baseline across the whole
  run, not just its first 12 cycles). Drain: 54 points in 8.24 days =
  **~6.55%/day, 0.144%/cycle**, a linear projection of **~15 days per
  charge -- about half the ~1 month design goal.** Raw readings in
  `notes/soak-2026-09-11.md`. **Gauge caveat:** the projection assumes
  `M5.Power.getBatteryLevel()` is linear in remaining capacity, which is
  unverified (how the gauge works is unconfirmed), and the device may stop
  completing cycles before the gauge reads 0% -- so ~15 days is an
  estimate, not a measurement. This soak is continuing as the
  run-to-empty (above), which produces the measured figure. The
  2026-09-24 reading was taken with USB attached for the serial monitor,
  for about 2 minutes (unplugged since); any charge that added inflates
  the final days-per-charge figure. The run-to-empty measures `237ef2f`
  firmware, which fetches current wind from Open-Meteo only -- the
  observed-wind change below is not in it and adds awake time, so the
  measured figure is for the pre-observed-wind firmware. `237ef2f` also
  still has the ~15.5s USB serial wait on every battery wake (see the
  serial-wait entry below), so its awake time on battery is ~93s, not the
  ~78s measured with USB attached.

**Not yet exercised:**
- Observed wind (implemented and built with `pio run`, **not yet
  flashed** -- held until the run-to-empty ends). `fetchWind()`
  (`src/main.cpp`) now fetches NOAA `product=wind`, `date=latest` for the
  current speed, direction, and gust, uses it only if the reading is within
  30 min of now, and otherwise keeps Open-Meteo's `current` values.
  Open-Meteo is still fetched every cycle for the forecast bars. Each cycle
  logs a `WINDSRC src=obs|fcst|none reason=... age_min=...` line. Adds one
  HTTPS request per cycle: an estimated 1-2s more awake time (unmeasured),
  on a 5s timeout. Same change: `httpGetJson()` takes a per-call timeout
  and now also applies it to the TLS handshake, which previously kept
  WiFiClientSecure's 120s default.
- Skipping the USB serial wait on battery (implemented and built with
  `pio run`, **not yet flashed**). `setup()`'s `while (!Serial && ... <
  15000)` wait ran the full 15s (plus a 500ms delay) on every battery
  wake. `Serial` is `HWCDC` here (`ARDUINO_USB_MODE=1`), and `!Serial`
  stays true without a host: `isPlugged()` goes false within ~5ms of the
  last USB start-of-frame packet, and a host is the only thing that sends
  them. Confirmed by reading the framework's `HWCDC.cpp`, not measured on
  hardware. The wait now runs only when `Serial.isPlugged()` is true, so
  behavior with a host attached (the monitor and `hil.py` workflow) is
  unchanged. `Serial` writes and `flush()` don't block without a host
  (they drop data), so the rest of the logging costs nothing on battery.
- Current draw. The 92.53uA standby and ~150mA average-awake figures are
  still datasheet/arithmetic estimates, not measurements on this board --
  see "Design constraints" below and the `M5.begin()` entry in "Verified
  corrections". Real measured awake time is ~78s/cycle against an original
  ~20s assumption, and per soak 2 above that gap does matter: projected
  battery life is ~15 days, not ~1 month. The run-to-empty measures
  days-per-charge directly, so a current measurement is now only needed to
  attribute energy across `M5.begin()`, Wi-Fi, and the panel refresh.

**Next steps:**
DONE.md is the authoritative scope. This section is the working to-do list toward it; anything here that doesn't serve a DONE.md criterion is out of scope.
1. Let the run-to-empty finish (~2026-10-01, see "Current state"; do not
   interrupt it). "Empty" = the device no longer completes a wake cycle.
   Record the last completed cycle's time -- the frozen frame's footer
   (`UPD HH:MM`) shows it, so check the panel daily to pin down the date --
   and its cycle number (estimate from elapsed time at ~31.7 min/cycle if
   it can't be read back). Then enter the measured days-per-charge in
   "Design constraints", compare it to the ~1 month goal, and document the
   shortfall, stating the firmware version measured (`237ef2f`).
2. Wi-Fi `NO_AP_FOUND` still unresolved. `connectWifi()` (`src/main.cpp`)
   only distinguishes connected vs. not, via `WiFi.status() != WL_CONNECTED`
   in a timeout loop -- it doesn't log or branch on *which* status came
   back, so a `NO_AP_FOUND` occurrence can't yet be told apart from a wrong
   password, a timeout, or the AP being out of range.

**After the run-to-empty ends** (everything waiting on hardware; do none of
it before then, except the baseline timing, which only needs watching):
- [ ] Baseline timing on `237ef2f`, on battery -- can be done now without
      touching the device. The wake alarm fires on a whole minute (the
      RX8130 alarm has no seconds field), 30-31 min after the footer's
      `UPD HH:MM`. Time from that minute to the visible start of the panel
      refresh, over 2-3 wakes.
- [ ] Record the run-to-empty result (Next steps #1).
- [ ] Flash the current firmware (`pio run -t upload --upload-port COMx`).
      The battery charges over USB meanwhile.
- [ ] With the monitor attached from boot: the log starts at the first
      line as before (the serial wait still runs with a host), and the
      first few `WINDSRC` lines show `src=obs`.
- [ ] Flash `m5stack-papercolor-test`, run `python tools/hil.py test
      --all` (expect 5/5 PASS), then flash `m5stack-papercolor` back.
- [ ] Unplugged, on battery: repeat the baseline timing. Expect the panel
      refresh to start ~14s earlier (~15.5s serial wait removed, minus the
      estimated 1-2s the observed-wind request adds).

---

## READ THIS FIRST (instructions for Claude)

Your training data is unreliable for this board. It is **not** the older
monochrome M5Paper (S3). The panel, the power management IC, and parts of the
API differ. Before writing or changing any hardware-facing code:

1. **For library APIs, read `.pio/libdeps/m5stack-papercolor/`.** This is the
   authoritative source: it is the exact code being compiled into the binary,
   at the exact versions PlatformIO resolved. Do not infer method names, and do
   not read a separate clone that may have drifted from what is built.
   - `M5Unified/`, `M5GFX/`, `M5PM1/`, `ArduinoJson/`
     (`M5GFX/` may actually be `M5GFX@src-<hash>/` -- hash tied to the
     resolved git commit -- look for that if the plain name doesn't exist)
   - If this directory is empty, run `pio run` once to populate it.
2. **For how M5 actually drives this hardware, read
   `refs/M5PaperColor-UserDemo/`.** This is the factory firmware and is the
   single best reference for the panel power-up sequence, the PM1 rails, and
   the low-power sleep path.
3. If a symbol appears in neither location, say so instead of guessing.

Set up `refs/` once:

```bash
cd refs
git clone --depth 1 https://github.com/m5stack/M5PaperColor-UserDemo
```

The libraries do **not** need cloning — `pio run` fetches them into
`.pio/libdeps/`.

---

## Hardware facts

| Item | Value |
|---|---|
| SoC | ESP32-S3R8, dual-core LX7 @240MHz |
| Flash / PSRAM | 16MB / 8MB Octal |
| Display | 4" E Ink Spectra 6 (E6), ED2208-DOA / EL040EF1, **400x600 portrait** |
| Refresh time | **15–30 seconds**, longer with more color complexity |
| Colors | **6 only**: black, white, red, yellow, blue, green. No grayscale, no blending. |
| Sensors | SHT40 temp/humidity (I2C 0x44) |
| RTC | RX8130CE (I2C 0x32) |
| PMIC | M5PM1 (I2C 0x6e) |
| Battery | 1250mAh |
| Power | standby 92.53uA, full load 211.97mA |

### Pinmap

E-paper (SPI): `G15` CLK, `G13` MOSI, `G44` CS, `G43` DC, `G11` BUSY, `G12` RST.
Panel power is gated by the PM1 on `PYG0` (`PY_EPD_EN`) — **the panel will not
work if this rail is off.**

microSD (shares the SPI bus with the panel): `G47` CS, `G15` CLK, `G13` MOSI,
`G14` MISO. SD power is `PYG3` (`PY_SD_PWR_EN`) on the PM1.

Buttons: `G10` = A, `G9` = B, `G1` = C.
RGB LEDs: `G21` (power gated by PM1 `LDO3V3_EN_PP`).
IR TX: `G48`.
System I2C (SHT40, RTC, PM1): `G2` SCL, `G3` SDA.
Grove PORT.A (HY2.0-4P): `G4`, `G5`, power direction via PM1 `BOOST5V_EN_PP`.

### Gotchas that have bitten people

- SD and EPD share SPI. Don't hold both transactions open.
- Ghosting is a common complaint on this panel. Budget a periodic full-refresh
  / clear cycle, not just partial updates.
- Nothing on this display should rely on anti-aliasing or gradients. Every
  pixel must land on one of the six palette colors.

### Verified corrections (do not re-litigate)

- `M5Unified` has **no** `Sht4x` member on this board. The SHT40 at I2C 0x44
  must be read with raw I2C transactions over the shared system bus --
  `M5.In_I2C.start()/write()/read()/stop()` -- not the standalone Arduino
  `Wire` object. Command 0xFD (high-precision measure), ~10ms delay, read 6
  bytes, decode with the Sensirion SHT4x linear formula. Matches
  `refs/M5PaperColor-UserDemo/main/hal/hal.cpp Hal::sht40Read()`. Confirmed by
  compile error + reading source, 2026-08-28.

- `Serial` is native USB CDC on this board (`ARDUINO_USB_CDC_ON_BOOT=1` in
  `platformio.ini`), not a UART-to-USB bridge chip. There's no hardware
  buffering the port while the host OS enumerates it, so anything printed to
  `Serial` right after `Serial.begin()` is lost if a terminal attaches after
  boot rather than before. Wait on `while (!Serial && millis() - start <
  TIMEOUT) delay(10);` with a timeout -- the board must still boot fine with
  nothing attached (e.g. running on battery in the field), so don't wait
  forever. Confirmed 2026-08-28.

- M5GFX ships **no bold DejaVu** -- only regular-weight `DejaVu9/12/18/24/40
  /56/72` (GFXfont, converted straight from `DejaVuSans.ttf`, named by pixel
  line-height, in `.pio/libdeps/m5stack-papercolor/M5GFX@src-<hash>/src/
  lgfx/Fonts/Custom/` -- hash tied to the resolved git commit). For bold, the closest bundled family is Adafruit's "Free Fonts"
  `FreeSansBold9/12/18/24pt7b` (nominal point size, not pixel height, and a
  different type family -- visually close, not identical). Mapping settled
  on for this project (see the comment above `drawHeader()` in
  `src/main.cpp`): F_HUGE->`FreeSansBold18pt7b`(42px), F_BIG->
  `FreeSansBold12pt7b`(29px), F_MED->`FreeSansBold9pt7b`(22px), F_REG->
  `DejaVu18`(18px), F_SMALL->`DejaVu12`(13px), F_TINY->`DejaVu9`(10px).
  Every bundled GFXfont's charset is **ASCII-only (0x20-0x7E)** -- no
  ▲/▼/°/· glyphs, unlike `tools/preview.py`'s PIL-rendered TTF. Hand-draw
  those instead (`fillTriangle`, `drawCircle`) rather than printing the
  Unicode character -- see the trend triangle in `drawNowStrip()` and the
  degree ring in `drawWind()`. Also: the font before any `setFont()` call is
  `Font0`, a 6x8 GLCD bitmap font -- `setTextSize()` alone changes scale, not
  font family, and does not get you a bigger typeface. Confirmed by reading
  `.pio/libdeps/m5stack-papercolor/M5GFX@src-<hash>/src/lgfx/Fonts/` and
  testing on-device, 2026-08-28.

- `M5.begin()` does **not** power the e-paper rail on this board. M5Unified's
  own board-init switch for `board_M5PaperColor`
  (`M5Unified/src/utility/Power_Class.cpp`) only enables the RGB LDO and PM1
  GPIO3 (SD power) — it never touches GPIO0 (`PY_EPD_EN`). The panel stays
  blank unless the app drives PM1 GPIO0 high itself, e.g. with the standalone
  `M5PM1` library (`pm1.pinMode(M5PM1_GPIO_NUM_0, OUTPUT);
  pm1.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);`), matching
  `refs/M5PaperColor-UserDemo/main/hal/hal.cpp Hal::init()`. Confirmed by
  reading source, 2026-08-28.

- The 92.53uA standby figure requires a full PM1 shutdown, **not**
  `M5.Power.timerSleep()` / `esp_deep_sleep_start()`. On this board
  `Power_Class::_powerOff()` takes the `esp_sleep_enable_ext0_wakeup(GPIO_7)`
  branch (PaperColor sets `_rtcIntPin = GPIO_NUM_7`) and skips the PM1
  shutdown call entirely — the PM1's LDO/DCDC rails stay powered through
  the "sleep." The low-power path is the one the factory demo uses
  (`hal.cpp` + `app_manager.cpp`): configure PM1 GPIO2 as a WAKE pin tied to
  the RX8130's nIRQ (`gpioSetFunc(..., M5PM1_GPIO_FUNC_WAKE)`, pull-up,
  falling edge, wake-enable), schedule an RX8130 alarm
  (`M5.Rtc.setAlarmIRQ()`), then call `pm1.shutdown()`. This cuts power to
  the whole board, and the RX8130 alarm re-powers it via the PM1's external
  wake input. Confirmed by reading source, 2026-08-28. **Confirmed working
  on real hardware, 2026-08-29**: `setup()` now calls `sleepUntilNext()` for
  real, and the board was observed cycling PM1 shutdown -> RX8130 wake ->
  cold boot -> full fetch cycle repeatedly at both a shortened 2-minute test
  cadence and the production 30-minute cadence.

- Because that shutdown is a real power cut, **nothing in RAM survives
  between cycles** — `RTC_DATA_ATTR` resets just like a cold boot, since the
  ESP32 itself loses power along with everything else. Only the PM1's own
  battery-backed RTC RAM (`pm1.writeRtcRAM()` / `readRtcRAM()`, 32 bytes)
  persists across a cycle. Anything that needs to survive (e.g. a
  full-refresh cadence counter) must live there instead of in a static or
  `RTC_DATA_ATTR` variable. Confirmed by reading source, 2026-08-28.

- Schedule the RX8130 wake alarm off `M5.Rtc.getDateTime()` (the RTC's own
  battery-backed clock), not off `time(nullptr)` / the system clock. The
  system clock only becomes correct after a successful NTP sync each cycle;
  the RTC stays correct even on a cycle where Wi-Fi or NTP fails, as long as
  it's kept synced (`M5.Rtc.setDateTime()`) whenever NTP does succeed.
  Matches `Hal::scheduleNextWakeMinutes()` in the reference demo. Confirmed
  by reading source, 2026-08-28.

- On this Windows dev machine, `pio device monitor`'s upload/monitor port
  auto-detect is **not reliable** once the board's native USB CDC has
  actually vanished (i.e. mid-PM1-shutdown) -- it can silently latch onto an
  unrelated port (seen once: "Intel Active Management Technology - SOL
  (COM3)", a chipset management interface, not the board) and produce a
  monitor session that looks normal but never shows any real output. To
  watch for or flash during a real sleep/wake cycle: poll
  `Get-CimInstance -ClassName Win32_PnPEntity | Where-Object { $_.Name -match
  'USB Serial Device' }` to see the board's actual COM port appear/disappear,
  and pass `--port`/`--upload-port` explicitly (e.g. `COM4`) rather than
  trusting auto-detect. Confirmed 2026-08-29 while validating the sleep path.

- NOAA CO-OPS's `begin_date` parameter does **not** accept the `today`
  keyword the way the plain `date` parameter does -- `begin_date=today`
  returns `{"error":{"message":" Wrong Date..."}}` and silently empties the
  response (confirmed with a direct `curl` against the live endpoint).
  Compute an actual `yyyyMMdd` string instead (`strftime(..., "%Y%m%d", ...)`
  in `main.cpp`, `dt.date.today().strftime("%Y%m%d")` in `preview.py`) when
  using `begin_date`+`range` to widen a query past what `date=today` covers.
  Confirmed 2026-08-29.

- ArduinoJson's `deserializeJson(doc, ...)` calls `dst.clear()` internally
  before parsing, every time (`Deserialization/deserialize.hpp`,
  `doDeserialize()`) -- so reusing the same `JsonDocument` across sequential
  HTTP retry attempts is safe; a failed attempt can't leave stale/partial
  data that a later successful attempt would merge with. Confirmed by
  reading source, 2026-08-29.

- M5GFX's `setTextDatum()` only affects the bare 2-arg-position
  `drawString(str, x, y)` call (it reads the persisted `_text_style.datum`).
  `drawCenterString()`/`drawRightString()` pass their own explicit datum
  override per call and ignore whatever `setTextDatum()` last set, and
  `setCursor()`+`print()` ignores datum entirely (it's cursor-based, not
  anchor-based) -- so existing `print()`/`drawRightString()` call sites don't
  need to change around a `setTextDatum()` call, but it's still good hygiene
  to reset to `textdatum_t::top_left` right after using `middle_center` for
  true 2-axis-centered text (e.g. the compass N/E/S/W labels in `drawWind()`)
  in case future code adds a bare `drawString()` call. Confirmed by reading
  `M5GFX@src-<hash>/src/lgfx/v1/LGFXBase.hpp` (hash tied to the resolved
  git commit) and an example `.ino`'s usage, 2026-08-29.

- Arduino-ESP32's `getLocalTime()` (`esp32-hal-time.c`) does **not** mean "NTP
  has synced" -- it just loops until `time(nullptr)`'s `tm_year > 2016`, with
  no check that a real SNTP round-trip ever happened. On this board,
  `M5.begin()` calls `M5.Rtc.setSystemTimeFromRtc()` internally
  (`M5Unified.cpp _begin_rtc_imu()`, confirmed by reading source) *before*
  Wi-Fi/NTP even starts, seeding the system clock from the RX8130's stored
  value. If that stored value is already a plausible-looking date (any year
  > 2016), `getLocalTime()` returns success on its very first check, `setup()`
  then writes that same stale time straight back via `M5.Rtc.setDateTime()`,
  and the real NTP correction (if it ever lands) applies asynchronously later
  in the cycle via ESP-IDF's own SNTP callback -- with no guarantee it lands
  before the rest of `setup()` reads the clock again. This was the actual
  cause of a multi-day-long streak (not just one cycle) of the NEXT HIGH/LOW
  block and tide-curve dots going blank: `fetchTides()`'s `todayStr` (used to
  build the hilo `begin_date`) was computed from the stale pre-NTP clock,
  while `drawNowStrip()`'s `now` (compared against the fetched events) read
  the real, since-corrected clock later in the same cycle -- so every
  fetched event legitimately compared as already in the past, even though
  the fetch itself succeeded every time (confirmed live via a temporary
  Serial dump of parsed event epochs vs `now`, showing events dated ~9.5
  months before `now` despite a successful, non-retried fetch). Fixed by
  polling the real `sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED`
  (`esp_sntp.h`) instead of trusting `getLocalTime()` alone, before reading
  the clock for `M5.Rtc.setDateTime()`. Confirmed by reading
  `esp32-hal-time.c`, `M5Unified.cpp`, `RTC_Class.cpp`, and `esp_sntp.h`, and
  by reproducing + fixing on real hardware, 2026-08-30.

- Since the canvas-buffered rewrite (`src/main.cpp`, 2026-09-04), the six
  `C_BLACK/C_WHITE/C_RED/C_YELLOW/C_BLUE/C_GREEN` constants are **palette
  indices (0-5)**, not RGB values. The render target is an `M5Canvas` at
  `color_depth_t::palette_4bit`, and LGFX_Sprite's palette-mode draw calls
  take a palette index as the "color" argument -- confirmed by reading
  `misc/colortype.hpp`'s `convert_uint32_to_palette4()`, which just extracts
  the low nibble of whatever is passed. The real RGB values live in
  `PALETTE_RGB[6]`, consumed only by `canvas.createPalette()`. **Hazard:**
  passing `C_WHITE` etc. straight to `M5.Display` (still true RGB888) is not
  white, it's near-black (`0x000001`). Any new direct-to-`M5.Display` draw
  call must not reuse these constants -- route it through the canvas instead
  (see `clearScreenFull()`).

- `M5Canvas(&M5.Display)` defaults `_psram = true` (`M5GFX.h`), so
  `createSprite()` allocates from the 8MB PSRAM, not the ~320KB internal
  heap. Confirmed on real hardware 2026-09-04: a 400x600 `palette_4bit`
  canvas (120000 bytes) moved `ESP.getFreeHeap()` by only 64 bytes. Draws to
  the canvas never touch the panel or trigger a refresh -- only the final
  `pushSprite()` + `display()` do.

- `Panel_ED2208::display()` nearest-matches every RGB888 pixel against its
  own native 6-color table (`epd_palette[]` in `Panel_ED2208.cpp`) with an
  ordered Bayer dither, independent of whatever wrote the RGB888 -- and that
  table's ideal blue/green (`{100,64,255}`, `{67,138,28}`) don't exactly
  equal this project's `PALETTE_RGB` blue/green (`0x0000BF`, `0x007C00`).
  This was already true before the canvas rewrite; "pixel-identical" output
  was never about exact color-value equality, only about delivering the same
  RGB888 bytes into `M5.Display`'s framebuffer that direct-draw always
  produced. Confirmed by reading `Panel_ED2208.cpp`, 2026-09-04.

- The canvas buffer is the deterministic artifact for automated
  verification -- it's what a test harness should dump and compare.
  Physical panel output is not comparable byte-for-byte, because of the
  dithering above.

- `Snapshot` carries a `time_t now` field, sampled exactly once in
  `setup()` right after the `connectWifi()` conditional (unconditionally,
  so a Wi-Fi-failure cycle still gets a valid clock). All four draw
  functions read `s.now` instead of calling `time(nullptr)` independently
  -- that previously let the header clock, next-event label, tide
  now-line, and footer disagree if the render straddled a minute boundary.
  Consequence: the render path is a pure function of `Snapshot` with no
  wall-clock dependency, which is what makes reproducible automated
  rendering possible. Confirmed by reading source, 2026-09-04.

- `M5.begin()` costs **~50 seconds on every single boot** -- on both
  `m5stack-papercolor` and `m5stack-papercolor-test`, not a TIER1_TEST
  artifact. Measured with `millis()` bracketing in `setup()` (both envs) and
  confirmed live across roughly ten separate boots (test and production
  combined), consistently landing in the 50.3-50.5 second range. Splits into
  two phases, bracketed the same way inside `M5Unified::begin()`
  (`M5Unified.hpp`):
  - `Display.init()` (board/panel autodetection): ~17s.
  - `_begin(cfg)` (RTC/IMU/audio/power init): ~33s.
  Disabling `cfg.internal_rtc`, `internal_imu`, `internal_mic`, and
  `internal_spk` together (all four, at once, in `src/main.cpp`'s
  `TIER1_TEST` branch) changed neither number by a single millisecond across
  multiple trials -- **all four subsystems are ruled out**. `Power_Class`
  PMIC auto-probing (M5Stack boards commonly try several possible PMIC chip
  types before matching the real one) is the next candidate for the ~33s
  remainder, unconfirmed. This cost is currently unavoidable and not
  reducible by any config flag found so far -- it is a real property of
  `M5.begin()` on this board as used today, not a test-harness artifact.
  Confirmed live on real hardware (both envs, including two independent
  production wake cycles), 2026-09-05.

- Raw `ESP_LOG*` calls inside vendored library `.cpp` files (confirmed for
  `M5GFX.cpp`; `M5Unified.cpp` uses no `ESP_LOG` tag at all) **do not reach
  the USB CDC console on this build, by any mechanism tried**. Five
  independent attempts, all negative: (1) `-DCORE_DEBUG_LEVEL=5` globally --
  made things *worse* by unlocking `log_v()`/`log_d()` framework-wide
  (I2C/SPI/PSRAM internals, not just the library of interest), flooding even
  a 32KB TX buffer during the ~50s `M5.begin()` window and evicting lines
  that previously showed reliably; (2) scoped
  `esp_log_level_set("M5GFX", ESP_LOG_VERBOSE)` alone (traced the actual
  runtime gate to `esp32-hal-misc.c`'s unconditional
  `esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL)`, with
  `CONFIG_LOG_DEFAULT_LEVEL` baked into the precompiled framework's
  `sdkconfig.h` as `1`/ERROR-only -- confirmed by reading
  `tools/sdk/esp32s3/qio_opi/include/sdkconfig.h`); (3) `(2)` plus
  `Serial.setDebugOutput(true)` (traced through `HWCDC::setDebugOutput()`'s
  `ets_install_putc2()` mechanism in `HWCDC.cpp`); (4) a manual
  `ESP_LOGI("M5GFX", ...)` call placed directly in `main.cpp` itself (not a
  vendored file, ruling out vendored-code timing/ordering) with `(2)`+`(3)`
  active; (5) `(4)` plus `esp_log_set_vprintf()` explicitly pointed at a
  function that writes to `Serial` -- the actual documented ESP-IDF API for
  redirecting log output. Root cause not found; would require reading
  `esp_log`'s own implementation, not just its headers. Meanwhile, Arduino's
  own `log_i()`/`log_e()` macros (`esp32-hal-log.h`, used in framework files
  like `esp32-hal-i2c.c`) show up over the same port with no special
  handling, and functions **header-inlined into `main.cpp.o`** (e.g.
  `M5Unified::begin()`, defined in `M5Unified.hpp`, not a separately
  compiled `M5Unified.cpp` translation unit) can be bracketed with plain
  `::printf()`/`Serial.printf()` and work immediately -- that's what let the
  `M5.begin()` split above get measured at all. **Any further narrowing of
  vendored-library timing must use this header-inlined bracketing
  technique, not `ESP_LOG*` calls or new prints added to a vendored `.cpp`
  file** -- those are confirmed dead on this build. Confirmed live,
  2026-09-05.

- `tools/hil.py`'s framebuffer reads used to rely on pyserial's inherited
  `readline()`, which does one blocking syscall per byte on Windows, and
  the Windows driver-level RX buffer defaults to 4096 bytes unless raised.
  Both are now fixed: `_LineReader` reads via bulk `Serial.read(n)` calls
  (capped at 64KB, but sized to what's actually waiting -- see the
  2026-09-10 entry below, a *fixed* 64KB request turned out to be its own
  bug) instead of relying on `readline()`, and `Device.__init__` raises the
  driver buffer to 128KB via `set_buffer_size()`. Don't reintroduce a bare
  `self.ser.readline()` call anywhere in this file -- route all reads
  through the shared `_LineReader` instance. Re-checkable by reading
  `serialutil.py` / `serialwin32.py` (pyserial), 2026-09-08.

- The 2026-09-08 entry above this one originally read "~10.2s, about
  16KB/s" for transfer time, called that rate invariant across
  `CHUNK_BYTES` of 4KB/8KB/16KB/32KB, and blamed a `USB_SERIAL_JTAG`
  hardware ceiling. **All of that was wrong** -- not a hardware limit, a
  host-side bug in `tools/hil.py` itself, confirmed live 2026-09-10 and
  corrected here rather than appended, so a future skim doesn't pick up
  the wrong number.
  Real transfer time (RENDER sent -> `---FB-END---` read off the wire) for
  the same ~400x600 dump (~162KB of base64) is **~220-235ms, roughly
  700KB/s** -- consistent with the device's own `DUMP_MS` enqueue figure
  (~160ms) for the first time, because it was always in the right
  ballpark; `tools/hil.py` was the thing lying.
  Mechanism: `_LineReader` requested a *fixed* `RX_BULK_READ` (64KB) on
  every underlying `Serial.read(n)` call, regardless of how much of the
  transfer actually remained. On Windows, this port's COMMTIMEOUTS
  configuration (`ReadIntervalTimeout=0`, `ReadTotalTimeoutMultiplier=0`,
  `ReadTotalTimeoutConstant=timeout*1000`) makes `ReadFile` block for the
  *entire* configured timeout waiting for the full requested count -- it
  does not return early just because some data arrived. The transfer's
  final read is almost never an exact multiple of 64KB, so its last
  underlying `Serial.read(65536)` call would ask for far more than the
  ~31KB actually left, and sit for the whole `--timeout` before returning
  data that (confirmed by per-read timestamp logging) had already arrived
  within the first ~200ms. Raising `--timeout` from 10s to 30s to 60s and
  watching "transfer time" track it to within ~0.2s at each step (10230ms,
  30212ms, 60203ms) is what exposed this -- a real measurement doesn't
  move when you change an unrelated timeout.
  This is also why the `CHUNK_BYTES` 4KB/8KB/16KB/32KB trial looked
  invariant: `RX_BULK_READ` (the host's request size) was constant across
  every trial regardless of `CHUNK_BYTES` (the device's write size), so
  all four were measuring the exact same host-side artifact, not device
  throughput. Whether `CHUNK_BYTES` affects real transfer speed is
  therefore back to genuinely unproven -- that experiment never actually
  tested it.
  Fix: `_LineReader` now requests `min(chunk_size, ser.in_waiting or 1)`
  instead of a fixed `chunk_size` -- a call that already has data
  available returns immediately with exactly that; a call with nothing
  buffered yet asks for exactly 1 byte, so it returns the instant *any*
  new data shows up instead of waiting on a specific count that may never
  come, and doesn't spin (`ser.read(1)` genuinely blocks until a byte
  exists or the timeout confirms a real stall). Confirmed live,
  2026-09-10.
  The 2026-09-05 chunking change (batching `Serial.write()` into 16KB
  device-side calls) is unaffected by any of this and still worth keeping
  -- it fixed a real, different problem (thousands of tiny `Serial.write()`
  calls serializing against per-call mutex/connection-state overhead), on
  the device side; today's bug and fix are entirely host-side.
  `--timeout` (`SERIAL_TIMEOUT_S`) is back down from 30s to **10s**,
  justified by the real number this time: ~40x margin over the ~230ms
  real transfer, and 2x the device's own `tier1WriteAll()` 5000ms
  give-up threshold (`src/main.cpp`) with room for that function's `#ERR`
  line to actually arrive after it gives up, rather than the host timing
  out first and reporting a generic timeout instead of the device's real
  error.

- A host-side SDL renderer was feasible but not yet built, as of
  2026-09-13 (see the follow-up entry below for the finished version).
  M5GFX 0.2.28 vendors LovyanGFX's SDL backend
  (`lgfx/v1/platforms/sdl/`) -- it compiles to an empty translation unit on
  the ESP32 target (the whole file is gated behind `#if defined(SDL_h_)`),
  so seeing it in the build log does not mean SDL is active. Backend
  selection is automatic: `lgfx/v1/platforms/device.hpp`'s `#if/#elif`
  chain picks the SDL panel when neither `ESP_PLATFORM` nor `ARDUINO` is
  defined and SDL2 headers are on the include path -- no manual
  `#include <SDL.h>` needed. M5GFX ships a working example at
  `examples/PlatformIO_SDL/` (own `platformio.ini`, README with
  Windows/MSYS2 + SDL2 setup steps). `M5Canvas` is defined in `M5GFX.h`
  itself, not M5Unified, and its constructor takes a generic `LovyanGFX*`
  -- portable to a host build; only `&M5.Display` as the parent pointer is
  M5Unified-specific. `radians()`/`constrain()`/`min()`/`max()` used
  inside the draw functions are Arduino.h macros, not standard library --
  a host build needs `std::min`/`std::max` and local helpers instead.
  Toolchain prerequisites are **not installed on this machine** as of
  2026-09-13: no MSYS2, no gcc/g++ on PATH, no SDL2 dev files, PlatformIO
  `native` platform not installed. Confirmed by reading source, 2026-09-13.

- **The SDL host build is done and working, 2026-09-16** (`src/sdl_main.cpp`,
  `[env:native]` in `platformio.ini`). Toolchain: MSYS2 installed via
  `winget install -e --id MSYS2.MSYS2`, then `pacman -S
  mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-make` for the
  GCC/SDL2 dev files, plus `pio pkg install -g -p native`. `C:\msys64\
  mingw64\bin` was added to the user `PATH` permanently, so new shells pick
  up `gcc`/`SDL2.dll` without extra setup -- a shell already running before
  that change won't see it until restarted. Three real bugs surfaced
  getting it to actually render, none of them in `render.cpp` itself:
  - mingw-w64 hides `localtime_r()` (used in `render.cpp`) behind
    `_POSIX_THREAD_SAFE_FUNCTIONS`, and unlike glibc this isn't implied by
    `-std=gnu++14` alone -- needs `-D_POSIX_THREAD_SAFE_FUNCTIONS`
    explicitly (plain `-std=c++14`, i.e. strict ANSI, hides it too, for a
    different reason). Both confirmed by build failure
    (`'localtime_r' was not declared`) before adding the fixes.
  - `src/sdl_main.cpp` needs its own `build_src_filter` exclusion
    (`-<sdl_main.cpp>`) on the two ESP32 envs -- without it, PlatformIO's
    default source filter compiles it there too (it defines its own
    `main()`/`setup()`/`loop()`, which would collide with `main.cpp`'s).
    The file's own `#if !defined(SDL_h_) #error ...` guard does catch the
    mismatch and fails the build, but GCC keeps parsing past `#error`
    rather than stopping immediately, so the failing build's log is
    dominated by confusing secondary errors (`'lgfx::Panel_sdl' has not
    been declared`) instead of just the one real one -- exclude the file
    per-env, don't rely on the guard alone.
  - **The actual blank-window bug**: M5GFX's SDL `autodetect()`
    (`M5GFX.cpp`) sets `board_M5PaperColor`'s rotation to `r=1` in its
    board table, which swaps `gfx.width()`/`gfx.height()` to 600x400 even
    though the window itself is still the physical 400x600 -- confirmed by
    logging both and seeing exactly that swap. `render.cpp`'s
    `SCREEN_W`/`SCREEN_H` (400x600, from `layout.h`) aren't rotation-aware,
    so `canvas.pushSprite(0, 0)` was blitting a 400-wide/600-tall sprite
    onto a parent that thought it was 600 wide -- the geometry didn't fit,
    and the window stayed on its blank initial framebuffer. Neither
    `pushSprite()` nor `createPalette()` (both are called, in the right
    order, matching `drawAll()` in `main.cpp`) were actually missing --
    this cost nothing at the render-logic level, it was purely a host-only
    orientation mismatch. Fixed with an explicit `gfx.setRotation(0);`
    right after `gfx.init()` in `sdl_main.cpp`, overriding the SDL board
    table's guess rather than trusting it. This is a host-preview-only fix
    -- it says nothing about rotation on real hardware, which was never in
    question (confirmed pixel-identical via `hil.py` well before this).
  Confirmed live: `pio run -e native -t upload` renders
  `test/fixtures/example.json` correctly in a live window.

- **The SDL host build's output is byte-for-byte identical to the real
  device's**, not just visually similar -- confirmed 2026-09-16, closing
  the loop this whole effort was for. Set `SDL_PREVIEW_DUMP_RAW=<path>` and
  `sdl_main.cpp` writes the raw `canvas.getBuffer()`/`bufferLength()` bytes
  (the exact same palette_4bit buffer `main.cpp`'s `tier1HandleRender()`
  base64-encodes over serial for the real device) to that path and exits
  immediately, skipping the SDL window entirely -- no need to babysit a GUI
  for a scripted check. `tools/hil.py`'s new `decode-raw` subcommand
  decodes that file with the exact same `decode_palette4()` function
  already used for the device's serial dump (not a second
  reimplementation) and, given `--golden`, compares it byte-for-byte the
  same way `test`/`test --all` do:
  ```
  SDL_PREVIEW_DUMP_RAW=raw.bin pio run -e native -t upload
  python tools/hil.py decode-raw raw.bin --golden test/golden/example.png
  ```
  First run surfaced two real, unrelated bugs in `sdl_main.cpp` -- neither
  in `render.cpp`, both in the small amount of host-only glue code around
  it, confirmed by the mismatch's `diff.png` showing changes isolated to
  text, not layout/graphics:
  - `parseSnapshot()`'s `stationLabel` fallback was `"STATION"`; the real
    firmware's `tier1ParseSnapshot()` (which this mirrors) falls back to
    `STATION_LABEL` (`config.h`, `"GOLDEN GATE"`) for fixtures that predate
    that field -- every fixture under `test/fixtures/` does. Fixed by
    hardcoding `"GOLDEN GATE"` directly (this file intentionally doesn't
    include `config.h`, to stay decoupled from real Wi-Fi credentials).
  - **Every timestamp was exactly 7 hours off.** `TIER1_TEST` (the ESP32
    path the goldens were captured against) never calls
    `configTzTime()`/`setenv()` -- no Wi-Fi/NTP path exists there at all --
    so `render.cpp`'s `localtime_r()` calls run under ESP-IDF's
    unconfigured default, UTC. This host build's C runtime instead
    defaulted to this dev machine's own OS timezone (Pacific), which is
    exactly what produced the 7-hour gap. Fixed by forcing UTC explicitly
    at the top of `setup()` -- `_putenv("TZ=UTC0"); _tzset();` on Windows/
    mingw, since it doesn't declare POSIX `setenv()` by default;
    `setenv()`/`tzset()` on other platforms. Deliberately **not** the same
    as production (`main.cpp` sets `TZ_STRING`, Pacific, once NTP
    succeeds) -- this specifically matches `TIER1_TEST`'s unconfigured
    state, since that's what `test/golden/*.png` reflects.
  After both fixes: `decode-raw --golden test/golden/example.png` reports
  `MATCH`, exit 0, confirming the SDL host path and the real device path
  produce identical output for the same input -- not close, not visually
  similar, the same bytes.

- `src/render.h`/`src/render.cpp` (the render-module extraction, 2026-09-13)
  don't include `Arduino.h` themselves, but on the **ESP32 build** it's
  still present in `render.cpp`'s translation unit regardless: `M5GFX.h` ->
  `lgfx/v1/platforms/esp32/Bus_SPI.hpp` -> `misc/datawrapper.hpp`
  transitively includes it (M5GFX's ESP32 backend needs Arduino's `Stream`
  class), so its macros are still live even though this file never asks for
  them. Confirmed the hard way: a local `constexpr float DEG_TO_RAD = ...`
  (meant to replace the `radians()` macro's constant, see the entry above)
  got silently textually replaced by Arduino.h's own `#define DEG_TO_RAD
  0.0174...` before the compiler ever parsed it, producing `error: expected
  unqualified-id before numeric constant`. Renamed to `kDegToRad` to avoid
  the collision -- value unchanged. Also confirmed by reading `Arduino.h`
  directly: this ESP32 core's `constrain`, `radians`, and `DEG_TO_RAD` are
  `#define`s, but `min`/`max` deliberately are not (its own comment: "can't
  define max() / min() because of conflicts with C++"), so `std::min`/
  `std::max` were never at risk of the same collision. Net effect: dropping
  `render.cpp`'s own `#include <Arduino.h>` does make this module's *own*
  code stop depending on Arduino's macros (the actual goal, and what makes
  it portable to a build that genuinely never sees `Arduino.h`, like a
  future SDL host build that never compiles M5GFX's ESP32 backend) -- it
  just doesn't mean "no Arduino.h in the include graph" is literally true
  for the ESP32 target specifically. Confirmed live, 2026-09-13.

- **The soak-test sleep-interval bug** (89.3 min/cycle observed against a
  configured `UPDATE_MINUTES=30` -- ~58-60 min of excess sleep every cycle,
  reproducing identically regardless of the configured interval) was a
  `tm_isdst` bug in `sleepUntilNext()` (`src/main.cpp`), not an RX8130
  register issue. `nextTm` is built as `struct tm nextTm = {};`, which
  zero-initializes `tm_isdst` to `0` ("assume standard time"). `TZ_STRING`
  (`config.h`) is `PST8PDT,M3.2.0,M11.1.0` -- a DST-observing zone, and the
  device was running during DST (PDT, UTC-7) when this was diagnosed. Fed
  `tm_isdst=0`, `mktime()` computed the epoch as if the wall-clock digits
  it was given were **standard-time (PST, UTC-8)** digits, one absolute
  hour later than what those same digits mean under the real, active PDT
  rule -- so the alarm ends up armed for an epoch that reads back as one
  hour later than intended, every time, independent of `UPDATE_MINUTES`
  (confirmed by a follow-up rerun at `UPDATE_MINUTES=2`, which added the
  same ~58-60 min excess rather than a scaled one -- ruling out a
  multiplicative RX8130-side cause and pointing at a fixed offset instead).
  **Dormant outside DST**: whenever standard time is actually in effect
  (i.e. outside the `M3.2.0`-`M11.1.0` window), `tm_isdst=0` happens to be
  correct by coincidence and the bug doesn't reproduce -- so it would not
  have shown up in a winter soak test.
  **The trap that delayed diagnosis**: a temporary diagnostic that logged
  `nextTm` after `mktime()`, the `time_t` it returned, and that same
  `time_t` round-tripped through `localtime_r()` initially looked like it
  *ruled out* `tm_isdst` -- `mktime()`'s output and the `localtime_r()`
  round-trip agreed with each other, and `mktime()` reported `tm_isdst=1`
  (correctly identifying DST as active). But `mktime()`'s post-call
  `tm_isdst`/fields describe **the epoch it decided to resolve to**, not a
  validation of the caller's original input -- it does not re-check "was
  the isdst you gave me actually correct for these digits." Once `mktime()`
  had already misapplied the PST offset to compute the epoch, that epoch
  *was*, by then, genuinely a DST-era timestamp one hour later, so
  `localtime_r()` reading it back under the real DST rule was always going
  to agree with `mktime()`'s own output -- self-consistency between the two
  proved only that both functions were internally correct given the
  (already-wrong) epoch, not that the epoch itself was right. Decoding the
  logged epoch to UTC (`date -u -r <epoch>`) and comparing it against both
  the PST and PDT interpretation of the intended local time is what
  actually separated the two candidates: the PST interpretation of the
  *intended* digits matched the epoch exactly, confirming the offset
  mistake. Fix: `nextTm.tm_isdst = -1;` before the `mktime()` call (lets
  `mktime()` determine DST itself), matching the pattern already used by
  `parseLocal()`/`parseIso()` elsewhere in this same file. Confirmed by
  grep that those two functions were the only other `mktime()` call sites
  in `src/`, and both already did this correctly -- this was the only
  instance of the bug class. `tools/*.py` never goes through
  `mktime()`/`tm_isdst` (uses naive `datetime.strptime()`), so the sweep
  found nothing there either. Fixed in source, commit `237ef2f`, and
  **confirmed on real hardware, 2026-09-16**: a fresh soak segment (cycle
  385 -> cycle 397, same day) ran at ~29.2 min/cycle against the ~31.3 min
  expected baseline -- see "Current state" for the full readout, including
  why that 29.2 figure is itself unexplained and the soak 2 average (31.7
  min/cycle over 375 cycles) is the figure that confirms the fix.

---

## Design constraints for this project

- **Update cadence: 30 minutes** by default. Do not go below 15 — the refresh
  itself takes up to 30s and each cycle costs battery.
- Everything must be readable at arm's length in daylight. Assume the user
  glances at it for two seconds.
- Layout is designed host-side in `tools/preview.py`, which renders the exact
  400x600 palette-constrained image. **Iterate there first.** A layout change
  is milliseconds on the laptop and 30 seconds on the device.
- Target battery life: roughly a month per charge -- **this estimate is now
  known wrong, not just unmeasured.** It assumed ~20s awake per cycle
  (arithmetic below); measured real awake time (2026-09-05, both
  `m5stack-papercolor` and `m5stack-papercolor-test`, `millis()`-bracketed
  and confirmed live) is **~78s/cycle** (~50s in `M5.begin()` alone, ~10.5s
  Wi-Fi + fetch, ~17s render + panel refresh) -- roughly 4x the assumed
  figure. Do **not** just recalculate by swapping 78s in for 20s below: the
  mA figures (~150mA average awake, 92.53uA standby) were never measured
  either (see "Current state" / "Not yet exercised"), and awake current
  likely doesn't scale linearly across such different phases (a ~50s mostly
  memory/I2C-bound `M5.begin()` call, Wi-Fi TX, and an actual panel refresh
  probably don't draw the same). The whole budget needs re-deriving from a
  real current measurement across an actual cycle, not from arithmetic on a
  corrected time. Original (now-superseded) arithmetic, kept for reference:
  ~20s awake at ~150mA average = ~0.85mAh per cycle, 48 cycles/day ≈ 41mAh,
  plus ~2.2mAh/day sleeping ≈ 43mAh/day against 1250mAh.
  Soak 1 (2026-09-11 to 2026-09-14, 100%->98% over 45 cycles / 66h57m)
  was originally read as "month-shaped, not week-shaped" -- **that
  conclusion is withdrawn**; soak 2 overturned it (see "Current state").
  Its 89.3 min/cycle cadence was a `tm_isdst` bug, root-caused and fixed
  2026-09-16 (see "Verified corrections"); soak 2 then ran at 31.7
  min/cycle (~45.5 cycles/day, close to the original 48/day assumption),
  so the cycles-per-day term is no longer in question.
  **Current best estimate (soak 2, 2026-09-16 to 2026-09-24): ~15 days per
  charge**, a linear projection from 100%->46% over 8.24 days / 375
  cycles -- about half the one-month goal. Rough, gauge-dependent
  breakdown, assuming the 54% used was 54% of the rated 1250mAh: ~675mAh
  over 8.24 days = ~82mAh/day, ~1.8mAh/cycle; the datasheet standby
  (~2.2mAh/day) is under 3% of that, so ~97% of daily energy is awake
  time -- ~1.75mAh per ~93s awake window on battery (the ~78s measured
  with USB attached, plus the ~15.5s serial wait `237ef2f` runs on every
  battery wake -- see "Current state"), i.e. ~67mA average awake, below
  the ~150mA assumed above. Treat all of this as an estimate until
  the run-to-empty (in progress, ~2026-10-01) replaces it with a measured
  days-per-charge figure.

---

## Data sources

### Tides — NOAA CO-OPS (US only, no API key)

Base: `https://api.tidesandcurrents.noaa.gov/api/prod/datagetter`

Hourly predicted water level for the curve:
```
?date=today&station=9414290&product=predictions&datum=MLLW
&interval=h&units=english&time_zone=lst_ldt&format=json&application=papercolor
```

High/low events for the annotations:
```
?date=today&station=9414290&product=predictions&datum=MLLW
&interval=hilo&units=english&time_zone=lst_ldt&format=json&application=papercolor
```

Observed water level right now:
```
?date=latest&station=9414290&product=water_level&datum=MLLW&units=english
&time_zone=lst_ldt&format=json&application=papercolor
```

Response shape: `{"predictions":[{"t":"2026-08-28 00:00","v":"3.412","type":"H"},...]}`
— `t` local time, `v` feet, `type` only present with `interval=hilo`.

`9414290` is San Francisco (Golden Gate). Find a closer station via the station
map or the metadata API: `https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations.json`

### Wind — two sources, prefer observed

1. **Observed**, if the CO-OPS station has met sensors: same datagetter with
   `product=wind`. Returns speed `s`, direction degrees `d`, gust `g`.
   9414290 does: its sensor list (`mdapi/prod/webapi/stations/9414290/
   sensors.json`) has an active Wind sensor, C1, 24 ft above the site.
   Checked 2026-09-24: `date=latest` was 10 min old, and 14 days of history
   (2026-09-11 to 2026-09-25 GMT) showed a reading every 6 min with no
   gaps, no empty values, and all quality flags `0,0`. With
   `units=english`, `s` and `g` are knots (checked against `units=metric`:
   4.4 m/s = 8.55 kn). Implemented in `fetchWind()` but **not yet flashed**
   -- see "Current state".
2. **Forecast**, always available: Open-Meteo, no API key. Still fetched
   every cycle for the forecast bars, and its `current` values are the
   fallback when the observed reading fails or is more than 30 min old.

```
https://api.open-meteo.com/v1/forecast?latitude=37.81&longitude=-122.47
&hourly=wind_speed_10m,wind_direction_10m,wind_gusts_10m
&current=wind_speed_10m,wind_direction_10m,wind_gusts_10m
&wind_speed_unit=kn&timezone=auto&forecast_days=2
```

### TLS note

Both endpoints are HTTPS. Simplest path on-device is `WiFiClientSecure` with
`setInsecure()`. If you want real verification, pin the root CA and be prepared
to update it when it rotates.

### JSON sizing

Hourly predictions ≈ 25 entries, hilo ≈ 4, Open-Meteo hourly ≈ 48. All small.
If you switch to 6-minute predictions (240 entries), allocate the ArduinoJson
document in PSRAM and use a filter — don't parse the whole body on the stack.

---

## Build and flash

```bash
pio run                       # build (just the two ESP32 envs -- see [platformio] default_envs)
pio run -t upload             # flash (see button note below -- usually not needed)
pio device monitor -b 115200  # serial log
python tools/preview.py       # regenerate the layout preview PNG (PIL, not the real M5GFX draw path)
pio run -e native -t upload   # host-side SDL preview -- real render path, see Verified corrections
```

You (Claude) can and should run these directly. Read the compiler output and
the serial log yourself rather than asking the user to paste them.

- Uploading does **not** require a physical power-button press in the common
  case. `pio run -t upload --upload-port COMx` succeeded repeatedly (10+
  times, no button touched) as long as the board was actually powered and
  running -- either awake mid-cycle, or sitting in `TIER1_TEST`'s command
  loop, which never sleeps: esptool's own RTS-pin reset was enough to enter
  the bootloader every time. The button **is** needed when the PM1 has
  actually cut power between wake cycles (mid-sleep) -- there the port
  doesn't exist at all, and upload fails with a distinct error
  (`Could not open COMx, the port is busy or doesn't exist` /
  `FileNotFoundError`, not an esptool bootloader-sync failure). Check
  whether the port enumerates at all before assuming a button press is
  needed; if the board is genuinely mid-sleep, either wait for the next
  RX8130 wake or ask for the button -- don't retry blindly either way, and
  don't assume every upload failure means "needs the button" without
  checking which failure mode it actually is. Originally (2026-09-04)
  documented as always required; corrected 2026-09-05 after 10+ successful
  button-free uploads during the boot-delay investigation.

---

## Test harness (Tier 1 / hil.py)

```bash
python tools/hil.py ping                                            # confirm the board is alive
python tools/hil.py render test/fixtures/example.json --out actual.png
python tools/hil.py test test/fixtures/example.json --golden test/golden/example.png
python tools/hil.py test --all                                      # every fixture, one session
```

Requires a board flashed with the `m5stack-papercolor-test` PlatformIO env
(`pio run -e m5stack-papercolor-test -t upload --upload-port COMx`) -- no
Wi-Fi, no NOAA/Open-Meteo fetch, no PM1 sleep, no panel refresh. `render`
sends a Snapshot fixture over USB serial, gets the rendered canvas buffer
back as base64, and decodes it to a PNG. `test` does the same and diffs the
result against a golden image byte-for-byte (`images_equal()` in
`tools/hil.py` -- exact match, not a perceptual/threshold diff).

A real round trip (`render` or `test`, RENDER sent -> framebuffer fully
decoded) is **~230ms** -- see the transfer-time entry above. That's what
makes this worth running on every render-affecting change, not just
occasionally: it's closer to a fast unit test than a hardware step.

- **Exit codes are distinct, not overloaded.** `EXIT_MISMATCH` (1) means
  `test` compared a real render against the golden and found a difference
  -- a genuine finding, act on it. `EXIT_DEVICE_ERROR` (2) means the
  comparison never happened at all -- board unplugged, wrong firmware,
  protocol error, missing/malformed fixture, or missing golden file -- so
  it needs the opposite response (fix the environment, not the render). A
  script driving this in a loop can safely treat 2 as "retry or alert
  separately" and 1 as "this is real." (Both used to be a bare
  `sys.exit(1)`, indistinguishable without parsing stderr text -- and a
  connection failure during `Device()` construction, plus a bad `--fixture`
  path, both used to bypass the error handling entirely and escape as raw
  uncaught tracebacks with Python's default exit code 1. Fixed 2026-09-10:
  `Device.__init__` now wraps `serial.SerialException` as `DeviceError`,
  `cmd_render`/`cmd_test` now catch fixture-loading failures too, and
  `Device(...)` construction itself moved inside each command's `try` --
  it was outside it, in all four commands, so its own DeviceError could
  never reach the `except` beneath it.) On mismatch, `test` writes
  `expected.png`, `actual.png`, and `diff.png` (the golden with mismatched
  pixels highlighted in magenta) into `--out-dir` (default: cwd) before
  exiting 1, so the failure is inspectable without re-running anything.
  Exit 0 with `MATCH: ...` on stdout means the render is pixel-identical
  to the golden.

- **`test --all`** walks `test/fixtures/*.json`, pairs each with
  `test/golden/<name>.png`, and runs them in one serial session instead of
  reconnecting per fixture -- ~230ms per fixture, ~1.2s for the current
  five. Prints a PASS/FAIL line per fixture plus a summary count. Exit
  codes: 0 only if every fixture matches, 1 if any fixture fails, 2 if a
  `DeviceError` aborts the run outright (remaining fixtures then go
  untested, unlike a single fixture's failure). **A missing golden's exit
  code differs by mode:** exit 1 in `--all` (that fixture fails, the run
  continues) vs. exit 2 in single-fixture `test` (bucketed with
  `EXIT_DEVICE_ERROR` above). Deliberate, not an oversight -- `--all` wants
  "exit 0" to mean "everything that could be checked passed," so a missing
  golden has to read the same as a real mismatch there, not the same as a
  dead connection. Mismatches write to
  `<out-dir>/<fixture>/{expected,actual,diff}.png` -- one subdirectory per
  fixture, so multiple failures in a run don't collide.

- **Fixtures are generated, not hand-authored.** `test/fixtures/example.json`
  comes from `tools/make_fixture.py`, which derives both `tide[]` (hourly
  samples) and `events[]` (the model's true extrema) from one shared
  analytic tide curve -- the same one `tools/preview.py`'s `synthetic()`
  uses -- instead of two independently-typed arrays. A hand-edited
  `events[]` previously drifted out of sync with `tide[]` (two of four
  events landed 45-90 minutes from where the sampled curve actually
  peaks/troughs, one with a magnitude the curve never reached), and it
  took an actual render to notice -- nothing about the JSON itself looked
  wrong on inspection. Regenerate with `python tools/make_fixture.py --out
  test/fixtures/example.json`; don't hand-edit `tide[]`/`events[]` directly.

- **Goldens live in `test/golden/` and are committed.** `actual.png`,
  `expected.png`, and `diff.png` are gitignored -- they're `test` output,
  regenerated every run, not source of truth. Only the golden itself is
  checked in.

- **Regenerating a golden is a deliberate, separate act.** It asserts
  "this new output is correct," a judgment call a diff can't make for
  you -- never overwrite `test/golden/*.png` as a side effect of the
  change it's meant to validate, and never fold that update into the same
  commit. Regenerate it (`python tools/hil.py render ... --out
  test/golden/example.png`), look at it, and commit it on its own with a
  reason (e.g. "update golden for the wind-chip layout change" -- not
  just "update golden").

---

## Repo layout

```
src/config.h              Wi-Fi creds, station IDs, coordinates, cadence
src/main.cpp              wake -> connect -> fetch -> draw -> sleep
src/render.h/.cpp         drawHeader()/drawNowStrip()/... -- shared by main.cpp and sdl_main.cpp
src/sdl_main.cpp          [env:native] host entry point -- see Verified corrections
src/layout.h              GENERATED from layout.json -- do not edit by hand
tools/preview.py          host-side layout renderer, palette-accurate (PIL, not the real M5GFX draw path)
tools/gen_layout_header.py  layout.json -> src/layout.h, run automatically by `pio run`
layout.json               shared pixel-geometry source of truth for both of the above
refs/                     vendored upstream sources, read-only reference
```

Pixel coordinates (box positions, radii, offsets -- not colors, fonts, or text)
live in `layout.json`, not in `tools/preview.py` or `src/main.cpp` directly.
Change layout there; `pio run` regenerates `src/layout.h` automatically via
the `extra_scripts` hook in `platformio.ini`, and `preview.py` reads
`layout.json` itself at runtime. `layout.json`'s `preview_only` section now
holds only elements specific to `preview.py`'s own PIL rendering (font-metric
offsets), not features missing from `main.cpp` -- see "Current state" at the
top of this file for what's confirmed working on real hardware.
