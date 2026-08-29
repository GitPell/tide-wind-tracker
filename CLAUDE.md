# Tide & Wind Tracker — M5Stack PaperColor (SKU C151)

Battery-powered e-paper dashboard showing tide state and wind conditions for a
coastal location. Wakes on a timer, fetches data over Wi-Fi, redraws, sleeps.

---

## Current state (as of 2026-08-28)

**Works, confirmed on real hardware:**
- Build pipeline: `pio run` regenerates `src/layout.h` from `layout.json` and
  compiles clean.
- Full cycle: Wi-Fi connect -> NTP -> NOAA tide fetch -> Open-Meteo wind fetch
  all succeeding (`tides=1 wind=1` in `monitor.log`).
- Panel power-up (PM1 GPIO0 `EPD_EN`) and the full draw: header, "now strip"
  (current tide value, trend arrow, next high/low), tide curve, wind section,
  wind forecast bars all render correctly and are legible on the panel.
- Real fonts throughout, replacing the tiny default `Font0` bitmap font
  everything started out using (see Verified corrections below).
- SHT40 indoor temp/humidity read.

**Implemented but not yet confirmed on hardware:** the forecast section label
("WIND, NEXT 24H (kt)"), hour-of-day ticks, footer, and the hand-drawn degree
ring next to the wind direction were just added -- built clean, not yet seen
on the actual panel. Get a fresh device photo before trusting the spacing,
the same way the GUST/FROM line spacing needed one correction after the
first look.

**Known gaps vs. the `tools/preview.py` design** (tracked in `layout.json`'s
`preview_only` section): compass N/E/S/W labels around the wind circle, and
the barbed wind arrowhead (`main.cpp` draws a plain thick line instead) --
both cosmetic, not bugs.

**Not yet exercised at all:**
- The real low-power sleep path. `setup()` currently ends with
  `delay(60000); ESP.restart();` (look for the `// TODO: restore before
  battery testing` comment) instead of calling `sleepUntilNext()`. Every test
  cycle so far has been a software restart, not a real PM1 shutdown + RX8130
  wake -- the actual cold-boot-every-cycle path, and the PM1-RTC-RAM-backed
  full-refresh counter that depends on it, has never run on this hardware.
- Battery life / current draw. The 92.53uA standby figure is a datasheet
  target derived from reading the reference firmware, not a measurement on
  this board.

**Next steps, roughly in order:**
1. Flash and photograph the latest changes (forecast label/ticks, footer,
   degree ring); fix any spacing issues the same way the GUST/FROM cramping
   was fixed.
2. Decide whether the compass labels and arrow barbs are worth adding, or
   leave the device's simplified rendering as final.
3. Swap `delay(60000); ESP.restart();` back for `sleepUntilNext()` and
   confirm on real hardware that the board actually wakes on schedule via
   the RX8130 alarm.
4. Measure real standby/active current and run a multi-day battery soak test.

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
  wake input. Confirmed by reading source, 2026-08-28.

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

---

## Design constraints for this project

- **Update cadence: 30 minutes** by default. Do not go below 15 — the refresh
  itself takes up to 30s and each cycle costs battery.
- Everything must be readable at arm's length in daylight. Assume the user
  glances at it for two seconds.
- Layout is designed host-side in `tools/preview.py`, which renders the exact
  400x600 palette-constrained image. **Iterate there first.** A layout change
  is milliseconds on the laptop and 30 seconds on the device.
- Target battery life: roughly a month per charge. Rough arithmetic: ~20s awake
  at ~150mA average = ~0.85mAh per cycle, 48 cycles/day ≈ 41mAh, plus ~2.2mAh/day
  sleeping ≈ 43mAh/day against 1250mAh. Treat as ballpark, measure for real.

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
pio run -t upload             # flash (hold side reset to enter download mode)
pio device monitor -b 115200  # serial log
python tools/preview.py       # regenerate the layout preview PNG
```

You (Claude) can and should run these directly. Read the compiler output and
the serial log yourself rather than asking the user to paste them.

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
`layout.json` itself at runtime. `layout.json` still documents (in its
`preview_only` section) two elements `preview.py` renders that `main.cpp`
does not implement -- compass cardinal labels and the barbed wind
arrowhead. See "Current state" at the top of this file for what's confirmed
working on real hardware versus just built.
