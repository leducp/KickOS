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
echo "$1 ${VARIANT:--} $APP ${AMP_PARTITION:-0}" >> "$FLEET_FIXTURE/flashed"
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
    { "name": "esp32c6-wroom-amp2-n0", "generator": "Ninja", "binaryDir": "b2" }
  ]
}
EOF
printf 'RIG_SESSION=%s\nRIG_TREE=%s\n' "$TMP/session" "$TMP/tree" > "$TMP/rig.conf"

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
    rm -f "$F/flashed"
    FLEET_FIXTURE="$F" KICKOS_RIG="$TMP/rig.conf" TREE="$TMP/tree" TAG=planted \
        bash "$TMP/tools/bench/bench-fleet.sh" "$@" > "$TMP/fleet.out" 2>&1
}

RX_BUS='045b:82a0 E2L: X
0403:6001 FT1'
C6_BUS='1a86:55d3 C6'

fleet "$RX_BUS" rx72m
got=$?
[ "$got" -eq 0 ] || bad "a pass capturing every rx72m image exits $got, not 0"
grep -qxF 'rx72m flat fpclass 0' "$F/flashed" \
    || bad "the rx72m pass did not capture fpclass from its flat build"
if grep -qxF 'rx72m flat hello 0' "$F/flashed"; then
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

[ "$rc" -eq 0 ] || exit 1
echo "PASS: an absent board fails the pass; the flat fpclass and an owed AMP clause count, each"
echo "  under its own label; a loopback is judged only where the rig declares its fitting"
