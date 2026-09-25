# Definition of Done: tide-wind-tracker

This file defines when the project is finished. Anything not listed here is out of scope.

**Agents:** use this file as the reference for gap audits. Do not add, remove, or reword criteria without explicit user instruction. Report status against each item; do not check boxes yourself.

**Sequencing rule:** the baseline run-to-empty (started 2026-09-16, expected to end around 2026-10-01) must not be interrupted. Do not flash or connect USB until it completes; USB charges the battery and invalidates the measurement. Firmware changes may be written and reviewed (and render changes checked with the SDL build) before then, and flashed together afterwards.

## 1. Runs unattended

- [x] Wake cadence verified on hardware: 31.7 min/cycle average over 375 cycles (soak 2, 2026-09-16 to 2026-09-24) after the `tm_isdst` fix (see CLAUDE.md, notes/soak-2026-09-11.md)
- [x] No hang on failure: every wait is bounded (Wi-Fi 20s, time sync 10s, fixed hilo retries; per HTTP request: DNS ~14s, TCP connect 5s, TLS handshake 120s on `237ef2f` (the measured firmware) and 15s after the observed-wind change (5s for the wind request), 15s between received bytes) and setup always reaches `drawAll()` and `sleepUntilNext()` (audit 2026-09-24)
- [ ] A battery wake does not wait for a USB serial connection (verify `main.cpp` serial wait behavior without USB; fix if it waits)
- [ ] Wi-Fi failures are diagnosable: `connectWifi()` logs the status code, disconnect reason, and visible networks on failure; the cause of `NO_AP_FOUND` is identified or documented
- [ ] A failed fetch renders an explicit error state, never plausible-looking default values (e.g. 0 kn, 0.0 ft), covered by a fixture and golden
- [ ] Observed wind resolved: either implemented per "Data sources", or CLAUDE.md records Open-Meteo-only as deliberate, with the reason
- [ ] Days per charge measured by the baseline run-to-empty ("empty" = device no longer completes a wake cycle; last completed cycle number and time recorded), entered in the CLAUDE.md battery budget, and compared to the ~1 month design goal. The shortfall (projected ~15 days) is documented as a known limitation. The firmware version measured is stated, along with any later change that affects awake time

## 2. Reproducible from a clean clone

- [ ] `src/config.example.h` committed (exists); the README gives the step to copy it to `src/config.h` and which values to fill in
- [ ] 2.4 GHz Wi-Fi requirement documented (the ESP32-S3 has no 5 GHz radio)
- [x] DejaVu fonts vendored in `tools/fonts/` with their license file (`9ead3ff`)
- [x] Library versions pinned in `platformio.ini` (M5Unified, M5GFX, M5PM1) to the versions currently in use (`81f0813`)
- [ ] Firmware builds from a fresh clone following only the README steps
- [ ] SDL host build documented in the README, including the MSYS2 install-path assumption, and builds from a fresh clone

## 3. Tested

- [ ] SDL host render is byte-identical to the device goldens for every fixture
- [ ] Every golden update has a stated reason in its commit message (initial goldens are exempt)
- [ ] `hil.py test --all` passes on device against all goldens, re-run as the last step before tagging

## 4. Ready to publish (if chosen later)

- [x] Git history scanned for credentials: `src/config.h` never tracked; only placeholder Wi-Fi values committed; NOAA and Open-Meteo need no API keys (audit 2026-09-24)
- [x] LICENSE file added (`2602686`)
- [ ] Commit author emails (all noreply, verified) and station/location choice confirmed intentional by Chris

## 5. Documented

- [ ] CLAUDE.md "Current state" and "Next steps" brought up to date (stale headers fixed, overlapping sections merged, soak 2 results recorded, and the "month-shaped" battery conclusion from soak 1 corrected)
- [ ] README covers: what it is, a photo of the device, a sample render, an architecture overview, build and run steps, the testing approach, and known limitations
- [ ] Known limitations in the README include measured battery life vs the one-month goal, the ~50s `M5.begin()` startup cost as its main cause, and TLS via `setInsecure()`
- [ ] Write-up drafted (audience and format: TBD by Chris)

## Out of scope / future work

- Tier 2 camera-based verification of the physical panel
- Showing stale data after a failed fetch (would require persisting the last snapshot to flash)
- OBS/FCST source label on the display
- Reducing the `M5.begin()` startup cost (PMIC auto-probe theory unconfirmed). This is the primary lever for battery life: ~97% of daily energy goes to awake time, and `M5.begin()` is most of the ~78s awake window. Lead future-work item in the write-up
- Pinning the TLS root CA in place of `setInsecure()`
- 6-minute tide predictions
- Choosing a closer NOAA station (a `config.h` setting)
- Root cause of `ESP_LOG*` output not reaching the console (workaround in place)
- Whether `CHUNK_BYTES` affects transfer speed
- Anything discovered during an audit goes here unless it blocks a criterion above

## Finish line

When every box is checked: tag `v1.0` and stop. Making the repo public is a separate decision, not part of this definition of done.