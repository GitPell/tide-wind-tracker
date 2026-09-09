# Tide & Wind Tracker — M5Stack PaperColor (SKU C151)

Battery-powered e-paper dashboard showing tide state and wind conditions for a
coastal location. Wakes on a timer, fetches data over Wi-Fi, redraws, sleeps.

---

## Current state (as of 2026-09-05)

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

**Not yet exercised:**
- Battery life / current draw. The 92.53uA standby figure is a datasheet
  target derived from reading the reference firmware, not a measurement on
  this board. Worse than merely unmeasured as of 2026-09-05: the awake side
  of the budget is now **known wrong** -- see "Design constraints" below and
  the `M5.begin()` entry in "Verified corrections". Real measured awake time
  is ~78s/cycle against an assumed ~20s.

**Next steps:**
1. Measure real standby/active current and run a multi-day battery soak
   test -- more urgent now that measured awake time (~78s/cycle) is ~4x the
   assumed figure the existing battery estimate was built on.
2. Narrow the ~33s unexplained remainder inside `M5Unified`'s `_begin(cfg)`
   (RTC/IMU/mic/speaker already ruled out -- see "Verified corrections").
   Use header-inlined `millis()` bracketing, not `ESP_LOG*` or new prints in
   a vendored `.cpp` file -- confirmed dead on this build, see below.

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
  line-height, in `.pio/libdeps/m5stack-papercolor/M5GFX/src/lgfx/Fonts/
  Custom/`). For bold, the closest bundled family is Adafruit's "Free Fonts"
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
  `.pio/libdeps/m5stack-papercolor/M5GFX/src/lgfx/Fonts/` and testing
  on-device, 2026-08-28.

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
  `M5GFX/src/lgfx/v1/LGFXBase.hpp` and an example `.ino`'s usage, 2026-08-29.

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
2. **Forecast**, always available: Open-Meteo, no API key.

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
pio run                       # build
pio run -t upload             # flash (see button note below -- usually not needed)
pio device monitor -b 115200  # serial log
python tools/preview.py       # regenerate the layout preview PNG
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
src/layout.h              GENERATED from layout.json -- do not edit by hand
tools/preview.py          host-side layout renderer, palette-accurate
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
