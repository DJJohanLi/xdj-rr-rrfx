# Testing status

## Test bench (every build)

`tests/harness.c` runs the real library under qemu against emulated player functions at their
real addresses: 45 checks covering selection with FX on/off, FX ON/OFF, the physical
switch, the Reverb quantize rule, all Pitch steps (ends, snapping, exact undo), the gesture
(timing, undo, browse, commit, 10 s timeout, switch cancel) and the debug switch.

## Real hardware (XDJ-RR, firmware 1.08)

v1.0.0 itself was checked on hardware before release: load, select mode, commits (Spiral,
Delay, Roll, Pitch with quantize on), Pitch steps, physical switch back to stock, and the
debug switch (`RRFX_DEBUG.txt` → `DEBUG=ON`). Behaviour is otherwise identical to
v1.0.24-beta2, which had the full test sessions below.

| Area | Result |
|---|---|
| Loading via ptrace injection, same PID, re-insert safe | OK, many sessions (v1.0.16 → beta2) |
| All 8 types: name, arrows, FX ON/OFF, LEVEL/DEPTH | OK |
| Hold gesture 1.2 s, browse, commit, timeout, physical switch → stock | OK |
| Pitch −50…+100 % steps, memory, ends | OK |
| Quantize on/off (incl. toggling while a selected type runs) | OK, same as stock |
| CH SELECT CH1/CH2/MASTER with a selected type | OK |
| Roll capture/length behaviour | Stock engine behaviour |
| rekordbox 7.2.18 PC mode: mod stays out of the way, works again after disconnect | OK |
| Longest session | about 54 min |

## Wanted from the community

- **Long sessions (2 h+)**, ideally a real gig. Please report how long it ran and attach the
  log folder (`RRFX_UI_V100/attempt-N/`), with `RRFX_DEBUG.txt` on the stick if possible.
- Other rekordbox versions.
