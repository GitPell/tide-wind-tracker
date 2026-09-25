# Tide & Wind Tracker — M5Stack PaperColor (SKU C151)

Battery-powered e-paper dashboard showing tide state and wind conditions for a
coastal location. Wakes on a timer, fetches data over Wi-Fi, redraws, sleeps.

---

## READ THIS FIRST (instructions for Claude)

> **A run-to-empty battery test is in progress -- do not interrupt it.**
> Started 2026-09-16 (cycle 385, 100%), expected to end around 2026-10-01.
> Until it does: no flashing and no USB connection (USB charges the battery
> and invalidates the measurement) -- so no upload to any env
> (`pio run -e <env> -t upload`), no `pio device monitor`, no `tools/hil.py` against the device. Firmware and
> render changes can still be written, built with plain `pio run`, and
> checked with the SDL build (`pio run -e native`), then flashed together
> afterwards. See DONE.md's sequencing rule and the checklist below.

The project's finish line is defined in DONE.md. Read it before planning
work, and treat anything not listed there as out of scope.

Your training data is unreliable for this board. It is **not** the older
monochrome M5Paper (S3). The panel, the power management IC, and parts of the
API differ. Before writing or changing any hardware-facing code:

1. **For library APIs, read `.pio/libdeps/m5stack-papercolor/`.** This is the
   authoritative source: it is the exact code being compiled into the binary,
   at the exact versions pinned in `platformio.ini` (see "Pinned versions").
   Do not infer method names, and do not read a separate clone that may have
   drifted from what is built.
   - `M5Unified/`, `M5GFX/`, `M5PM1/`, `ArduinoJson/`
     (`M5GFX/` may be named `M5GFX@src-<hash>/` -- the hash comes from the
     `lib_deps` spec -- look for that if the plain name doesn't exist)
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

## Status (updated 2026-09-25)

DONE.md is the authoritative scope; this section is the working state
toward it.

**Works, confirmed on real hardware:**
- Full cycle: Wi-Fi -> NTP -> NOAA tides -> Open-Meteo wind -> draw -> PM1
  shutdown -> RX8130 wake -> cold boot, at a 2-minute test cadence and the
  production 30-minute cadence. The PM1-RTC-RAM cycle counter survives power
  cuts and reflashing.
- The complete draw (header, now strip, tide curve, wind rose and chip,
  forecast bars, footer) with real fonts, rendered through an off-screen
  canvas pushed once per cycle. `layout.json`'s `preview_only` section holds
  only `tools/preview.py`'s own PIL font offsets, not missing features.
- SHT40 indoor temperature/humidity; per-cycle battery logging (`BATLOG`).
- The wake cadence after the `tm_isdst` fix (`237ef2f`): 31.7 min/cycle over
  375 cycles (see "Battery budget").
- Test harness: `python tools/hil.py test --all` passed 5/5 on 2026-09-14
  against the five original goldens (`calm`, `example`, `high_wind`,
  `long_station`, `no_events`).
- SDL host preview (`pio run -e native`): renders of all five original
  fixtures are byte-identical to their device goldens (`hil.py decode-raw
  --golden`, 2026-09-24; rechecked on the pinned M5GFX 2026-09-25).

**Built, not yet on hardware** (all held until the run-to-empty ends; the
run-to-empty measures `237ef2f`, which has none of these):
- Observed wind from NOAA, with Open-Meteo fallback (`6d5d443`) -- see
  "Data sources". Adds an estimated 1-2 s awake per cycle (unmeasured).
- Skipping the USB serial wait when no host is attached (`5c5c8ba`) -- saves
  ~15.5 s per battery wake; see "Verified corrections" -> USB serial.
- Wi-Fi failure diagnostics (`d237a73`) -- see "Serial log lines".
- Time-sync failure handling (`b9607a8`) -- found by reading the code, never
  observed; see "Verified corrections" -> Time.
- Error display (`fd2298b`) -- see "Design constraints". Five failure
  fixtures (`fail_wifi`, `fail_all`, `fail_wind`, `fail_hilo`,
  `fail_clock`) render via the SDL build but have no goldens yet.

**Open items:**
1. Let the run-to-empty finish (~2026-10-01; do not interrupt it). "Empty" =
   the device no longer completes a wake cycle. Record the last completed
   cycle's time -- the frozen frame's footer (`UPD HH:MM`) shows it, so check
   the panel daily to pin down the date -- and its cycle number (estimate
   from elapsed time at ~31.7 min/cycle if it can't be read back). Then
   enter the measured days-per-charge in "Battery budget", compare it to the
   ~1 month goal, and document the shortfall, stating the firmware measured
   (`237ef2f`).
2. Wi-Fi `NO_AP_FOUND` still unresolved. The diagnostics above are built but
   not flashed; the cause still has to be identified from a real
   occurrence, or documented as unknown, per DONE.md.
3. Current draw is unmeasured (the 92.53 uA standby and ~150 mA awake
   figures are datasheet/arithmetic). The run-to-empty measures
   days-per-charge directly, so a current measurement is only needed to
   attribute energy across `M5.begin()`, Wi-Fi, and the panel refresh.

---

## After the run-to-empty ends

Everything waiting on hardware. Do none of it before then, except the
baseline timing, which only needs watching.

- [ ] Baseline timing on `237ef2f`, on battery -- can be done now without
      touching the device. The wake alarm fires on a whole minute (the
      RX8130 alarm has no seconds field), 30-31 min after the footer's
      `UPD HH:MM`. Time from that minute to the visible start of the panel
      refresh, over 2-3 wakes.
- [ ] Record the run-to-empty result (Open items #1).
- [ ] On the first boot after recharging, check the header time is
      correct -- the RTC may lose time on a full discharge, and the error
      display doesn't flag that case (see "Design constraints" -> Error
      display).
- [ ] Flash the current firmware (`pio run -e m5stack-papercolor -t upload
      --upload-port COMx`). The battery charges over USB meanwhile.
- [ ] With the monitor attached from boot: the log starts at the first
      line as before (the serial wait still runs with a host), and the
      first few `WINDSRC` lines show `src=obs`.
- [ ] Flash `m5stack-papercolor-test`. First capture goldens for the new
      fixtures (`fail_wifi`, `fail_all`, `fail_wind`, `fail_hilo`,
      `fail_clock`): `python tools/hil.py render test/fixtures/<name>.json
      --out test/golden/<name>.png`. Review each, and commit them on their
      own with a stated reason (e.g. "add goldens for the error-display
      fixtures"). Until then `test --all` counts each missing golden as a
      failure. Then run `python tools/hil.py test --all` and expect 10/10
      PASS. Flash `m5stack-papercolor` back. Finally, confirm the SDL
      renders of the five new fixtures match their new goldens
      (`SDL_PREVIEW_DUMP_RAW` + `hil.py decode-raw --golden`). That
      completes DONE.md's "SDL byte-identical for every fixture" item; the
      original five already match (see "Status").
- [ ] Unplugged, on battery: repeat the baseline timing. Expect the panel
      refresh to start ~14s earlier (~15.5s serial wait removed, minus the
      estimated 1-2s the observed-wind request adds).
- [ ] Provoke a Wi-Fi failure with a nonexistent `WIFI_SSID` (see "Serial
      log lines"), confirm the `WIFIFAIL`/`WIFISCAN` lines appear as
      expected, then restore `config.h` and reflash.
- [ ] Provoke a time-sync failure: a test build whose `configTzTime()`
      call points at a nonexistent NTP server, so Wi-Fi still connects
      but SNTP never completes. Expect the `ntp failed: not synced in 10s,
      using RTC time` line and a correct header time. On the next normal
      cycle, the header time must still be correct (the RTC wasn't
      corrupted). Then restore and reflash.

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

---

## Verified corrections (do not re-litigate)

Each entry is a rule plus the evidence behind it. The stories of how most of
these were found are in `notes/writeup-material.md`.

### Board and power

- SD and EPD share SPI. Don't hold both transactions open.
- Ghosting is a common complaint on this panel. Budget a periodic full-refresh
  / clear cycle, not just partial updates (`FULL_REFRESH_EVERY` in
  `config.h`, via `clearScreenFull()`).
- Nothing on this display should rely on anti-aliasing or gradients. Every
  pixel must land on one of the six palette colors.
- `M5.begin()` does **not** power the e-paper rail. M5Unified's board-init
  for `board_M5PaperColor` (`M5Unified/src/utility/Power_Class.cpp`) enables
  only the RGB LDO and PM1 GPIO3 (SD power), never GPIO0 (`PY_EPD_EN`). Drive
  it with the standalone `M5PM1` library (`pm1.pinMode(M5PM1_GPIO_NUM_0,
  OUTPUT); pm1.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);`), matching
  `refs/M5PaperColor-UserDemo/main/hal/hal.cpp Hal::init()`. Read in source,
  2026-08-28.
- The 92.53uA standby figure requires a full PM1 shutdown, **not**
  `M5.Power.timerSleep()` / `esp_deep_sleep_start()`. On this board
  `Power_Class::_powerOff()` takes the `esp_sleep_enable_ext0_wakeup(GPIO_7)`
  branch (`_rtcIntPin = GPIO_NUM_7`) and skips the PM1 shutdown, leaving its
  LDO/DCDC rails powered. The low-power path (factory demo `hal.cpp` +
  `app_manager.cpp`): configure PM1 GPIO2 as a WAKE pin tied to the RX8130's
  nIRQ (`gpioSetFunc(..., M5PM1_GPIO_FUNC_WAKE)`, pull-up, falling edge,
  wake-enable), arm an RX8130 alarm (`M5.Rtc.setAlarmIRQ()`), then
  `pm1.shutdown()`. That cuts power to the whole board; the alarm re-powers
  it. Confirmed cycling on hardware 2026-08-29 (`f1aa36b`).
- Because that shutdown is a real power cut, **nothing in RAM survives
  between cycles** — `RTC_DATA_ATTR` resets like a cold boot. Only the PM1's
  battery-backed RTC RAM (`pm1.writeRtcRAM()` / `readRtcRAM()`, 32 bytes)
  persists; the cycle counter lives there (bytes 0-3).
- Schedule the RX8130 wake alarm off `M5.Rtc.getDateTime()` (the RTC's own
  battery-backed clock), not `time(nullptr)`. The system clock is only right
  after a successful NTP sync; the RTC stays right through Wi-Fi/NTP
  failures as long as it's re-synced (`M5.Rtc.setDateTime()`) whenever NTP
  succeeds. Matches `Hal::scheduleNextWakeMinutes()` in the reference demo.
- The RX8130 alarm has **minute resolution**: `setAlarmIRQ()`
  (`RX8130_Class.cpp`) sets only minute/hour/day fields. A wake lands on a
  whole minute, so cycle cadence can't resolve awake-time changes smaller
  than a minute.
- `M5.begin()` costs **~50 seconds on every boot** (50.3-50.5 s, both envs,
  ~10 boots, 2026-09-05): ~17 s in `Display.init()` (board/panel
  autodetection), ~33 s in `_begin(cfg)`. Disabling `cfg.internal_rtc`,
  `internal_imu`, `internal_mic`, and `internal_spk` together changed
  neither number -- **all four are ruled out**. `Power_Class` PMIC
  auto-probing is the untested next candidate. No config flag found reduces
  it. Measured by `millis()` bracketing inside the header-inlined
  `M5Unified::begin()` (see Tooling).

### USB serial (HWCDC)

- `Serial` is the ESP32-S3's built-in USB Serial/JTAG (`HWCDC`, from
  `ARDUINO_USB_CDC_ON_BOOT=1` + `ARDUINO_USB_MODE=1`), not a UART bridge.
  Nothing buffers output while the host enumerates, so output right after
  `Serial.begin()` is lost if a terminal attaches after boot. Wait on
  `while (!Serial && millis() - start < TIMEOUT) delay(10);` -- with a
  timeout, because the board must boot fine with nothing attached.
- `!Serial` stays true with no host: `operator bool()` returns
  `isCDC_Connected()`, which is false whenever `isPlugged()` is false, and
  `isPlugged()` tracks USB start-of-frame packets (a host sends one every
  1ms; a 1 kHz tick hook marks the port unplugged after ~5ms without one).
  So on battery the 15 s wait always ran out -- ~15.5 s wasted per wake with
  the 500ms delay after it. `setup()` now waits only when
  `Serial.isPlugged()` is true (`5c5c8ba`); the flag is set at system init,
  before `setup()`, and `Serial.begin()` doesn't reset it. `TIER1_TEST`'s
  own wait is unchanged (it always runs with a host). Read in the
  framework's `HWCDC.cpp`, 2026-09-24; not yet measured on hardware.
- Without a host, `Serial` writes and `flush()` drop data instead of
  blocking, so logging costs nothing on battery -- and is lost. Anything
  that must be diagnosable on battery has to go on the panel.

### Sensors

- `M5Unified` has **no** `Sht4x` member on this board. Read the SHT40 at I2C
  0x44 with raw transactions on the shared system bus --
  `M5.In_I2C.start()/write()/read()/stop()`, not Arduino `Wire`. Command 0xFD
  (high-precision), ~10ms delay, read 6 bytes, decode with the Sensirion
  SHT4x linear formula. Matches `refs/M5PaperColor-UserDemo/main/hal/hal.cpp
  Hal::sht40Read()`. Confirmed by compile error + reading source,
  2026-08-28.

### Display and rendering

- M5GFX ships **no bold DejaVu** -- only regular `DejaVu9/12/18/24/40/56/72`
  (GFXfont from `DejaVuSans.ttf`, named by pixel line-height, in
  `M5GFX/src/lgfx/Fonts/Custom/`). For bold, the closest bundled family is
  Adafruit's `FreeSansBold9/12/18/24pt7b` (nominal point size; visually
  close, not identical). Mapping used here (comment above `drawHeader()` in
  `src/render.cpp`): F_HUGE->`FreeSansBold18pt7b`(42px),
  F_BIG->`FreeSansBold12pt7b`(29px), F_MED->`FreeSansBold9pt7b`(22px),
  F_REG->`DejaVu18`(18px), F_SMALL->`DejaVu12`(13px),
  F_TINY->`DejaVu9`(10px). Every bundled GFXfont is **ASCII-only
  (0x20-0x7E)** -- no ▲/▼/°/· glyphs, unlike `tools/preview.py`'s TTF.
  Hand-draw them (`fillTriangle`, `drawCircle`), as the trend triangle in
  `drawNowStrip()` and the degree ring in `drawWind()` do. The font before
  any `setFont()` is `Font0` (6x8 GLCD); `setTextSize()` only scales it.
  Confirmed by reading source and on-device, 2026-08-28.
- `setTextDatum()` only affects the bare 2-arg-position `drawString(str, x,
  y)` (it reads the persisted `_text_style.datum`). `drawCenterString()`/`drawRightString()` pass their own datum, and
  `setCursor()`+`print()` ignore datum entirely. Reset to
  `textdatum_t::top_left` after using `middle_center` (e.g. the compass
  N/E/S/W labels) in case a later bare `drawString()` inherits it. Read in
  `LGFXBase.hpp`, 2026-08-29.
- The six `C_BLACK/C_WHITE/C_RED/C_YELLOW/C_BLUE/C_GREEN` constants are
  **palette indices (0-5)**, not RGB. The render target is an `M5Canvas` at
  `color_depth_t::palette_4bit`, whose draw calls take the low nibble as an
  index (`misc/colortype.hpp` `convert_uint32_to_palette4()`); the real RGB
  lives in `PALETTE_RGB[6]`, used only by `canvas.createPalette()`.
  **Hazard:** passing `C_WHITE` etc. straight to `M5.Display` (RGB888) draws
  near-black (`0x000001`). Route any new direct draw through the canvas (see
  `clearScreenFull()`).
- `M5Canvas(&M5.Display)` defaults `_psram = true` (`M5GFX.h`):
  `createSprite()` allocates from the 8MB PSRAM, not the ~320KB internal
  heap. A 400x600 `palette_4bit` canvas (120000 bytes) moved
  `ESP.getFreeHeap()` by only 64 bytes (hardware, 2026-09-04). Canvas draws
  never touch the panel: `drawAll()` composes the whole frame, and only its
  single `pushSprite()` + `display()` reach the panel --
  `display()` blocks for the whole 15-30 s refresh.
- `Panel_ED2208::display()` nearest-matches every RGB888 pixel to its own
  6-color table (`epd_palette[]` in `Panel_ED2208.cpp`) with an ordered Bayer dither, and that
  table's blue/green (`{100,64,255}`, `{67,138,28}`) differ from this
  project's `PALETTE_RGB` blue/green (`0x0000BF`, `0x007C00`). So physical
  panel output can't be compared byte-for-byte: **the canvas buffer is the
  deterministic artifact** for automated verification -- it's what a test
  harness should dump and compare.
- `Snapshot` carries a `time_t now` field, sampled once in `setup()` after
  the `connectWifi()` conditional (unconditionally, so a Wi-Fi-failure cycle
  still gets a clock). Every draw function reads `s.now` instead of calling
  `time(nullptr)`, so the header clock, next-event label, now-line and
  footer can't disagree across a minute boundary, and the render is a pure
  function of `Snapshot` -- what makes reproducible automated rendering
  possible.
- `src/render.h`/`src/render.cpp` include no `Arduino.h`, but on the ESP32
  build it's still in their translation unit via `M5GFX.h` ->
  `lgfx/v1/platforms/esp32/Bus_SPI.hpp` -> `misc/datawrapper.hpp`. Its
  `#define`s (`constrain`, `radians`, `DEG_TO_RAD`; deliberately not
  `min`/`max`, so `std::min`/`std::max` are safe) are live there: a local `constexpr float DEG_TO_RAD` was textually replaced
  (`error: expected unqualified-id before numeric constant`), hence the name
  `kDegToRad`. Don't name anything after an Arduino macro in render code.

### Time

- Arduino-ESP32's `getLocalTime()` (`esp32-hal-time.c`) does **not** mean
  "NTP has synced" -- it only waits for `tm_year > 2016`. `M5.begin()` has
  already seeded the system clock from the RX8130
  (`M5.Rtc.setSystemTimeFromRtc()` in `M5Unified.cpp _begin_rtc_imu()`), so
  a plausible stored date passes instantly while the real SNTP correction
  lands later in the cycle. That split made fetched hilo events compare as
  already past for days (see `notes/writeup-material.md`). Poll
  `sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED` (`esp_sntp.h`)
  before reading the clock. Reproduced and fixed on hardware, 2026-08-30
  (`bebec80`).
- **Don't write an unsynced clock back to the RTC** (found by reading the
  code, 2026-09-24; not observed or verified on hardware). After the 10s
  poll, `setup()` used to write the clock to the RTC whether or not sync
  completed. `setSystemTimeFromRtc()` (`RTC_Class.cpp`) reads the RTC's
  digits as UTC (it forces `TZ=GMT0` around its `mktime()`), but the RTC
  holds *local* digits, so the seed epoch is the true epoch minus the UTC
  offset -- under `TZ_STRING` it reads 7h early (PDT) or 8h early (PST), and
  the header clock, `fetchTides()`'s `todayStr`, and the next-event choice
  all used it.
  Fix (`b9607a8`): write the RTC only after a completed sync; otherwise call
  `sntp_stop()` (so a late sync can't move the clock mid-cycle) and
  `setenv("TZ", "UTC0")`/`tzset()`, so `localtime()` shows the RTC's digits.
  That keeps the whole cycle in one "local digits read as UTC" frame:
  `s.now`, and NOAA `lst_ldt` / Open-Meteo `timezone=auto` timestamps parsed
  by `parseLocal()`/`parseIso()`. A Wi-Fi-failure cycle already runs in that
  frame (it never calls `configTzTime()`), and `sleepUntilNext()` schedules
  from the RTC's own digits, so the wake alarm is unaffected either way.
- **Always set `tm_isdst = -1` before `mktime()`.** `struct tm nextTm = {}`
  in `sleepUntilNext()` left it 0 ("standard time"). `TZ_STRING` is
  `PST8PDT,M3.2.0,M11.1.0`; during PDT (UTC-7), `mktime()` then read the
  wake digits as PST (UTC-8) and armed the alarm one hour late, every cycle,
  independent of `UPDATE_MINUTES` (soak 1: 89.3 min/cycle at 30; a rerun at
  `UPDATE_MINUTES=2` added the same ~58-60 min -- a fixed offset, not a
  multiplicative RX8130 fault). **Dormant outside DST**, so a winter test
  wouldn't show it. **The trap:** a diagnostic that compared `mktime()`'s
  output with a `localtime_r()` round trip seemed to rule this out, because
  the two agreed -- but `mktime()`'s post-call fields describe the epoch it
  chose, not whether the caller's `tm_isdst` was right, so both were
  consistently wrong. Decoding the epoch to UTC (`date -u -r <epoch>`) and
  comparing the PST vs PDT reading of the intended digits is what settled
  it. Fixed with `nextTm.tm_isdst = -1;` (`237ef2f`), matching
  `parseLocal()`/`parseIso()`; those are the only other `mktime()` calls in
  `src/`, and `tools/*.py` uses naive `datetime.strptime()`. Confirmed on
  hardware by soak 2 (see "Battery budget").

### Network and data

- NOAA CO-OPS's `begin_date` does **not** accept the `today` keyword the way
  `date` does -- `begin_date=today` returns `{"error":{"message":" Wrong
  Date..."}}` and an empty response. Compute `yyyyMMdd` instead
  (`strftime(..., "%Y%m%d", ...)` in `main.cpp`,
  `dt.date.today().strftime("%Y%m%d")` in `preview.py`). Checked with
  `curl`, 2026-08-29.
- ArduinoJson's `deserializeJson(doc, ...)` calls `dst.clear()` before
  parsing (`Deserialization/deserialize.hpp`, `doDeserialize()`), so reusing
  one `JsonDocument` across retries can't merge stale data. Read in source,
  2026-08-29.
- `WiFiClientSecure`'s TLS handshake timeout defaults to **120 s**, and its
  socket is non-blocking, so a stalled handshake spins until then.
  `httpGetJson()` sets it from its per-call timeout (`6d5d443`): 15 s for
  normal fetches, 5 s for observed wind. Per request the other waits are TCP
  connect 5 s (HTTPClient's default) and DNS ~14 s (lwip's own). Read in the
  framework's `WiFiClientSecure.cpp`/`ssl_client.cpp`, 2026-09-24.
- Read the Wi-Fi disconnect reason **before** calling `WiFi.disconnect()`:
  that call fires its own `ASSOC_LEAVE` disconnect event and overwrites it.

### SDL host build

- **SDL host build** (`src/sdl_main.cpp`, `[env:native]`): links the real
  `src/render.cpp` against M5GFX's vendored LovyanGFX SDL backend
  (`lgfx/v1/platforms/sdl/`, picked automatically by
  `lgfx/v1/platforms/device.hpp` when
  neither `ESP_PLATFORM` nor `ARDUINO` is defined; an empty TU on ESP32).
  Toolchain: `winget install -e --id MSYS2.MSYS2`, then `pacman -S
  mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-make`, plus
  `pio pkg install -g -p native`; `C:\msys64\mingw64\bin` is on the user
  `PATH`, so new shells find `gcc`/`SDL2.dll` (shells started earlier won't
  see it). Build gotchas, none in
  `render.cpp`:
  - mingw-w64 hides `localtime_r()` behind `_POSIX_THREAD_SAFE_FUNCTIONS`,
    not implied by `-std=gnu++14` (strict `-std=c++14` hides it too): pass
    `-D_POSIX_THREAD_SAFE_FUNCTIONS`.
  - Exclude it from the ESP32 envs (`build_src_filter = ... -<sdl_main.cpp>`).
    Its `#error` guard fires, but GCC keeps going and buries it under errors
    like `'lgfx::Panel_sdl' has not been declared`.
  - M5GFX's SDL `autodetect()` (`M5GFX.cpp`) gives `board_M5PaperColor`
    rotation 1, swapping `gfx.width()`/`gfx.height()` to 600x400, while
    `SCREEN_W`/`SCREEN_H` (400x600) aren't rotation-aware -- so
    `canvas.pushSprite(0, 0)` left a blank window. Call
    `gfx.setRotation(0)` after `gfx.init()` (host-only; device rotation was
    never in question).
- **The SDL host build's output is byte-for-byte identical to the device's.**
  `SDL_PREVIEW_DUMP_RAW=<path>` writes the raw `canvas.getBuffer()`/`bufferLength()` bytes
  (the same palette_4bit buffer `tier1HandleRender()` sends over serial) and
  exits without a window; `hil.py decode-raw` decodes it with the same
  `decode_palette4()` and, with `--golden`, compares exactly like `test`:
  ```
  SDL_PREVIEW_DUMP_RAW=raw.bin pio run -e native -t upload
  python tools/hil.py decode-raw raw.bin --golden test/golden/example.png
  ```
  Two host-glue details make that hold: `parseSnapshot()` falls back to
  `"GOLDEN GATE"` for a missing `stationLabel` (as `tier1ParseSnapshot()`
  falls back to `STATION_LABEL`; `sdl_main.cpp` deliberately doesn't include
  `config.h`), and `setup()` forces UTC (`_putenv("TZ=UTC0"); _tzset();` on
  mingw) because `TIER1_TEST`, which the goldens reflect, never sets a
  timezone -- the PC's Pacific default put every timestamp 7 hours off.
  First matched for `example` 2026-09-16 (`bd381d3`), then all five original
  fixtures 2026-09-24.

### Tooling (this Windows dev machine)

- `pio device monitor`'s port auto-detect is **not reliable** once the
  board's USB has vanished mid-PM1-shutdown: it once latched onto "Intel
  Active Management Technology - SOL (COM3)" and showed a normal-looking,
  empty session. Poll `Get-CimInstance -ClassName Win32_PnPEntity |
  Where-Object { $_.Name -match 'USB Serial Device' }` for the real port and
  pass `--port`/`--upload-port` explicitly (e.g. `COM4`).
- Raw `ESP_LOG*` calls in vendored library `.cpp` files (confirmed for
  `M5GFX.cpp`) **never reach the console on this build**. Five attempts failed (2026-09-05; list in
  `notes/writeup-material.md`); the runtime gate is `esp32-hal-misc.c`'s
  `esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL)` with
  `CONFIG_LOG_DEFAULT_LEVEL` = 1 (ERROR) in the precompiled
  `tools/sdk/esp32s3/qio_opi/include/sdkconfig.h`, yet the attempts that
  worked around it still failed; root cause not found (it would need
  reading `esp_log`'s own implementation). `-DCORE_DEBUG_LEVEL=5` makes things worse (floods a 32KB
  TX buffer). What works: Arduino's `log_i()`/`log_e()` (`esp32-hal-log.h`), and plain
  `::printf()`/`Serial.printf()` bracketing inside functions **header-inlined into
  `main.cpp.o`** (e.g. `M5Unified::begin()` in `M5Unified.hpp`). Use that
  technique for any further vendored-library timing.
- `tools/hil.py` serial reads (2026-09-08 and 2026-09-10 fixes): route every
  read through the shared `_LineReader`; never a bare `self.ser.readline()`
  (one blocking syscall per byte on Windows). `Device.__init__` raises the
  driver RX buffer from 4096 bytes to 128KB (`set_buffer_size()`).
  `_LineReader` must request `min(chunk_size, ser.in_waiting or 1)`, never a
  fixed count: this port's COMMTIMEOUTS (`ReadIntervalTimeout=0`,
  `ReadTotalTimeoutMultiplier=0`, `ReadTotalTimeoutConstant=timeout*1000`)
  make `ReadFile` wait the whole timeout for the full count (the 2026-09-10
  entry's finding; re-checkable in pyserial's `serialutil.py`/
  `serialwin32.py`). Real transfer (RENDER sent -> `---FB-END---`) for a 400x600 dump (~162KB
  of base64) is
  **~220-235ms, ~700KB/s**; the older "~10.2s / 16KB/s" figure was that
  artifact, as was the apparent `CHUNK_BYTES` invariance -- whether
  `CHUNK_BYTES` matters is unproven. The device-side 16KB write batching
  is a separate, real fix. `--timeout` (`SERIAL_TIMEOUT_S`) is **10s**: ~40x
  the real transfer and 2x `tier1WriteAll()`'s 5000ms give-up, so the
  device's `#ERR` line arrives before the host times out. `DUMP_MS`
  (~160ms) is `Serial.write()` enqueue time, not transfer time.
- The VS Code PlatformIO extension installs the default env's `lib_deps`
  from the repo's `platformio.ini` whenever `.pio/libdeps` changes, even
  with `platformio-ide.autoRebuildAutocompleteIndex` off. It can race a
  concurrent build (duplicate installs, then `opening dependency file ... .d:
  No such file or directory`). Don't validate an alternate config
  (`pio run -c ...`) while VS Code has the project open without checking
  `integrity.dat` and the dependency graph afterwards.
- `pio run -v` fails at the `firmware.bin` step with a SCons `TypeError`
  while *printing* the command line (tool-scons 4.11.1, `Action.py
  print_cmd_line`). Build without `-v`; a `-v` run with nothing left to
  build still prints the dependency graph with paths.
- No usable Python on `PATH` (only the Microsoft Store stubs). PlatformIO's
  own `~/.platformio/penv/Scripts/python.exe` (3.11.7) has Pillow and
  pyserial, and runs `tools/*.py`.

---

## Battery budget

Target: roughly a month per charge on the 1250mAh cell. **Current best
estimate: ~15 days** -- about half.

- **Soak 2** (`237ef2f`, 2026-09-16 14:32 cycle 385 at 100% -> 2026-09-24
  20:23 cycle 760 at 46%): 375 cycles over 197h51m (**8.24 days**), **31.7
  min/cycle** (~45.5 cycles/day). Drain 54 points = **~6.55%/day,
  0.144%/cycle**; linear projection **~15 days per charge**. Raw readings in
  `notes/soak-2026-09-11.md`. **Gauge caveat:** assumes
  `M5.Power.getBatteryLevel()` is linear in remaining capacity (unverified;
  how the gauge works is unconfirmed), and the device may stop completing
  cycles before the gauge reads 0%. The 2026-09-24 reading was taken with USB
  attached for about 2 minutes; any charge that added inflates the final
  figure.
- **Soak 1** (2026-09-11 to 2026-09-14, 100%->98% over 45 cycles / 66h57m)
  was read as "month-shaped, not week-shaped" drain. **Withdrawn:** soak 2
  drained ~3.3x faster per cycle (0.144% vs 0.044%). The cause is
  unconfirmed -- the gauge may be nonlinear near full charge. Don't use soak
  1's figures. (Its 89.3 min/cycle cadence was the `tm_isdst` bug.)
- **Cadence caveat:** a 12-cycle segment early in soak 2 (cycle 385
  14:32:48 -> cycle 397 20:22:38, 2026-09-16) ran at ~29.2 min/cycle, which
  the minute-resolution alarm can't produce (a 30-min alarm plus ~78-93s
  awake gives at least 31 min). Unexplained, not investigated.
- **Awake time:** ~78 s/cycle measured with USB attached (2026-09-05; ~50 s
  `M5.begin()`, ~10.5 s Wi-Fi + fetch, ~17 s render + refresh) -- about 4x
  the original ~20 s assumption. On battery `237ef2f` also sat out the
  ~15.5 s USB serial wait, so ~93 s. Observed wind will add ~1-2 s and the
  serial-wait fix remove ~15.5 s.
- **Energy split (rough, gauge-dependent):** 54% of 1250mAh ≈ 675mAh over
  8.24 days = ~82mAh/day, ~1.8mAh/cycle. Datasheet standby (~2.2mAh/day) is
  under 3% of that, so ~97% of daily energy is awake time: ~1.75mAh per ~93s
  window, i.e. ~67mA average awake -- below the ~150mA once assumed (the original
  budget: ~20 s at ~150mA = ~0.85mAh/cycle, 48 cycles/day ≈ 41mAh, plus
  ~2.2mAh/day sleeping ≈ 43mAh/day). The run-to-empty (on
  `237ef2f`, ending ~2026-10-01) replaces the projection with a measured
  days-per-charge figure.

---

## Design constraints for this project

- **Update cadence: 30 minutes** by default. Do not go below 15 — the refresh
  itself takes up to 30s and each cycle costs battery.
- Everything must be readable at arm's length in daylight. Assume the user
  glances at it for two seconds.
- Layout is designed host-side in `tools/preview.py`, which renders the exact
  400x600 palette-constrained image. **Iterate there first.** A layout change
  is milliseconds on the laptop and 30 seconds on the device. (The SDL build
  is the real render path; `preview.py` is PIL.)

### Error display

A failed fetch never draws a plausible-looking value. `Snapshot` carries
`fail` (`FAIL_*` bits, `render.h`) and `wifiReason`;
`setup()`/`fetchTides()`/`fetchWind()` set the bits from what actually
arrived (`fd2298b`).

- **Header:** red with `NO WIFI` (line 2: date/time + the disconnect reason,
  e.g. `NO_AP_FOUND`, or `NO_IP` / `TIMEOUT`) or `NO DATA` (every fetch
  failed); yellow with `PARTIAL DATA` or `NO TIME SYNC`. The title replaces
  the station name.
- **Line 2 for partial data:** `MISSING` plus one token per missing source,
  in fixed order `CLOCK LEVEL HILO TIDE WIND FCST` (clock first, then top to
  bottom on screen). Too wide: drop the date (time only); still too wide:
  trailing tokens become `+N`. With today's tokens the time-only form always
  fits, so `+N` can't currently trigger.
- **Sections:** `--` for the tide level and for wind speed, gust and
  direction (no arrow, chip outline only); `NO TIDE DATA` in the tide box;
  `NO FORECAST` in the bar area. An SHT40 failure shows `--C --%` without
  changing the header colour.
- The Wi-Fi reason on the panel is what makes a Wi-Fi failure diagnosable on
  battery, where serial output is lost.
- **Not handled:** after a full discharge the RX8130 may lose time
  (unverified). If the first boot after recharging also fails Wi-Fi or time
  sync, the header shows the RTC's reset time with nothing marking it wrong.

---

## Serial log lines

One grep-able line per event, visible only with a USB host attached (see
"Verified corrections" -> USB serial).

| Prefix | When | Fields |
|---|---|---|
| `BATLOG` | every cycle | `cycle=`, `level=%`, `vbat_mv=` (-1 if the PM1 read failed) |
| `WINDSRC` | every cycle | `src=obs\|fcst\|none`, `reason=` (`ok`, `http`, `noaa-error`, `parse`, `stale`), `age_min=` |
| `WIFIFAIL` | Wi-Fi timeout only | `status=<WL_ name>(<n>)`, `reason=<code>(<name>)`, `elapsed_ms=` |
| `WIFISCAN` | Wi-Fi timeout only | one per network: `ssid="..." ch= rssi= target=0\|1`; then `done count=N target_seen=0\|1` (or `failed code=`) |

- `WIFIFAIL`'s reason comes from an `ARDUINO_EVENT_WIFI_STA_DISCONNECTED`
  handler. The success path only pays for registering it; the failure path
  adds an active scan, ~1.5-4s (framework cap 10s). Scan lines include
  neighbors' SSIDs -- check a log before pasting it anywhere public.
- Also: `ntp failed: not synced in 10s, using RTC time` on a time-sync
  failure, and `tides=%d wind=%d events=%d` after the fetches.
- To test the Wi-Fi lines, provoke a failure: set `WIFI_SSID` in
  `src/config.h` (gitignored) to a network that doesn't exist, flash, and
  watch one cycle with the monitor attached. Expect
  `status=WL_NO_SSID_AVAIL(1) reason=201(NO_AP_FOUND)` and `target_seen=0`.
  A wrong `WIFI_PASS` should give an authentication reason instead (e.g.
  `AUTH_FAIL` or `4WAY_HANDSHAKE_TIMEOUT`). Restore `config.h` and reflash.

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
The firmware widens this with `begin_date=<yyyyMMdd>&range=48` so tomorrow's
first event is always in view, retries once, then falls back to `date=today`.
hilo looked like the flakiest call early on, but its multi-day blank streak
was the clock bug (see "Verified corrections" -> Time).

Observed water level right now (falls back to the nearest hourly
prediction):
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
   `product=wind`, `date=latest`. Returns speed `s`, direction degrees `d`,
   gust `g` (as strings). 9414290 has an active Wind sensor, C1, 24 ft above
   the site (`mdapi/prod/webapi/stations/9414290/sensors.json`). Checked
   2026-09-24: `date=latest` was 10 min old, and 14 days of history
   (2026-09-11 to 2026-09-25 GMT) showed a reading every 6 min with no gaps,
   no empty values, and all quality flags `0,0`. With `units=english`, `s`
   and `g` are knots (checked against `units=metric`: 4.4 m/s = 8.55 kn).
   `fetchWind()` uses it only if the reading is within 30 min of now, on a
   5 s timeout, and logs `WINDSRC` (`6d5d443`, **not yet flashed**).
2. **Forecast**, always available: Open-Meteo, no API key. Fetched every
   cycle for the forecast bars; its `current` values are the fallback when
   the observed reading fails or is more than 30 min old.

```
https://api.open-meteo.com/v1/forecast?latitude=37.81&longitude=-122.47
&hourly=wind_speed_10m,wind_direction_10m,wind_gusts_10m
&current=wind_speed_10m,wind_direction_10m,wind_gusts_10m
&wind_speed_unit=kn&timezone=auto&forecast_days=2
```

### TLS note

Both endpoints are HTTPS. The device uses `WiFiClientSecure` with
`setInsecure()` (no cert pinning). If you want real verification, pin the
root CA and be prepared to update it when it rotates. See "Verified
corrections" -> Network for the handshake timeout.

### JSON sizing

Hourly predictions ≈ 25 entries, hilo ≈ 4, Open-Meteo hourly ≈ 48. All small.
If you switch to 6-minute predictions (240 entries), allocate the ArduinoJson
document in PSRAM and use a filter — don't parse the whole body on the stack.

---

## Build and flash

```bash
pio run                       # build (just the two ESP32 envs -- see [platformio] default_envs)
pio run -e m5stack-papercolor -t upload --upload-port COMx   # flash (see notes below)
pio device monitor -b 115200  # serial log
python tools/preview.py       # regenerate the layout preview PNG (PIL, not the real M5GFX draw path)
pio run -e native -t upload   # host-side SDL preview (real render path); SDL_PREVIEW_FIXTURE=<json> picks the fixture
SDL_PREVIEW_DUMP_RAW=raw.bin pio run -e native -t upload   # dump the canvas instead of opening a window
```

You (Claude) can and should run these directly. Read the compiler output and
the serial log yourself rather than asking the user to paste them.

- **Always name the env when uploading.** `default_envs` lists both ESP32
  envs, so a bare `pio run -t upload` flashes them in turn and leaves the
  *test* firmware on the board. Use `-e m5stack-papercolor` (production) or
  `-e m5stack-papercolor-test` (harness).
- Uploading does **not** need the power button while the board is powered
  (awake mid-cycle, or in `TIER1_TEST`'s command loop): esptool's RTS reset
  enters the bootloader (10+ button-free uploads, 2026-09-05). The button
  **is** needed mid-sleep, when the PM1 has cut power and the port doesn't
  exist -- a distinct error (`Could not open COMx, the port is busy or
  doesn't exist` / `FileNotFoundError`, not an esptool sync failure). Check
  whether the port enumerates before assuming either; then wait for the next
  RX8130 wake or ask for the button. Don't retry blindly.

---

## Pinned versions

Every build dependency is pinned in `platformio.ini`, to exactly what was
installed on 2026-09-25 -- the versions every hardware result in this file
was measured on. **Upgrading any of them means re-running `python
tools/hil.py test --all` on the device** before trusting the build (and, if
M5GFX changes, re-checking the SDL renders with `hil.py decode-raw
--golden`).

| Dependency | Pinned to | Envs |
|---|---|---|
| espressif32 platform | 6.12.0 | both ESP32 |
| framework-arduinoespressif32 | 3.20017.241212+sha.dcc1105b (Arduino-ESP32 2.0.17) | both ESP32 |
| tool-esptoolpy (builds `firmware.bin` from the ELF) | 2.40900.250804 | both ESP32 |
| toolchain-xtensa-esp32s3 | 8.4.0+2021r2-patch5 (already exact in the platform's `platform.json`) | both ESP32 |
| M5GFX | git `d91077b9a607b59404e4e4a49f775c792bfae382` (0.2.28) | all three |
| M5Unified | git `8530f5377d782e4a25a6c482de2e71c3f75ca8eb` (0.2.21) | both ESP32 |
| M5PM1 | git `be9a5456c007c333e7ac963f33bfde1ffa5d82ee` (1.0.7) | both ESP32 |
| ArduinoJson | 7.4.3 | all three |
| native platform | 1.2.1 | native |

Not pinnable from `platformio.ini`: the native env's compiler is MSYS2's own
GCC (16.2.0 on 2026-09-25), which PlatformIO doesn't manage.

- **Keep M5GFX listed before M5Unified in `lib_deps`.** M5Unified's
  `library.json` depends on `M5GFX >=0.2.28`. If that is resolved first,
  PlatformIO downloads the newest registry M5GFX (0.2.30 on 2026-09-25)
  instead of the pinned commit. Confirmed with a fresh install: listed
  first, only the four pinned libraries were installed.
- **Native M5GFX was aligned to the device's commit.** Before pinning, the
  native env had resolved M5GFX 0.2.29 (`641944b`), newer than the device's
  0.2.28 (`d91077b`). On `d91077b` it builds with 0 warnings, all five
  original fixtures still `MATCH` their device goldens, and all five
  failure fixtures render byte-identical to their earlier `641944b`
  renders (2026-09-25).
- **Pinning changed no code or data (verified 2026-09-25).** Clean builds
  are deterministic, and pinned vs. unpinned `firmware.bin` differ only in
  embedded M5GFX source paths, `app_elf_sha256`, and the image trailer --
  full analysis in the pinning commit's message (`81f0813`). Consequence:
  `firmware.bin` hashes can change when only a libdeps folder name changes,
  with identical code.
- Two traps hit during that verification (the VS Code extension race and
  the `pio run -v` crash) are under "Verified corrections" -> Tooling.

---

## Test harness (Tier 1 / hil.py)

```bash
python tools/hil.py ping                                            # confirm the board is alive
python tools/hil.py render test/fixtures/example.json --out actual.png
python tools/hil.py test test/fixtures/example.json --golden test/golden/example.png
python tools/hil.py test --all                                      # every fixture, one session
python tools/hil.py decode-raw raw.bin --golden test/golden/example.png   # SDL dump, no device
```

Requires a board flashed with the `m5stack-papercolor-test` PlatformIO env
(`pio run -e m5stack-papercolor-test -t upload --upload-port COMx`) -- no
Wi-Fi, no NOAA/Open-Meteo fetch, no PM1 sleep, no panel refresh. `render`
sends a Snapshot fixture over USB serial, gets the rendered canvas buffer
back as base64, and decodes it to a PNG. `test` does the same and diffs the
result against a golden image byte-for-byte (`images_equal()` in
`tools/hil.py` -- exact match, not a perceptual/threshold diff).

A real round trip (RENDER sent -> framebuffer fully decoded) is **~230ms**
(see "Verified corrections" -> Tooling) -- closer to a fast unit test than a
hardware step, so run it on every render-affecting change.

- **Exit codes are distinct.** `EXIT_MISMATCH` (1): `test` compared a real
  render against the golden and found a difference -- act on it.
  `EXIT_DEVICE_ERROR` (2): the comparison never happened (board unplugged,
  wrong firmware, protocol error, missing/malformed fixture, missing golden)
  -- fix the environment, not the render. A loop can treat 2 as "retry or
  alert" and 1 as "real". On mismatch, `test` writes `expected.png`,
  `actual.png`, and `diff.png` (the golden with mismatched pixels in
  magenta) into `--out-dir` (default: cwd). Exit 0 with `MATCH: ...` means
  pixel-identical.
- **`test --all`** walks `test/fixtures/*.json`, pairs each with
  `test/golden/<name>.png`, and runs them in one serial session (~230ms per
  fixture). Prints PASS/FAIL per fixture plus a summary. Exit 0 only if
  every fixture matches, 1 if any fails, 2 if a `DeviceError` aborts the run
  (remaining fixtures go untested). **A missing golden's exit code differs
  by mode, deliberately:** 1 in `--all` (that fixture fails, the run
  continues -- "exit 0" must mean everything checkable passed) vs. 2 in
  single-fixture `test`. Mismatches write to
  `<out-dir>/<fixture>/{expected,actual,diff}.png`.
- **Fixtures are generated, not hand-authored.** `tools/make_fixture.py`
  derives both `tide[]` (hourly samples) and `events[]` (the model's true
  extrema) from one analytic tide curve -- the one `tools/preview.py`'s
  `synthetic()` uses -- because a hand-typed `events[]` once drifted 45-90
  minutes from the curve unnoticed (2026-09-09 investigation). Regenerate
  with `python tools/make_fixture.py --out test/fixtures/example.json`;
  don't hand-edit `tide[]`/`events[]`. Failure fixtures come from the same
  generator: `--fail NAME` (repeatable; names as in `render.cpp`'s
  `FAIL_INFO`) and `--wifi-reason`. Each failure also blanks the data it
  leaves missing, so a fixture can't carry values its own flags say never
  arrived. For example: `--fail wifi --wifi-reason NO_AP_FOUND --out
  test/fixtures/fail_wifi.json`, or `--fail wind_now --fail forecast` for
  `fail_wind.json`.
- **Goldens live in `test/golden/` and are committed.** `actual.png`,
  `expected.png`, and `diff.png` are gitignored -- `test` output, not source
  of truth.
- **Regenerating a golden is a deliberate, separate act.** It asserts "this
  new output is correct," a judgment call a diff can't make for you -- never
  overwrite `test/golden/*.png` as a side effect of the change it's meant to
  validate, and never fold that update into the same commit. Regenerate it
  (`python tools/hil.py render ... --out test/golden/example.png`), look at
  it, and commit it on its own with a reason (e.g. "update golden for the
  wind-chip layout change" -- not just "update golden").

---

## Repo layout

```
src/config.h              Wi-Fi creds, station IDs, coordinates, cadence (gitignored; copy src/config.example.h)
src/main.cpp              wake -> connect -> fetch -> draw -> sleep; also the TIER1_TEST harness
src/render.h/.cpp         Snapshot, FAIL_* flags, drawHeader()/drawNowStrip()/... -- shared by main.cpp and sdl_main.cpp
src/sdl_main.cpp          [env:native] host entry point -- see Verified corrections -> SDL host build
src/layout.h              GENERATED from layout.json -- do not edit by hand
layout.json               shared pixel-geometry source of truth (firmware and preview.py)
palette.json              the six palette colors
tools/hil.py              Tier 1 device test harness + decode-raw for SDL dumps
tools/make_fixture.py     generates test/fixtures/*.json
tools/preview.py          host-side layout renderer, palette-accurate (PIL, not the real M5GFX draw path)
tools/gen_layout_header.py  layout.json -> src/layout.h, run automatically by `pio run`
tools/fonts/              DejaVu TTFs for preview.py, with their own LICENSE
test/fixtures/, test/golden/  Snapshot fixtures and committed golden PNGs
notes/                    soak logs, experiment notes, write-up material
docs/                     implementation plans
DONE.md                   definition of done
README.md, LICENSE        README (placeholder) and MIT license
refs/                     vendored upstream sources, read-only reference (gitignored)
```

Pixel coordinates (box positions, radii, offsets -- not colors, fonts, or text)
live in `layout.json`, not in `tools/preview.py` or `src/*.cpp` directly.
Change layout there; `pio run` regenerates `src/layout.h` automatically via
the `extra_scripts` hook in `platformio.ini`, and `preview.py` reads
`layout.json` itself at runtime. `layout.json`'s `preview_only` section holds
only `preview.py`'s own PIL font-metric offsets.
