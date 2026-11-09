#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/bench/bench-capture.sh, run over stub flash tools and a pseudo-terminal console, leaves no
# console reader and no stamper behind a refusal: a stamper failing at the capture window's end
# under the wrapped reader, one failing at the f411disco cut under its bare reader, a failed
# load under the wrapped reader, and blackpill's J-Link route, which drives its own part on the
# serial it is given, cut to the reset's boot under the wrapped reader; and a J-Link reading VTref 0 V.
# A capture that ran its window records, in <log>.bus, whether the device it selected for each of
# the board's presence rows is on the bus after it: by the serial it was given, or by the port the
# row's one device sat on before the flash; another unit of the same kind is not that device, and
# a selection no serial or single device establishes is recorded unidentified. A USB CDC console is read through one open of its node, which receives the bytes
# the device sent before it, its identity rows first, with its tty raw before the first byte.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
command -v python3 > /dev/null || fail "python3 not found"
command -v pgrep > /dev/null || fail "pgrep not found"

python3 -c 'import os, pty, sys, time
m, s = pty.openpty()
print(os.ttyname(s), flush=True)
os.close(s)
time.sleep(600)' > "$TMP/pts" &
KOS_CHILD_PID=$!
n=0
until [ -s "$TMP/pts" ]; do
    n=$((n + 1))
    [ "$n" -lt 500 ] || fail "no pseudo-terminal was opened"
    sleep 0.01
done
ln -s "$(cat "$TMP/pts")" "$TMP/tty"

mkdir -p "$TMP/root/tools" "$TMP/root/boards" "$TMP/bin" "$TMP/late"
cp -R boards/picopi boards/f411disco boards/blackpill boards/teensy41 "$TMP/root/boards/"
cp tools/flash-jlink.sh tools/flash-common.sh "$TMP/root/tools/"
# STUB_REPLUG: a command run as the load, which moves devices on the planted bus.
printf '#!/bin/sh\n[ -z "${STUB_REPLUG:-}" ] || eval "$STUB_REPLUG"\nexit 0\n' > "$TMP/root/tools/flash.sh"
printf '#!/bin/sh\n[ -z "${STUB_REPLUG:-}" ] || eval "$STUB_REPLUG"\necho "$STUB_LOAD_MSG"\nexit "${STUB_LOAD_RC:-0}"\n' \
    > "$TMP/root/tools/flash-picotool.sh"
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/picotool"
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/st-flash"
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/teensy_loader_cli"
printf '#!/bin/sh\necho "$*" >> "%s"\nif [ -n "${STUB_VTREF:-}" ]; then\n    echo "VTref=${STUB_VTREF}V"\n    exit 0\nfi\necho identified.\n' \
    "$TMP/jlink.calls" > "$TMP/bin/JLinkExe"
chmod +x "$TMP/root/tools/flash.sh" "$TMP/root/tools/flash-picotool.sh" "$TMP/bin/picotool" \
    "$TMP/bin/st-flash" "$TMP/bin/JLinkExe" "$TMP/bin/teensy_loader_cli"
printf 'RIG_CONSOLE_PICOPI=%s\nRIG_CONSOLE_F411DISCO=%s\nRIG_CONSOLE_BLACKPILL=%s\nRIG_CONSOLE_TEENSY41=%s\n' \
    "$TMP/tty" "$TMP/tty" "$TMP/tty" "$TMP/tty" > "$TMP/rig.conf"
: > "$TMP/image"
: > "$TMP/image.bin"

# The capture's scripts beside a stamper that is ready, then fails when it is stopped.
cp tools/bench/*.sh tools/bench/*.py "$TMP/late/"
printf '%s\n' 'import signal, sys, time' \
    'open(sys.argv[1] + ".times", "w").close()' \
    'signal.signal(signal.SIGTERM, lambda *_: sys.exit(3))' \
    'while True:' \
    '    time.sleep(0.01)' > "$TMP/late/stamp_lines.py"

# The capture's scripts over a planted sysfs bus, $TMP/sys.
mkdir -p "$TMP/bus" "$TMP/sys"
cp tools/bench/*.sh tools/bench/*.py "$TMP/bus/"
mv "$TMP/bus/bench-host.sh" "$TMP/bus/bench-host-real.sh"
cat > "$TMP/bus/bench-host.sh" <<EOF
. "$TMP/bus/bench-host-real.sh"
bench_run() {
    sed "s|/sys/bus/usb/devices|$TMP/sys|g" | bash -s
}
EOF
# <vid:pid>[/<serial>][@<port>]...: the planted bus, one device per argument, at port 1-<n> unless
# it names its own.
plant_bus() {
    rm -rf "${TMP:?}/sys"
    mkdir -p "$TMP/sys"
    local n=0 d id port
    for d in "$@"; do
        n=$((n + 1))
        port="1-$n"
        case $d in
            *@*)
                port="${d##*@}"
                d="${d%@*}"
                ;;
        esac
        id="${d%%/*}"
        mkdir "$TMP/sys/$port"
        printf '%s\n' "${id%%:*}" > "$TMP/sys/$port/idVendor"
        printf '%s\n' "${id#*:}" > "$TMP/sys/$port/idProduct"
        case $d in
            */*)
                printf '%s\n' "${d#*/}" > "$TMP/sys/$port/serial"
                ;;
        esac
    done
}

# <pattern>: waits a bounded while for every process whose command line holds <pattern> to end;
# returns 1, ending them, if one outlives that.
gone() {
    local n=0
    while pgrep -f "$1" > /dev/null; do
        n=$((n + 1))
        if [ "$n" -ge 500 ]; then
            pkill -f "$1"
            return 1
        fi
        sleep 0.01
    done
}

SN=
# <name> <board> <capture script> <word in the refusal> [VAR=value]...: the capture refuses, saying
# <word>, and leaves no reader of the console and no stamper running.
refuses() {
    local name=$1 board=$2 script=$3 word=$4
    shift 4
    if env ROOT="$TMP/root" KICKOS_RIG="$TMP/rig.conf" PATH="$TMP/bin:$PATH" CAP_SECS=1 \
           EXPECT_COMMIT=0123abcd EXPECT_ARCH=armv6m BENCH_HOST= PEER_ERASE= "$@" \
           bash "$script" "$board" planted "$TMP/image" "$TMP/$name.log" ${SN:+"$SN"} \
           > "$TMP/$name.out" 2>&1; then
        bad "$name: the capture is accepted"
    elif ! grep -qF -e "$word" "$TMP/$name.out"; then
        bad "$name: the capture refuses, but not for '$word': $(grep REFUSING "$TMP/$name.out")"
    fi
    gone "$TMP/tty" || bad "$name: a reader of the console outlives the refusal"
    gone "$TMP/$name.log" || bad "$name: the stamper outlives the refusal"
}

refuses window-end picopi "$TMP/late/bench-capture.sh" 'stamp_lines.py failed during the capture'
refuses cut f411disco "$TMP/late/bench-capture.sh" 'stamp_lines.py failed during the capture'
refuses load picopi "$PWD/tools/bench/bench-capture.sh" 'picotool could not flash' STUB_LOAD_RC=1
refuses load-why picopi "$PWD/tools/bench/bench-capture.sh" 'picotool could not flash picopi: ERROR: planted words' \
    STUB_LOAD_RC=1 'STUB_LOAD_MSG=ERROR: planted words'
SN=000177003338
refuses jlink-cut blackpill "$PWD/tools/bench/bench-capture.sh" 'no whole banner title follows the reset'
SN=
# <board> <record> <what> [VAR=value]...: a capture over the planted bus, which printed nothing and
# is refused once its window is over, records exactly <record> for the board's presence rows.
bus_records() {
    local board=$1 want=$2 what=$3
    shift 3
    refuses bus "$board" "$TMP/bus/bench-capture.sh" 'the capture produced nothing' CAP_SECS=0 "$@"
    [ "$(cat "$TMP/bus.log.bus" 2>/dev/null)" = "$want" ] \
        || bad "$what is not recorded as [$want]: $(cat "$TMP/bus.log.bus" 2>&1)"
}
loader='the RP2040 in BOOTSEL, which picotool loads'
SN=E0C9125B0D9B
plant_bus 2e8a:0003/E0C9125B0D9B 1366:1020
bus_records picopi "2e8a:0003|present|serial $SN|$loader" "picopi in BOOTSEL after the window"
plant_bus 1366:1020
bus_records picopi "2e8a:0003|absent|serial $SN|$loader" "picopi out of BOOTSEL after the window"
plant_bus 2e8a:0003/5F04F8E7E07F809A 2e8a:0003/0123456789AB 1366:1020
bus_records picopi "2e8a:0003|absent|serial $SN|$loader" \
    "picopi out of BOOTSEL with two other RP2040s in theirs"
plant_bus 2e8a:0003/5F04F8E7E07F809A 2e8a:0003/E0C9125B0D9B 1366:1020
bus_records picopi "2e8a:0003|present|serial $SN|$loader" \
    "picopi in BOOTSEL beside another RP2040 in its own"
plant_bus 2e8a:0003/E0C9125B0D9B 2e8a:0003/E0C9125B0D9B
bus_records picopi "2e8a:0003|ambiguous|serial $SN|$loader" "two RP2040s carrying picopi's serial"
SN=
plant_bus 2e8a:0003
bus_records picopi "2e8a:0003|unidentified|none: no serial was given to select it by|$loader" \
    "picopi in BOOTSEL with no serial to select it by"
halfkay='HalfKay, the bootloader teensy_loader_cli loads'
plant_bus 16c0:0478@1-4
bus_records teensy41 "16c0:0478|present|port 1-4|$halfkay" "teensy41 back in HalfKay on its port" \
    EXPECT_ARCH=armv7m
bus_records teensy41 "16c0:0478|absent|port 1-4|$halfkay" \
    "teensy41 off the bus with another Teensy in HalfKay on another port" EXPECT_ARCH=armv7m \
    "STUB_REPLUG=mv $TMP/sys/1-4 $TMP/sys/1-5"
plant_bus 16c0:0478@1-4 16c0:0478@1-5
bus_records teensy41 "16c0:0478|unidentified|none: 2 devices at 16c0:0478 before the flash|$halfkay" \
    "two Teensys in HalfKay before the flash" EXPECT_ARCH=armv7m
# The connect, the load and the reset.
calls=$(awk '/-device STM32F411CE / && /-SelectEmuBySN 000177003338( |$)/' "$TMP/jlink.calls" 2>/dev/null | wc -l)
[ "$calls" -eq 3 ] \
    || bad "blackpill's J-Link route drives STM32F411CE on the serial it is given in $calls calls, not 3: $(cat "$TMP/jlink.calls" 2>/dev/null)"

# A probe reading 0 V on VTref is refused by name on its first connect, not after every speed.
rm -f "$TMP/jlink.calls"
SN=000177003338
refuses vtref0 blackpill "$PWD/tools/bench/bench-capture.sh" 'the probe reads VTref 0 V' STUB_VTREF=0.000
SN=
calls=$(wc -l < "$TMP/jlink.calls" 2>/dev/null)
[ "${calls:-0}" -eq 1 ] \
    || bad "a probe reading VTref 0 V is connected ${calls:-0} times, not once: $(cat "$TMP/jlink.calls" 2>/dev/null)"

# picopi's own USB CDC console, a node under the KickOS by-id glob that sends its identity rows
# once first opened and logs every open. A pty keeps what an opener left unread where
# cdc_acm discards it at the last close, so the count of opens is what tells a reader that loses
# the identity from one that keeps it.
mkdir -p "$TMP/cdc" "$TMP/cdcdev"
cp "$TMP"/bus/* "$TMP/cdc/"
sed "s|^KICKOS_CDC_CONSOLE=.*|KICKOS_CDC_CONSOLE=\"$TMP/cdcdev/usb-KickOS_KickOS_console_*-if00\"|" \
    tools/bench/board-rows.sh > "$TMP/cdc/board-rows.sh"
grep -qF "$TMP/cdcdev/" "$TMP/cdc/board-rows.sh" || fail "the planted CDC console glob was not set"
CDC_TITLE='   KickOS 0.5.1  -  microkernel RTOS'
python3 -c 'import ctypes, os, pty, select, sys, termios, time
libc = ctypes.CDLL(None, use_errno=True)
m, s = pty.openpty()
name = os.ttyname(s)
os.close(s)
ino = libc.inotify_init1(os.O_NONBLOCK)
if ino < 0 or libc.inotify_add_watch(ino, name.encode(), 0x20) < 0:
    sys.exit("no inotify watch on " + name)
os.symlink(name, sys.argv[1] + "/usb-KickOS_KickOS_console_planted-if00")
sent = False
while os.path.isdir(sys.argv[1]):
    if select.select([ino], [], [], 0.05)[0]:
        n = len(os.read(ino, 4096)) // 16
        with open(sys.argv[1] + ".opens", "a") as f:
            f.write("open\n" * n)
        if not sent:
            # As the console does: held from the open until the reader sets its line, 0.5 s at most.
            end = time.monotonic() + 0.5
            while termios.tcgetattr(m)[3] & termios.ICANON and time.monotonic() < end:
                time.sleep(0.001)
            os.write(m, ("\r\n" + sys.argv[2] + "\r\n   board   picopi\r\n[planted] after the rows\r\n").encode())
            sent = True' "$TMP/cdcdev" "$CDC_TITLE" 2> "$TMP/cdcdev.err" &
CDC_PID=$!
n=0
until [ -L "$TMP/cdcdev/usb-KickOS_KickOS_console_planted-if00" ]; do
    n=$((n + 1))
    [ "$n" -lt 500 ] || fail "the planted CDC console never appeared: $(cat "$TMP/cdcdev.err")"
    sleep 0.01
done
plant_bus 2e8a:0003
if ! env ROOT="$TMP/root" KICKOS_RIG="$TMP/rig.conf" PATH="$TMP/bin:$PATH" CAP_SECS=1 \
       CONSOLE_USB_CDC=1 EXPECT_COMMIT=0123abcd EXPECT_ARCH=armv6m BENCH_HOST= PEER_ERASE= \
       PYBIN=python3 bash "$TMP/cdc/bench-capture.sh" picopi planted "$TMP/image" "$TMP/cdc.log" \
       > "$TMP/cdc.out" 2>&1; then
    bad "the USB CDC capture is refused: $(grep REFUSING "$TMP/cdc.out")"
elif ! tr -d '\r' < "$TMP/cdc.log" | grep -qxF "$CDC_TITLE"; then
    bad "the USB CDC capture does not carry the identity rows the console sent first: $(head -c 200 "$TMP/cdc.log")"
fi
# A reader that leaves the line discipline cooked receives each CR as a newline.
crs=$(tr -cd '\r' < "$TMP/cdc.log" | wc -c)
[ "$crs" -eq 4 ] \
    || bad "the USB CDC capture carries $crs of the 4 CRs the console sent: the reader's tty was not raw when the first bytes arrived"
opens=$(grep -c . "$TMP/cdcdev.opens" 2>/dev/null)
[ "${opens:-0}" -eq 1 ] \
    || bad "the USB CDC reader opened its console ${opens:-0} times, not once: cdc_acm discards what an opener received when it closes, so an open ahead of the reader's own loses the identity rows the device sends first"
gone "$TMP/cdcdev/" || bad "a reader of the USB CDC console outlives its capture"
kill "$CDC_PID" 2>/dev/null
wait "$CDC_PID" 2>/dev/null

# tools/flash-picotool.sh over a planted picotool and sysfs: the USB serial it is given selects that
# device's bus and address for the erase and the load, a serial not on the bus and two RP devices
# with no serial are refused by name before anything is written.
mkdir -p "$TMP/pt/tools" "$TMP/pt/boards" "$TMP/ptbin" "$TMP/ptsys/a" "$TMP/ptsys/b"
cp -R boards/picopi "$TMP/pt/boards/"
cp tools/flash-common.sh "$TMP/pt/tools/"
sed "s|/sys/bus/usb/devices|$TMP/ptsys|g" tools/flash-picotool.sh > "$TMP/pt/tools/flash-picotool.sh"
printf '2e8a\n' > "$TMP/ptsys/a/idVendor"
printf '5F04F8E7E07F809A\n' > "$TMP/ptsys/a/serial"
printf '7\n' > "$TMP/ptsys/a/busnum"
printf '47\n' > "$TMP/ptsys/a/devnum"
printf '2e8a\n' > "$TMP/ptsys/b/idVendor"
printf 'E0C9125B0D9B\n' > "$TMP/ptsys/b/serial"
printf '5\n' > "$TMP/ptsys/b/busnum"
printf '2\n' > "$TMP/ptsys/b/devnum"
cat > "$TMP/ptbin/picotool" <<'EOF'
#!/bin/sh
case "$1" in
    version) echo "picotool v2.2.0" ;;
    info)
        if [ -n "${STUB_MULTI:-}" ]; then
            printf 'Multiple RP-series devices in BOOTSEL mode found:\n\nRP2350 device at bus 7, address 47:\n\nRP2040 device at bus 5, address 2:\n'
        fi
        ;;
    *) echo "$*" >> "$STUB_CALLS" ;;
esac
EOF
chmod +x "$TMP/ptbin/picotool"
ptrun() {
    rm -f "$TMP/pt.calls"
    env "$@" STUB_CALLS="$TMP/pt.calls" FLASH_IMAGE="$TMP/image" FLASH_ERASE_RANGES="0x10100000:0x10101000" \
        PATH="$TMP/ptbin:$PATH" bash "$TMP/pt/tools/flash-picotool.sh" picopi planted > "$TMP/pt.out" 2>&1
}
if ! ptrun STUB_MULTI=1 PICOTOOL_SN=E0C9125B0D9B; then
    bad "flash-picotool refuses two RP devices although a serial names the board's: $(cat "$TMP/pt.out")"
fi
[ "$(grep -c -e '--bus 5 --address 2$' "$TMP/pt.calls")" -eq 2 ] \
    || bad "flash-picotool does not select the device by serial for both the erase and the load: $(cat "$TMP/pt.calls")"
if ptrun STUB_MULTI=1 PICOTOOL_SN=0123456789AB; then
    bad "flash-picotool takes a serial that no device on the bus carries"
elif [ -e "$TMP/pt.calls" ]; then
    bad "flash-picotool wrote to a device although its serial is not on the bus: $(cat "$TMP/pt.calls")"
fi
if ptrun STUB_MULTI=1; then
    bad "flash-picotool accepts two RP devices in BOOTSEL with no serial"
elif ! grep -q 'more than one RP device is in BOOTSEL' "$TMP/pt.out"; then
    bad "flash-picotool refuses two RP devices, but not by name: $(cat "$TMP/pt.out")"
elif [ -e "$TMP/pt.calls" ]; then
    bad "flash-picotool wrote to a device it was not told: $(cat "$TMP/pt.calls")"
fi
ptrun || bad "flash-picotool refuses one RP device with no serial: $(cat "$TMP/pt.out")"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: a refused capture leaves no console reader and no stamper running"
