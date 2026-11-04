#!/bin/sh
# Build RRFX v1.0.0 for the Pioneer XDJ-RR (firmware 1.08).
# Copyright (C) 2026 Johan Li and contributors. GPL-3.0-or-later.
#
# Needs (see docs/BUILDING.md):
#   RR_RUNTIME  directory with the unit's own libc.so.6 and libpthread.so.0 (glibc 2.13)
#   RR_KEY      your own key file for the unit's autoexec.bin encryption
#   RR_UPD      optional: official XDJRR.UPD 1.08, used only to verify RR_KEY
# None of these are part of the repository, and none may ever be committed.
#
# Output: build/ (rrfx_ui100.so, rbp_inject, autoexec.sh, autoexec.bin, test log)
set -e
cd "$(dirname "$0")"
: "${RR_RUNTIME:?set RR_RUNTIME to the directory holding the unit's libc.so.6 and libpthread.so.0}"
: "${RR_KEY:?set RR_KEY to your own XDJ-RR key file (never commit it)}"
for f in libc.so.6 libpthread.so.0; do
    [ -f "$RR_RUNTIME/$f" ] || { echo "missing $RR_RUNTIME/$f"; exit 1; }
done

CC=arm-linux-gnueabi-gcc
B=build
rm -rf "$B"; mkdir -p "$B/linklibs" "$B/test"
for f in libc.so.6 libpthread.so.0; do ln -s "$(cd "$RR_RUNTIME" && pwd)/$f" "$B/linklibs/$f"; done
ln -s libc.so.6 "$B/linklibs/libc.so"; ln -s libpthread.so.0 "$B/linklibs/libpthread.so"

# 1. The library, linked against the DEVICE's glibc 2.13 (not the host's).
CB=$($CC -print-file-name=crtbeginS.o)
CE=$($CC -print-file-name=crtendS.o)
$CC -marm -march=armv7-a -mfloat-abi=softfp -O2 -fPIC \
    -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -Wall -Wextra -Wno-unused-parameter -c src/rrfx_ui.c -o "$B/rrfx_ui.o"
$CC -shared -nostdlib -o "$B/rrfx_ui100.so" "$CB" "$B/rrfx_ui.o" "$CE" \
    -L"$B/linklibs" -Wl,-rpath-link,"$B/linklibs" -lc -lpthread -lgcc \
    -Wl,--hash-style=sysv -Wl,-soname,rrfx_ui100.so

# Every imported symbol must exist in the device's libc/libpthread.
for s in $(arm-linux-gnueabi-objdump -T "$B/rrfx_ui100.so" | awk '/UND/ && $2!="w" {print $NF}'); do
    ok=0
    for L in "$B/linklibs/libc.so.6" "$B/linklibs/libpthread.so.0"; do
        arm-linux-gnueabi-objdump -T "$L" | awk '{print $NF}' | grep -qx "$s" && ok=1
    done
    [ $ok = 1 ] || { echo "IMPORT NOT ON DEVICE: $s"; exit 1; }
done
echo "imports: all present in device glibc"

# 2. Test bench under qemu (fixed rbp addresses emulated; no device needed).
$CC -marm -march=armv7-a -O1 -w -o "$B/test/harness" tests/harness.c -ldl -Wl,-Ttext-segment=0x40000000
cp "$B/rrfx_ui100.so" "$B/test/"
(cd "$B/test" && qemu-arm-static -L /usr/arm-linux-gnueabi ./harness > harness_result.txt 2>&1) \
    || { cat "$B/test/harness_result.txt"; exit 1; }
grep -q 'RESULT ALL_PASS' "$B/test/harness_result.txt"
echo "harness: ALL_PASS ($(grep -c '^PASS' "$B/test/harness_result.txt") checks)"

# 3. The injector (fully static).
$CC -static -O2 -Wall -o "$B/rbp_inject" src/rbp_inject.c
INJ=$(sha1sum "$B/rbp_inject" | cut -d' ' -f1)
[ "$INJ" = 62b52de1fd0e9aabc7dbe5751ef8d5334b07841d ] \
    && echo "rbp_inject: bit-identical to the hardware-proven binary" \
    || echo "NOTE: rbp_inject differs from the proven binary (different compiler?) -- test with care"

# 4. Install script with the real SHA1s, then the encrypted autoexec.bin.
UIL=$(sha1sum "$B/rrfx_ui100.so" | cut -d' ' -f1)
sed -e "s/@INJECTOR_SHA1@/$INJ/" -e "s/@UILIB_SHA1@/$UIL/" src/autoexec.in.sh > "$B/autoexec.sh"
sh -n "$B/autoexec.sh"
python3 tools/pack_autoexec.py --key "$RR_KEY" ${RR_UPD:+--check-updater "$RR_UPD"} \
    --script "$B/autoexec.sh" --injector "$B/rbp_inject" --library "$B/rrfx_ui100.so" \
    --out "$B/autoexec.bin"
cp usb/RRFX_ARM.txt "$B/"
(cd "$B" && sha1sum rrfx_ui100.so rbp_inject autoexec.sh autoexec.bin)
