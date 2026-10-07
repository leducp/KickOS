#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every capture judge (kickos_app_judge) against PLANTED captures: the fixtures in
# tests/integration/app_captures are written by hand from each app's own print statements, with
# CRLF line ends as the bench records them, and none of them is a silicon recording. A judge
# passes its fixture and refuses each damaged copy the table below names, with the token the row
# names, so no check it holds is vacuous and each refusal is the clause the row aims at. A row is
#   <judge>|<fixture>|<CMakeCache line or empty>|<token>|<op>|<literal>|<replacement>[|<op>|<literal>|<replacement>]
# where op `drop` removes every line carrying the literal, `swap` replaces its first occurrence
# on each line, `after` adds the replacement as a line after each line carrying it, `order`
# exchanges the first line carrying the literal with the first line carrying the replacement,
# `cache` judges the fixture whole with the replacement as the cache line instead, and
# `source:<path>` judges it whole over a source tree holding only <path>, the replacement added
# as a line after each line of it carrying the literal. An edit
# applies to the fixture and its arrival stamps alike; `log-<op>` edits the fixture alone and
# `times-<op>` the stamps alone. A second edit applies to the first's result. A judge refuses as `FAIL: <token>: ...` (gate.sh jfail).
# <judge> is the script, then `;<arg>` for each argument its kickos_app_judge ARGS pass it, and
# `;@<fitting>` for each bench fitting the rig declares on the board; none declares none.
# Every judge an app CMake or a gate fragment names must have a passing row, and every app a
# board directory builds names a judge, is human-judged or sits on WAIVED. With --listing, every
# (judge, arguments) pair one build's image listing names must have a row.
# A passing judge names each verdict clause a capture does not carry as `NOT EVALUATED: <clause>`,
# and each (judge, arguments) pair's fixture must print exactly the clauses OWED declares for it.
# A judge reads an exit status only through status_clause (tests/lib/gate.sh), which prints that
# line over a capture, so a pair whose path reads one owes it and must have its OWED row.

set -u
. "$(dirname "$0")/../lib/gate.sh"

rc=0

# <board>/<app> per line: a board app judged by no capture.
WAIVED=""

ROWS="tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||word|drop|[f411spi] word 2:|
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||error|swap|rx=0x3c PASS|rx=0x3d FAIL
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||error|after|poking UNGRANTED|[f411spi] UNGRANTED ACCESS DID NOT FAULT
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||loopback-start|drop|starting loopback|
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||loopback|drop|loopback PASS|
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||announce|drop|poking UNGRANTED|
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||announce|order|loopback PASS|poking UNGRANTED
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||killed|drop|=== THREAD FAULT ===|
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||kill-address|swap|ADDR=0x40020400|ADDR=0x40020404
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||kill-address|swap|ADDR=0x40020400|ADDR=0x40020404|after|poking UNGRANTED|  ADDR=0x40020400
tests/integration/check_f411spi.sh;@spi1-loopback|f411spi.capture||panic|swap|PC=0x080002f4|KERNEL PANIC: PC=0x080002f4
tests/integration/check_f411spi.sh|f411spi.capture||announce|drop|poking UNGRANTED|
tests/integration/check_f411spi.sh|f411spi-unwired.capture||word|drop|[f411spi] word 2:|
tests/integration/check_f411spi.sh|f411spi-unwired.capture||word|order|[f411spi] word 1:|[f411spi] word 3:
tests/integration/check_f411spi.sh|f411spi-unwired.capture||loopback-start|drop|starting loopback|
tests/integration/check_f411spi.sh|f411spi-unwired.capture||loopback|drop|loopback FAIL|
tests/integration/check_f411spi.sh|f411spi-unwired.capture||error|after|poking UNGRANTED|[f411spi] UNGRANTED ACCESS DID NOT FAULT
tests/integration/check_f411spi.sh|f411spi-unwired.capture||error|after|[f411spi] word 0:|[f411spi] ERROR: planted rc -1
tests/integration/check_f411spi.sh|f411spi-unwired.capture||error|after|[f411spi] word 1:|[f411spi] TXE timeout on word 2
tests/integration/check_f411spi.sh|f411spi-unwired.capture||announce|drop|poking UNGRANTED|
tests/integration/check_f411spi.sh|f411spi-unwired.capture||announce|order|loopback FAIL|poking UNGRANTED
tests/integration/check_f411spi.sh|f411spi-unwired.capture||killed|drop|=== THREAD FAULT ===|
tests/integration/check_f411spi.sh|f411spi-unwired.capture||kill-address|swap|ADDR=0x40020400|ADDR=0x40020404
tests/integration/check_f411spi.sh|f411spi-unwired.capture||panic|swap|PC=0x080002f4|KERNEL PANIC: PC=0x080002f4
tests/integration/check_k64drv.sh|k64drv.capture||timer-start|drop|counting the 1 kHz LPO|
tests/integration/check_wallclock.sh|wallclock.capture||mark-1|drop|[wallclock] mark 1|
tests/integration/check_wallclock.sh|wallclock.capture||mark-2|drop|[wallclock] mark 2|
tests/integration/check_wallclock.sh|wallclock.capture||mark-2|order|[wallclock] mark 1|[wallclock] mark 2
tests/integration/check_wallclock.sh|wallclock.capture||done|drop|[wallclock] done|
tests/integration/check_wallclock.sh|wallclock.capture||panic|after|[wallclock] mark 2|KERNEL PANIC: planted
tests/integration/check_wallclock.sh|wallclock.capture||declared|swap|sleeping 5000000000 ns|sleeping 5000000001 ns
tests/integration/check_wallclock.sh|wallclock.capture||declared|swap|mark 0, sleeping 5000000000|mark 0, sleeping 4000000000
tests/integration/check_wallclock.sh|wallclock.capture||sleep-short|swap|advanced 5000025108 ns|advanced 4999025108 ns
tests/integration/check_wallclock.sh|wallclock.capture||kernel-long|swap|advanced 5000025108 ns|advanced 5060000000 ns|swap|11.222058|11.264733
tests/integration/check_wallclock.sh|wallclock.capture||kernel-host|swap|advanced 5000025108 ns|advanced 5040000000 ns|swap|11.222058|11.179733
tests/integration/check_wallclock.sh|wallclock.capture||interval|source:user/apps/common/wallclock/main.cc|uint64_t const from = kos::clock_now();|        kos::print(planted);
tests/integration/check_wallclock.sh|wallclock.capture||host-time|swap|11.222058|26.222058
tests/integration/check_wallclock.sh|wallclock.capture||host-time|swap|11.222058|8.722058
tests/integration/check_wallclock.sh|wallclock.capture||host-time|swap|11.222058|11.272058
tests/integration/check_wallclock.sh|wallclock.capture||no-times|times-drop|.||times-drop|#|
tests/integration/check_wallclock.sh|wallclock.capture||stamps|times-swap|advanced 5000025108 ns|advanced 5000025109 ns
tests/integration/check_wallclock.sh|wallclock.capture||stamps|times-drop|[wallclock] mark 0|
tests/integration/check_wallclock.sh|wallclock.capture||stamps|log-drop|[wallclock] mark 0|
tests/integration/check_wallclock.sh|wallclock-nomark0.capture||mark-2|drop|[wallclock] mark 2|
tests/integration/check_wallclock.sh|wallclock-usb.capture||no-banner|drop|   KickOS 0.5.1  -  microkernel RTOS|
tests/integration/check_wallclock.sh|wallclock-usb.capture||mark-2|drop|[wallclock] mark 2|
tests/integration/check_wallclock.sh|wallclock-nomark0.capture||host-time|swap|11.222058|11.272058
tests/integration/check_k64drv.sh|k64drv.capture||tick|drop|[k64drv] tick 7|
tests/integration/check_k64drv.sh|k64drv.capture||tick|swap|[k64drv] tick 2|[k64drv] tick 22
tests/integration/check_k64drv.sh|k64drv.capture||tick|order|[k64drv] tick 4|[k64drv] tick 5
tests/integration/check_k64drv.sh|k64drv.capture||slot-read|drop|a thread holding no window|
tests/integration/check_k64drv.sh|k64drv.capture||done|drop|[k64drv] done|
tests/integration/check_k64drv.sh|k64drv.capture||fault|after|[k64drv] tick 10|=== THREAD FAULT === thread 'k64read' killed, system continues
tests/integration/check_k64drv.sh|k64drv.capture||error|after|[k64drv] tick 3|[k64drv] ERROR: planted rc -1
tests/integration/check_k64drv.sh|k64drv.capture||panic|after|[k64drv] done|KERNEL PANIC: planted
tests/integration/check_k64console.sh|k64console.capture||ctor-line|drop|pre-publish ctor line|
tests/integration/check_k64console.sh|k64console.capture||driver-up|drop|[k64uartirq] device up|
tests/integration/check_k64console.sh|k64console.capture||main-line|drop|post-publish line|
tests/integration/check_k64console.sh|k64console.capture||worker-line|drop|[worker] line 3|
tests/integration/check_k64console.sh|k64console.capture||worker-line|order|[worker] line 1|[worker] line 2
tests/integration/check_k64console.sh|k64console.capture||worker-done|drop|[worker] done|
tests/integration/check_k64console.sh|k64console.capture||error|after|[worker] line 1|[k64console] ERROR: planted
tests/integration/check_k64console.sh|k64console.capture||panic|after|[worker] done|KERNEL PANIC: planted
tests/integration/check_k64console.sh|k64console.capture||verdict|cache||K64CONSOLE_SCRAMBLE_TEST:BOOL=ON
tests/integration/check_k64console.sh|k64console-scramble.capture|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|verdict|drop|KERNEL PANIC|
tests/integration/check_k64console.sh|k64console-scramble.capture|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|driver-up|drop|[k64uartirq] device up|
tests/integration/check_k64console.sh|k64console-scramble.capture|K64CONSOLE_SCRAMBLE_TEST:BOOL=ON|panic|cache||K64CONSOLE_SCRAMBLE_TEST:BOOL=OFF
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|driver-up|drop|SPI service up|
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|case|drop|zero-tx loopback: PASS|
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|error|swap|single-byte loopback: PASS|single-byte loopback: FAIL (rc=-5)
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|case|swap|single-byte loopback: PASS|single-byte loopback: MISMATCH
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|case|order|single-byte loopback: PASS|zero-tx loopback: PASS
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|loopback|drop|loopback PASS (the SPI|
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|panic|after|loopback PASS (the SPI|KERNEL PANIC: planted
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|byte-test|cache||K64DSPI_LOOPBACK:BOOL=OFF
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||byte-test|drop|BYTE_TEST PASS|
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||device-open|drop|device open rc=|
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||case|cache||K64DSPI_LOOPBACK:BOOL=ON
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||posture|cache||KICKOS_SPI_LOCAL_ENGINE:BOOL=ON
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||error|swap|BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)|BYTE_TEST FAIL: a transfer failed
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||byte-test|swap|BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)|BYTE_TEST MISMATCH: no valid signature
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||xfer|swap|0x87654321 (xfer OK)|(xfer ERR rc=-5)
tests/integration/check_k64dspi.sh|k64dspi-lan9252.capture||device-open|drop|device open rc=|
tests/integration/check_k64dspi.sh|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|driver-up|drop|SPI service up|
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||byte-test|drop|BYTE_TEST attempt 1:|
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||byte-test|drop|BYTE_TEST MISMATCH|
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||byte-test|order|BYTE_TEST attempt 1:|BYTE_TEST MISMATCH
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||device-open|drop|device open rc=|
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||driver-up|drop|SPI service up|
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||error|after|BYTE_TEST attempt 2:|[k64dspi] ERROR: planted
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||panic|after|BYTE_TEST MISMATCH|KERNEL PANIC: planted
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||xfer|swap|0x0 (xfer OK)|(xfer ERR rc=-5)
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||error|swap|0x0 (xfer OK)|(xfer ERR rc=-5)|swap|BYTE_TEST MISMATCH: no valid signature; check CS (D9/PTC4), baud/mode, or shield seating|BYTE_TEST FAIL: a transfer failed
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||error|swap|BYTE_TEST MISMATCH|BYTE_TEST FAIL
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||device-open|swap|rc=0 achieved=10000000 Hz|rc=-16 achieved=0 Hz
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||posture|cache||KICKOS_SPI_LOCAL_ENGINE:BOOL=ON
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|case|drop|zero-tx loopback: MISMATCH|
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|case|drop|device open: PASS|
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|error|swap|device open: PASS|device open: FAIL (rc=-16)
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|device-open|swap|rc=0 achieved=1000000 Hz|rc=-16 achieved=0 Hz
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|error|swap|single-byte loopback: MISMATCH|single-byte loopback: FAIL (rc=-5)
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|error|swap|loopback: MISMATCH|loopback: FAIL (rc=-5)|swap|loopback MISMATCH (every transfer completed, rx != tx)|loopback FAIL (see per-case lines above)
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|loopback|drop|loopback MISMATCH (every|
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|error|after|zero-tx loopback: MISMATCH|[k64dspi] ERROR: planted
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|panic|after|loopback MISMATCH (every|KERNEL PANIC: planted
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|verdict|swap|: MISMATCH|: PASS
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|verdict|swap|loopback MISMATCH (every transfer completed, rx != tx)|loopback PASS (the SPI bus echoes tx == rx)
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|verdict|swap|zero-tx loopback: MISMATCH|zero-tx loopback: PASS|after|loopback MISMATCH (every|[k64dspi] loopback PASS (the SPI bus echoes tx == rx)
tests/integration/check_k64dspi.sh|k64dspi-loopback-unwired.capture|K64DSPI_LOOPBACK:BOOL=ON|case|after|single-byte loopback: MISMATCH|[k64dspi] single-byte loopback: PASS
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|verdict|after|loopback PASS (the SPI|[k64dspi] loopback MISMATCH (every transfer completed, rx != tx)
tests/integration/check_k64dspi.sh;@dspi0-loopback|k64dspi-loopback.capture|K64DSPI_LOOPBACK:BOOL=ON|case|after|zero-tx loopback: PASS|[k64dspi] zero-tx loopback: MISMATCH
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||verdict|drop|BYTE_TEST attempt 8:|
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||verdict|swap|attempt 3: 0x0 (xfer OK)|attempt 3: 0x87654321 (xfer OK)
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||verdict|swap|BYTE_TEST MISMATCH: no valid signature; check CS (D9/PTC4), baud/mode, or shield seating|BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||verdict|swap|attempt 8: 0x0 (xfer OK)|attempt 8: 0x87654321 (xfer OK)
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||verdict|order|BYTE_TEST attempt 1:|BYTE_TEST attempt 2:
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||verdict|after|BYTE_TEST attempt 8:|[k64dspi] BYTE_TEST attempt 9: 0x0 (xfer OK)
tests/integration/check_k64dspi.sh|k64dspi-lan9252-unwired.capture||verdict|after|BYTE_TEST MISMATCH|[k64dspi] LAN9252 BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||verdict|after|BYTE_TEST attempt 1:|[k64dspi] BYTE_TEST attempt 2: 0x0 (xfer OK)
tests/integration/check_k64dspi.sh;@lan9252|k64dspi-lan9252.capture||verdict|swap|attempt 1: 0x87654321 (xfer OK)|attempt 1: 0x0 (xfer OK)
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
tests/integration/check_c6txidle.sh|c6txidle.capture||state|drop|ST_UTX_OUT 2|
tests/integration/check_c6txidle.sh|c6txidle.capture||verdict|drop|PASS the flush|
tests/integration/check_c6txidle.sh|c6txidle.capture||verdict|after|ST_UTX_OUT 2|[c6txidle] FAIL the flush returned before the last frame could finish
tests/integration/check_c6txidle.sh|c6txidle.capture||error|after|[c6txidle] TAIL|[c6txidle] ERROR: the console FIFO never stayed empty
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
tests/integration/check_consoledemo.sh|consoledemo.capture||driver-up|drop|[xmcuartirq] device up|
tests/integration/check_consoledemo.sh|consoledemo.capture||main-line|drop|post-publish line|
tests/integration/check_consoledemo.sh|consoledemo.capture||main-line|order|[xmcuartirq] device up|post-publish line
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
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||vectors|swap|self clk=40000000|self clk=120000000
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||vectors|swap|self clk=40000000|self clk=160000000
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||vectors|swap|self clk=40000000|self clk=80000000
tests/integration/check_c6_amp_capture.sh|ampping-c6.capture||vectors|swap|self clk=40000000|self clk=312500
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
tests/integration/check_system_default.sh;sysdefault: main returns 3;3|sysdefault.capture||line|drop|sysdefault: main returns 3|
tests/integration/check_system_default.sh;sysdefault: main returns 3;3|sysdefault.capture||line|swap|main returns 3|main returns 4
tests/integration/check_system_default.sh;sysdefault: main returns 3;3|sysdefault.capture||panic|after|sysdefault: main returns 3|KERNEL PANIC: planted
tests/integration/check_system_default.sh;sysdefault: main returns 3;3|sysdefault.capture||fault|after|sysdefault: main returns 3|=== THREAD FAULT === thread 'main' killed, system continues
tests/integration/check_qemu_hello.sh|hello.capture||rounds|drop|pong 3|
tests/integration/check_qemu_hello.sh|hello.capture||rounds|drop|ping 3|
tests/integration/check_qemu_hello.sh|hello.capture||cpu|swap|cpu     rxv3|cpu     unknown
tests/integration/check_qemu_hello.sh|hello.capture||cpu|drop|   cpu |
tests/integration/check_qemu_hello.sh|hello.capture||panic|after|pong 4|KERNEL PANIC: planted
tests/integration/check_stress.sh|stress.capture||start|drop|stress: scheduler|
tests/integration/check_stress.sh|stress.capture||skip|swap|STRESS PASS|STRESS SKIP (board thread/sem pool too small)
tests/integration/check_stress.sh|stress.capture||error|after|stress: runnable|STRESS FAIL
tests/integration/check_stress.sh|stress.capture||verdict|drop|STRESS PASS|
tests/integration/check_stress.sh|stress.capture||panic|after|STRESS PASS|KERNEL PANIC: planted
tests/integration/check_qemu_tlsprobe.sh|tlsprobe.capture||error|after|[tlsprobe] w1 |[tlsprobe] FAIL w1 read its peer's value
tests/integration/check_qemu_tlsprobe.sh|tlsprobe.capture||verdict|drop|[tlsprobe] PASS|
tests/integration/check_qemu_tlsprobe.sh|tlsprobe.capture||panic|after|[tlsprobe] PASS|KERNEL PANIC: planted
tests/integration/check_qemu_cxxtest.sh|cxxtest.capture||error|swap|PASS: typeid|FAIL: typeid
tests/integration/check_qemu_cxxtest.sh|cxxtest.capture||verdict|drop|ALL PASS|
tests/integration/check_qemu_cxxtest.sh|cxxtest.capture||panic|after|ALL PASS|KERNEL PANIC: planted
tests/integration/check_qemu_cxxterm.sh|cxxterm.capture||throw|drop|cxxterm: throwing|
tests/integration/check_qemu_cxxterm.sh|cxxterm.capture||returned|after|cxxterm: throwing|cxxterm: returned
tests/integration/check_qemu_cxxterm.sh|cxxterm.capture||terminate|swap|[St13runtime_error]|[St9exception]
tests/integration/check_qemu_cxxterm.sh|cxxterm.capture||terminate|drop|terminate: uncaught|
tests/integration/check_qemu_cxxterm.sh|cxxterm.capture||panic|after|terminate: uncaught|KERNEL PANIC: planted
tests/integration/check_libc_exit.sh;--atexit|libc_exit.capture||spawn|after|regression|worker spawn refused
tests/integration/check_libc_exit.sh;--atexit|libc_exit.capture||worker|drop|worker: exit()|
tests/integration/check_libc_exit.sh;--atexit|libc_exit.capture||survived|drop|main: survived|
tests/integration/check_libc_exit.sh;--atexit|libc_exit.capture||main-exit|drop|main: exit()|
tests/integration/check_libc_exit.sh;--atexit|libc_exit.capture||atexit|drop|main: atexit handler|
tests/integration/check_libc_exit.sh;--atexit|libc_exit.capture||atexit|order|main: exit()|main: atexit handler
tests/integration/check_libc_exit.sh;--atexit|libc_exit.capture||panic|after|main: atexit handler|KERNEL PANIC: planted
tests/integration/check_slaypeer.sh|slaypeer.capture||setup|after|slay rc: 0|[slaypeer] ERROR: planted
tests/integration/check_slaypeer.sh|slaypeer.capture||kept|after|slay rc: 0|[slaypeer] SLAYPEER FAIL: planted
tests/integration/check_slaypeer.sh|slaypeer.capture||rc|swap|slay rc: 0|slay rc: -110
tests/integration/check_slaypeer.sh|slaypeer.capture||pass|drop|SLAYPEER PASS|
tests/integration/check_slaypeer.sh|slaypeer.capture||panic|after|SLAYPEER PASS|KERNEL PANIC: planted
tests/integration/check_sched_exit.sh|sched_exit.capture||worker|drop|worker: running|
tests/integration/check_sched_exit.sh|sched_exit.capture||worker-exit|drop|worker: exiting|
tests/integration/check_sched_exit.sh|sched_exit.capture||survived|drop|main: survived|
tests/integration/check_sched_exit.sh|sched_exit.capture||child|after|main: survived|parked spawn refused
tests/integration/check_sched_exit.sh|sched_exit.capture||main-exit|drop|main: exiting with|
tests/integration/check_sched_exit.sh|sched_exit.capture||panic|after|main: exiting with|KERNEL PANIC: planted
tests/integration/check_fpclass.sh|fpclass.capture||arm|drop|[fpclass] order 1,2 |
tests/integration/check_fpclass.sh|fpclass.capture||arm|swap|[0] want [0] ok|[1] want [0] BAD
tests/integration/check_fpclass.sh|fpclass.capture||verdict|drop|[fpclass] PASS|
tests/integration/check_fpclass.sh|fpclass.capture||error|swap|[fpclass] PASS|[fpclass] FAIL
tests/integration/check_fpclass.sh|fpclass.capture||panic|after|[fpclass] PASS|KERNEL PANIC: planted
tests/integration/check_app_arms.sh;rootauth;5|rootauth.capture||error|after|[rootauth] ok - declared KOS_AUTH_MEMORY|[rootauth] ERROR: planted
tests/integration/check_app_arms.sh;rootauth;5|rootauth.capture||arms|drop|[rootauth] ok - main narrowed|
tests/integration/check_app_arms.sh;rootauth;5|rootauth.capture||verdict|swap|PASS (5 arms)|PASS (4 arms)
tests/integration/check_app_arms.sh;rootauth;5|rootauth.capture||verdict|drop|[rootauth] PASS|
tests/integration/check_app_arms.sh;rootauth;5|rootauth.capture||panic|after|[rootauth] PASS|KERNEL PANIC: planted
tests/integration/check_app_arms.sh;objbudget;5;endpoint_create at the ceiling rc=-12|objbudget.capture||absent|swap|at the ceiling rc=-11|at the ceiling rc=-12
tests/integration/check_app_arms.sh;objbudget;5;endpoint_create at the ceiling rc=-12|objbudget.capture||arms|after|[objbudget] ok - and the ceiling|[objbudget] ok - planted extra arm
tests/integration/check_qemu_fp.sh|fp_switch.capture||error|after|FP OK: 10 rounds|  FP FAIL: s20 = 7, expected 20 (round 11)
tests/integration/check_qemu_fp.sh|fp_switch.capture||verdict|drop|FP OK:|
tests/integration/check_qemu_fp.sh|fp_switch.capture||panic|after|FP OK: 20 rounds|KERNEL PANIC: planted
tests/integration/check_trapnest.sh;480|trapnest.capture||error|after|worker parks sp|[trapnest] ERROR: ticker spawn refused
tests/integration/check_trapnest.sh;480|trapnest.capture||worker|drop|[trapnest] worker done|
tests/integration/check_trapnest.sh;480|trapnest.capture||join|drop|main ran after the worker|
tests/integration/check_trapnest.sh;480|trapnest.capture||tally|drop|[nestwitness] traps=|
tests/integration/check_trapnest.sh;480|trapnest.capture||traps|swap|traps=64|traps=0
tests/integration/check_trapnest.sh;480|trapnest.capture||onstack|swap|onstack=0|onstack=3
tests/integration/check_trapnest.sh;480|trapnest.capture||panic|after|main ran after the worker|KERNEL PANIC: planted
tests/integration/check_gpioblink.sh|gpioblink.capture||start|drop|driving port|
tests/integration/check_gpioblink.sh|gpioblink.capture||error|after|driving port|[gpioblink] ERROR: pinmux rc -1, window /dev/port/5
tests/integration/check_gpioblink.sh|gpioblink.capture||cycle|swap|cycle 4 led=1 readback=1|cycle 4 led=1 readback=0
tests/integration/check_gpioblink.sh|gpioblink.capture||cycle|drop|cycle 9 |
tests/integration/check_gpioblink.sh|gpioblink.capture||verdict|swap|PASS (10 cycles, readback ok)|FAIL (readback did not track the drive)
tests/integration/check_gpioblink.sh|gpioblink.capture||verdict|drop|[gpioblink] PASS|
tests/integration/check_gpioblink.sh|gpioblink.capture||fault|after|[gpioblink] PASS|=== THREAD FAULT === thread 'gpioblink' killed, system continues
tests/integration/check_gpioblink.sh|gpioblink.capture||panic|after|[gpioblink] PASS|KERNEL PANIC: planted
tests/integration/check_specfault.sh|specfault.capture||announce|drop|[specfault] reading|
tests/integration/check_specfault.sh|specfault.capture||error|after|[specfault] reading|[specfault] ERROR: read was permitted (wrap not no-access?)
tests/integration/check_specfault.sh|specfault.capture||fault|drop|=== THREAD FAULT|
tests/integration/check_specfault.sh|specfault.capture||address|swap|ADDR=0x60800000|ADDR=0x60800004
tests/integration/check_specfault.sh|specfault.capture||address|drop|ADDR=|
tests/integration/check_specfault.sh|specfault.capture||address|swap|reading 0x60800000|reading 0x60800004
tests/integration/check_specfault.sh|specfault.capture||order|order|[specfault] reading|=== THREAD FAULT
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||no-banner|drop|   KickOS 0.5.1  -  microkernel RTOS|
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||no-banner|swap|   KickOS 0.5.1  -  microkernel RTOS|[usbcdcwit] commit 0123abcd
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||accepted|swap|accepted=16384 of|accepted=16000 of
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||accepted|swap|err=0|err=-11
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||accepted|drop|accepted=|
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||verdict|after|tx=16384|[usbcdcwit] FAIL (the channel stopped and did not recover)
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||verdict|drop|[usbcdcwit] PASS|
tests/integration/check_usbcdcwit.sh|usbcdcwit.capture||panic|after|[usbcdcwit] PASS|KERNEL PANIC: planted
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||key|drop|HOW TO READ THIS CAPTURE|
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||key|drop|both absent ==|
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||arm|after|park arm:|[reclaimwit] DRAINTAIL 0123456789abcdef0123456789abcdef <<<DRAIN-END>>>
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||mute|after| 4. the app then|[reclaimwit] MUTE kernel console while the driver holds it
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||sink|after|handle_close rc=0|[reclaimwit] routed through the driver, which discards it
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||live|drop|LIVE kernel console after|
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||live|after|LIVE kernel console after|[reclaimwit] LIVE kernel console after the driver died
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||verdict|drop|PASS reclaim fired|
tests/integration/check_reclaimwit.sh;park|reclaimwit.capture||panic|after|park arm:|KERNEL PANIC: planted
tests/integration/check_reclaimwit.sh;drain|reclaimwit-drain.capture||arm|after|drain arm:|[reclaimwit] park arm: the system stays up, nothing further is printed
tests/integration/check_reclaimwit.sh;drain|reclaimwit-drain.capture||drain-tail|swap|<<<DRAIN-END>>>|<<<DRAIN-
tests/integration/check_reclaimwit.sh;drain|reclaimwit-drain.capture||live|drop|LIVE kernel console after|
tests/integration/check_reclaimwit.sh;drain|reclaimwit-drain.capture||verdict|drop|PASS reclaim fired|
tests/integration/check_reclaimwit.sh;drain|reclaimwit-drain.capture||panic|after|DRAINTAIL|KERNEL PANIC: planted
tests/integration/check_mpu_fault.sh;thread-kill|mpu_fault.capture||error|after|A: writing my own region|[domain] ERROR: planted
tests/integration/check_mpu_fault.sh;thread-kill|mpu_fault.capture||confined|after|A: my region ok|[domain] cross-domain write completed
tests/integration/check_mpu_fault.sh;thread-kill|mpu_fault.capture||control|drop|A: my region ok|
tests/integration/check_mpu_fault.sh;thread-kill|mpu_fault.capture||announce|drop|expect fault at|
tests/integration/check_mpu_fault.sh;thread-kill|mpu_fault.capture||killed|drop|=== THREAD FAULT|
tests/integration/check_mpu_fault.sh;thread-kill|mpu_fault.capture||address|swap|ADDR=0x21000|ADDR=0x21004
tests/integration/check_mpu_fault.sh;thread-kill|mpu_fault.capture||panic|after|ADDR=0x21000|KERNEL PANIC: planted
tests/integration/check_rootfault.sh;thread-kill|rootfault.capture||error|after|child: wrote|[rootfault] ERROR: planted
tests/integration/check_rootfault.sh;thread-kill|rootfault.capture||confined|after|main: writing the child|[rootfault] main: NOT confined
tests/integration/check_rootfault.sh;thread-kill|rootfault.capture||control|drop|child: wrote my own|
tests/integration/check_rootfault.sh;thread-kill|rootfault.capture||announce|drop|main: writing the child|
tests/integration/check_rootfault.sh;thread-kill|rootfault.capture||killed|drop|=== THREAD FAULT|
tests/integration/check_rootfault.sh;thread-kill|rootfault.capture||address|swap|ADDR=0x20000|ADDR=0x20004
tests/integration/check_rootfault.sh;thread-kill|rootfault.capture||panic|after|ADDR=0x20000|KERNEL PANIC: planted
tests/integration/check_rootfault.sh;panic|rootfault-panic.capture||error|after|child: wrote|[rootfault] ERROR: planted
tests/integration/check_rootfault.sh;panic|rootfault-panic.capture||confined|after|main: writing the child|[rootfault] main: NOT confined
tests/integration/check_rootfault.sh;panic|rootfault-panic.capture||control|drop|child: wrote my own|
tests/integration/check_rootfault.sh;panic|rootfault-panic.capture||announce|drop|main: writing the child|
tests/integration/check_rootfault.sh;panic|rootfault-panic.capture||marker|drop|MPU FAULT: thread|
tests/integration/check_rootfault.sh;panic|rootfault-panic.capture||address|swap|attempted write at 0xffff9e832000|attempted write at 0xffff9e832004
tests/integration/check_app_arms.sh;ringpriv;5|ringpriv.capture||error|after|[ringpriv] ok - CONTROL.SPSEL=1|[ringpriv] ERROR: planted
tests/integration/check_app_arms.sh;ringpriv;5|ringpriv.capture||arms|drop|[ringpriv] ok - a permitted unprivileged msr|
tests/integration/check_app_arms.sh;ringpriv;5|ringpriv.capture||verdict|swap|PASS (5 arms)|PASS (4 arms)
tests/integration/check_app_arms.sh;ringpriv;5|ringpriv.capture||panic|after|[ringpriv] PASS|KERNEL PANIC: planted
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||case|drop|[panicgate] case 1|
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||line|swap|message on the wire|message on the wir
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||line|drop|KERNEL PANIC:|
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||returned|after|KERNEL PANIC:|[panicgate] ERROR: kos_panic returned
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||fault|after|[panicgate] case 1|=== MPU FAULT === planted
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||fault|after|[panicgate] case 1|=== ARMV8A EXCEPTION === planted
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||fault|after|[panicgate] case 1|=== RISC-V S-TRAP === planted
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] message on the wire|panicgate.capture||fault|after|[panicgate] case 1|=== X86_64 EXCEPTION === planted
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV...;CUTME|panicgate-oversized.capture||absent|swap|UV...|UV...CUTME
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV...;CUTME|panicgate-oversized.capture||line|swap|UV...|UVWXYZ
tests/integration/check_fault_dump.sh;THREAD FAULT|fault.capture||marker|drop|=== THREAD FAULT|
tests/integration/check_fault_dump.sh;THREAD FAULT|fault.capture||doubled|after|=== THREAD FAULT|=== THREAD FAULT === thread 'main' killed, system continues
tests/integration/check_fault_dump.sh;THREAD FAULT|fault.capture||faulted|after|[fault] executing|[fault] ERROR: illegal instruction did not fault
tests/integration/check_system_default.sh;sysdefault: main returns 3 while a thread spins;3|sysdefault-spin.capture||line|drop|while a thread spins|
tests/integration/check_system_default.sh;sysdefault: main returns 3 while a thread spins;3|sysdefault-spin.capture||spinner|after|sched   tickless|sysdefault: spinner never ran
tests/integration/check_system_default.sh;sysdefault: main returns 3 while a thread spins;3|sysdefault-spin.capture||fault|after|while a thread spins|=== THREAD FAULT === thread 'spinner' killed, system continues
tests/integration/check_system_default.sh;sysdefault: main returns 3 while a thread spins;3|sysdefault-spin.capture||panic|after|while a thread spins|KERNEL PANIC: planted
tests/integration/check_system_default.sh;sysdefault: main faults;139;main|sysdefault-fault.capture||fault|drop|=== THREAD FAULT|
tests/integration/check_system_default.sh;sysdefault: main faults;139;main|sysdefault-fault.capture||fault|swap|thread 'main' killed|thread 'spinner' killed
tests/integration/check_system_default.sh;sysdefault: main faults;139;main|sysdefault-fault.capture||line|drop|sysdefault: main faults|
tests/integration/check_system_default.sh;sysdefault: main faults;139;main|sysdefault-fault.capture||panic|after|=== THREAD FAULT|KERNEL PANIC: planted
tests/integration/check_system_default.sh;sysdefault: main faults;139;main|sysdefault-fault.capture||order|order|sysdefault: main faults|=== THREAD FAULT
tests/integration/check_faultsurvive.sh;survive;rxv3;terminated|faultsurvive.capture||reached|drop|[fs] worker about to fault|
tests/integration/check_faultsurvive.sh;survive;rxv3;terminated|faultsurvive.capture||killed|drop|=== THREAD FAULT|
tests/integration/check_faultsurvive.sh;survive;rxv3;terminated|faultsurvive.capture||survived|drop|[fs] survivor ran|
tests/integration/check_faultsurvive.sh;survive;rxv3;terminated|faultsurvive.capture||order|order|=== THREAD FAULT|[fs] survivor ran
tests/integration/check_faultsurvive.sh;survive;rxv3;terminated|faultsurvive.capture||error|after|[fs] survivor ran|[fs] ERROR: join did not report the worker gone
tests/integration/check_faultsurvive.sh;survive;rxv3;terminated|faultsurvive.capture||panic|after|[fs] survivor ran|KERNEL PANIC: planted
tests/integration/check_faultsurvive.sh;overflow;rxv3;terminated|faultsurvive-overflow.capture||dump|drop|MPU FAULT: thread|
tests/integration/check_faultsurvive.sh;overflow;rxv3;terminated|faultsurvive-overflow.capture||cause|swap|attempted write|attempted read
tests/integration/check_faultsurvive.sh;overflow;rxv3;terminated|faultsurvive-overflow.capture||redirected|after|[fs] worker about to fault|=== THREAD FAULT === thread 'faulter' killed, system continues
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|faultsurvive-kwrite.capture||refusal|drop|=== RX CONTAINED|
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|faultsurvive-kwrite.capture||survived|drop|[fs] survivor ran|
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|faultsurvive-kwrite.capture||order|order|=== RX CONTAINED|[fs] survivor ran
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|faultsurvive-kwrite.capture||kwrite|after|[fs] worker about to fault|[fs] [trapwitness] CORRUPTED: planted
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|faultsurvive-kwrite.capture||panic|after|[fs] survivor ran|KERNEL PANIC: planted
tests/integration/check_fault_dump.sh;RX EXCEPTION (trap)|fault-rx.capture||marker|drop|=== RX EXCEPTION|
tests/integration/check_fault_dump.sh;RX EXCEPTION (trap)|fault-rx.capture||doubled|after|PC=0xffc00400|=== RX EXCEPTION (trap) ===
tests/integration/check_fault_dump.sh;RX EXCEPTION (trap)|fault-rx.capture||faulted|after|[fault] executing|[fault] ERROR: illegal instruction did not fault
tests/integration/check_app_arms.sh;ringpriv;1|ringpriv-noring.capture||arms|drop|[ringpriv] ok - CONTROL.nPRIV=0|
tests/integration/check_app_arms.sh;ringpriv;1|ringpriv-noring.capture||verdict|swap|PASS (1 arms)|PASS (2 arms)
tests/integration/check_app_arms.sh;ringpriv;1|ringpriv-noring.capture||error|after|[ringpriv] ok - CONTROL.nPRIV=0|[ringpriv] ERROR: planted
tests/integration/check_app_arms.sh;ringpriv;2|ringpriv-armv6m.capture||arms|drop|[ringpriv] ok - CONTROL.SPSEL=1|
tests/integration/check_app_arms.sh;ringpriv;2|ringpriv-armv6m.capture||verdict|swap|PASS (2 arms)|PASS (5 arms)
tests/integration/check_fault_dump.sh;HARD FAULT|fault-hard.capture||announce|drop|[fault] executing|
tests/integration/check_fault_dump.sh;HARD FAULT|fault-hard.capture||order|order|[fault] executing|=== HARD FAULT
tests/integration/check_fault_dump.sh;THREAD FAULT|fault.capture||announce|drop|[fault] executing|
tests/integration/check_fault_dump.sh;THREAD FAULT|fault.capture||order|order|[fault] executing|=== THREAD FAULT
tests/integration/check_fault_dump.sh;RX EXCEPTION (trap)|fault-rx.capture||announce|drop|[fault] executing|
tests/integration/check_fault_dump.sh;RX EXCEPTION (trap)|fault-rx.capture||order|order|[fault] executing|=== RX EXCEPTION
tests/integration/check_fault_dump.sh;HARD FAULT|fault-hard.capture||marker|drop|=== HARD FAULT|
tests/integration/check_fault_dump.sh;SIM FAULT|fault-sim.capture||marker|drop|=== SIM FAULT|
tests/integration/check_fault_dump.sh;SIM FAULT|fault-sim.capture||doubled|after|=== SIM FAULT|=== SIM FAULT (illegal instruction) at 0xaaaacd2a0366 ===
tests/integration/check_fault_dump.sh;SIM FAULT|fault-sim.capture||faulted|after|[fault] executing|[fault] ERROR: illegal instruction did not fault
tests/integration/check_fault_dump.sh;SIM FAULT|fault-sim.capture||announce|drop|[fault] executing|
tests/integration/check_fault_dump.sh;SIM FAULT|fault-sim.capture||order|order|[fault] executing|=== SIM FAULT
tests/integration/check_faultsurvive.sh;survive;armv7m;terminated|faultsurvive-armv7m.capture||killed|drop|=== THREAD FAULT|
tests/integration/check_faultsurvive.sh;survive;armv7m;terminated|faultsurvive-armv7m.capture||order|order|=== THREAD FAULT|[fs] survivor ran
tests/integration/check_faultsurvive.sh;survive;armv7m;terminated|faultsurvive-armv7m.capture||order|order|[fs] worker about to fault|=== THREAD FAULT
tests/integration/check_faultsurvive.sh;overflow;armv7m;terminated|faultsurvive-armv7m-overflow.capture||dump|drop|=== MPU FAULT|
tests/integration/check_faultsurvive.sh;overflow;armv7m;terminated|faultsurvive-armv7m-overflow.capture||cause|swap|CFSR=0x92|CFSR=0x2
tests/integration/check_faultsurvive.sh;overflow;armv7m;terminated|faultsurvive-armv7m-overflow.capture||order|order|[fs] worker about to fault|=== MPU FAULT
tests/integration/check_faultsurvive.sh;offstack;armv7m;terminated|faultsurvive-armv7m-offstack.capture||dump|drop|=== HARD FAULT|
tests/integration/check_faultsurvive.sh;offstack;armv7m;terminated|faultsurvive-armv7m-offstack.capture||cause|swap|CFSR=0x10000|CFSR=0x10010
tests/integration/check_faultsurvive.sh;unread;armv7m;terminated|faultsurvive-armv7m-unread.capture||cause|swap|CFSR=0x10010|CFSR=0x10800
tests/integration/check_faultsurvive.sh;unread;armv7m;terminated|faultsurvive-armv7m-unread.capture||frame|drop|frame not read|
tests/integration/check_faultsurvive.sh;unread;armv7m;terminated|faultsurvive-armv7m-unread.capture||doubled|after|CFSR=0x10010|=== MPU FAULT ===
tests/integration/check_faultsurvive.sh;survive;armv6m;terminated|faultsurvive-armv6m.capture||killed|drop|=== THREAD FAULT|
tests/integration/check_faultsurvive.sh;survive;armv6m;terminated|faultsurvive-armv6m.capture||survived|drop|[fs] survivor ran|
tests/integration/check_faultsurvive.sh;overflow;armv6m;terminated|faultsurvive-armv6m-overflow.capture||dump|drop|=== HARD FAULT|
tests/integration/check_faultsurvive.sh;overflow;armv6m;terminated|faultsurvive-armv6m-overflow.capture||cause|swap|(PSP)|(MSP)
tests/integration/check_faultsurvive.sh;overflow;armv6m;terminated|faultsurvive-armv6m-overflow.capture||cause|after|R12=0x0|  CFSR=0x10 HFSR=0x0
tests/integration/check_faultsurvive.sh;offstack;armv6m;terminated|faultsurvive-armv6m-offstack.capture||cause|swap|(PSP)|(MSP)
tests/integration/check_faultsurvive.sh;offstack;armv6m;terminated|faultsurvive-armv6m-offstack.capture||dump|drop|=== HARD FAULT|
tests/integration/check_faultsurvive.sh;survive;rv32imac;terminated|faultsurvive-rv32imac.capture||killed|drop|=== THREAD FAULT|
tests/integration/check_faultsurvive.sh;survive;rv32imac;terminated|faultsurvive-rv32imac.capture||order|order|=== THREAD FAULT|[fs] survivor ran
tests/integration/check_faultsurvive.sh;lowedge;rv32imac;terminated|faultsurvive-rv32imac-lowedge.capture||lowband|drop|[lowband] INTACT|
tests/integration/check_faultsurvive.sh;lowedge;rv32imac;terminated|faultsurvive-rv32imac-lowedge.capture||lowband|swap|[lowband] INTACT: the kernel wrote nothing below the parked sp|[lowband] CORRUPTED: the kernel ran below stack_lo on a U-mode sp
tests/integration/check_faultsurvive.sh;lowedge;rv32imac;terminated|faultsurvive-rv32imac-lowedge.capture||killed|drop|=== THREAD FAULT|
tests/integration/check_faultsurvive.sh;overflow;rv32imac;contained|faultsurvive-rv32imac-contained.capture||refusal|drop|=== RISC-V CONTAINED|
tests/integration/check_faultsurvive.sh;overflow;rv32imac;contained|faultsurvive-rv32imac-contained.capture||attribution|swap|thread 'faulter' sp|thread 'other' sp
tests/integration/check_faultsurvive.sh;overflow;rv32imac;contained|faultsurvive-rv32imac-contained.capture||order|order|=== RISC-V CONTAINED|[fs] survivor ran
tests/integration/check_faultsurvive.sh;overflow;rv32imac;contained|faultsurvive-rv32imac-contained.capture||order|order|[fs] worker about to fault|=== RISC-V CONTAINED
tests/integration/check_faultsurvive.sh;offstack;rv32imac;contained|faultsurvive-rv32imac-contained.capture||refusal|drop|=== RISC-V CONTAINED|
tests/integration/check_faultsurvive.sh;offstack;rv32imac;contained|faultsurvive-rv32imac-contained.capture||attribution|swap|thread 'faulter' sp|thread 'other' sp
tests/integration/check_faultsurvive.sh;offstack;rv32imac;contained|faultsurvive-rv32imac-contained.capture||order|order|=== RISC-V CONTAINED|[fs] survivor ran
tests/integration/check_faultsurvive.sh;kwrite;rv32imac;contained|faultsurvive-rv32imac-contained.capture||refusal|drop|=== RISC-V CONTAINED|
tests/integration/check_faultsurvive.sh;kwrite;rv32imac;contained|faultsurvive-rv32imac-contained.capture||attribution|swap|thread 'faulter' sp|thread 'other' sp
tests/integration/check_faultsurvive.sh;kwrite;rv32imac;contained|faultsurvive-rv32imac-contained.capture||order|order|=== RISC-V CONTAINED|[fs] survivor ran
tests/integration/check_faultsurvive.sh;misalign;rv32imac;contained|faultsurvive-rv32imac-contained.capture||refusal|drop|=== RISC-V CONTAINED|
tests/integration/check_faultsurvive.sh;misalign;rv32imac;contained|faultsurvive-rv32imac-contained.capture||attribution|swap|thread 'faulter' sp|thread 'other' sp
tests/integration/check_faultsurvive.sh;misalign;rv32imac;contained|faultsurvive-rv32imac-contained.capture||order|order|=== RISC-V CONTAINED|[fs] survivor ran
tests/integration/check_faultsurvive.sh;kwrite;rv32imac;contained|faultsurvive-rv32imac-contained.capture||kwrite|after|[fs] worker about to fault|[fs] [trapwitness] CORRUPTED: planted
tests/integration/check_faultsurvive.sh;offstack;rxv3;terminated|faultsurvive-rx-offstack.capture||cause|swap|PSW=0x130003|PSW=0x030003
tests/integration/check_faultsurvive.sh;offstack;rxv3;terminated|faultsurvive-rx-offstack.capture||dump|drop|=== RX EXCEPTION|
tests/integration/check_faultsurvive.sh;offstack;rxv3;terminated|faultsurvive-rx-offstack.capture||cause|swap|(privileged instruction)|(undefined instruction)
tests/integration/check_faultsurvive.sh;offstack;rxv3;terminated|faultsurvive-rx-offstack.capture||order|order|[fs] worker about to fault|=== RX EXCEPTION
tests/integration/check_faultsurvive.sh;misalign;rxv3;contained|faultsurvive-rx-misalign.capture||usp|swap|USP=0x21ffa|USP=0x21ff8
tests/integration/check_faultsurvive.sh;misalign;rxv3;contained|faultsurvive-rx-misalign.capture||refusal|drop|=== RX CONTAINED|
tests/integration/check_faultsurvive.sh;misalign;rxv3;contained|faultsurvive-rx-misalign.capture||order|order|=== RX CONTAINED|[fs] survivor ran
tests/integration/check_faultsurvive.sh;misalign;rxv3;contained|faultsurvive-rx-misalign.capture||order|order|[fs] worker about to fault|=== RX CONTAINED
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|faultsurvive-kwrite.capture||usp|swap|USP=0x8|USP=0xa
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|faultsurvive-kwrite.capture||usp|drop|USP=|
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: P08|panicgate-terse.capture||line|swap|KERNEL PANIC: P08|KERNEL PANIC: P07
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: P08|panicgate-terse.capture||case|drop|[panicgate] case|
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: user panic (no readable message)|panicgate-null.capture||line|swap|(no readable message)|(no message)
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: user panic (no readable message)|panicgate-null.capture||order|order|[panicgate] case|KERNEL PANIC:
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] ctl???? end|panicgate-control.capture||line|swap|ctl???? end|ctl??? end
tests/integration/check_qemu_panicgate.sh;KERNEL PANIC: [panicgate] ctl???? end|panicgate-control.capture||fault|after|[panicgate] case 5|=== HARD FAULT === planted
tests/integration/check_mpu_fault.sh;panic|mpu_fault-panic.capture||marker|drop|=== MPU FAULT|
tests/integration/check_mpu_fault.sh;panic|mpu_fault-panic.capture||address|swap|MMFAR=0x20011000|MMFAR=0x20011004
tests/integration/check_mpu_fault.sh;panic|mpu_fault-panic.capture||control|drop|A: my region ok|"

OWED="tests/integration/check_fault_dump.sh;HARD FAULT|the fault ended the system with the status its marker implies
tests/integration/check_fault_dump.sh;RX EXCEPTION (trap)|the fault ended the system with the status its marker implies
tests/integration/check_fault_dump.sh;SIM FAULT|the fault ended the system with the status its marker implies
tests/integration/check_fault_dump.sh;THREAD FAULT|the fault ended the system with the status its marker implies
tests/integration/check_f411spi.sh|loopback (bench wiring absent)
tests/integration/check_k64dspi.sh|the bus peer (bench wiring absent)
tests/integration/check_faultsurvive.sh;kwrite;rv32imac;contained|the system exited 0 once main outlived the refusal
tests/integration/check_faultsurvive.sh;kwrite;rxv3;contained|the system exited 0 once main outlived the refusal
tests/integration/check_faultsurvive.sh;lowedge;rv32imac;terminated|the system exited 0 once main returned
tests/integration/check_faultsurvive.sh;misalign;rv32imac;contained|the system exited 0 once main outlived the refusal
tests/integration/check_faultsurvive.sh;misalign;rxv3;contained|the system exited 0 once main outlived the refusal
tests/integration/check_faultsurvive.sh;offstack;armv6m;terminated|the escalation ended the system with exit 132
tests/integration/check_faultsurvive.sh;offstack;armv7m;terminated|the escalation ended the system with exit 132
tests/integration/check_faultsurvive.sh;offstack;rv32imac;contained|the system exited 0 once main outlived the refusal
tests/integration/check_faultsurvive.sh;offstack;rxv3;terminated|the escalation ended the system with exit 132
tests/integration/check_faultsurvive.sh;overflow;armv6m;terminated|the escalation ended the system with exit 132
tests/integration/check_faultsurvive.sh;overflow;armv7m;terminated|the escalation ended the system with exit 132
tests/integration/check_faultsurvive.sh;unread;armv7m;terminated|the escalation ended the system with exit 132
tests/integration/check_faultsurvive.sh;overflow;rv32imac;contained|the system exited 0 once main outlived the refusal
tests/integration/check_faultsurvive.sh;overflow;rxv3;terminated|the escalation ended the system with exit 0
tests/integration/check_faultsurvive.sh;survive;armv6m;terminated|the system exited 0 once main returned
tests/integration/check_faultsurvive.sh;survive;armv7m;terminated|the system exited 0 once main returned
tests/integration/check_faultsurvive.sh;survive;rv32imac;terminated|the system exited 0 once main returned
tests/integration/check_faultsurvive.sh;survive;rxv3;terminated|the system exited 0 once main returned
tests/integration/check_libc_exit.sh;--atexit|main's exit() ended the system with status 7
tests/integration/check_qemu_cxxterm.sh|terminate stopped the image
tests/integration/check_reclaimwit.sh;drain|the drain arm shut the system down with status 0
tests/integration/check_rootfault.sh;thread-kill|main's fault ended the system with KOS_EXIT_FAULT (139)
tests/integration/check_sched_exit.sh|main's exit with a child alive ended the system with status 7
tests/integration/check_slaypeer.sh|the system exited 0 once main returned past PASS
tests/integration/check_system_default.sh;sysdefault: main faults;139;main|main's task ending ended the system with status 139
tests/integration/check_system_default.sh;sysdefault: main returns 3;3|main's task ending ended the system with status 3
tests/integration/check_system_default.sh;sysdefault: main returns 3 while a thread spins;3|main's task ending ended the system with status 3"

# --listing <kickos-images.txt>: every judge a build's image listing names, with its arguments,
# has a row above, so no judged (script, args) pair goes without a fixture.
if [ "${1:-}" = --listing ]; then
    [ -f "${2:-}" ] || fail "usage: check_app_judges.sh --listing <kickos-images.txt>"
    PROVED="$(printf '%s\n' "$ROWS" | cut -d '|' -f 1 | sort -u)"
    pairs=0
    while IFS='|' read -r image _stdout judge args; do
        case "$judge" in
            tests/integration/check_tap_stream.sh | - | emulator | emulator-owed | human | inapplicable | "") continue ;;
            *) ;;
        esac
        key="$judge"
        if [ -n "$args" ]; then
            key="$judge;$args"
        fi
        pairs=$((pairs + 1))
        if ! printf '%s\n' "$PROVED" | grep -qxF -- "$key"; then
            bad "$image is judged by $key, which no fixture row proves"
        fi
    done < "$2"
    [ "$rc" -eq 0 ] || exit 1
    echo "PASS: each of $pairs judged image(s) names a judge and arguments a fixture row proves"
    exit 0
fi

require_repo_root
scratch_dir

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

# <in> <op> <literal> <replacement> <out>: plant over <in> and <in>.times, where it exists.
plant_pair() {
    _pp_log="$2"
    _pp_times="$2"
    case "$2" in
        log-*)
            _pp_log="${2#log-}"
            _pp_times=none
            ;;
        times-*)
            _pp_log=none
            _pp_times="${2#times-}"
            ;;
    esac
    plant "$1" "$_pp_log" "$3" "$4" "$5"
    rm -f "$5.times"
    if [ -f "$1.times" ]; then
        plant "$1.times" "$_pp_times" "$3" "$4" "$5.times"
    fi
}

# <a> <b>: the two captures and their arrival stamps are the same bytes.
same_pair() {
    cmp -s "$1" "$2" || return 1
    if [ -f "$1.times" ] || [ -f "$2.times" ]; then
        cmp -s "$1.times" "$2.times" || return 1
    fi
    return 0
}

# <judge> <cache line> <capture>: the judge's exit status over the capture.
judged() {
    mkdir -p "$TMP/build"
    printf '%s\n' "$2" > "$TMP/build/CMakeCache.txt"
    _jd_log="$3"
    _jd_ifs="$IFS"
    IFS=';'
    set -f
    # shellcheck disable=SC2086
    set -- $1
    set +f
    IFS="$_jd_ifs"
    _jd_script="$1"
    shift
    KOS_CAPTURE="$_jd_log" sh "$_jd_script" "$TMP/build" "${JUDGE_SOURCE:-$PWD}" cmake "$@" \
        > "$TMP/judge.out" 2>&1
}

# <judge> <output>: the NOT EVALUATED clauses in <output> are not exactly those OWED declares.
owed_differs() {
    _od_got="$(sed -n 's/^NOT EVALUATED: //p' "$2" | sort)"
    _od_want="$(printf '%s\n' "$OWED" \
        | KOS_OD_KEY="$1" awk -F '|' '$1 == ENVIRON["KOS_OD_KEY"] { print $2 }' | sort)"
    [ "$_od_got" != "$_od_want" ]
}

passed=""
goodruns=""
while IFS='|' read -r judge fixture cache token op lit rep op2 lit2 rep2; do
    [ -n "$judge" ] || continue
    [ -f "${judge%%;*}" ] || fail "no judge ${judge%%;*}"
    [ -f "tests/integration/app_captures/$fixture" ] || fail "no fixture $fixture"
    [ -n "$token" ] || fail "the row for $judge over $fixture names no token"
    plant_pair "tests/integration/app_captures/$fixture" none "" "" "$TMP/good.log"
    case "$passed" in
        *"|$judge $fixture $cache|"*) ;;
        *)
            if ! judged "$judge" "$cache" "$TMP/good.log"; then
                bad "$judge refuses its fixture $fixture: $(tail -n 1 "$TMP/judge.out")"
            elif owed_differs "$judge" "$TMP/judge.out"; then
                bad "$judge over $fixture names NOT EVALUATED [$(sed -n 's/^NOT EVALUATED: //p' \
                    "$TMP/judge.out" | paste -sd ';' -)], not the clauses OWED declares for it"
            fi
            passed="$passed|$judge $fixture $cache|"
            goodruns="$goodruns$judge|$fixture|$cache
"
            ;;
    esac
    what="the $op of '$lit'"
    if [ "$op" = cache ]; then
        what="'$rep' for its cache"
        judged "$judge" "$rep" "$TMP/good.log"
        jrc=$?
    elif [ "${op#source:}" != "$op" ]; then
        src="${op#source:}"
        what="'$rep' after '$lit' in $src"
        rm -rf "$TMP/source"
        mkdir -p "$TMP/source/$(dirname "$src")"
        KOS_PLANT_LIT="$lit" KOS_PLANT_NEW="$rep" awk '
            { print }
            index($0, ENVIRON["KOS_PLANT_LIT"]) > 0 { print ENVIRON["KOS_PLANT_NEW"] }' "$src" \
            > "$TMP/source/$src"
        if cmp -s "$src" "$TMP/source/$src"; then
            bad "$what leaves $src unchanged"
            continue
        fi
        JUDGE_SOURCE="$TMP/source" judged "$judge" "$cache" "$TMP/good.log"
        jrc=$?
    else
        plant_pair "$TMP/good.log" "$op" "$lit" "$rep" "$TMP/bad.log"
        if [ -n "$op2" ]; then
            what="$what and the $op2 of '$lit2'"
            plant_pair "$TMP/bad.log" "$op2" "$lit2" "$rep2" "$TMP/bad2.log"
            mv "$TMP/bad2.log" "$TMP/bad.log"
            rm -f "$TMP/bad.log.times"
            if [ -f "$TMP/bad2.log.times" ]; then
                mv "$TMP/bad2.log.times" "$TMP/bad.log.times"
            fi
        fi
        if same_pair "$TMP/good.log" "$TMP/bad.log"; then
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

NAMED="$(git ls-files user/apps tests/integration/gates | grep -E '(CMakeLists\.txt|\.cmake)$' \
    | while read -r f; do
        sed -n 's/^ *kickos_app_judge([^ ]* \([^ )]*\).*/\1/p' "$f"
    done | sort -u)"
[ -n "$NAMED" ] || fail "no app CMake names a judge, so the coverage check below reads nothing"
for judge in $NAMED; do
    case "$passed" in
        *"|$judge "*|*"|$judge;"*) ;;
        *) bad "$judge judges an image and no fixture row proves it" ;;
    esac
done

# <judge>: the lines of <judge> that read an exit status other than through status_clause.
raw_status_reads() {
    grep -nE '[$][{]?(RC|KOS_STATUS|POLL_ALIVE)([^A-Za-z0-9_]|$)' "$1"
}

for judge in $NAMED; do
    if raw_status_reads "$judge" > "$TMP/raw"; then
        bad "$judge reads an exit status outside status_clause, at line \
$(head -n 1 "$TMP/raw" | cut -d: -f1)"
    fi
    if grep -qE '(status_clause|cnot_evaluated) ' "$judge" \
        && ! printf '%s\n' "$OWED" | cut -d '|' -f 1 | cut -d ';' -f 1 | grep -qxF -- "$judge"; then
        bad "$judge can name a clause NOT EVALUATED and OWED declares none for it"
    fi
done

# Planted: a new arm that reads the exit status itself is refused.
mkdir -p "$TMP/planted"
awk '/^status_clause / {
         print "if [ \"$atexit\" -eq 1 ] && ! judging_capture && [ \"$RC\" -gt 128 ]; then"
         print "    fail \"planted\""
         print "fi"
     }
     { print }' tests/integration/check_libc_exit.sh > "$TMP/planted/check_libc_exit.sh"
cmp -s tests/integration/check_libc_exit.sh "$TMP/planted/check_libc_exit.sh" \
    && fail "the planted status arm was not added"
raw_status_reads "$TMP/planted/check_libc_exit.sh" > /dev/null \
    || bad "a new arm reading the exit status outside status_clause is not refused"

# Every pair OWED declares is run by a fixture row, so its clauses were compared above.
while IFS= read -r key; do
    printf '%s' "$goodruns" | cut -d '|' -f 1 | grep -qxF -- "$key" \
        || bad "OWED declares $key and no fixture row runs it"
done <<KEYS
$(printf '%s\n' "$OWED" | cut -d '|' -f 1 | sort -u)
KEYS

# Planted: a judge whose NOT EVALUATED line is silenced still passes its fixture, and
# owed_differs refuses it.
key='tests/integration/check_libc_exit.sh;--atexit'
mkdir -p "$TMP/silenced/tests/integration"
cp -R tests/lib "$TMP/silenced/tests/lib"
sed 's/^    echo "NOT EVALUATED: $1"$/    :/' tests/lib/gate.sh > "$TMP/silenced/tests/lib/gate.sh"
cmp -s tests/lib/gate.sh "$TMP/silenced/tests/lib/gate.sh" \
    && fail "cnot_evaluated was not silenced in the planted gate.sh"
cp "${key%%;*}" "$TMP/silenced/${key%%;*}"
run="$(printf '%s' "$goodruns" | KOS_OD_KEY="$key" awk -F '|' '$1 == ENVIRON["KOS_OD_KEY"] { print; exit }')"
plant_pair "tests/integration/app_captures/$(printf '%s' "$run" | cut -d '|' -f 2)" none "" "" \
    "$TMP/good.log"
if ! judged "$TMP/silenced/$key" "$(printf '%s' "$run" | cut -d '|' -f 3)" "$TMP/good.log"; then
    bad "$key with its NOT EVALUATED silenced refuses its fixture: $(tail -n 1 "$TMP/judge.out")"
elif ! owed_differs "$key" "$TMP/judge.out"; then
    bad "$key with its NOT EVALUATED silenced passes its fixture unrefused"
fi

# A fitting reaches a judge as an argument and from nowhere else: a judge run straight from a
# shell that exports a fitting list judges the unjumpered capture as unwired.
plant tests/integration/app_captures/f411spi-unwired.capture none "" "" "$TMP/good.log"
mkdir -p "$TMP/build"
: > "$TMP/build/CMakeCache.txt"
if ! KOS_WIRED=spi1-loopback KOS_FITTINGS=spi1-loopback KOS_CAPTURE="$TMP/good.log" \
        sh tests/integration/check_f411spi.sh "$TMP/build" "$PWD" cmake > "$TMP/judge.out" 2>&1; then
    bad "check_f411spi.sh takes a fitting from the environment: $(tail -n 1 "$TMP/judge.out")"
elif ! grep -qxF 'NOT EVALUATED: loopback (bench wiring absent)' "$TMP/judge.out"; then
    bad "check_f411spi.sh run with a fitting in the environment does not owe the loopback"
fi

BOARD_APPS="$(git ls-files 'user/apps/*/*/CMakeLists.txt' | grep -v '^user/apps/common/')"
[ -n "$BOARD_APPS" ] || fail "git ls-files found no board app, so the judge check below reads nothing"
for f in $BOARD_APPS; do
    app="${f#user/apps/}"
    app="${app%/CMakeLists.txt}"
    if grep -qE '^ *kickos_(app_judge|human_judged)\(' "$f"; then
        continue
    fi
    if printf '%s\n' "$WAIVED" | grep -qxF -- "$app"; then
        continue
    fi
    bad "$app names no judge (kickos_app_judge), is not human-judged (kickos_human_judged) and is not on WAIVED"
done

[ "$rc" -eq 0 ] || exit 1
echo "PASS: every capture judge passes its fixture and refuses each damaged copy as its row names, and every board app names one"
