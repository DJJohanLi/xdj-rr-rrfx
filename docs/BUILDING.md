# Building RRFX from source

Most users don't need this: the ready-made `autoexec.bin` + `RRFX_ARM.txt` on the
Releases page are all you need. This guide is for people who want to change the code.

## What is NOT in this repository (and must never be added)

| Input | Why you need it | Where it comes from |
|---|---|---|
| The XDJ-RR autoexec key file (`RR_KEY`) | The unit only runs an `autoexec.bin` encrypted with it | **Not distributed by this project.** Supply your own. |
| The unit's `libc.so.6` + `libpthread.so.0`, glibc 2.13 (`RR_RUNTIME`) | The library must link against the device's glibc, not your host's | From your own unit's firmware (root filesystem) |
| Official `XDJRR.UPD` 1.08 (`RR_UPD`, optional) | Only used to verify that your key is correct | Pioneer DJ / AlphaTheta support site |

Keep these outside the repository folder. `.gitignore` blocks the usual file names, and
`tools/check_no_secrets.sh` checks every file before you commit (run it).

## Toolchain (Ubuntu 24.04 tested)

```sh
sudo apt install gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi qemu-user-static python3-pip
pip install cryptography pycdlib
```

## Build

```sh
export RR_RUNTIME=/path/outside/the/repo/runtime      # contains libc.so.6, libpthread.so.0
export RR_KEY=/path/outside/the/repo/your-key-file
export RR_UPD=/path/outside/the/repo/XDJRR.UPD        # optional key check
sh build.sh
```

`build.sh` does, in order:

1. Builds `rrfx_ui100.so` against the device's glibc and checks that every imported symbol
   exists in the device's libc/libpthread.
2. Runs the **test bench** (`tests/harness.c`) under qemu. It recreates, at the player's
   fixed addresses, every function and vtable slot the library touches, loads the real
   library and drives every path: selection, FX ON/OFF, physical switch, Pitch steps,
   the hold gesture with its timings, the timeout, and the debug switch. All checks must pass.
3. Builds `rbp_inject` (static). With Ubuntu 24.04's gcc 13.3 it is **bit-identical** to the
   binary that has been used on hardware since v1.0.16 (SHA1 `62b52de1…`); the script tells you.
4. Writes `autoexec.sh` with the real SHA1s of the injector and library, then packs and encrypts
   `autoexec.bin` with `tools/pack_autoexec.py` and verifies it by decrypting it again.

Output goes to `build/`. Copy `build/autoexec.bin` and `build/RRFX_ARM.txt` to a stick.

## Testing changes on hardware

- The test bench can't run real ptrace injection; only the device can. Every change to
  the library needs a hardware run.
- Change the version in **all** of: `src/rrfx_ui.c` (`VERSION`, log path), `src/autoexec.in.sh`
  (`VER`, library/marker/log names, output folder), `build.sh`, `tools/pack_autoexec.py` and
  `tests/harness.c`. Then add the previous version to the "older build" refusal lists in
  `autoexec.in.sh`, so two builds can never be loaded in one boot.
- Keep the safety rules from [HOW_IT_WORKS.md](HOW_IT_WORKS.md): RAM-only, never restart rbp,
  check stock values before patching, all-or-nothing.
