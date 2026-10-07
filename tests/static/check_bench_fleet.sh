#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/bench/bench-fleet.sh's verdict over planted passes: a stub bench.sh answers the image
# listings and the captures, and a stub bus says which boards are present.
#
#   check_bench_fleet.sh <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
CMAKE="${1:?usage: check_bench_fleet.sh <cmake>}"
PATH="$(dirname "$CMAKE"):$PATH"
export PATH

mkdir -p "$TMP/tools/bench" "$TMP/fixture" "$TMP/tree" "$TMP/session"
for f in bench-fleet.sh rig.sh board-rows.sh; do
    cp "tools/bench/$f" "$TMP/tools/bench/$f"
done
cat > "$TMP/tools/bench/bench-host.sh" <<'EOF'
BENCH_WHERE="the planted bus"
bench_host_select() { :; }
bench_bus_read() { USB_BUS="$(cat "$FLEET_FIXTURE/bus")"; [ -n "$USB_BUS" ] || return 2; }
usb_present() { printf '%s\n' "$USB_BUS" | grep -q "^$1 "; }
usb_serial_of() { return 1; }
EOF
cat > "$TMP/tools/bench/bench.sh" <<'EOF'
#!/bin/sh
if [ "${LIST_IMAGES:-0}" = 1 ]; then
    f="$FLEET_FIXTURE/$1${VARIANT:+-$VARIANT}.images"
    if [ -f "$f" ]; then
        cat "$f"
    fi
    exit 0
fi
if [ "${BUILD_ONLY:-0}" = 1 ]; then
    echo "build $1 $APP" >> "$FLEET_FIXTURE/events"
    exit 0
fi
echo "$1 ${VARIANT:--} $APP ${AMP_PARTITION:-0} $TAG" >> "$FLEET_FIXTURE/flashed"
echo "start $1" >> "$FLEET_FIXTURE/events"
if [ -f "$FLEET_FIXTURE/$1.peer" ]; then
    n=0
    until grep -qx "start $(cat "$FLEET_FIXTURE/$1.peer")" "$FLEET_FIXTURE/events"; do
        n=$((n + 1))
        if [ "$n" -ge 100 ]; then
            echo "alone $1" >> "$FLEET_FIXTURE/events"
            break
        fi
        sleep 0.1
    done
fi
if [ -f "$FLEET_FIXTURE/hold" ]; then
    sleep "$(cat "$FLEET_FIXTURE/hold")"
fi
echo "end $1" >> "$FLEET_FIXTURE/events"
if [ -f "$FLEET_FIXTURE/$APP.capture" ]; then
    . "$(dirname "$0")/rig.sh"
    rig_load "$TREE"
    judge=$(awk -F '|' -v a="$APP" '$1 == a { print $3; exit }' "$FLEET_FIXTURE/$1.images")
    rig_judge "$1" "$FLEET_FIXTURE/$APP.capture" "$FLEET_FIXTURE/build" "$judge" ""
    exit $?
fi
if [ -f "$FLEET_FIXTURE/$APP${VARIANT:+-$VARIANT}.out" ]; then
    cat "$FLEET_FIXTURE/$APP${VARIANT:+-$VARIANT}.out"
elif [ -f "$FLEET_FIXTURE/$APP.out" ]; then
    cat "$FLEET_FIXTURE/$APP.out"
fi
exit 0
EOF
chmod +x "$TMP/tools/bench/bench-fleet.sh" "$TMP/tools/bench/bench.sh"
cat > "$TMP/tree/CMakePresets.json" <<'EOF'
{
  "version": 3,
  "configurePresets": [
    { "name": "rx72m-flat", "generator": "Ninja", "binaryDir": "b1" },
    { "name": "esp32c6-wroom-amp2-n0", "generator": "Ninja", "binaryDir": "b2" },
    { "name": "esp32c6-wroom-amp2-n1", "generator": "Ninja", "binaryDir": "b3" },
    { "name": "esp32-wroom-st", "generator": "Ninja", "binaryDir": "b4" },
    { "name": "esp32-wroom-smp", "generator": "Ninja", "binaryDir": "b5" },
    { "name": "esp32-wroom-bench", "generator": "Ninja", "binaryDir": "b6" }
  ]
}
EOF
printf 'RIG_SESSION=%s\nRIG_TREE=%s\n' "$TMP/session" "$TMP/tree" > "$TMP/rig.conf"
mkdir -p "$TMP/tree/boards/esp32-wroom/configs/bench"
printf 'CONFIG_KICKOS_BENCH=y\n' > "$TMP/tree/boards/esp32-wroom/configs/bench/defconfig"

F="$TMP/fixture"
{
    printf 'hello|kernel|tests/integration/check_qemu_hello.sh|\n'
    printf 'fpclass|kernel|tests/integration/check_fpclass.sh|\n'
} > "$F/rx72m.images"
cp "$F/rx72m.images" "$F/rx72m-flat.images"
printf 'hello|kernel|tests/integration/check_qemu_hello.sh|\n' > "$F/esp32c6-wroom.images"
printf 'ampping_n0|kernel|tests/integration/check_c6_amp_capture.sh|\n' \
    > "$F/esp32c6-wroom-amp2-n0.images"
printf 'NOT EVALUATED: the planted clause\n' > "$F/ampping_n0.out"

# <bus lines> <board>...: the fleet's exit status, its output in $TMP/fleet.out.
fleet() {
    printf '%s\n' "$1" > "$F/bus"
    shift
    rm -f "$F/flashed" "$F/events"
    FLEET_FIXTURE="$F" KICKOS_RIG="$TMP/rig.conf" TREE="$TMP/tree" TAG=planted \
        bash "$TMP/tools/bench/bench-fleet.sh" "$@" > "$TMP/fleet.out" 2>&1
}

RX_BUS='045b:82a0 E2L: X
0403:6001 FT1'
C6_BUS='1a86:55d3 C6'

fleet "$RX_BUS" rx72m
got=$?
[ "$got" -eq 0 ] || bad "a pass capturing every rx72m image exits $got, not 0"
grep -qxF 'rx72m flat fpclass 0 plantedflat' "$F/flashed" \
    || bad "the rx72m pass did not capture fpclass from its flat build"
if grep -q '^rx72m flat hello ' "$F/flashed"; then
    bad "the rx72m pass captured hello from its flat build, which only the enforcing build owes"
fi
grep -qE '^  rx72m +fpclass \(flat\) +captured$' "$TMP/fleet.out" \
    || bad "the rx72m coverage table does not show fpclass (flat) captured"

printf 'NOT EVALUATED: the flat clause\n' > "$F/fpclass-flat.out"
fleet "$RX_BUS" rx72m
got=$?
[ "$got" -eq 3 ] || bad "a pass owing a clause of the flat fpclass exits $got, not 3"
grep -qE '^  rx72m +fpclass +captured$' "$TMP/fleet.out" \
    || bad "the enforcing fpclass row shows a clause only its flat run owes"
grep -qE '^  rx72m +fpclass \(flat\) +captured, partly owed: the flat clause$' "$TMP/fleet.out" \
    || bad "the flat fpclass row does not owe its own clause"
rm -f "$F/fpclass-flat.out"

fleet "$C6_BUS" rx72m
got=$?
[ "$got" -eq 1 ] || bad "a pass whose only board is absent exits $got, not 1"
grep -q '^rx72m  *ABSENT' "$TMP/fleet.out" || bad "the absent rx72m is not reported ABSENT"
grep -q '^INCOMPLETE: 1 board(s) were absent' "$TMP/fleet.out" \
    || bad "the pass with an absent board is not reported INCOMPLETE"

fleet "$C6_BUS" esp32c6-wroom
got=$?
[ "$got" -eq 3 ] || bad "a pass owing an AMP partition's clause exits $got, not 3"
grep -qE '^  esp32c6-wroom +amp_partition +captured, partly owed: the planted clause$' \
    "$TMP/fleet.out" || bad "the AMP partition's NOT EVALUATED clause is not owed in the table"
grep -qxF 'esp32c6-wroom amp2-n0 ampping_n0 1 plantedamp2n0' "$F/flashed" \
    || bad "the esp32c6-wroom partition was not flashed under its own tag"
if grep -q '^esp32c6-wroom amp2-n1 ' "$F/flashed"; then
    bad "a partition's node 1 preset was flashed as an image of its own"
fi

# A partition whose node 0 build cannot say what it ships is a refused configure, not an image
# with no judge.
mv "$F/esp32c6-wroom-amp2-n0.images" "$F/amp.images"
fleet "$C6_BUS" esp32c6-wroom
got=$?
[ "$got" -eq 1 ] || bad "a pass whose partition build cannot list its images exits $got, not 1"
grep -q '^esp32c6-wroom/amp2-n0  *REFUSED (the tree was not able to say which images' "$TMP/fleet.out" \
    || bad "a partition build that cannot list its images is not reported as a refused configure"
if grep -qE '^  esp32c6-wroom +amp_partition +NO JUDGE$' "$TMP/fleet.out"; then
    bad "a partition build that cannot list its images is reported as an image with no judge"
fi
mv "$F/amp.images" "$F/esp32c6-wroom-amp2-n0.images"

# Only a partition's later nodes are passed over: another partition's node 0 and a node whose
# partition the tree does not declare are each captured.
cp "$TMP/tree/CMakePresets.json" "$TMP/presets.keep"
sed -i 's|^    { "name": "esp32c6-wroom-amp2-n1", "generator": "Ninja", "binaryDir": "b3" },$|&\
    { "name": "esp32c6-wroom-amp3-n0", "generator": "Ninja", "binaryDir": "b7" },\
    { "name": "esp32c6-wroom-amp3-n1", "generator": "Ninja", "binaryDir": "b8" },\
    { "name": "esp32c6-wroom-solo-n1", "generator": "Ninja", "binaryDir": "b9" },|' \
    "$TMP/tree/CMakePresets.json"
grep -q 'esp32c6-wroom-solo-n1' "$TMP/tree/CMakePresets.json" || fail "the planted presets were not added"
cp "$F/esp32c6-wroom-amp2-n0.images" "$F/esp32c6-wroom-amp3-n0.images"
printf 'hello|kernel|tests/integration/check_qemu_hello.sh|\n' > "$F/esp32c6-wroom-solo-n1.images"
fleet "$C6_BUS" esp32c6-wroom
for row in 'esp32c6-wroom amp3-n0 ampping_n0 1 plantedamp3n0' 'esp32c6-wroom solo-n1 hello 0 plantedsolon1'; do
    grep -qxF "$row" "$F/flashed" || bad "the esp32c6-wroom pass did not flash [$row]"
done
if grep -qE '^esp32c6-wroom amp[23]-n1 ' "$F/flashed"; then
    bad "a partition's node 1 preset was flashed as an image of its own"
fi
cp "$TMP/presets.keep" "$TMP/tree/CMakePresets.json"

# A tree whose presets cannot be listed fails the pass instead of declaring no variant.
printf '{\n' > "$TMP/tree/CMakePresets.json"
fleet "$RX_BUS" rx72m
got=$?
[ "$got" -eq 1 ] || bad "a pass over a tree whose presets cannot be listed exits $got, not 1"
grep -q '^rx72m  *REFUSED (the tree was not able to list its presets' "$TMP/fleet.out" \
    || bad "a tree whose presets cannot be listed is not reported"
cp "$TMP/presets.keep" "$TMP/tree/CMakePresets.json"

# The default variant has one definition, which bench.sh and the fleet pass both read.
defs=$(grep -lE 'VARIANT=st$|VARIANT:?-st\}' tools/bench/*.sh)
[ "$defs" = tools/bench/board-rows.sh ] \
    || bad "the default variant is defined in [$(printf '%s' "$defs" | tr '\n' ' ')], not in board-rows.sh alone"
grep -qF 'VARIANT="${VARIANT-$BENCH_DEFAULT_VARIANT}"' tools/bench/bench.sh \
    || bad "bench.sh does not default its variant to BENCH_DEFAULT_VARIANT"

# Every variant the tree declares for a board, each under its own tag; a measurement posture is
# reported and not flashed.
{
    printf 'hello|kernel|tests/integration/check_qemu_hello.sh|\n'
    printf 'cxxtest|kernel|tests/integration/check_qemu_cxxtest.sh|\n'
} > "$F/esp32-wroom.images"
{
    cat "$F/esp32-wroom.images"
    printf 'slaypeer|kernel|tests/integration/check_slaypeer.sh|\n'
} > "$F/esp32-wroom-smp.images"
printf 'bench|kernel|-|\n' > "$F/esp32-wroom-bench.images"
ESP_BUS='1a86:7523 CH340'
fleet "$ESP_BUS" esp32-wroom
got=$?
[ "$got" -eq 0 ] || bad "a pass capturing every esp32-wroom variant exits $got, not 0"
for row in 'esp32-wroom - hello 0 planted' 'esp32-wroom - cxxtest 0 planted' \
           'esp32-wroom smp hello 0 plantedsmp' 'esp32-wroom smp cxxtest 0 plantedsmp' \
           'esp32-wroom smp slaypeer 0 plantedsmp'; do
    grep -qxF "$row" "$F/flashed" || bad "the esp32-wroom pass did not flash [$row]"
done
if grep -q '^esp32-wroom bench ' "$F/flashed"; then
    bad "the esp32-wroom pass flashed its KICKOS_BENCH variant, which the bench sweep measures"
fi
grep -qE '^  esp32-wroom +cxxtest \(smp\) +captured$' "$TMP/fleet.out" \
    || bad "the esp32-wroom coverage table does not show cxxtest (smp) captured"
grep -qE '^  esp32-wroom +variant bench +measurement posture' "$TMP/fleet.out" \
    || bad "the esp32-wroom coverage table does not report its measurement variant"
printf 'slaypeer|kernel|-|\n' >> "$F/esp32-wroom-smp.images"
fleet "$ESP_BUS" esp32-wroom
got=$?
[ "$got" -eq 1 ] || bad "a pass where an smp image names no judge exits $got, not 1"
grep -qE '^  esp32-wroom +slaypeer \(smp\) +NO JUDGE$' "$TMP/fleet.out" \
    || bad "an smp image that names no judge is not reported NO JUDGE"

# A capture whose judge rests on a bench fitting: the rig declares it or the clause is owed, and
# no rig line at all declares nothing.
# <rig line or empty>: rig_wired f411disco under a rig carrying it.
wired_f411() {
    { cat "$TMP/rig.conf"; printf '%s\n' "$1"; } > "$TMP/rig-wired.conf"
    (
        KICKOS_RIG="$TMP/rig-wired.conf"
        . "$TMP/tools/bench/rig.sh"
        rig_find "$TMP/tree" && rig_wired f411disco
    )
}
[ -z "$(wired_f411 '')" ] || bad "a rig with no RIG_WIRED_F411DISCO declares a fitting"
[ "$(wired_f411 "$(printf 'RIG_WIRED_F411DISCO="  spi1-loopback\tlan9252 "')")" = "spi1-loopback lan9252" ] \
    || bad "rig_wired does not read RIG_WIRED_F411DISCO as its space-separated names"
[ -z "$(wired_f411 'RIG_WIRED_BLACKPILL=spi1-loopback')" ] \
    || bad "rig_wired declares another board's fitting on f411disco"
# <rig line>: rig_wired frdmk64f under a rig carrying it, with its refusal on stdout.
wired_k64() {
    { cat "$TMP/rig.conf"; printf '%s\n' "$1"; } > "$TMP/rig-wired.conf"
    (
        KICKOS_RIG="$TMP/rig-wired.conf"
        . "$TMP/tools/bench/rig.sh"
        rig_find "$TMP/tree" && rig_wired frdmk64f 2>&1
    )
}
[ "$(wired_k64 'RIG_WIRED_FRDMK64F=lan9252')" = lan9252 ] \
    || bad "rig_wired does not read a single frdmk64f fitting"
if _k64="$(wired_k64 'RIG_WIRED_FRDMK64F="dspi0-loopback lan9252"')"; then
    bad "rig_wired accepts lan9252 and dspi0-loopback declared together on frdmk64f"
fi
case "$_k64" in
    REFUSING:*) ;;
    *) bad "rig_wired does not name its refusal of two exclusive fittings: $_k64" ;;
esac

F411_BUS='0483:3748 STLINK'
printf 'f411spi|kernel|tests/integration/check_f411spi.sh|\n' > "$F/f411disco.images"
mkdir -p "$F/build"
: > "$F/build/CMakeCache.txt"
cp tests/integration/app_captures/f411spi-unwired.capture "$F/f411spi.capture"
fleet "$F411_BUS" f411disco
got=$?
[ "$got" -eq 3 ] || bad "a pass on an unjumpered f411disco with no fitting declared exits $got, not 3"
grep -qE '^  f411disco +f411spi +captured, partly owed: loopback \(bench wiring absent\)$' \
    "$TMP/fleet.out" || bad "f411spi's loopback is not owed where the rig declares no jumper"
printf 'RIG_WIRED_F411DISCO=spi1-loopback\n' >> "$TMP/rig.conf"
fleet "$F411_BUS" f411disco
got=$?
[ "$got" -eq 1 ] || bad "a pass whose declared jumper echoes nothing exits $got, not 1"
grep -q '^f411disco/f411spi  *FAILED' "$TMP/fleet.out" \
    || bad "f411spi's mismatch is not a failure where the rig declares the jumper"
cp tests/integration/app_captures/f411spi.capture "$F/f411spi.capture"
fleet "$F411_BUS" f411disco
got=$?
[ "$got" -eq 0 ] || bad "a pass whose declared jumper echoes every word exits $got, not 0"
grep -qE '^  f411disco +f411spi +captured$' "$TMP/fleet.out" \
    || bad "f411spi is not captured whole where the rig declares the jumper and the words echo"
printf 'RIG_SESSION=%s\nRIG_TREE=%s\n' "$TMP/session" "$TMP/tree" > "$TMP/rig.conf"
fleet "$F411_BUS" f411disco
got=$?
[ "$got" -eq 3 ] || bad "an echoing f411spi under a rig declaring no jumper exits $got, not 3"

# Two boards sharing nothing are captured at once, each capture of one waiting for the other's
# first; every image is built before the first flash, and one board's captures keep their order.
printf 'hello|kernel|tests/integration/check_qemu_hello.sh|\n' > "$F/f411disco.images"
printf 'RIG_CONSOLE_RX72M=/dev/planted-a\nRIG_CONSOLE_F411DISCO=/dev/planted-b\n' >> "$TMP/rig.conf"
printf 'f411disco\n' > "$F/rx72m.peer"
printf 'rx72m\n' > "$F/f411disco.peer"
fleet "$RX_BUS
$F411_BUS" rx72m f411disco
got=$?
[ "$got" -eq 0 ] || bad "a pass capturing every image of two boards sharing nothing exits $got, not 0"
if grep -q '^alone ' "$F/events"; then
    bad "two boards sharing no probe and no console were captured one after the other"
fi
awk '$1 == "start" { s = 1 } $1 == "build" && s { bad = 1 } END { exit bad }' "$F/events" \
    || bad "an image was built after the first flash"
[ "$(grep '^rx72m ' "$F/flashed" | cut -d' ' -f2,3 | paste -sd ',' -)" = '- hello,- fpclass,flat fpclass' ] \
    || bad "the rx72m captures did not run in their queued order"
for row in 'rx72m +hello' 'f411disco +hello'; do
    grep -qE "^  $row +captured\$" "$TMP/fleet.out" || bad "the merged coverage table does not show [$row] captured"
done
rm -f "$F/rx72m.peer" "$F/f411disco.peer"

# The same two boards on one console cable are captured one after the other.
printf 'RIG_CONSOLE_F411DISCO=/dev/planted-a\n' >> "$TMP/rig.conf"
printf '0.3\n' > "$F/hold"
fleet "$RX_BUS
$F411_BUS" rx72m f411disco
got=$?
[ "$got" -eq 0 ] || bad "a pass over two boards sharing a console exits $got, not 0"
[ "$(grep -c '^start ' "$F/events")" -eq 4 ] || bad "the pass over two boards sharing a console did not capture four images"
awk '$1 == "start" { if (open) bad = 1; open = 1 } $1 == "end" { open = 0 } END { exit bad }' "$F/events" \
    || bad "two boards sharing a console cable were captured at the same time"
rm -f "$F/hold"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: an absent board fails the pass; every declared variant is captured under its own tag"
echo "  and label, a measurement posture is reported and not flashed; a loopback is judged only"
echo "  where the rig declares its fitting; boards sharing nothing are captured at once and boards"
echo "  sharing a cable one after the other, every image built first"
