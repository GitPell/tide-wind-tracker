# Process notes: tide-wind-tracker

Raw material for the write-up, alongside `notes/writeup-material.md`. Written by Claude (claude.ai chat) from the project's planning conversations, which are not in the repo:

- **HIL design** (around 2026-09-02): the tier ladder, camera rig, and Raspberry Pi discussion.
- **Exact-match preview and Tier 1 build-out** (around 2026-09-03 to 2026-09-17): the SDL decision, render-module extraction, and the soak.
- **Finishing** (2026-09-24 to 2026-09-25): DONE.md, the firmware freeze, and documentation.

**For agents using this file:** where these notes and the repo disagree, the repo and git history win. Verify dates, figures and commit references against git before using them in the write-up, and flag anything that doesn't match.

*Checked against the repo and git history on 2026-09-27; corrections applied. Details that only exist in the planning conversations (the camera rig, prices, prompt wording, planning dates) aren't in the repo and weren't checkable.*

---

## 1. The goal

The project's real purpose was to find out how to let an AI coding agent (Claude Code) iterate on firmware whose output is a physical e-paper panel. The tide and wind dashboard is the test subject.

The founding question, paraphrased: *how can Claude Code make a code change, flash the device, observe the display, compare it to the expected result, and loop on discrepancies?*

The key reframing came early: **verification, not code generation, is the bottleneck.** The agent can write drawing code quickly. What it can't do is see a Spectra 6 panel. The panel takes seconds to refresh, has six colours and no anti-aliasing, and wears with use.

## 2. The tier ladder, and why the camera comes last

The design discussion proposed a three-tier ladder instead of a camera-first loop:

- **Tier 0, host render (milliseconds).** Originally `tools/preview.py`, a Python/PIL reimplementation of the drawing code.
- **Tier 1, framebuffer dump (seconds).** A test build of the firmware (`TIER1_TEST`) renders a fixture into the same off-screen canvas the real firmware uses and sends the canvas bytes back over USB serial, without refreshing the panel (and with no Wi-Fi or sleep). The host compares them byte-for-byte against a committed golden image.
- **Tier 2, camera (about a minute per run).** It only proves that the *panel* did what the framebuffer said: waveform problems, ghosting, power-rail issues, odd red or yellow behaviour.

The argument for this order: a framebuffer check catches the large majority of rendering bugs (wrong coordinates, font metrics, palette snapping), and it pinpoints them exactly, with no panel wear and no optics. A camera-only loop would spend a minute per iteration on bugs a seconds-long check localises precisely. Tier 2 was framed from the start as a luxury tier that might never be needed if Tier 1 was built well.

**Build Tier 1 on the workstation first, with no Raspberry Pi.** Adding a second machine before the loop worked would have mixed render bugs with SSH, mount and sync bugs.

## 3. The determinism decisions that made Tier 1 work

- **Fixtures instead of live data.** Live NOAA and Open-Meteo data change between any two renders, so every comparison would differ. Tests draw from JSON `Snapshot` fixtures instead.
- **Fixtures over serial, not over the network.** Fixtures go into the device over the USB serial link, not from a mock server over Wi-Fi. This isolates rendering from fetching. It also meant pixel-accurate regression testing worked while Wi-Fi (`NO_AP_FOUND`) was still unresolved.
- **The canvas buffer is the test artifact.** An `M5Canvas` in `palette_4bit` mode (120,000 bytes, in PSRAM) is deterministic, while what the panel physically shows is not.
- **Byte-exact goldens, zero tolerance.** Any difference either fails the test or becomes a deliberate golden update, committed separately with a stated reason. A golden is updated by rendering straight to its path (`hil.py render ... --out test/golden/<name>.png`), never autonomously by the agent. (`hil.py` has no `--update-golden` flag.)
- **A single-sourced palette.** `palette.json` feeds the firmware (via generated `src/layout.h`), `tools/preview.py` and `tools/hil.py`, removing a three-way drift.
- **Generated fixtures.** `tools/make_fixture.py` builds fixtures from a shared analytic tide model, so a fixture's `tide[]` and `events[]` can't contradict each other.
- **Draw functions are pure functions of `Snapshot`.** The worry was that an agent under pressure will "helpfully" add a device call into drawing code. In the repo, the rule is documented in CLAUDE.md as a property (the "`Snapshot` carries a `time_t now` field" entry) and enforced structurally: `render.h`/`render.cpp` include no Arduino, M5Unified or network headers, and the SDL build compiles them with no Arduino at all. A four-way time race between draw functions (header clock, next-event label, now-line, footer) was fixed by putting `time_t now` into `Snapshot` (`f9da748`).
- **Result:** `tools/hil.py test --all` runs every fixture in one serial session at about 230 ms per fixture.

## 4. Making Tier 0 exact: one renderer, not two

`preview.py` kept drifting from the device, starting with a font-size mismatch, because it was a *reimplementation*. The decision was to stop having two renderers. The same C++ drawing code would be compiled for the desktop against M5GFX's SDL backend, and a pixel-identical match would be a tested invariant rather than something eyeballed.

Steps:
- Extract the render module (`src/render.h` / `src/render.cpp`) with no Arduino, M5Unified or network includes. This was planned first as a reviewable document (`docs/superpowers/plans/2026-09-13-render-module-extraction.md`), then carried out as a refactor verified against all five goldens (`34bf24b`). It wasn't purely mechanical: `Snapshot` gained a `stationLabel` field so the renderer wouldn't need `config.h`, which holds Wi-Fi credentials (recorded in the plan's amendments).
- Build it on Windows with MSYS2, MinGW-w64 GCC, SDL2 and PlatformIO's native platform.
- **Milestone: the SDL output was byte-identical to a device-captured golden,** proven with `hil.py decode-raw`. "The preview is the display." The goal was that and not merely "SDL builds".
- Getting there exposed two bugs, both host-environment leaks into something that should be a pure function of the fixture:
  - duplicated station-name parsing that had drifted from the device parser;
  - a 7-hour timezone offset. The goldens had been captured on a device with no timezone configured (UTC), so the host has to render in UTC too.
- Extended on 2026-09-24 to all five original fixtures, all matching.
- At pinning (`81f0813`), the native build's M5GFX was moved from 0.2.29 to the device's exact commit, so the match holds by design and not by luck.

**The payoff came during the run-to-empty week.** With the device off-limits (no USB allowed), the entire error-display feature (`fd2298b`) was built and checked through the SDL build:
- the five existing fixtures were confirmed unchanged by a byte-for-byte `cmp` of raw dumps;
- the new fixtures were reviewed as PNGs.

Only golden capture waits for hardware.

## 5. Deferred on purpose: Tier 2 and the Raspberry Pi rig

The design discussion worked out a full rig, then deliberately deferred it:

- **Camera.** Raspberry Pi HQ Camera (IMX477) with a 16 mm lens at f/5.6, about 27 cm working distance, roughly 5.7× linear oversampling.
  - It was chosen over newer Pi camera modules, because autofocus drifts between runs and fixed wide lenses create geometry problems.
  - Lighting mattered more than the camera: the glossy Spectra 6 front layer produces specular highlights.
  - Spend order: prove the pipeline with phone photos and desktop OpenCV code ($0), then lighting, then the Pi, and the camera only after the approach was validated.
- **Raspberry Pi 5 as the test rig.** Dedicated 2.4 GHz test access point; mock API server (including failure injection: dropped AP, HTTP 500, truncated JSON, slow responses); OTA flashing; overnight soaks; later, a self-hosted CI runner.
- **Power profiling** (Nordic PPK2, about $110, or INA226) to measure wake energy and sleep current directly.

Tier 1 made most of this optional. The fixtures-over-serial decision removed the need for a test AP and mock server for render testing.

**The irony:** the deferred power-profiling tier is exactly what the battery question later needed. Battery life turned out to be the project's main open result, and it had to be measured indirectly through soak tests and a percentage gauge. (Tier 2 camera verification remains out of scope in DONE.md.)

## 6. Working method: one human, two Claudes

- **Roles.**
  - Chris: decisions and approval of every change.
  - Claude in chat: architecture, prompt writing, and reviewing Claude Code's reports and diffs.
  - Claude Code: all repo work.
- **CLAUDE.md is the handoff document** between sessions. Each Claude Code session starts with a read-only orientation prompt: read CLAUDE.md and `git log`, report where things stand, change nothing.
- **Standing rules for the agent:** report before changing; show the diff before writing; never flash, commit or push without explicit instruction; never update goldens autonomously; don't tick your own DONE.md boxes.
- **Git discipline:** branch for uncertain work; commit at green states; merge when something is done *and verified*; tag hardware-verified states.
- **Two-phase prompts for anything with a design choice:** first report and propose, stop; then implement after approval.
- **The review habit that paid off most:** asking for evidence (file and line citations, raw API responses, byte comparisons) rather than accepting assurances. Several of the catches in section 9 came from the agent correcting itself once it had to show its work.

## 7. Finishing the project (2026-09-24 to 2026-09-25)

**Defining done.** "What's left?" had no clear answer: CLAUDE.md mixed the product definition with a debugging log, and there was no finish line. So `DONE.md` was written as a checkable definition of done with an explicit out-of-scope list, committed at repo root with a pointer from CLAUDE.md (`4656547`).
- The first audit against it found 19 unchecked items: 3 already done, 5 partly done, 11 not started.
- It also found several ambiguous criteria, each resolved explicitly. One criterion named a file that doesn't exist, `config.h.example`; that was a reviewer error in the first draft.

**The battery result reframed the plan.** Soak 2 (8.24 days, 375 cycles, 31.7 min/cycle, 100%→46%) projected about 15 days per charge, half the one-month goal. It overturned soak 1's "month-shaped" conclusion.
- Standby is about 3% of daily energy, so battery life scales with awake time. `M5.begin()` (~50 s) is most of the awake window.
- **Decision (scenario A):** accept and document the shortfall; keep `M5.begin()` as the lead future-work item. A timeboxed fix was considered and declined in favour of wrapping up.

**The firmware freeze.** Reflashing needs USB, which charges the battery and spoils a measurement. So every firmware change had to land before the final measurement. The soak was continued as a run-to-empty (expected to end around 2026-10-01), with all changes written, built and reviewed but not flashed:

| Commit | Change |
|---|---|
| `7b52c2d` | Soak 2 results recorded; the "month-shaped" conclusion withdrawn |
| `6d5d443` | NOAA observed wind, with Open-Meteo fallback. The station had a live sensor; the design had called for it, the code had never implemented it. |
| `5c5c8ba` | Skip the 15 s USB serial wait on battery wakes: about 17% of awake time spent waiting for a terminal that couldn't exist |
| `d237a73` | Wi-Fi failure diagnostics (`WIFIFAIL` / `WIFISCAN`) |
| `b9607a8` | Don't write an unsynced clock back to the RTC. Found by reading code, never observed. |
| `fd2298b` | Explicit error display (red/yellow header, Wi-Fi reason shown on the panel) instead of plausible zeros. A failed cycle used to show "0.0 ft" and a green, calm 0 kt northerly wind. |
| `81f0813` | Every build dependency pinned, with byte-level proof that pinning changed no code. It also found a hidden unpinned M5GFX download through M5Unified's own dependency. |

**Documentation:**

| Commit | Change |
|---|---|
| `2602686`, `465a710`, `d237998` | LICENSE (added; DONE.md ticked; full name in the copyright line) |
| `4aa9fcb` | CLAUDE.md restructure, 1212 → 841 lines. A completeness check extracted 752 specific values and caught about 20 real omissions before commit; discovery narratives moved to `notes/writeup-material.md`. |
| `a668ea1` | README |
| `fee0e62` | Fresh-clone test from GitHub passed; DONE.md §2 complete |

**Still waiting on hardware:** the post-run-to-empty checklist in CLAUDE.md.

## 8. Where HIL stops

The bugs that mattered most were ones a framebuffer can't see:

- **The `tm_isdst` sleep bug.** A 30-minute cycle took about 89 minutes, found through a soak test. A zero-initialised `struct tm` made `mktime()` treat local digits as standard time during daylight time.
- **The 15 s serial wait.** Invisible with USB attached, which is exactly when anyone was watching.
- **The battery shortfall itself,** and the percentage gauge's unknown linearity.
- **The unsynced RTC write-back.** Found only by reading code; hard to provoke on hardware.
- **Silent Wi-Fi failures on battery,** where serial output is simply dropped. The fix was to make the panel itself the diagnostic channel.

Soak testing became a slow form of hardware-in-the-loop, and careful code reading caught what no test reached.

## 9. What went wrong, and how it was caught

These are errors by the agent and by the reviewing Claude, in no particular order.

- **A wrong cadence figure.** A 29.2 min/cycle figure was cited as the evidence behind a checked DONE.md box. It later proved impossible with the RX8130's minute-resolution alarm. It was caught while the serial-wait analysis read the alarm code, and replaced by soak 2's 31.7 min average over 375 cycles.
- **"HTTP 15s timeout" in the audit.** The TLS handshake actually defaulted to 120 s. The agent caught this itself while implementing a per-request timeout, and corrected its own earlier audit.
- **An overclaimed gauge mechanism.** A flat top-of-charge voltage was stated as the likely cause, but Li-ion voltage falls fastest near full charge. The reviewer caught it, and CLAUDE.md now records the cause as unconfirmed.
- **"Month-shaped" battery drain** from a 3-day soak that only saw 100%→98%. Overturned by soak 2's data.
- **"The soak will produce the current measurement."** A soak gives cycles versus capacity, not mA. The slip was in the agent's orientation summary during the render-extraction planning (around 2026-09-13), not in CLAUDE.md, which had already separated the two items on 2026-09-10 (`746adfc`). The reviewer caught the summary.
- **The upload command.** The documented `pio run -t upload` would have flashed both default envs and left the *test* firmware on the board (from reading `default_envs` in `platformio.ini`; never observed, since nothing was flashed). The agent spotted it in CLAUDE.md's "Build and flash" section while drafting the README, and reported it with the fresh-clone test results. The repo-wide grep that followed found the same command in the post-run checklist itself. All fixed in `a668ea1`.
- **A wrong file name in DONE.md.** The first draft named `config.h.example` where the file is `src/config.example.h`. This was a reviewer error, caught by the first audit.
- **"Show stale data on failure."** Also in the first DONE.md draft. It's impractical without flash persistence, since the only state the firmware keeps across a power cycle is 32 bytes of PMIC RAM. The audit flagged it, and it moved to out of scope.
- **A hidden M5GFX dependency.** It would have made the library pins look right while a fresh clone pulled a newer, untested version. Found by the pinning verification.
- **About 20 facts dropped** from the CLAUDE.md restructure draft. Caught by the mechanical completeness check before commit.

**The pattern:** almost every catch came from *requiring evidence*: citations, raw data, byte comparisons, a fresh clone, a mechanical completeness check. Accepting a summary would have missed them, and several were caught by the agent itself once it had to show its work.
