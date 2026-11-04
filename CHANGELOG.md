# Changelog

## v1.0.0 (first public release)

- All 8 Beat FX selectable on the XDJ-RR (firmware 1.08) with the hold-both-arrows gesture (1.2 s).
- Correct name, arrow steps, FX ON/OFF, LEVEL/DEPTH and quantize for every type.
- Pitch uses its real −50…+100 % range (−50, −25, −10, +10, +25, +50, +75, +100).
- Debug key log, only when `RRFX_DEBUG.txt` is on the stick.
- Refuses to load if an older test build was loaded since power-on ("reboot first").
- Portable build (`RR_KEY` / `RR_RUNTIME`), own packing script, 45-check qemu test bench.

## Development history (private test builds, summarised)

- **v1.0.24-beta2:** Pitch ladder, 1.2 s hold, key-code logging (Color FX map).
- **v1.0.23-beta1:** first full beta; select-mode indicators dropped by design.
- **v1.0.19–v1.0.22:** UI-correct selection (name, arrows, FX ON follow the type); on-panel gesture.
- **v1.0.16–v1.0.18:** first working ptrace injection into the live player (libdl symlink fix);
  engine-level type selection proven audible for all 8 types.
- **v1.0.1–v1.0.12:** relaunch approach abandoned (initialisation race inside the player on
  relaunch); decision to stay RAM-only and never restart the player.
