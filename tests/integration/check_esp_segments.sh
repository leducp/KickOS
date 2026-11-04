#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every image the ROM boots through esptool holds at most the 16 segments the ROM loader reads,
# and the ROM loads nothing into an image's window. esptool builds the boot image from the
# ELF's load segments, which the chip's esptool options must name (--use-segments), and makes
# one segment of each run of them whose bytes meet, so the count is read off each image's
# program headers: the PT_LOAD entries with file bytes, by load address, those whose bytes meet
# merged. A window is __kickos_image<n>_window_start to _end; one the loaded bytes meet is a
# window the boot image carries, its .bss and heap as zeros. The images are the ELFs beside the
# link maps the list names, and <esptool options> are the chip's, comma-separated. With
# --app-bin, each count must also be the one esptool wrote into <image>.app.bin, the header's
# second byte. Prints each image's count. --self-test reads planted program headers, symbols
# and headers.
#
#   check_esp_segments.sh --images <list> <readelf> <nm> <esptool options> [--app-bin]
#   check_esp_segments.sh --self-test

set -u
. "$(dirname "$0")/../lib/gate.sh"

MAX_SEGMENTS=16
MAGIC=233

# <readelf> <elf>: writes $TMP/loads, `<start> <end>` of each PT_LOAD's file bytes by start,
# and prints how many esptool segments they make, or nothing where readelf lists no PT_LOAD.
load_runs() {
    "$1" -lW "$2" > "$TMP/phdrs" 2>/dev/null || return 0
    awk '$1 == "LOAD" { print $4, $5 }' "$TMP/phdrs" | sort > "$TMP/phdr_rows"
    : > "$TMP/loads"
    _lr_count=0
    _lr_seen=0
    _lr_end=-1
    while read -r _lr_at _lr_size; do
        _lr_seen=1
        _lr_at=$((_lr_at))
        _lr_size=$((_lr_size))
        if [ "$_lr_at" -eq 0 ] || [ "$_lr_size" -eq 0 ]; then
            continue
        fi
        if [ "$_lr_at" -ne "$_lr_end" ]; then
            _lr_count=$((_lr_count + 1))
        fi
        _lr_end=$((_lr_at + _lr_size))
        echo "$_lr_at $_lr_end" >> "$TMP/loads"
    done < "$TMP/phdr_rows"
    if [ "$_lr_seen" -eq 1 ]; then
        echo "$_lr_count"
    fi
}

# <nm> <elf>: each image's window the loaded bytes in $TMP/loads meet, as `image <n>`;
# status 2 where nm names no window.
loaded_windows() {
    "$1" "$2" 2>/dev/null \
        | awk '{ n = $3; sub(/^_*kickos_image/, "", n) }
               $3 ~ /^_*kickos_image[0-9]+_window_(start|end)$/ {
                   sub(/_window_.*/, "", n); print n, $3 ~ /_start$/, "0x" $1 }' \
        | sort -k1,1n -k2,2nr > "$TMP/window_ends"
    [ -s "$TMP/window_ends" ] || return 2
    while read -r _lw_n _lw_is_start _lw_at; do
        if [ "$_lw_is_start" -eq 1 ]; then
            _lw_start=$((_lw_at))
            continue
        fi
        _lw_end=$((_lw_at))
        while read -r _lw_lo _lw_hi; do
            if [ "$_lw_lo" -lt "$_lw_end" ] && [ "$_lw_hi" -gt "$_lw_start" ]; then
                echo "image $_lw_n"
                break
            fi
        done < "$TMP/loads"
    done < "$TMP/window_ends"
    return 0
}

# <app.bin>: its segment count, or nothing where the header's magic is not esptool's.
app_bin_segments() {
    set -- $(od -An -tu1 -N2 "$1")
    [ "${1:-}" = "$MAGIC" ] || return 0
    echo "${2:-}"
}

scratch_dir

if [ "${1:-}" = --self-test ]; then
    # A readelf and an nm that print the planted program headers and symbols of their ELF.
    printf '#!/bin/sh\ncat "$2.phdrs"\n' > "$TMP/readelf"
    printf '#!/bin/sh\ncat "$1.syms"\n' > "$TMP/nm"
    chmod +x "$TMP/readelf" "$TMP/nm"
    OPTS='--use-segments,--flash-mode,dio'
    # <name> <list> <window start> <window end> <PT_LOAD row>...: a planted image named in
    # <list>, its task image 1's window at the two addresses, each row `<paddr> <filesz>`.
    plant() {
        _pl_name="$1"
        _pl_list="$2"
        printf '%s D __kickos_image1_window_start\n%s D __kickos_image1_window_end\n' \
            "$3" "$4" > "$TMP/$_pl_name.syms"
        shift 4
        : > "$TMP/$_pl_name.map"
        : > "$TMP/$_pl_name"
        {
            echo 'Program Headers:'
            echo '  Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align'
            for _pl_row in "$@"; do
                set -- $_pl_row
                echo "  LOAD           0x001000 0x3ffb7400 $1 $2 0x00f00 RW  0x1000"
            done
            echo '  TLS            0x001da8 0x3ffb0da8 0x3ffb0da8 0x000c0 0x000c0 R   0x8'
        } > "$TMP/$_pl_name.phdrs"
        echo "$TMP/$_pl_name.map" >> "$TMP/$_pl_list"
    }
    # <name> <count>: its .app.bin header, holding <count> segments.
    app_bin() {
        printf "\\351\\$(printf '%03o' "$2")\\002\\040" > "$TMP/$1.app.bin"
    }
    # <list> [--app-bin]: the gate over the planted list, into $TMP/run.out.
    gate() {
        sh "$0" --images "$TMP/$1" "$TMP/readelf" "$TMP/nm" "$OPTS" ${2:-} > "$TMP/run.out" 2>&1
    }
    # Out of address order, a window with no file bytes between two runs that meet, and a
    # run apart: two segments.
    plant two ok.list 3ffb6050 3ffb7000 '0x3ffb173c 0x000f0' '0x3ffb0000 0x0173c' \
        '0x3ffb6050 0x00000' '0x40080000 0x05700'
    plant one ok.list 40821800 40824000 '0x40800000 0x181c4'
    gate ok.list || fail "images of 2 and 1 segments are refused: $(cat "$TMP/run.out")"
    grep -qF 'two: 2 segments' "$TMP/run.out" \
        || fail "runs that meet are not merged, or no count is printed: $(cat "$TMP/run.out")"
    grep -qF 'one: 1 segments' "$TMP/run.out" \
        || fail "a single run is not one segment: $(cat "$TMP/run.out")"
    # 17 runs apart from one another.
    _rows=""
    _i=0
    while [ "$_i" -lt 17 ]; do
        _rows="$_rows 0x3ff$(printf '%02x' $((0xb0 + _i)))000"
        _i=$((_i + 1))
    done
    set --
    for _at in $_rows; do
        set -- "$@" "$_at 0x00010"
    done
    plant one over.list 40821800 40824000 '0x40800000 0x181c4'
    plant seventeen over.list 40821800 40824000 "$@"
    if gate over.list; then
        fail "an image of 17 segments passes"
    fi
    grep -qF 'seventeen: 17 segments' "$TMP/run.out" \
        || fail "17 segments are refused without naming the image: $(cat "$TMP/run.out")"
    # A window whose .data loads where it runs, inside one run.
    plant inplace window.list 3ffb5f60 3ffb6050 '0x3ffb0000 0x06000'
    if gate window.list; then
        fail "an image whose window the ROM loads passes"
    fi
    grep -qF 'inplace: the boot image loads the window of image 1' "$TMP/run.out" \
        || fail "a loaded window is refused for another reason: $(cat "$TMP/run.out")"
    OPTS='--flash-mode,dio'
    if gate ok.list; then
        fail "esptool options with no --use-segments pass"
    fi
    OPTS='--use-segments,--flash-mode,dio'
    : > "$TMP/none.map"
    : > "$TMP/none"
    echo 'There are no program headers in this file.' > "$TMP/none.phdrs"
    : > "$TMP/none.syms"
    echo "$TMP/none.map" > "$TMP/none.list"
    if gate none.list; then
        fail "an ELF with no program headers passes"
    fi
    # The cross-check against esptool's own count.
    app_bin two 2
    app_bin one 1
    gate ok.list --app-bin \
        || fail "headers that agree with the program headers are refused: $(cat "$TMP/run.out")"
    app_bin two 3
    if gate ok.list --app-bin; then
        fail "an .app.bin of another count than the program headers passes"
    fi
    rm "$TMP/two.app.bin"
    if gate ok.list --app-bin; then
        fail "a missing .app.bin passes under --app-bin"
    fi
    echo "PASS: program headers count as esptool merges them; an image of 17 segments, one whose"
    echo "  window the ROM loads, options with no --use-segments, and under --app-bin a header of"
    echo "  another count or none are refused"
    exit 0
fi

[ "$#" -ge 5 ] && [ "$1" = --images ] \
    || fail "usage: check_esp_segments.sh --images <list> <readelf> <nm> <options> [--app-bin]"
LIST="$2"
READELF="$3"
NM="$4"
APP_BIN=0
if [ "${6:-}" = --app-bin ]; then
    APP_BIN=1
fi
case ",$5," in
    *',--use-segments,'*) ;;
    *) fail "the chip's esptool options ($5) name no --use-segments, so esptool counts sections and
  the program headers say nothing of its count" ;;
esac
[ -f "$LIST" ] || fail "no image list at $LIST"
grep -v '^$' "$LIST" > "$TMP/maps"
[ -s "$TMP/maps" ] || fail "$LIST names no image"
COUNT=0
BAD=0
while IFS= read -r MAP; do
    ELF="${MAP%.map}"
    NAME="$(basename "$ELF")"
    [ -f "$ELF" ] || fail "$NAME: no ELF at $ELF"
    N="$(load_runs "$READELF" "$ELF")"
    [ -n "$N" ] || fail "$NAME: $READELF lists no program header of $ELF"
    echo "$NAME: $N segments"
    if [ "$N" -gt "$MAX_SEGMENTS" ]; then
        echo "FAIL: $NAME: $N segments, above the $MAX_SEGMENTS the ROM loader reads" >&2
        BAD=$((BAD + 1))
    fi
    loaded_windows "$NM" "$ELF" > "$TMP/loaded" || fail "$NAME: $NM names no image window"
    while read -r _word IMAGE; do
        echo "FAIL: $NAME: the boot image loads the window of image $IMAGE" >&2
        BAD=$((BAD + 1))
    done < "$TMP/loaded"
    if [ "$APP_BIN" -eq 1 ]; then
        BIN="$ELF.app.bin"
        [ -f "$BIN" ] || fail "$NAME: no $BIN, so esptool refused the image"
        E="$(app_bin_segments "$BIN")"
        [ -n "$E" ] || fail "$NAME: $BIN holds no esptool image header"
        if [ "$E" != "$N" ]; then
            echo "FAIL: $NAME: esptool wrote $E segments, the program headers give $N" >&2
            BAD=$((BAD + 1))
        fi
    fi
    COUNT=$((COUNT + 1))
done < "$TMP/maps"
if [ "$BAD" -ne 0 ]; then
    fail "$BAD finding(s) over $COUNT image(s)"
fi
echo "PASS: every one of $COUNT image(s) holds at most $MAX_SEGMENTS segments and loads no window"
