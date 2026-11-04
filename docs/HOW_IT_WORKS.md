# How RRFX works

Target: Pioneer XDJ-RR, firmware 1.08, player program `/root/pdj/rbp`
(SHA1 `5501b31257262756ce8c64bf3b08e0ea6d61ccce`, ARMv7, glibc 2.13, not PIE). All addresses
below are for exactly that binary. The install script refuses any other.

## 1. Loading: never restart the player

The XDJ-RR runs `autoexec.sh` from an encrypted `autoexec.bin` on an inserted USB stick.
The script:

1. Checks the stick has `RRFX_ARM.txt`, then checks the SHA1 of `rbp`, `/lib/libdl.so.2`, the
   injector and the library. Any mismatch stops it before anything is touched.
2. Copies the injector and library to RAM (`/dev/shm/rrfx`).
3. Runs `rbp_inject <pid> rrfx_ui100.so`. The injector ptrace-attaches to the running rbp,
   saves its registers, makes it call `dlopen()` once, collects the handle via a deliberate
   return-to-0 trap, restores the original registers and detaches. rbp keeps the same PID and
   carries on.

Why not restart rbp with a preload: a relaunched rbp loses an initialisation race inside the
binary (a network timer queries the UI manager before it exists) and crashes. A cold boot
always wins that race. Injecting into the already-running process avoids it completely.

## 2. Hooks: 7 vtable slots, all-or-nothing

The library's constructor checks that all 7 slots still hold their stock values. Only then
does it replace them (atomic store, page made writable just for the store). Otherwise it
logs `INSTALL_REFUSED` and changes nothing.

| Slot | Stock | Purpose |
|---|---|---|
| `0x47782c` MasterInnards #13 `onEv_BeatEffectType` | `0x2bfa34` | Physical switch moved → stock handler, then back to stock types |
| `0x477830` MasterInnards #14 `onEv_BeatEffectEnable` | `0x2bedfc` | FX ON engages the selected type (stock table only maps 3) |
| `0x477838` MasterInnards #16 `onEv_BeatFxBeat` | `0x2c0c10` | Pitch uses its own % steps; all others stock |
| `0x46d3dc` Mixer `asEventCode` | `0x2475d0` | BEAT ◀/▶ hold gesture |
| `0x46d3cc` Mixer `updateMasterStatus` | `0x247a50` | Show the browsed name |
| `0x46d3f4` Mixer `getMasterStat` | `0x247a1c` | Show the browsed name |
| `0x46d438` thunk to `getMasterStat` | `0x247a14` | Show the browsed name |

## 3. Selecting a type the way the stock switch does

The engine call `DjEngineIF::setBeatEffectType` (`0x4a740`) alone isn't enough. The UI keeps
its own "selected effect" code at `ui::MasterInnards + 60`, and that code drives the
on-screen name, the FX ON button, the arrow parameter scheme and the quantize rule. The commit
therefore reproduces the stock type handler, generalised to all 8 types:

- FX on: `setBeatEffectType(t)`; FX off: `switchBeatEffectMaxMinValue(t)` (`0x4b2a0`)
- `+60` = display code of t (name images: 0 Delay, 1 Echo, 2 Spiral, 3 Reverb, 4 Trans,
  5 Flanger, 6 Pitch, 7 Roll). Engine enum: 1 Reverb, 2 Trans, 3 Roll, 4 Flanger, 5 Echo,
  6 Delay, 7 Spiral, 8 Pitch.
- Quantize: off if quantize mode (`+236`) is off or t is Reverb, else
  `isBeatEffectQuantizeValid()` → `setBeatEffectQuantizeOnOff()`. This is the same rule as the
  stock quantize and CH SELECT handlers, which also read `+60`.
- Re-send the cached LEVEL/DEPTH value (`+228`) so the engine matches the knob.

Selection runs on rbp's key thread (gesture) or on the JUCE message thread
(`RRFX_SELECT.txt` backup), the same threads the stock handlers use.

## 4. Pitch

The engine's Pitch range is −50…+100 % (default −50, rodata `0x3c3f74`/`78`/`7c`). The stock
arrow handler only has the Reverb 1…100 % ladder. RRFX steps Pitch through
−50, −25, −10, +10, +25, +50, +75, +100 with `get/setBeatEffectPercent`
(`0x4b0a8` / `0x4aff8`). The engine keeps each type's own value between switches.

## 5. The gesture and the display

A held key sends one press and one release, so hold time is measured between them. Hold both
≥ 1.2 s → select mode. The first key's normal parameter step is undone when the chord starts.
While browsing, only the name byte of the display status buffer is overridden. The real
`+60` is never touched until the commit. A 125 ms timer handles the timeout (10 s) and
restores the real name.

## 6. rekordbox (PC) mode

Connected to rekordbox, the BEAT arrows and FX ON never reach the RR's own mixer UI (they go
to rekordbox), so the gesture can't fire and nothing is swallowed. LEVEL/DEPTH and the type
switch still pass to the stock handlers unchanged. After disconnecting, the mod works again.

## 7. Mixer key codes (`asEventCode`, key+8 id; key+11: 00 press, 02 release, 05 value)

| id | Control |
|---|---|
| `0x4491` / `0x4492` | BEAT ◀ / ▶ |
| `0x448b` | Beat FX ON |
| `0x448f` | Beat FX type switch |
| `0x448e` | LEVEL/DEPTH (probably) |
| `0x50a5`, `0x50a4`, `0x50a6`, `0x50a3` | Sound Color FX buttons, left to right |
| `0x501d` (key+10 = 1 / 2) | COLOR knob CH1 / CH2 |

SHIFT is a deck key and doesn't appear in the mixer key stream.

## Safety rules (for any change)

RAM-only (no flash writes); never kill or restart rbp; identity-check every input; check
stock values before patching, all-or-nothing; a power-off must always return to stock.
