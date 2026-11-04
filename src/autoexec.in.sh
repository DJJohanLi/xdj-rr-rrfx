#!/bin/sh
# RRFX v1.0.0 for Pioneer XDJ-RR firmware 1.08 -- all 8 Beat FX selectable.
# Copyright (C) 2026 Johan Li and contributors. GPL-3.0-or-later.
#
# The XDJ-RR runs this script from an inserted USB stick (autoexec.bin). It
# loads rrfx_ui.so into the RUNNING player process (rbp) with rbp_inject, once
# per boot. After that, effects are selected on the front panel (hold both BEAT
# arrows). The stick can be removed once the log says LOADED.
#
# Safety: RAM-only. Nothing is written to flash. rbp is never killed or
# restarted. Every input is identity-checked (rbp, libdl, injector, library
# SHA1) before anything is touched; the library itself patches 7 function
# pointers in rbp's in-RAM vtables only if all 7 still hold their stock values.
# Anything unexpected => nothing is changed. A power-off returns to stock.
#
# Every insert (including the first) also:
#   - passes RRFX_SELECT.txt (1-8 or a type name), if present, as a selection;
#   - turns the debug key log on (RRFX_DEBUG.txt present) or off (absent);
#   - copies the library's log back to the stick (RRFX_UI_V100/attempt-N/).

usb="$1"
VER=1.0.0
RAM=/dev/shm/rrfx
RBP=/root/pdj/rbp
RBP_SHA1=5501b31257262756ce8c64bf3b08e0ea6d61ccce
LIBDL=/lib/libdl.so.2
LIBDL_SHA1=700c14efe86238f6cd1dcb1fa969e9f78b32e824
INJECTOR_SRC=/mnt/iso/rbp_inject
INJECTOR=$RAM/rbp_inject
INJECTOR_SHA1=@INJECTOR_SHA1@
UILIB_SRC=/mnt/iso/rrfx_ui100.so
UILIB=$RAM/rrfx_ui100.so
UILIB_SHA1=@UILIB_SHA1@
UILOG=/dev/shm/rrfx_ui100.log
REQ=/dev/shm/rrfx_req
DEBUGFLAG=/dev/shm/rrfx_debug
MARK=/dev/shm/rrfx-v100-applied

case "$usb" in
    /media/usb[0-9]/*) ;;
    *) exit 0 ;;
esac
[ -d "$usb" ] || exit 0

out="$usb/RRFX_UI_V100"
mkdir -p "$out" 2>/dev/null || exit 0
n=1
while [ -e "$out/attempt-$n" ] && [ "$n" -lt 100 ]; do n=$((n + 1)); done
dir="$out/attempt-$n"
mkdir "$dir" 2>/dev/null || exit 0
LOG="$dir/log.txt"

log() { echo "$*" >> "$LOG"; sync; }
fetch_uilog() { [ -e "$UILOG" ] && cp "$UILOG" "$dir/rrfx_ui_log.txt" 2>/dev/null && sync; }
finish() { fetch_uilog; log "RESULT=$1"; exit 0; }
sha1_of() { s=$(sha1sum "$1" 2>/dev/null) || return 1; echo "${s%% *}"; }

log "RRFX_UI=$VER"
log "USB=$usb"
log "UPTIME=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)"
fetch_uilog

[ -f "$usb/RRFX_ARM.txt" ] || finish NOT_ARMED
log "ARM=PRESENT_NOT_CONSUMED"

# Debug key log: on while RRFX_DEBUG.txt is on the stick, off otherwise.
if [ -f "$usb/RRFX_DEBUG.txt" ]; then
    touch "$DEBUGFLAG" 2>/dev/null && log "DEBUG=ON"
else
    rm -f "$DEBUGFLAG" 2>/dev/null
fi

# Optional selection.
type=""
if [ -f "$usb/RRFX_SELECT.txt" ]; then
    sel_raw=$(sed -n '1p' "$usb/RRFX_SELECT.txt" 2>/dev/null | tr -d ' \t\r\n')
    sel_upper=$(echo "$sel_raw" | tr 'a-z' 'A-Z')
    log "SELECT_FILE_RAW=$sel_raw"
    case "$sel_upper" in
        1|REVERB)  type=1; name=Reverb ;;
        2|TRANS)   type=2; name=Trans ;;
        3|ROLL)    type=3; name=Roll ;;
        4|FLANGER) type=4; name=Flanger ;;
        5|ECHO)    type=5; name=Echo ;;
        6|DELAY)   type=6; name=Delay ;;
        7|SPIRAL)  type=7; name=Spiral ;;
        8|PITCH)   type=8; name=Pitch ;;
        *) finish SELECT_FILE_INVALID_VALUE_USE_1_TO_8_OR_A_NAME ;;
    esac
    log "REQUESTED_TYPE=$type($name)"
fi

[ "$(sha1_of "$RBP")" = "$RBP_SHA1" ] || finish RBP_IDENTITY_REFUSED
log "RBP_IDENTITY=OK"
old=$(pidof rbp 2>/dev/null)
count=0
for p in $old; do count=$((count + 1)); done
log "RBP_PID=$old"
[ "$count" -eq 1 ] || finish "RBP_PID_COUNT_REFUSED_$count"
[ "$(readlink "/proc/$old/exe" 2>/dev/null)" = "$RBP" ] || finish RBP_EXE_REFUSED

# Never mix with an older test build of this mod in the same boot.
for m in 116 117 119 120 121 122 123 124; do
    [ -e "/dev/shm/rrfx-v$m-applied" ] && finish "OLDER_RRFX_BUILD_ACTIVE_THIS_BOOT_REBOOT_FIRST"
done
for l in rrfx_ui.so rrfx_ui120.so rrfx_ui121.so rrfx_ui122.so rrfx_ui123.so rrfx_ui124.so; do
    grep -qF "$l" "/proc/$old/maps" 2>/dev/null && finish "OLDER_RRFX_BUILD_LOADED_THIS_BOOT_REBOOT_FIRST"
done

if grep -qF 'rrfx_ui100.so' "/proc/$old/maps" 2>/dev/null; then
    log "UILIB=ALREADY_LOADED"
else
    [ -e "$MARK" ] && finish "MARKER_SET_BUT_LIBRARY_NOT_MAPPED_REFUSED"
    [ "$(sha1_of "$LIBDL")" = "$LIBDL_SHA1" ] || finish LIBDL_IDENTITY_REFUSED
    log "LIBDL_IDENTITY=OK"
    log "RBP_STATE_BEFORE=$(awk '{print $3}' "/proc/$old/stat" 2>/dev/null)"
    mkdir -p "$RAM" 2>/dev/null && chmod 700 "$RAM" || finish RAM_DIR_FAILED

    rm -f "$INJECTOR.next"
    cp "$INJECTOR_SRC" "$INJECTOR.next" 2>/dev/null || finish INJECTOR_COPY_FAILED
    [ "$(sha1_of "$INJECTOR.next")" = "$INJECTOR_SHA1" ] || finish INJECTOR_IDENTITY_REFUSED
    chmod 755 "$INJECTOR.next" && mv -f "$INJECTOR.next" "$INJECTOR" || finish INJECTOR_INSTALL_FAILED

    rm -f "$UILIB.next"
    cp "$UILIB_SRC" "$UILIB.next" 2>/dev/null || finish UILIB_COPY_FAILED
    [ "$(sha1_of "$UILIB.next")" = "$UILIB_SHA1" ] || finish UILIB_IDENTITY_REFUSED
    chmod 644 "$UILIB.next" && mv -f "$UILIB.next" "$UILIB" || finish UILIB_INSTALL_FAILED
    log "STAGED=OK"

    touch "$MARK"
    inj_out=$("$INJECTOR" "$old" "$UILIB" 2>&1)
    inj_rc=$?
    echo "$inj_out" >> "$LOG"
    log "INJECTOR_RC=$inj_rc"
    sync
    still=$(pidof rbp 2>/dev/null)
    log "RBP_PID_AFTER=$still"
    [ "$still" = "$old" ] || finish RBP_PID_CHANGED_CHECK_LOG
    log "RBP_STATE_AFTER=$(awk '{print $3}' "/proc/$old/stat" 2>/dev/null)"
    [ "$inj_rc" -eq 0 ] || finish INJECT_FAILED_CHECK_LOG
    sleep 1
    grep -q 'INSTALL_OK' "$UILOG" 2>/dev/null && log "HOOKS=INSTALL_OK" || log "HOOKS=NOT_CONFIRMED_SEE_rrfx_ui_log"
fi

[ -n "$type" ] || finish LOADED_USE_THE_ARROWS_HOLD_BOTH_1_2S

before=$(grep -c "SELECT_APPLIED type=$type" "$UILOG" 2>/dev/null)
[ -n "$before" ] || before=0
echo "$type" > "$REQ.tmp" && mv -f "$REQ.tmp" "$REQ" || finish REQUEST_WRITE_FAILED
i=0
while [ -e "$REQ" ] && [ "$i" -lt 20 ]; do sleep 1; i=$((i + 1)); done
[ -e "$REQ" ] && { rm -f "$REQ"; finish REQUEST_NOT_PICKED_UP; }
sleep 1
fetch_uilog
after=$(grep -c "SELECT_APPLIED type=$type" "$UILOG" 2>/dev/null)
[ -n "$after" ] || after=0
[ "$after" -gt "$before" ] && finish "SELECT_OK_$name"
finish SELECT_SENT_CHECK_rrfx_ui_log
