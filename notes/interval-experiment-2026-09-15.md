# Sleep-interval experiment — predictions recorded before running (2026-09-15)

Purpose: distinguish between candidate root causes for the soak-test sleep-
interval discrepancy by rerunning at `UPDATE_MINUTES=2` and comparing the
observed period against three pre-registered outcomes. Recorded before the
experiment runs; not to be revised after seeing the result.

## Baseline model

Expected period (no bug) = awake time + configured interval
= ~1.3 min (~78s measured `M5.begin()`+fetch+render, see CLAUDE.md) + `UPDATE_MINUTES`

At `UPDATE_MINUTES=30`: expected ≈ 31.3 min. Soak observed: 89.3 min/cycle.
Excess = 89.3 - 31.3 = **58.0 min**. (The earlier "almost exactly 3x" read on
89.3 vs. 30 was against the wrong baseline — the awake-time offset wasn't
accounted for. Ratio against the *correct* baseline is 89.3/31.3 ≈ 2.85x,
not a clean 3x.)

At `UPDATE_MINUTES=2`: expected ≈ 3.3 min.

## Three predictions, pre-registered

| Outcome | Predicted period | Interpretation |
|---|---|---|
| A — doesn't reproduce | **~3.3 min** | Matches the no-bug baseline. The discrepancy is specific to something about the 30-minute configuration (or the conditions present during the soak), not a general property of the alarm-scheduling code. |
| B — multiplicative | **~10 min** | 2.85x-2.98x × 3.3 min ≈ 9.4-9.8 min. Whatever caused the ~2.85-2.98x factor at 30 scales the *whole* period, not just the configured-minutes term — consistent with something like an RTC clock-rate/prescaler issue that makes elapsed time itself run slow from the device's perspective. |
| C — fixed additive | **~61 min** | 3.3 + 58.0 min. The same ~58-minute excess shows up regardless of configured interval — consistent with a fixed delay somewhere in the path (e.g. a hardcoded wait, a retry/backoff, a one-time miscalculation), not a scaling error. |

## Rule

Whichever bucket the measured period actually falls into is the answer.
Do not construct a new explanation that fits the observed number after the
fact — if the result doesn't cleanly match A, B, or C, that itself is a
finding (record it as "none of the three," not as a retrofit).

## Experiment Data

1st run
Start: cycle = 355, time = 20260915 11:31
End: cycle = 360, time 20260915 15:20

2nd Run
15:29:03.182 > === cycle 362 ===
21:45:02.277 > === cycle 368 ===