# Definition of Done: tide-wind-tracker

This file defines when the project is finished. Anything not listed here is out of scope.

**Agents:** use this file as the reference for gap audits. Do not add, remove, or reword criteria without explicit user instruction. Report status against each item; do not check boxes yourself.

## 1. Runs unattended

- [x] Wake cadence verified on hardware: 29.2 min observed after the `tm_isdst` fix (see CLAUDE.md, notes/soak-2026-09-11.md)
- [ ] Days per charge measured (run-to-empty soak, or measured current draw), recorded in the CLAUDE.md battery budget, and compared to the ~1 month design goal. A shortfall counts as done if it is measured and documented
- [ ] Wi-Fi failures are diagnosable: `connectWifi()` logs the actual status code, and the cause of `NO_AP_FOUND` is identified or documented
- [ ] A Wi-Fi or data-fetch failure does not hang the device: it renders a stale/error state and goes back to sleep *(verify; may already be true)*
- [ ] Observed wind resolved: either implemented per "Data sources", or CLAUDE.md records Open-Meteo-only as deliberate, with the reason (e.g., station 9414290 has no wind sensor)

## 2. Reproducible from a clean clone

- [ ] `config.h.example` committed; the README lists which values to fill in
- [ ] 2.4 GHz Wi-Fi requirement documented (the ESP32-S3 has no 5 GHz radio)
- [ ] DejaVu fonts vendored in `tools/fonts/` with their license file
- [ ] Firmware builds from a fresh clone using only the documented PlatformIO steps
- [ ] SDL host build (MSYS2, MinGW-w64, SDL2) documented and builds from a fresh clone

## 3. Tested

- [ ] `hil.py test --all` passes on device against all committed goldens
- [ ] SDL host render is byte-identical to the device goldens
- [ ] Every golden commit has a stated reason in its commit message

## 4. Ready to publish (if chosen later)

- [ ] Full git history scanned for credentials (Wi-Fi SSID/password, anything that lived in `config.h`); none found, or history rewritten
- [ ] LICENSE file added
- [ ] Commit author emails and location/station choices reviewed and intentional

## 5. Documented

- [ ] CLAUDE.md "Current state" and "Next steps" brought up to date (stale headers fixed, overlapping sections merged)
- [ ] README covers: what it is, a photo of the device, a sample render, an architecture overview, build and run steps, the testing approach, and known limitations
- [ ] Known limitations include the ~50s `M5.begin()` startup cost and TLS via `setInsecure()`
- [ ] Write-up drafted

## Out of scope / future work

- Tier 2 camera-based verification of the physical panel
- Reducing the `M5.begin()` startup cost (PMIC auto-probe theory unconfirmed)
- Pinning the TLS root CA in place of `setInsecure()`
- 6-minute tide predictions
- Choosing a closer NOAA station (a `config.h` setting)
- Root cause of `ESP_LOG*` output not reaching the console (workaround in place)
- Whether `CHUNK_BYTES` affects transfer speed
- Anything discovered during an audit goes here unless it blocks a criterion above

## Finish line

When every box is checked: tag `v1.0` and stop. Making the repo public is a separate decision, not part of this definition of done.
