#!/bin/sh
# Pre-commit safety check for RRFX: fails if anything that must never be published
# is about to be committed. Run from the repository root before every commit/push.
# Optional: RR_KEY=/path/to/your/key also checks that no file contains the key itself.
# Part of RRFX, GPL-3.0-or-later.
cd "$(dirname "$0")/.." || exit 2
fail=0
bad() { echo "BLOCKED: $1"; fail=1; }

if git rev-parse --git-dir >/dev/null 2>&1; then
    files=$(git ls-files --cached --others --exclude-standard)
else
    files=$(find . -type f ! -path './.git/*' ! -path './build/*' | sed 's|^\./||')
fi

for f in $files; do
    [ -f "$f" ] || continue
    case "$f" in
        *.key|*.UPD|*.upd|*.cramfs|*.iso|*.mot|*.bin|*.so|*.so.*|*.tar.gz|*.zip) bad "$f (forbidden file type)";;
        rbp|*/rbp|*private-inputs*|*rootfs*|*imagedata*) bad "$f (Pioneer/AlphaTheta material)";;
    esac
    size=$(wc -c < "$f")
    [ "$size" -gt 262144 ] && [ "$f" != LICENSE ] && bad "$f is ${size} bytes (only small source files belong here)"
    magic=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
    case "$magic" in
        7f454c46) bad "$f is an ELF binary";;
        1f8b*)    bad "$f is gzip data";;
        504b0304) bad "$f is a zip archive";;
        28cd3d45|453dcd28) bad "$f is a cramfs image";;
    esac
done

if [ -n "$RR_KEY" ] && [ -f "$RR_KEY" ]; then
    needle=$(head -n 1 "$RR_KEY" | head -c 31)
    for f in $files; do
        [ -f "$f" ] && grep -qF -- "$needle" "$f" 2>/dev/null && bad "$f CONTAINS THE KEY"
    done
fi

if [ $fail = 0 ]; then echo "check_no_secrets: OK ($(echo "$files" | wc -w) files checked)"; fi
exit $fail
