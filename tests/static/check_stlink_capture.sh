#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ST-Link capture's cut to the reset's boot (tools/bench/reset-boot.sh) against every banner
# row kbanner() prints and over planted logs, and its start (FLASH_STLINK_RESET=1
# tools/flash-stlink.sh) over a stub st-flash.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
. tools/bench/reset-boot.sh

# Every format kbanner() prints, both columns, rendered: the first is the title, the rest are rows
# the cut must recognise, so a boot that lost its head past any of them is not taken for none.
KBANNER=$(awk '/void kbanner\(\)/ { on = 1 }
    on { print; o = gsub(/[{]/, "&"); c = gsub(/[}]/, "&"); d += o - c; if (o > 0) { s = 1 } if (s && d == 0) { exit } }' \
    kernel/init/kmain.cc)
mapfile -t MACROS < <(grep -oE 'KDIAG_F_BANNER_[A-Z_]+' <<< "$KBANNER" | awk '!seen[$0]++')
[ "${#MACROS[@]}" -ge 2 ] || fail "found no banner rows in kbanner() (kernel/init/kmain.cc)"
# Arguments per format, one rendering per line, tab-separated; '-' renders a format with none. A
# format whose argument kbanner() assigns from string literals is rendered with those instead.
declare -A SAMPLES=(
    [KDIAG_F_BANNER_NAME]=$'0.5.1\n0.5.1-rc1'
    [KDIAG_F_BANNER_BOARD]=$'f411disco\npizero2350-amp2-n0'
    [KDIAG_F_BANNER_ARCH]=$'armv7m\nx86_64\nsim'
    [KDIAG_F_BANNER_CPU]=$'cortex-m4\ncortex-m33+nodsp\nrv32imac_zicsr\nrxv3\nx86-64'
    [KDIAG_F_BANNER_BUILD]='2026-07-22 15:44:04 +0200'
    [KDIAG_F_BANNER_APP]=$'hello Oct  6 2026 12:00:00\nselftest_p2 Oct 16 2026 12:00:00 +0200'
    [KDIAG_F_BANNER_COMMIT]=$'0355b756\n0355b756-dirty\nv0.5.1-3-g0355b756\nnogit'
    [KDIAG_F_BANNER_HEAP]=$'0\n64\n4096'
    [KDIAG_F_BANNER_NOHEAP]='-'
    [KDIAG_F_BANNER_KSTACK]=$'1024\t8\t8192'
)
CR=$(printf '\r')
nrows=0
for i in "${!MACROS[@]}"; do
    m=${MACROS[$i]}
    def=$(grep -E "^#define $m +KICKOS_DIAG_PICK\(\".*\", \".*\"\)$" include/kickos/diag.h)
    if [ -z "$def" ]; then
        bad "kbanner() prints $m, which include/kickos/diag.h does not define as a two-column pick"
        continue
    fi
    arg=$(grep -oE "$m, *[A-Za-z_][A-Za-z0-9_]*" <<< "$KBANNER" | head -n1 | sed -E 's/.*, *//')
    lits=""
    if [ -n "$arg" ]; then
        lits=$(grep -oE "(^|[^A-Za-z0-9_])$arg = \"[^\"]*\"" <<< "$KBANNER" | sed -E 's/.*= "(.*)"/\1/')
    fi
    renders=$lits
    if [ -z "$renders" ]; then
        renders=${SAMPLES[$m]-}
    fi
    if [ -z "$renders" ]; then
        bad "the gate has no rendering of $m, which kbanner() prints"
        continue
    fi
    for col in 1 2; do
        fmt=$(sed -E "s/^#define $m +KICKOS_DIAG_PICK\(\"(.*)\", \"(.*)\"\)$/\\$col/" <<< "$def")
        nconv=$(grep -oE '%[a-z]' <<< "$fmt" | wc -l)
        while IFS= read -r tuple; do
            args=()
            if [ "$tuple" != "-" ]; then
                IFS=$'\t' read -ra args <<< "$tuple"
            fi
            if [ "${#args[@]}" -ne "$nconv" ]; then
                bad "the rendering '$tuple' of $m does not fit its format '$fmt'"
                continue
            fi
            # shellcheck disable=SC2059
            row=$(printf "$fmt" "${args[@]}")
            for line in "$row" "$row$CR"; do
                if [ "$i" -eq 0 ]; then
                    grep -qE "$RESET_BOOT_TITLE_RE" <<< "$line" \
                        || bad "RESET_BOOT_TITLE_RE does not match the title '$row'"
                    if grep -qE "$RESET_BOOT_ROW_RE" <<< "$line"; then
                        bad "RESET_BOOT_ROW_RE matches the title '$row'"
                    fi
                else
                    grep -qE "$RESET_BOOT_ROW_RE" <<< "$line" \
                        || bad "RESET_BOOT_ROW_RE does not match the banner row '$row' ($m)"
                    if grep -qE "$RESET_BOOT_TITLE_RE" <<< "$line"; then
                        bad "RESET_BOOT_TITLE_RE matches the banner row '$row' ($m)"
                    fi
                fi
            done
            nrows=$((nrows + 1))
        done <<< "$renders"
    done
done
[ "$nrows" -gt 0 ] || fail "rendered no banner row"
for line in '[blink] tick 41' 'ok 1 - case' '1..3' '# done' '[hello] up' 'not ok 2' 'a' 'h' 'c'; do
    if grep -qE "$RESET_BOOT_ROW_RE|$RESET_BOOT_TITLE_RE" <<< "$line"; then
        bad "an ordinary line is taken for a banner row: '$line'"
    fi
done

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
grep -q 'no whole banner title follows the reset' "$TMP/why" \
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

# The earlier image prints on past the reset point until the connect stops it.
tail_of_earlier late
printf 'ck 43\r\n[blink] tick 44\r\nK 0.5.1\r\n\r\nb f302nucleo\r\nc 0355b756\r\n[hello] up\r\n' >> "$TMP/late"
cut_log late "$SIZE" || bad "a terse boot after the earlier image's last lines is refused: $(cat "$TMP/why")"
head -n 1 "$TMP/late" | grep -q '^K 0.5.1' \
    || bad "the cut does not start at the terse title: $(head -n 1 "$TMP/late")"

# A boot whose head landed ahead of the reset point: what follows it is no whole banner.
tail_of_earlier headless
printf '\r\nm off\r\ns tickless\r\nc 0355b756\r\nh 1\r\n\r\n[hello] up\r\n' >> "$TMP/headless"
if cut_log headless "$SIZE"; then
    bad "a capture whose boot lost its banner title ahead of the reset point is kept headless"
fi
grep -q 'arrived without its banner title' "$TMP/why" \
    || bad "the headless refusal says: $(cat "$TMP/why")"

# A damaged title is refused even where a later boot's whole one would anchor a cut, which would
# hide the first boot.
for name in damaged notitle onlycommit nobanner tailrows tailrowsfull; do
    tail_of_earlier "$name"
done
banner "$TMP/damaged" '   KckOS 0.5.1  -  micrkernel RTOS' '   cmmit  0355b756'
printf '[panicgate] case 1\r\n' >> "$TMP/damaged"
banner "$TMP/notitle" '   KckOS 0.5.1  -  micrkernel RTOS' "$COMMIT"
printf '1..3\r\nok 1\r\n' >> "$TMP/notitle"
banner "$TMP/notitle" "$TITLE" "$COMMIT"
banner "$TMP/onlycommit" '   KckOS 0.5.1  -  micrkernel RTOS' "$COMMIT"
banner "$TMP/nobanner" '   KckOS 0.5.1  -  micrkernel RTOS' '   cmmit  0355b756'
printf '1..3\r\nok 1\r\n' >> "$TMP/nobanner"
banner "$TMP/nobanner" "$TITLE" "$COMMIT"
# Lost through its commit row: only the heap and kernel-stack rows are left of that boot.
printf 'h 64\r\nk 1024 8 8192\r\n\r\n[hello] up\r\n' >> "$TMP/tailrows"
banner "$TMP/tailrows" "$TITLE" "$COMMIT"
printf '   heap    64 KiB available\r\n   kstack  1024 B x 8 = 8192 B\r\n\r\n' >> "$TMP/tailrowsfull"
banner "$TMP/tailrowsfull" "$TITLE" "$COMMIT"
for name in damaged notitle onlycommit nobanner tailrows tailrowsfull; do
    if cut_log "$name" "$SIZE"; then
        bad "the $name capture, whose boot after the reset lost its title, is not refused"
    fi
    grep -q 'arrived without its banner title' "$TMP/why" \
        || bad "the $name refusal says: $(cat "$TMP/why")"
done

# The line the reset splits joins the earlier output to the reset's first bytes.
tail_of_earlier joined
printf 'x\r\n  ==============================================\r\n%s\r\n' "$TITLE" >> "$TMP/joined"
cut_log joined "$SIZE" || bad "a capture whose reset split a line is refused: $(cat "$TMP/why")"
head -n 1 "$TMP/joined" | grep -qE '^  =+'"$(printf '\r')"'$' \
    || bad "the line the reset split is read as damage: $(head -n 1 "$TMP/joined")"

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
