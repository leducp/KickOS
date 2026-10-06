#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every board app's judge (kickos_app_judge) against PLANTED captures: the fixtures in
# tests/integration/app_captures are written by hand from each app's own print statements, with
# CRLF line ends as the bench records them, and none of them is a silicon recording. A judge
# passes its fixture and refuses each damaged copy the table below names, with the token the row
# names, so no check it holds is vacuous and each refusal is the clause the row aims at. A row is
#   <judge>|<fixture>|<CMakeCache line or empty>|<token>|<op>|<literal>|<replacement>[|<op>|<literal>|<replacement>]
# where op `drop` removes every line carrying the literal, `swap` replaces its first occurrence
# on each line, `after` adds the replacement as a line after each line carrying it, `order`
# exchanges the first line carrying the literal with the first line carrying the replacement,
# and `cache` judges the fixture whole with the replacement as the cache line instead. A second
# edit applies to the first's result. A judge refuses as `FAIL: <token>: ...` (gate.sh jfail).
# Every judge an app CMake names must have a passing row, and every app a board directory
# builds names a judge or sits on WAIVED.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0

# <board>/<app> per line: a board app judged by no capture.
WAIVED=""

ROWS="tests/integration/check_f411spi.sh|f411spi.log||word|drop|[f411spi] word 2:|
tests/integration/check_f411spi.sh|f411spi.log||error|swap|rx=0x3c PASS|rx=0x3d FAIL
tests/integration/check_f411spi.sh|f411spi.log||error|after|poking UNGRANTED|[f411spi] UNGRANTED ACCESS DID NOT FAULT
tests/integration/check_f411spi.sh|f411spi.log||loopback-start|drop|starting loopback|
tests/integration/check_f411spi.sh|f411spi.log||loopback|drop|loopback PASS|
tests/integration/check_f411spi.sh|f411spi.log||announce|drop|poking UNGRANTED|
tests/integration/check_f411spi.sh|f411spi.log||announce|order|loopback PASS|poking UNGRANTED
tests/integration/check_f411spi.sh|f411spi.log||killed|drop|=== THREAD FAULT ===|
tests/integration/check_f411spi.sh|f411spi.log||kill-address|swap|ADDR=0x40020400|ADDR=0x40020404
tests/integration/check_f411spi.sh|f411spi.log||kill-address|swap|ADDR=0x40020400|ADDR=0x40020404|after|poking UNGRANTED|  ADDR=0x40020400
tests/integration/check_f411spi.sh|f411spi.log||panic|swap|PC=0x080002f4|KERNEL PANIC: PC=0x080002f4
tests/integration/check_k64drv.sh|k64drv.log||timer-start|drop|counting the 1 kHz LPO|
tests/integration/check_k64drv.sh|k64drv.log||tick|drop|[k64drv] tick 7|
tests/integration/check_k64drv.sh|k64drv.log||tick|swap|[k64drv] tick 2|[k64drv] tick 22
tests/integration/check_k64drv.sh|k64drv.log||tick|order|[k64drv] tick 4|[k64drv] tick 5
tests/integration/check_k64drv.sh|k64drv.log||slot-read|drop|a thread holding no window|
tests/integration/check_k64drv.sh|k64drv.log||done|drop|[k64drv] done|
tests/integration/check_k64drv.sh|k64drv.log||fault|after|[k64drv] tick 10|=== THREAD FAULT === thread 'k64read' killed, system continues
tests/integration/check_k64drv.sh|k64drv.log||error|after|[k64drv] tick 3|[k64drv] ERROR: planted rc -1
tests/integration/check_k64drv.sh|k64drv.log||panic|after|[k64drv] done|KERNEL PANIC: planted
tests/integration/check_k64console.sh|k64console.log||ctor-line|drop|pre-publish ctor line|
tests/integration/check_k64console.sh|k64console.log||driver-up|drop|[k64uart] driver up|
tests/integration/check_k64console.sh|k64console.log||main-line|drop|post-publish line|
tests/integration/check_k64console.sh|k64console.log||worker-line|drop|[worker] line 3|
tests/integration/check_k64console.sh|k64console.log||worker-line|order|[worker] line 1|[worker] line 2
tests/integration/check_k64console.sh|k64console.log||worker-done|drop|[worker] done|
tests/integration/check_k64console.sh|k64console.log||error|after|[worker] line 1|[k64console] ERROR: planted
tests/integration/check_k64console.sh|k64console.log||panic|after|[worker] done|KERNEL PANIC: planted
tests/integration/check_k64console.sh|k64console.log||verdict|cache||K64CONSOLE_SCRAMBLE_TEST:BOOL=ON
tests/integration/check_k64console.sh|k64console-scramble.log|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|verdict|drop|KERNEL PANIC|
tests/integration/check_k64console.sh|k64console-scramble.log|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|driver-up|drop|[k64uart] driver up|
tests/integration/check_k64console.sh|k64console-scramble.log|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|panic|cache||K64CONSOLE_SCRAMBLE_TEST:BOOL=OFF
tests/integration/check_k64dspi.sh|k64dspi-loopback.log|K64DSPI_LOOPBACK:BOOL=ON|driver-up|drop|SPI service up|
tests/integration/check_k64dspi.sh|k64dspi-loopback.log|K64DSPI_LOOPBACK:BOOL=ON|case|drop|zero-tx loopback: PASS|
tests/integration/check_k64dspi.sh|k64dspi-loopback.log|K64DSPI_LOOPBACK:BOOL=ON|error|swap|single-byte loopback: PASS|single-byte loopback: FAIL
tests/integration/check_k64dspi.sh|k64dspi-loopback.log|K64DSPI_LOOPBACK:BOOL=ON|case|order|single-byte loopback: PASS|zero-tx loopback: PASS
tests/integration/check_k64dspi.sh|k64dspi-loopback.log|K64DSPI_LOOPBACK:BOOL=ON|loopback|drop|loopback PASS (the SPI|
tests/integration/check_k64dspi.sh|k64dspi-loopback.log|K64DSPI_LOOPBACK:BOOL=ON|panic|after|loopback PASS (the SPI|KERNEL PANIC: planted
tests/integration/check_k64dspi.sh|k64dspi-loopback.log|K64DSPI_LOOPBACK:BOOL=ON|byte-test|cache||K64DSPI_LOOPBACK:BOOL=OFF
tests/integration/check_k64dspi.sh|k64dspi-lan9252.log||byte-test|drop|BYTE_TEST PASS|
tests/integration/check_k64dspi.sh|k64dspi-lan9252.log||device-open|drop|device open rc=|
tests/integration/check_k64dspi.sh|k64dspi-lan9252.log||case|cache||K64DSPI_LOOPBACK:BOOL=ON
tests/integration/check_k64dspi.sh|k64dspi-lan9252.log||posture|cache||KICKOS_SPI_LOCAL_ENGINE:BOOL=ON
tests/integration/check_rxdrv.sh|rxdrv.log||mux|swap|general I/O rc 0|general I/O rc -16
tests/integration/check_rxdrv.sh|rxdrv.log||console-pin|drop|refused (-KOS_EBUSY)|
tests/integration/check_rxdrv.sh|rxdrv.log||holder|drop|PASS periph_enable holder|
tests/integration/check_rxdrv.sh|rxdrv.log||error|swap|PASS periph_enable holder|FAIL periph_enable holder
tests/integration/check_rxdrv.sh|rxdrv.log||blink-start|drop|blinking LED6|
tests/integration/check_rxdrv.sh|rxdrv.log||blink|drop|PASS (pad tracked|
tests/integration/check_rxdrv.sh|rxdrv.log||non-holder|drop|periph_enable non-holder|
tests/integration/check_rxdrv.sh|rxdrv.log||non-holder|order|PASS (pad tracked|periph_enable non-holder
tests/integration/check_rxdrv.sh|rxdrv.log||announce|drop|poking UNGRANTED|
tests/integration/check_rxdrv.sh|rxdrv.log||killed|swap|thread 'rxpoke'|thread 'rxdrv'
tests/integration/check_rxdrv.sh|rxdrv.log||kill-address|swap|ADDR=0x8c199|ADDR=0x8c068
tests/integration/check_rxdrv.sh|rxdrv.log||error|after|poking UNGRANTED|[rxdrv] UNGRANTED ACCESS DID NOT FAULT (MPU not enforcing)
tests/integration/check_rxdrv.sh|rxdrv.log||panic|after|ADDR=0x8c199|KERNEL PANIC: planted
tests/integration/check_c6blink.sh|c6blink.log||holder|swap|holder rc -38 (want -38)|holder rc -1 (want -38)
tests/integration/check_c6blink.sh|c6blink.log||blink-start|drop|blinking GPIO10|
tests/integration/check_c6blink.sh|c6blink.log||blink|drop|PASS (pad tracked|
tests/integration/check_c6blink.sh|c6blink.log||error|swap|[c6blink] blink 4 pad=1/1|[c6blink] FAIL blink 4 pad=1/0
tests/integration/check_c6blink.sh|c6blink.log||non-holder|drop|periph_enable non-holder|
tests/integration/check_c6blink.sh|c6blink.log||non-holder|order|PASS (pad tracked|periph_enable non-holder
tests/integration/check_c6blink.sh|c6blink.log||announce|drop|poking UNGRANTED|
tests/integration/check_c6blink.sh|c6blink.log||killed|swap|thread 'c6poke'|thread 'c6blink'
tests/integration/check_c6blink.sh|c6blink.log||kill-address|swap|ADDR=0x6009157c|ADDR=0x60091040
tests/integration/check_c6blink.sh|c6blink.log||panic|after|ADDR=0x6009157c|=== RISC-V TRAP ===
tests/integration/check_c6lpprobe.sh|c6lpprobe.log||payload|drop|payload IRQ|
tests/integration/check_c6lpprobe.sh|c6lpprobe.log||doorbells|drop|PASS bidirectional|
tests/integration/check_c6lpprobe.sh|c6lpprobe.log||doorbells|order|payload IRQ|PASS bidirectional
tests/integration/check_c6lpprobe.sh|c6lpprobe.log||timeout|after|LP raw 1|c6lpprobe: timeout marker 0 code 0 pwr1 0 mux 0 pwr0 0 local 0
tests/integration/check_c6lpprobe.sh|c6lpprobe.log||panic|after|PASS bidirectional|KERNEL PANIC: planted
tests/integration/check_xmcspi.sh|xmcspi.log||error|swap|seam FDR: rc=0 wrote=0x80000167 read=0x80000167 LANDED|seam FDR: rc=0 wrote=0x80000167 read=0x0 DISCARDED/REFUSED
tests/integration/check_xmcspi.sh|xmcspi.log||seam|drop|seam BRG:|
tests/integration/check_xmcspi.sh|xmcspi.log||loopback-start|drop|starting SSC loopback|
tests/integration/check_xmcspi.sh|xmcspi.log||word|drop|[xmcspi] word 2:|
tests/integration/check_xmcspi.sh|xmcspi.log||error|swap|word 1: tx=0x3c rx=0x3c PASS|word 1: tx=0x3c rx=0x3d FAIL
tests/integration/check_xmcspi.sh|xmcspi.log||loopback|drop|loopback PASS|
tests/integration/check_xmcspi.sh|xmcspi.log||announce|drop|poking UNGRANTED|
tests/integration/check_xmcspi.sh|xmcspi.log||announce|order|loopback PASS|poking UNGRANTED
tests/integration/check_xmcspi.sh|xmcspi.log||killed|drop|=== THREAD FAULT ===|
tests/integration/check_xmcspi.sh|xmcspi.log||kill-address|swap|ADDR=0x50004648|ADDR=0x50004640
tests/integration/check_xmcspi.sh|xmcspi.log||panic|after|=== THREAD FAULT|KERNEL PANIC: planted
tests/integration/check_xmccshold.sh|xmccshold.log||error|swap|FEM=1: MSLS edges = 2 : PASS|FEM=1: MSLS edges = 8 : FAIL
tests/integration/check_xmccshold.sh|xmccshold.log||fault|after|FEM=0: MSLS edges|=== THREAD FAULT === thread 'xmccshold' killed, system continues
tests/integration/check_xmccshold.sh|xmccshold.log||fem1-run|drop|run 1 FEM=1|
tests/integration/check_xmccshold.sh|xmccshold.log||fem1-verdict|drop|FEM=1: MSLS edges|
tests/integration/check_xmccshold.sh|xmccshold.log||fem1-verdict|order|run 1 FEM=1|run 2 FEM=0
tests/integration/check_xmccshold.sh|xmccshold.log||fem0-run|drop|run 2 FEM=0|
tests/integration/check_xmccshold.sh|xmccshold.log||verdict|drop|VERDICT: hardware|
tests/integration/check_xmccshold.sh|xmccshold.log||panic|after|VERDICT: hardware|KERNEL PANIC: planted
tests/integration/check_pvprobe.sh|pvprobe.log||error|after|probe up (granted|[pvprobe] ERROR: planted
tests/integration/check_pvprobe.sh|pvprobe.log||probe-up|drop|probe up (granted|
tests/integration/check_pvprobe.sh|pvprobe.log||seam-b|drop|seam BRG: rc=0 wrote=0x2aa0000|
tests/integration/check_pvprobe.sh|pvprobe.log||pv-landed|swap|FDR[PV]: post=0x2aa DROPPED (post == pre)|FDR[PV]: post=0x155 LANDED (post == written)
tests/integration/check_pvprobe.sh|pvprobe.log||pv-landed|swap|BRG[PV]: post=0x2aa0000 DROPPED (post == pre)|BRG[PV]: post=0x1550000 LANDED (post == written)
tests/integration/check_pvprobe.sh|pvprobe.log||pv-landed|swap|CCR[PV]: post=0x4000 DROPPED (post == pre)|CCR[PV]: post=0xc001 LANDED (post == written)
tests/integration/check_pvprobe.sh|pvprobe.log||pv-dropped|drop|BRG[PV]: post=|
tests/integration/check_pvprobe.sh|pvprobe.log||sctr-landed|swap|post=0x7070101 LANDED (post == written)|post=0x3030100 DROPPED (post == pre)
tests/integration/check_pvprobe.sh|pvprobe.log||seam-a|swap|wrote=0x155 read=0x155 exact|wrote=0x155 read=0x154 DIFF (reserved/read-only bits)
tests/integration/check_pvprobe.sh|pvprobe.log||mask|swap|post=0xc001 unchanged|post=0xc000 CHANGED (value was not refused whole)
tests/integration/check_pvprobe.sh|pvprobe.log||mask|drop|mask refusal:|
tests/integration/check_pvprobe.sh|pvprobe.log||refusals|swap|off-allowlist rc=-22|off-allowlist rc=0
tests/integration/check_pvprobe.sh|pvprobe.log||refusals|swap|unheld-window rc=-1|unheld-window rc=0
tests/integration/check_pvprobe.sh|pvprobe.log||refusals|order|mask refusal:|refusals: off-allowlist
tests/integration/check_pvprobe.sh|pvprobe.log||announce|drop|poking UNGRANTED|
tests/integration/check_pvprobe.sh|pvprobe.log||killed|drop|=== THREAD FAULT ===|
tests/integration/check_pvprobe.sh|pvprobe.log||kill-address|swap|ADDR=0x50004648|ADDR=0x50004640
tests/integration/check_pvprobe.sh|pvprobe.log||kill-address|swap|ADDR=0x50004648|ADDR=0x50004640|after|poking UNGRANTED|  ADDR=0x50004648
tests/integration/check_pvprobe.sh|pvprobe.log||panic|after|ADDR=0x50004648|KERNEL PANIC: planted
tests/integration/check_consoledemo.sh|consoledemo.log||error|after|[worker] line 2|[consoledemo] ERROR: planted
tests/integration/check_consoledemo.sh|consoledemo.log||ctor-line|drop|pre-publish ctor line|
tests/integration/check_consoledemo.sh|consoledemo.log||driver-up|drop|[xmcuart] driver up|
tests/integration/check_consoledemo.sh|consoledemo.log||main-line|drop|post-publish line|
tests/integration/check_consoledemo.sh|consoledemo.log||main-line|order|[xmcuart] driver up|post-publish line
tests/integration/check_consoledemo.sh|consoledemo.log||worker-line|drop|[worker] line 3|
tests/integration/check_consoledemo.sh|consoledemo.log||worker-done|drop|[worker] done|
tests/integration/check_consoledemo.sh|consoledemo.log||panic|after|[worker] done|KERNEL PANIC: planted
tests/integration/check_conreclaim.sh|conreclaim.log||driver-up|drop|[testusic] driver up|
tests/integration/check_conreclaim.sh|conreclaim.log||writer|drop|writing through the test console driver|
tests/integration/check_conreclaim.sh|conreclaim.log||writer|order|[testusic] driver up|[conreclaim] writing through
tests/integration/check_conreclaim.sh|conreclaim.log||scramble|drop|scrambling the channel|
tests/integration/check_conreclaim.sh|conreclaim.log||verdict|drop|KERNEL PANIC|
tests/integration/check_conreclaim.sh|conreclaim.log||verdict|swap|KERNEL PANIC: [conreclaim] PASS|[conreclaim] PASS
tests/integration/check_conreclaim.sh|conreclaim.log||error|after|writing through the test console driver|[conreclaim] ERROR: the scramble request was answered -38, status 0
tests/integration/check_conreclaim.sh|conreclaim.log||error|after|[testusic] driver up|[testusic] ERROR: channel open refused
tests/integration/check_xmcssc.sh|xmcssc.log||bus-open|drop|bus open: PASS|
tests/integration/check_xmcssc.sh|xmcssc.log||error|swap|zero-tx loopback: PASS|zero-tx loopback: FAIL
tests/integration/check_xmcssc.sh|xmcssc.log||case|drop|multi-byte loopback: PASS|
tests/integration/check_xmcssc.sh|xmcssc.log||case|order|single-byte loopback: PASS|zero-tx loopback: PASS
tests/integration/check_xmcssc.sh|xmcssc.log||loopback|drop|loopback PASS (the SSC|
tests/integration/check_xmcssc.sh|xmcssc.log||panic|after|loopback PASS (the SSC|KERNEL PANIC: planted
tests/integration/check_inprstorm.sh|inprstorm.log||root-up|drop|MARKER: root up|
tests/integration/check_inprstorm.sh|inprstorm.log||reroute|drop|rerouting INPR|
tests/integration/check_inprstorm.sh|inprstorm.log||reroute|order|rerouting INPR|MARKER: root up
tests/integration/check_inprstorm.sh|inprstorm.log||no-fifo|after|rerouting INPR|[inprstorm] CCFG.TB=0: TX FIFO absent, vector unavailable
tests/integration/check_inprstorm.sh|inprstorm.log||heartbeat-1|drop|[inprstorm] heartbeat |
tests/integration/check_inprstorm.sh|inprstorm.log||heartbeat-2|drop|[inprstorm] heartbeat 7||drop|[inprstorm] heartbeat 8|
tests/integration/check_inprstorm.sh|inprstorm.log||panic|after|rerouting INPR|KERNEL PANIC: planted"

# <in> <op> <literal> <replacement> <out>: <in> with one edit, with CRLF line ends.
plant() {
    KOS_PLANT_LIT="$3" KOS_PLANT_NEW="$4" awk -v op="$2" '
        BEGIN { lit = ENVIRON["KOS_PLANT_LIT"]; rep = ENVIRON["KOS_PLANT_NEW"] }
        {
            sub(/\r/, "")
            n++
            line[n] = $0
            if (lit != "" && a == 0 && index($0, lit) > 0) { a = n }
            if (rep != "" && b == 0 && index($0, rep) > 0) { b = n }
        }
        END {
            if (op == "order" && a > 0 && b > 0) {
                t = line[a]
                line[a] = line[b]
                line[b] = t
            }
            for (i = 1; i <= n; i++) {
                at = 0
                if (lit != "") { at = index(line[i], lit) }
                if (op == "drop" && at > 0) { continue }
                if (op == "swap" && at > 0) {
                    line[i] = substr(line[i], 1, at - 1) rep substr(line[i], at + length(lit))
                }
                printf "%s\r\n", line[i]
                if (op == "after" && at > 0) { printf "%s\r\n", rep }
            }
        }' "$1" > "$5"
}

# <judge> <cache line> <capture>: the judge's exit status over the capture.
judged() {
    mkdir -p "$TMP/build"
    printf '%s\n' "$2" > "$TMP/build/CMakeCache.txt"
    KOS_CAPTURE="$3" sh "$1" "$TMP/build" "$PWD" cmake > "$TMP/judge.out" 2>&1
}

passed=""
while IFS='|' read -r judge fixture cache token op lit rep op2 lit2 rep2; do
    [ -n "$judge" ] || continue
    [ -f "$judge" ] || fail "no judge $judge"
    [ -f "tests/integration/app_captures/$fixture" ] || fail "no fixture $fixture"
    [ -n "$token" ] || fail "the row for $judge over $fixture names no token"
    plant "tests/integration/app_captures/$fixture" none "" "" "$TMP/good.log"
    case "$passed" in
        *"|$judge $fixture $cache|"*) ;;
        *)
            if ! judged "$judge" "$cache" "$TMP/good.log"; then
                bad "$judge refuses its fixture $fixture: $(tail -n 1 "$TMP/judge.out")"
            fi
            passed="$passed|$judge $fixture $cache|"
            ;;
    esac
    what="the $op of '$lit'"
    if [ "$op" = cache ]; then
        what="'$rep' for its cache"
        judged "$judge" "$rep" "$TMP/good.log"
        jrc=$?
    else
        plant "$TMP/good.log" "$op" "$lit" "$rep" "$TMP/bad.log"
        if [ -n "$op2" ]; then
            what="$what and the $op2 of '$lit2'"
            plant "$TMP/bad.log" "$op2" "$lit2" "$rep2" "$TMP/bad2.log"
            mv "$TMP/bad2.log" "$TMP/bad.log"
        fi
        if cmp -s "$TMP/good.log" "$TMP/bad.log"; then
            bad "$what leaves $fixture unchanged"
            continue
        fi
        judged "$judge" "$cache" "$TMP/bad.log"
        jrc=$?
    fi
    if [ "$jrc" -eq 0 ]; then
        bad "$judge passes $fixture with $what"
    elif ! grep -qF "FAIL: $token:" "$TMP/judge.out"; then
        bad "$judge refuses $fixture with $what, but not as $token: $(tail -n 1 "$TMP/judge.out")"
    fi
done <<ROWS
$ROWS
ROWS

NAMED="$(git ls-files user/apps | grep 'CMakeLists\.txt$' | while read -r f; do
    sed -n 's/^ *kickos_app_judge([^ ]* \([^)]*\)).*/\1/p' "$f"
done | sort -u)"
[ -n "$NAMED" ] || fail "no app CMake names a judge, so the coverage check below reads nothing"
for judge in $NAMED; do
    case "$passed" in
        *"|$judge "*) ;;
        *) bad "$judge judges a board app and no fixture row proves it" ;;
    esac
done

BOARD_APPS="$(git ls-files 'user/apps/*/*/CMakeLists.txt' | grep -v '^user/apps/common/')"
[ -n "$BOARD_APPS" ] || fail "git ls-files found no board app, so the judge check below reads nothing"
for f in $BOARD_APPS; do
    app="${f#user/apps/}"
    app="${app%/CMakeLists.txt}"
    if grep -q '^ *kickos_app_judge(' "$f"; then
        continue
    fi
    if printf '%s\n' "$WAIVED" | grep -qxF -- "$app"; then
        continue
    fi
    bad "$app names no judge (kickos_app_judge) and is not on WAIVED"
done

[ "$rc" -eq 0 ] || exit 1
echo "PASS: every board app names a judge that passes its fixture and refuses each damaged copy as its row names"
