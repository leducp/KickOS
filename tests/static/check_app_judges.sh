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

ROWS="tests/integration/check_f411spi.sh|f411spi.capture||word|drop|[f411spi] word 2:|
tests/integration/check_f411spi.sh|f411spi.capture||error|swap|rx=0x3c PASS|rx=0x3d FAIL
tests/integration/check_f411spi.sh|f411spi.capture||error|after|poking UNGRANTED|[f411spi] UNGRANTED ACCESS DID NOT FAULT
tests/integration/check_f411spi.sh|f411spi.capture||loopback-start|drop|starting loopback|
tests/integration/check_f411spi.sh|f411spi.capture||loopback|drop|loopback PASS|
tests/integration/check_f411spi.sh|f411spi.capture||announce|drop|poking UNGRANTED|
tests/integration/check_f411spi.sh|f411spi.capture||announce|order|loopback PASS|poking UNGRANTED
tests/integration/check_f411spi.sh|f411spi.capture||killed|drop|=== THREAD FAULT ===|
tests/integration/check_f411spi.sh|f411spi.capture||kill-address|swap|ADDR=0x40020400|ADDR=0x40020404
tests/integration/check_f411spi.sh|f411spi.capture||kill-address|swap|ADDR=0x40020400|ADDR=0x40020404|after|poking UNGRANTED|  ADDR=0x40020400
tests/integration/check_f411spi.sh|f411spi.capture||panic|swap|PC=0x080002f4|KERNEL PANIC: PC=0x080002f4
tests/integration/check_k64drv.sh|k64drv.capture||timer-start|drop|counting the 1 kHz LPO|
tests/integration/check_k64drv.sh|k64drv.capture||tick|drop|[k64drv] tick 7|
tests/integration/check_k64drv.sh|k64drv.capture||tick|swap|[k64drv] tick 2|[k64drv] tick 22
tests/integration/check_k64drv.sh|k64drv.capture||tick|order|[k64drv] tick 4|[k64drv] tick 5
tests/integration/check_k64drv.sh|k64drv.capture||slot-read|drop|a thread holding no window|
tests/integration/check_k64drv.sh|k64drv.capture||done|drop|[k64drv] done|
tests/integration/check_k64drv.sh|k64drv.capture||fault|after|[k64drv] tick 10|=== THREAD FAULT === thread 'k64read' killed, system continues
tests/integration/check_k64drv.sh|k64drv.capture||error|after|[k64drv] tick 3|[k64drv] ERROR: planted rc -1
tests/integration/check_k64drv.sh|k64drv.capture||panic|after|[k64drv] done|KERNEL PANIC: planted
tests/integration/check_k64console.sh|k64console.capture||ctor-line|drop|pre-publish ctor line|
tests/integration/check_k64console.sh|k64console.capture||driver-up|drop|[k64uart] driver up|
tests/integration/check_k64console.sh|k64console.capture||main-line|drop|post-publish line|
tests/integration/check_k64console.sh|k64console.capture||worker-line|drop|[worker] line 3|
tests/integration/check_k64console.sh|k64console.capture||worker-line|order|[worker] line 1|[worker] line 2
tests/integration/check_k64console.sh|k64console.capture||worker-done|drop|[worker] done|
tests/integration/check_k64console.sh|k64console.capture||error|after|[worker] line 1|[k64console] ERROR: planted
tests/integration/check_k64console.sh|k64console.capture||panic|after|[worker] done|KERNEL PANIC: planted
tests/integration/check_k64console.sh|k64console.capture||verdict|cache||K64CONSOLE_SCRAMBLE_TEST:BOOL=ON
tests/integration/check_k64console.sh|k64console-scramble.capture|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|verdict|drop|KERNEL PANIC|
tests/integration/check_k64console.sh|k64console-scramble.capture|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|driver-up|drop|[k64uart] driver up|
tests/integration/check_k64console.sh|k64console-scramble.capture|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|panic|cache||K64CONSOLE_SCRAMBLE_TEST:BOOL=OFF
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|driver-up|drop|SPI service up|
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|case|drop|zero-tx loopback: PASS|
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|error|swap|single-byte loopback: PASS|single-byte loopback: FAIL
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|case|order|single-byte loopback: PASS|zero-tx loopback: PASS
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|loopback|drop|loopback PASS (the SPI|
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|panic|after|loopback PASS (the SPI|KERNEL PANIC: planted
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|byte-test|cache||K64DSPI_LOOPBACK:BOOL=OFF
tests/integration/check_k64dspi.sh|k64dspi-lan9252.capture||byte-test|drop|BYTE_TEST PASS|
tests/integration/check_k64dspi.sh|k64dspi-lan9252.capture||device-open|drop|device open rc=|
tests/integration/check_k64dspi.sh|k64dspi-lan9252.capture||case|cache||K64DSPI_LOOPBACK:BOOL=ON
tests/integration/check_k64dspi.sh|k64dspi-lan9252.capture||posture|cache||KICKOS_SPI_LOCAL_ENGINE:BOOL=ON
tests/integration/check_rxdrv.sh|rxdrv.capture||mux|swap|general I/O rc 0|general I/O rc -16
tests/integration/check_rxdrv.sh|rxdrv.capture||console-pin|drop|refused (-KOS_EBUSY)|
tests/integration/check_rxdrv.sh|rxdrv.capture||holder|drop|PASS periph_enable holder|
tests/integration/check_rxdrv.sh|rxdrv.capture||error|swap|PASS periph_enable holder|FAIL periph_enable holder
tests/integration/check_rxdrv.sh|rxdrv.capture||blink-start|drop|blinking LED6|
tests/integration/check_rxdrv.sh|rxdrv.capture||blink|drop|PASS (pad tracked|
tests/integration/check_rxdrv.sh|rxdrv.capture||non-holder|drop|periph_enable non-holder|
tests/integration/check_rxdrv.sh|rxdrv.capture||non-holder|order|PASS (pad tracked|periph_enable non-holder
tests/integration/check_rxdrv.sh|rxdrv.capture||announce|drop|poking UNGRANTED|
tests/integration/check_rxdrv.sh|rxdrv.capture||killed|swap|thread 'rxpoke'|thread 'rxdrv'
tests/integration/check_rxdrv.sh|rxdrv.capture||kill-address|swap|ADDR=0x8c199|ADDR=0x8c068
tests/integration/check_rxdrv.sh|rxdrv.capture||error|after|poking UNGRANTED|[rxdrv] UNGRANTED ACCESS DID NOT FAULT (MPU not enforcing)
tests/integration/check_rxdrv.sh|rxdrv.capture||panic|after|ADDR=0x8c199|KERNEL PANIC: planted
tests/integration/check_c6blink.sh|c6blink.capture||holder|swap|holder rc -38 (want -38)|holder rc -1 (want -38)
tests/integration/check_c6blink.sh|c6blink.capture||blink-start|drop|blinking GPIO10|
tests/integration/check_c6blink.sh|c6blink.capture||blink|drop|PASS (pad tracked|
tests/integration/check_c6blink.sh|c6blink.capture||error|swap|[c6blink] blink 4 pad=1/1|[c6blink] FAIL blink 4 pad=1/0
tests/integration/check_c6blink.sh|c6blink.capture||non-holder|drop|periph_enable non-holder|
tests/integration/check_c6blink.sh|c6blink.capture||non-holder|order|PASS (pad tracked|periph_enable non-holder
tests/integration/check_c6blink.sh|c6blink.capture||announce|drop|poking UNGRANTED|
tests/integration/check_c6blink.sh|c6blink.capture||killed|swap|thread 'c6poke'|thread 'c6blink'
tests/integration/check_c6blink.sh|c6blink.capture||kill-address|swap|ADDR=0x6009157c|ADDR=0x60091040
tests/integration/check_c6blink.sh|c6blink.capture||panic|after|ADDR=0x6009157c|=== RISC-V TRAP ===
tests/integration/check_c6lpprobe.sh|c6lpprobe.capture||payload|drop|payload IRQ|
tests/integration/check_c6lpprobe.sh|c6lpprobe.capture||doorbells|drop|PASS bidirectional|
tests/integration/check_c6lpprobe.sh|c6lpprobe.capture||doorbells|order|payload IRQ|PASS bidirectional
tests/integration/check_c6lpprobe.sh|c6lpprobe.capture||timeout|after|LP raw 1|c6lpprobe: timeout marker 0 code 0 pwr1 0 mux 0 pwr0 0 local 0
tests/integration/check_c6lpprobe.sh|c6lpprobe.capture||panic|after|PASS bidirectional|KERNEL PANIC: planted
tests/integration/check_c6txidle.sh|c6txidle.capture||tail|swap|<<<TXIDLE-END>>>|<<<TXIDLE-END>>
tests/integration/check_c6txidle.sh|c6txidle.capture||tail|swap|<<<TXIDLE-END>>>|<<<TXIDLE-END>>~
tests/integration/check_c6txidle.sh|c6txidle.capture||tail|drop|[c6txidle] TAIL|
tests/integration/check_c6txidle.sh|c6txidle.capture||timing|drop|flush held|
tests/integration/check_c6txidle.sh|c6txidle.capture||timing|order|[c6txidle] TAIL|[c6txidle] frame
tests/integration/check_c6txidle.sh|c6txidle.capture||state|drop|ST_UTX_OUT 0|
tests/integration/check_c6txidle.sh|c6txidle.capture||verdict|drop|PASS the flush|
tests/integration/check_c6txidle.sh|c6txidle.capture||verdict|after|ST_UTX_OUT 0|[c6txidle] FAIL the flush returned before the last frame could finish
tests/integration/check_c6txidle.sh|c6txidle.capture||error|after|UART0 TX idle witness|[c6txidle] ERROR: the console FIFO never stayed empty
tests/integration/check_c6txidle.sh|c6txidle.capture||panic|after|PASS the flush|KERNEL PANIC: planted
tests/integration/check_xmcspi.sh|xmcspi.capture||error|swap|seam FDR: rc=0 wrote=0x80000167 read=0x80000167 LANDED|seam FDR: rc=0 wrote=0x80000167 read=0x0 DISCARDED/REFUSED
tests/integration/check_xmcspi.sh|xmcspi.capture||seam|drop|seam BRG:|
tests/integration/check_xmcspi.sh|xmcspi.capture||loopback-start|drop|starting SSC loopback|
tests/integration/check_xmcspi.sh|xmcspi.capture||word|drop|[xmcspi] word 2:|
tests/integration/check_xmcspi.sh|xmcspi.capture||error|swap|word 1: tx=0x3c rx=0x3c PASS|word 1: tx=0x3c rx=0x3d FAIL
tests/integration/check_xmcspi.sh|xmcspi.capture||loopback|drop|loopback PASS|
tests/integration/check_xmcspi.sh|xmcspi.capture||announce|drop|poking UNGRANTED|
tests/integration/check_xmcspi.sh|xmcspi.capture||announce|order|loopback PASS|poking UNGRANTED
tests/integration/check_xmcspi.sh|xmcspi.capture||killed|drop|=== THREAD FAULT ===|
tests/integration/check_xmcspi.sh|xmcspi.capture||kill-address|swap|ADDR=0x50004648|ADDR=0x50004640
tests/integration/check_xmcspi.sh|xmcspi.capture||panic|after|=== THREAD FAULT|KERNEL PANIC: planted
tests/integration/check_xmccshold.sh|xmccshold.capture||error|swap|FEM=1: MSLS edges = 2 : PASS|FEM=1: MSLS edges = 8 : FAIL
tests/integration/check_xmccshold.sh|xmccshold.capture||fault|after|FEM=0: MSLS edges|=== THREAD FAULT === thread 'xmccshold' killed, system continues
tests/integration/check_xmccshold.sh|xmccshold.capture||fem1-run|drop|run 1 FEM=1|
tests/integration/check_xmccshold.sh|xmccshold.capture||fem1-verdict|drop|FEM=1: MSLS edges|
tests/integration/check_xmccshold.sh|xmccshold.capture||fem1-verdict|order|run 1 FEM=1|run 2 FEM=0
tests/integration/check_xmccshold.sh|xmccshold.capture||fem0-run|drop|run 2 FEM=0|
tests/integration/check_xmccshold.sh|xmccshold.capture||verdict|drop|VERDICT: hardware|
tests/integration/check_xmccshold.sh|xmccshold.capture||panic|after|VERDICT: hardware|KERNEL PANIC: planted
tests/integration/check_pvprobe.sh|pvprobe.capture||error|after|probe up (granted|[pvprobe] ERROR: planted
tests/integration/check_pvprobe.sh|pvprobe.capture||probe-up|drop|probe up (granted|
tests/integration/check_pvprobe.sh|pvprobe.capture||seam-b|drop|seam BRG: rc=0 wrote=0x2aa0000|
tests/integration/check_pvprobe.sh|pvprobe.capture||pv-landed|swap|FDR[PV]: post=0x2aa DROPPED (post == pre)|FDR[PV]: post=0x155 LANDED (post == written)
tests/integration/check_pvprobe.sh|pvprobe.capture||pv-landed|swap|BRG[PV]: post=0x2aa0000 DROPPED (post == pre)|BRG[PV]: post=0x1550000 LANDED (post == written)
tests/integration/check_pvprobe.sh|pvprobe.capture||pv-landed|swap|CCR[PV]: post=0x4000 DROPPED (post == pre)|CCR[PV]: post=0xc001 LANDED (post == written)
tests/integration/check_pvprobe.sh|pvprobe.capture||pv-dropped|drop|BRG[PV]: post=|
tests/integration/check_pvprobe.sh|pvprobe.capture||sctr-landed|swap|post=0x7070101 LANDED (post == written)|post=0x3030100 DROPPED (post == pre)
tests/integration/check_pvprobe.sh|pvprobe.capture||seam-a|swap|wrote=0x155 read=0x155 exact|wrote=0x155 read=0x154 DIFF (reserved/read-only bits)
tests/integration/check_pvprobe.sh|pvprobe.capture||mask|swap|post=0xc001 unchanged|post=0xc000 CHANGED (value was not refused whole)
tests/integration/check_pvprobe.sh|pvprobe.capture||mask|drop|mask refusal:|
tests/integration/check_pvprobe.sh|pvprobe.capture||refusals|swap|off-allowlist rc=-22|off-allowlist rc=0
tests/integration/check_pvprobe.sh|pvprobe.capture||refusals|swap|unheld-window rc=-1|unheld-window rc=0
tests/integration/check_pvprobe.sh|pvprobe.capture||refusals|order|mask refusal:|refusals: off-allowlist
tests/integration/check_pvprobe.sh|pvprobe.capture||announce|drop|poking UNGRANTED|
tests/integration/check_pvprobe.sh|pvprobe.capture||killed|drop|=== THREAD FAULT ===|
tests/integration/check_pvprobe.sh|pvprobe.capture||kill-address|swap|ADDR=0x50004648|ADDR=0x50004640
tests/integration/check_pvprobe.sh|pvprobe.capture||kill-address|swap|ADDR=0x50004648|ADDR=0x50004640|after|poking UNGRANTED|  ADDR=0x50004648
tests/integration/check_pvprobe.sh|pvprobe.capture||panic|after|ADDR=0x50004648|KERNEL PANIC: planted
tests/integration/check_consoledemo.sh|consoledemo.capture||error|after|[worker] line 2|[consoledemo] ERROR: planted
tests/integration/check_consoledemo.sh|consoledemo.capture||ctor-line|drop|pre-publish ctor line|
tests/integration/check_consoledemo.sh|consoledemo.capture||driver-up|drop|[xmcuart] driver up|
tests/integration/check_consoledemo.sh|consoledemo.capture||main-line|drop|post-publish line|
tests/integration/check_consoledemo.sh|consoledemo.capture||main-line|order|[xmcuart] driver up|post-publish line
tests/integration/check_consoledemo.sh|consoledemo.capture||worker-line|drop|[worker] line 3|
tests/integration/check_consoledemo.sh|consoledemo.capture||worker-done|drop|[worker] done|
tests/integration/check_consoledemo.sh|consoledemo.capture||panic|after|[worker] done|KERNEL PANIC: planted
tests/integration/check_conreclaim.sh|conreclaim.capture||driver-up|drop|[testusic] driver up|
tests/integration/check_conreclaim.sh|conreclaim.capture||writer|drop|writing through the test console driver|
tests/integration/check_conreclaim.sh|conreclaim.capture||writer|order|[testusic] driver up|[conreclaim] writing through
tests/integration/check_conreclaim.sh|conreclaim.capture||scramble|drop|scrambling the channel|
tests/integration/check_conreclaim.sh|conreclaim.capture||verdict|drop|KERNEL PANIC|
tests/integration/check_conreclaim.sh|conreclaim.capture||verdict|swap|KERNEL PANIC: [conreclaim] PASS|[conreclaim] PASS
tests/integration/check_conreclaim.sh|conreclaim.capture||error|after|writing through the test console driver|[conreclaim] ERROR: the scramble request was answered -38, status 0
tests/integration/check_conreclaim.sh|conreclaim.capture||error|after|[testusic] driver up|[testusic] ERROR: channel open refused
tests/integration/check_xmcssc.sh|xmcssc.capture||bus-open|drop|bus open: PASS|
tests/integration/check_xmcssc.sh|xmcssc.capture||error|swap|zero-tx loopback: PASS|zero-tx loopback: FAIL
tests/integration/check_xmcssc.sh|xmcssc.capture||case|drop|multi-byte loopback: PASS|
tests/integration/check_xmcssc.sh|xmcssc.capture||case|order|single-byte loopback: PASS|zero-tx loopback: PASS
tests/integration/check_xmcssc.sh|xmcssc.capture||loopback|drop|loopback PASS (the SSC|
tests/integration/check_xmcssc.sh|xmcssc.capture||panic|after|loopback PASS (the SSC|KERNEL PANIC: planted
tests/integration/check_inprstorm.sh|inprstorm.capture||root-up|drop|MARKER: root up|
tests/integration/check_inprstorm.sh|inprstorm.capture||reroute|drop|rerouting INPR|
tests/integration/check_inprstorm.sh|inprstorm.capture||reroute|order|rerouting INPR|MARKER: root up
tests/integration/check_inprstorm.sh|inprstorm.capture||no-fifo|after|rerouting INPR|[inprstorm] CCFG.TB=0: TX FIFO absent, vector unavailable
tests/integration/check_inprstorm.sh|inprstorm.capture||heartbeat-1|drop|[inprstorm] heartbeat |
tests/integration/check_inprstorm.sh|inprstorm.capture||heartbeat-2|drop|[inprstorm] heartbeat 7||drop|[inprstorm] heartbeat 8|
tests/integration/check_inprstorm.sh|inprstorm.capture||panic|after|rerouting INPR|KERNEL PANIC: planted
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||load|drop|load:0x4083c000|
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||lp-rtc|drop|# c6amp: LP RTC|
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||vectors|swap|vectors=0x4083c001|vectors=0x4083c000
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||alive|swap|2 of 2 node app(s)|1 of 2 node app(s)
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||gate|swap|and 0x0 from node 0's timg0|and 0x5a3c from node 0's timg0
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||call|drop|ampping: node 0 calls node 1 port 3|
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||round|drop|  ping 3 -> pong 4|
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||attempt|after|  ping 4 -> pong 5|  (node 1 answered on attempt 2)
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||served|swap|its own record says 4|its own record says 3
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||done|drop|ampping: node 0 done|
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||doorbell|drop|the doorbell has no seat|
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||bells|swap|core=1 bells=6 drains=6|core=1 bells=6 drains=0
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||panic|after|ampping: node 0 done|KERNEL PANIC: planted
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||panic|after|ampping: node 0 done|=== RISC-V TRAP ===
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||fault|after|ampping: node 0 done|=== THREAD FAULT === thread 'ampping' killed, system continues
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||readback|drop|# accessctrl: 0xa0|
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||readback|swap|# accessctrl: 0xa0 = 0x9c|# accessctrl: 0xa0 = 0xbc
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||banners|after|# accessctrl: 0xa0|   KickOS 0.5.1  -  microkernel RTOS
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||gate|drop|ampping: gate: node 0 read 0x11|
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||probe|swap|fault CFSR=0x8200 BFAR=0x40070fe0|0x0
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||probe|swap|CFSR=0x8200|CFSR=0x8282
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||probe|swap|BFAR=0x40070fe0|BFAR=0x40070fe4
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||fault|after|ampping: node 0 done|=== THREAD FAULT === thread 'ampping' killed, system continues
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||alive|swap|2 of 2 node app(s)|1 of 2 node app(s)
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||call|drop|ampping: node 0 calls node 1 port 3|
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||round|drop|  ping 2 -> pong 3|
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||served|swap|its own record says 4|its own record says 3
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||done|drop|ampping: node 0 done|
tests/integration/check_pizero_amp_gate.sh|ampping-pizero.capture||panic|after|ampping: node 0 done|KERNEL PANIC: planted
tests/integration/check_system_default.sh|sysdefault.capture||line|drop|sysdefault: main returns 3|
tests/integration/check_system_default.sh|sysdefault.capture||line|swap|main returns 3|main returns 4
tests/integration/check_system_default.sh|sysdefault.capture||panic|after|sysdefault: main returns 3|KERNEL PANIC: planted
tests/integration/check_system_default.sh|sysdefault.capture||fault|after|sysdefault: main returns 3|=== THREAD FAULT === thread 'main' killed, system continues"

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
