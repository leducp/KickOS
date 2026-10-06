#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ST-Link capture's cut to the reset's boot (tools/bench/reset-boot.sh) over planted logs, and
# its start (FLASH_STLINK_RESET=1 tools/flash-stlink.sh) over a stub st-flash.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
. tools/bench/reset-boot.sh

# <file> <title> <commit row>: a banner as the console prints it.
banner() {
    printf '\r\n  ==============================================\r\n' >> "$1"
    printf '%s\r\n' "$2" >> "$1"
    printf '  ==============================================\r\n   board   f411disco\r\n' >> "$1"
    printf '%s\r\n\r\n' "$3" >> "$1"
}
TITLE='   KickOS 0.5.1  -  microkernel RTOS'
COMMIT='   commit  0355b756'

# <name>: a log holding an earlier boot's tail, its size in $SIZE.
tail_of_earlier() {
    printf '[blink] tick 41\r\n[blink] tick 42\r\n[blink] ti' > "$TMP/$1"
    SIZE=$(wc -c < "$TMP/$1")
}

# <name> <bytes>: reset_boot_cut's status over the log, its message in $TMP/why.
cut_log() {
    reset_boot_cut "$TMP/$1" "$2" > "$TMP/why"
}

tail_of_earlier clean
banner "$TMP/clean" "$TITLE" "$COMMIT"
printf '[panicgate] case 1\r\n' >> "$TMP/clean"
cut_log clean "$SIZE" || bad "the clean capture is refused: $(cat "$TMP/why")"
if grep -q 'blink' "$TMP/clean"; then
    bad "the earlier boot's tail survives the cut"
fi
head -n 1 "$TMP/clean" | grep -qE '^  =+'"$(printf '\r')"'$' \
    || bad "the cut does not start at the reset's banner rule: $(head -n 1 "$TMP/clean")"

printf '\r\n  ==============================================\r\n' > "$TMP/whole"
printf '%s\r\n' "$TITLE" >> "$TMP/whole"
cp "$TMP/whole" "$TMP/whole.before"
cut_log whole 0 || bad "a capture with nothing ahead of its banner is refused"
tail -n +2 "$TMP/whole.before" | cmp -s - "$TMP/whole" \
    || bad "a capture with nothing ahead of its banner lost more than its leading blank line"

tail_of_earlier none
printf '[blink] tick 43\r\n[blink] tick 44\r\n' >> "$TMP/none"
if cut_log none "$SIZE"; then
    bad "a capture in which no boot follows the reset is not refused"
fi
grep -q 'no whole banner title or commit line follows the reset' "$TMP/why" \
    || bad "the no-boot refusal says: $(cat "$TMP/why")"

# The earlier boot's own banner ahead of the reset is no boot the reset started.
: > "$TMP/pretitle"
banner "$TMP/pretitle" "$TITLE" "$COMMIT"
printf '[blink] tick 1\r\n' >> "$TMP/pretitle"
SIZE=$(wc -c < "$TMP/pretitle")
cp "$TMP/pretitle" "$TMP/pretitle-ticks"
if cut_log pretitle "$SIZE"; then
    bad "a capture whose only banner precedes the reset is not refused"
fi
printf '[blink] tick 2\r\n' >> "$TMP/pretitle-ticks"
if cut_log pretitle-ticks "$SIZE"; then
    bad "a capture whose only banner precedes the reset, with lines after it, is not refused"
fi

tail_of_earlier damaged
banner "$TMP/damaged" '   KckOS 0.5.1  -  micrkernel RTOS' '   cmmit  0355b756'
printf '[panicgate] case 1\r\n' >> "$TMP/damaged"
if cut_log damaged "$SIZE"; then
    bad "a lone boot whose title and commit both arrived damaged is not refused"
fi
grep -q 'no whole banner title or commit line follows the reset' "$TMP/why" \
    || bad "the damaged-banner refusal says: $(cat "$TMP/why")"

# The line the reset splits joins the earlier output to the reset's first bytes.
tail_of_earlier joined
printf 'x\r\n  ==============================================\r\n%s\r\n' "$TITLE" >> "$TMP/joined"
cut_log joined "$SIZE" || bad "a capture whose reset split a line is refused: $(cat "$TMP/why")"
head -n 1 "$TMP/joined" | grep -qE '^  =+'"$(printf '\r')"'$' \
    || bad "the line the reset split is read as damage: $(head -n 1 "$TMP/joined")"

tail_of_earlier notitle
banner "$TMP/notitle" '   KckOS 0.5.1  -  micrkernel RTOS' "$COMMIT"
printf '1..3\r\nok 1\r\n' >> "$TMP/notitle"
banner "$TMP/notitle" "$TITLE" "$COMMIT"
printf '1..3\r\n' >> "$TMP/notitle"
cut_log notitle "$SIZE" || bad "a damaged title with a whole commit is refused: $(cat "$TMP/why")"
[ "$(grep -c '^1\.\.3' "$TMP/notitle")" -eq 2 ] || bad "a damaged first title lost the reset's boot"
if grep -q 'blink' "$TMP/notitle"; then
    bad "the earlier boot's tail survives a cut at the commit line"
fi

tail_of_earlier onlycommit
banner "$TMP/onlycommit" '   KckOS 0.5.1  -  micrkernel RTOS' "$COMMIT"
printf '[panicgate] case 1\r\n' >> "$TMP/onlycommit"
cut_log onlycommit "$SIZE" || bad "a lone boot with a damaged title and a whole commit is refused"

tail_of_earlier nobanner
banner "$TMP/nobanner" '   KckOS 0.5.1  -  micrkernel RTOS' '   cmmit  0355b756'
printf '1..3\r\nok 1\r\n' >> "$TMP/nobanner"
banner "$TMP/nobanner" "$TITLE" "$COMMIT"
printf '1..3\r\n' >> "$TMP/nobanner"
cut_log nobanner "$SIZE" || bad "a damaged title and commit are refused: $(cat "$TMP/why")"
[ "$(grep -c '^1\.\.3' "$TMP/nobanner")" -eq 2 ] \
    || bad "a damaged first title and commit lost the reset's boot"
if grep -q 'tick 4' "$TMP/nobanner"; then
    bad "the earlier boot's whole lines survive a cut at the reset"
fi

mkdir -p "$TMP/bin" "$TMP/tmpdir"
cat > "$TMP/bin/st-flash" <<'EOF'
#!/bin/sh
echo "$*" > "$STUB_ARGS"
exit 1
EOF
chmod +x "$TMP/bin/st-flash"
: > "$TMP/image"
: > "$TMP/image.bin"
printf 'RIG_STLINK_F411DISCO=PLANTEDSN\n' > "$TMP/rig.conf"
if STUB_ARGS="$TMP/args" PATH="$TMP/bin:$PATH" TMPDIR="$TMP/tmpdir" KICKOS_RIG="$TMP/rig.conf" \
    FLASH_STLINK_RESET=1 FLASH_IMAGE="$TMP/image" bash tools/flash-stlink.sh f411disco planted \
    > "$TMP/flash.out" 2>&1; then
    bad "flash-stlink.sh passes a start whose read failed"
fi
grep -qE '^--serial PLANTEDSN --connect-under-reset read [^ ]+ 0x08000000 4$' "$TMP/args" \
    || bad "the start is not a read under reset on the probe: $(cat "$TMP/args" 2>/dev/null)"
[ -z "$(ls -A "$TMP/tmpdir")" ] || bad "a failed start leaves $(ls -A "$TMP/tmpdir") behind"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: the ST-Link capture starts from a read under reset and cuts to the reset's boot only"
