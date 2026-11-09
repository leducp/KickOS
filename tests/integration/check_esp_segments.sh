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
# An image naming __kickos_esp32_flash_text_start/_end/_load and __kickos_esp32_flash_image_offset
# keeps its task text in flash, which the ROM never loads (tools/esp-image.py): its load segments
# over that span count as no ROM segment and must end inside it. With --app-bin, no segment
# the header lists may load into the span, the boot image must end below the load, and the bytes
# at the load, counted from the image offset the .app.bin is flashed at, must be the ELF's own.
#
#   check_esp_segments.sh --images <list> <readelf> <nm> <esptool options> [--app-bin]
#   check_esp_segments.sh --self-test

set -u
. "$(dirname "$0")/../lib/gate.sh"

MAX_SEGMENTS=16
MAGIC=233

# <readelf> <elf>: writes $TMP/loads, `<start> <end>` of each PT_LOAD's file bytes by start,
# and prints how many esptool segments they make, or nothing where readelf lists no PT_LOAD.
# Those inside the flash text span (FT_START to FT_END, empty where FT_END is 0) go to
# $TMP/flash_rows instead, as `<file offset> <address> <size>`.
load_runs() {
    "$1" -lW "$2" > "$TMP/phdrs" 2>/dev/null || return 0
    awk '$1 == "LOAD" { print $4, $5, $2 }' "$TMP/phdrs" | sort > "$TMP/phdr_rows"
    : > "$TMP/loads"
    : > "$TMP/flash_rows"
    _lr_count=0
    _lr_seen=0
    _lr_end=-1
    while read -r _lr_at _lr_size _lr_off; do
        _lr_seen=1
        _lr_at=$((_lr_at))
        _lr_size=$((_lr_size))
        if [ "$_lr_at" -eq 0 ] || [ "$_lr_size" -eq 0 ]; then
            continue
        fi
        if [ "$_lr_at" -ge "$FT_START" ] && [ "$_lr_at" -lt "$FT_END" ]; then
            echo "$((_lr_off)) $_lr_at $_lr_size" >> "$TMP/flash_rows"
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

# <file> <offset>: the little-endian word there, in decimal, or nothing past the file's end.
le32_at() {
    od -An -tu4 --endian=little -j "$2" -N4 "$1" | tr -d ' '
}

# <nm> <elf>: sets FT_START, FT_END, FT_LOAD and FT_OFFSET from the image's flash text symbols,
# FT_END 0 where it names none; status 2 where it names some but not all.
flash_text() {
    FT_START=0
    FT_END=0
    FT_LOAD=0
    FT_OFFSET=0
    "$1" "$2" 2>/dev/null | awk '$3 ~ /^__kickos_esp32_flash_(text_start|text_end|text_load|image_offset)$/ {
        print $3, $1 }' > "$TMP/flash_syms"
    [ -s "$TMP/flash_syms" ] || return 0
    _ft_n=0
    while read -r _ft_sym _ft_val; do
        _ft_n=$((_ft_n + 1))
        case "$_ft_sym" in
            *_text_start) FT_START=$((0x$_ft_val)) ;;
            *_text_end) FT_END=$((0x$_ft_val)) ;;
            *_text_load) FT_LOAD=$((0x$_ft_val)) ;;
            *_image_offset) FT_OFFSET=$((0x$_ft_val)) ;;
        esac
    done < "$TMP/flash_syms"
    [ "$_ft_n" -eq 4 ] || return 2
    return 0
}

# <name>: the flash text's load segments, each of which must end inside the span. Sets rc.
flash_cover() {
    while read -r _fc_off _fc_addr _fc_size; do
        if [ $((_fc_addr + _fc_size)) -gt "$FT_END" ]; then
            echo "FAIL: $1: the flash text's load segment at 0x$(printf '%X' "$_fc_addr") ends at 0x$(printf '%X' $((_fc_addr + _fc_size))), past its span's end 0x$(printf '%X' "$FT_END")" >&2
            rc=1
        fi
    done < "$TMP/flash_rows"
}

# <name> <elf> <app.bin> <count>: the boot image's segments against the flash text, and the
# flash text's bytes in the .app.bin against the ELF's. Sets rc.
flash_bin() {
    _fb_at=24
    _fb_i=0
    while [ "$_fb_i" -lt "$4" ]; do
        _fb_addr="$(le32_at "$3" "$_fb_at")"
        _fb_len="$(le32_at "$3" $((_fb_at + 4)))"
        if [ -z "$_fb_addr" ] || [ -z "$_fb_len" ]; then
            echo "FAIL: $1: $3 ends inside its segment headers" >&2
            rc=1
            return
        fi
        if [ "$_fb_addr" -ge "$FT_START" ] && [ "$_fb_addr" -lt "$FT_END" ]; then
            echo "FAIL: $1: the boot image loads segment $_fb_i at 0x$(printf '%X' "$_fb_addr"), inside the flash text" >&2
            rc=1
        fi
        _fb_at=$((_fb_at + 8 + _fb_len))
        _fb_i=$((_fb_i + 1))
    done
    # The checksum pad and the digest, at most 16 and 32 bytes.
    if [ $((FT_OFFSET + _fb_at + 48)) -gt "$FT_LOAD" ]; then
        echo "FAIL: $1: the boot image reaches flash 0x$(printf '%X' $((FT_OFFSET + _fb_at))), past its text at 0x$(printf '%X' "$FT_LOAD")" >&2
        rc=1
        return
    fi
    while read -r _fb_off _fb_addr _fb_size; do
        _fb_bin=$((FT_LOAD - FT_OFFSET + _fb_addr - FT_START))
        if ! cmp -s -n "$_fb_size" "$2" "$3" "$_fb_off" "$_fb_bin"; then
            echo "FAIL: $1: the $_fb_size bytes at flash 0x$(printf '%X' $((FT_OFFSET + _fb_bin))) of $3 are not the ELF's text at 0x$(printf '%X' "$_fb_addr")" >&2
            rc=1
        fi
    done < "$TMP/flash_rows"
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
                echo "  LOAD           ${3:-0x001000} 0x3ffb7400 $1 $2 0x00f00 RW  0x1000"
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
    # Flash text: a span of 0x10000 at 0x400D_0000, loaded at flash 0x70000 by an image flashed
    # at 0x1000, two load segments over it at file offsets 0x100 and 0x8100 of the planted ELF.
    # <word>: four bytes, little-endian.
    le32() {
        printf "\\$(printf '%03o' $(($1 & 255)))\\$(printf '%03o' $((($1 >> 8) & 255)))"
        printf "\\$(printf '%03o' $((($1 >> 16) & 255)))\\$(printf '%03o' $((($1 >> 24) & 255)))"
    }
    # <name> <list> <load> <flash row>...: plant() with the flash symbols, and an ELF of 0x10100
    # bytes of text.
    plant_flash() {
        _pf_name="$1"
        _pf_list="$2"
        _pf_load="$3"
        shift 3
        plant "$_pf_name" "$_pf_list" 3ffbf000 3ffbf800 '0x3ffb0000 0x00100' '0x400a0000 0x00100' "$@"
        printf '400d0000 A __kickos_esp32_flash_text_start\n400e0000 A __kickos_esp32_flash_text_end\n' \
            >> "$TMP/$_pf_name.syms"
        printf '%s A __kickos_esp32_flash_text_load\n00001000 A __kickos_esp32_flash_image_offset\n' \
            "$_pf_load" >> "$TMP/$_pf_name.syms"
        yes 'flash text ' | head -c $((0x10100)) > "$TMP/$_pf_name"
    }
    # <name> <first segment's address>: its .app.bin, two segments of 0x100 bytes and the ELF's
    # text at 0x70000 - 0x1000.
    flash_bin_plant() {
        {
            printf '\351\002\002\040'
            le32 $((0x400a0424))
            head -c 16 /dev/zero
            le32 "$2"
            le32 256
            head -c 256 /dev/zero
            le32 $((0x400a0000))
            le32 256
            head -c 256 /dev/zero
        } > "$TMP/$1.app.bin"
        _fp_pad=$((0x6f000 - $(wc -c < "$TMP/$1.app.bin")))
        head -c "$_fp_pad" /dev/zero | tr '\000' '\377' >> "$TMP/$1.app.bin"
        tail -c +$((0x100 + 1)) "$TMP/$1" >> "$TMP/$1.app.bin"
    }
    plant_flash ft flash.list 00070000 '0x400d0000 0x08000 0x000100' '0x400d8000 0x08000 0x008100'
    flash_bin_plant ft $((0x3ffb0000))
    gate flash.list --app-bin || fail "a flash text image as esp-image.py writes it is refused: $(cat "$TMP/run.out")"
    grep -qF 'ft: 2 segments' "$TMP/run.out" \
        || fail "the flash text's load segments count as ROM segments: $(cat "$TMP/run.out")"
    grep -qF 'ft: flash text 0x10000 bytes at flash 0x70000' "$TMP/run.out" \
        || fail "the flash text is not named: $(cat "$TMP/run.out")"
    # One byte of the text in the .app.bin changed.
    printf 'X' | dd of="$TMP/ft.app.bin" bs=1 seek=$((0x6f000 + 0x9000)) conv=notrunc 2>/dev/null
    if gate flash.list --app-bin; then
        fail "an .app.bin whose flash text is not the ELF's passes"
    fi
    grep -qF 'ft: the 32768 bytes at flash 0x78000' "$TMP/run.out" \
        || fail "a changed text is refused for another reason: $(cat "$TMP/run.out")"
    flash_bin_plant ft $((0x400d0000))
    if gate flash.list --app-bin; then
        fail "a boot image loading a segment into the flash text passes"
    fi
    grep -qF 'ft: the boot image loads segment 0 at 0x400D0000, inside the flash text' "$TMP/run.out" \
        || fail "a loaded flash text is refused for another reason: $(cat "$TMP/run.out")"
    : > "$TMP/flash.list"
    plant_flash ft flash.list 00001200 '0x400d0000 0x08000 0x000100' '0x400d8000 0x08000 0x008100'
    flash_bin_plant ft $((0x3ffb0000))
    if gate flash.list --app-bin; then
        fail "a boot image reaching its flash text's load passes"
    fi
    grep -qF 'past its text at 0x1200' "$TMP/run.out" \
        || fail "a boot image over its text is refused for another reason: $(cat "$TMP/run.out")"
    : > "$TMP/flash.list"
    plant_flash ft flash.list 00070000 '0x400d0000 0x08000 0x000100' '0x400d8000 0x08004 0x008100'
    if gate flash.list; then
        fail "a load segment running past the flash text's end passes"
    fi
    grep -qF 'ft: the flash text'"'"'s load segment at 0x400D8000 ends at 0x400E0004, past its span'"'"'s end 0x400E0000' "$TMP/run.out" \
        || fail "a segment past the flash text is refused for another reason: $(cat "$TMP/run.out")"
    : > "$TMP/flash.list"
    plant_flash ft flash.list 00070000 '0x400d0000 0x08000 0x000100' '0x400d8000 0x08000 0x008100'
    grep -v '_load$' "$TMP/ft.syms" > "$TMP/ft.syms.cut"
    mv "$TMP/ft.syms.cut" "$TMP/ft.syms"
    if gate flash.list; then
        fail "an image naming three of the four flash text symbols passes"
    fi
    grep -qF 'names some of its flash text symbols, not all four' "$TMP/run.out" \
        || fail "three flash text symbols are refused for another reason: $(cat "$TMP/run.out")"
    echo "PASS: program headers count as esptool merges them; an image of 17 segments, one whose"
    echo "  window the ROM loads, options with no --use-segments, and under --app-bin a header of"
    echo "  another count or none are refused; flash text counts as no ROM segment, and a load"
    echo "  segment past its end, three of its symbols, and under --app-bin a boot image loading,"
    echo "  reaching or carrying other bytes of it are refused"
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
    flash_text "$NM" "$ELF" || fail "$NAME: $NM names some of its flash text symbols, not all four"
    N="$(load_runs "$READELF" "$ELF")"
    [ -n "$N" ] || fail "$NAME: $READELF lists no program header of $ELF"
    echo "$NAME: $N segments"
    rc=0
    if [ "$FT_END" -ne 0 ]; then
        echo "$NAME: flash text 0x$(printf '%X' $((FT_END - FT_START))) bytes at flash 0x$(printf '%X' "$FT_LOAD")"
        flash_cover "$NAME"
    fi
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
        elif [ "$FT_END" -ne 0 ]; then
            flash_bin "$NAME" "$ELF" "$BIN" "$E"
        fi
    fi
    if [ "$rc" -ne 0 ]; then
        BAD=$((BAD + 1))
    fi
    COUNT=$((COUNT + 1))
done < "$TMP/maps"
if [ "$BAD" -ne 0 ]; then
    fail "$BAD finding(s) over $COUNT image(s)"
fi
echo "PASS: every one of $COUNT image(s) holds at most $MAX_SEGMENTS segments and loads no window"
