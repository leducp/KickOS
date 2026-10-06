#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ESP reset-into-run + capture on ONE serial handle (so no boot output is lost to a
# separate reset step). Usage: cap_esp.py <port> <out> <secs> [until-regex]
#
# <out>.times receives one row per received line, `<seconds>\t<line>`, the seconds being host
# monotonic time from the reset pulse to the arrival of the line's last byte.
import sys, time, re
import serial

def main():
    port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
    until = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
    s = serial.Serial(port, 115200, timeout=0.2)
    # Boot the app (not the download ROM): GPIO0/boot high (DTR inactive), then pulse
    # EN via RTS (low -> high) to reset into run. CH343P auto-reset wiring.
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.1)
    s.setRTS(False)
    buf = bytearray()
    pending = bytearray()
    t0 = time.monotonic()
    with open(out, 'wb') as f, open(out + '.times', 'wb') as times:
        while time.monotonic() - t0 < secs:
            # Read what has arrived rather than a fixed count, or a line's arrival is stamped
            # up to one read timeout late.
            c = s.read(max(1, s.in_waiting))
            if c:
                now = time.monotonic() - t0
                f.write(c)
                f.flush()
                buf += c
                pending += c
                while b'\n' in pending:
                    line, _, rest = pending.partition(b'\n')
                    times.write(b'%.6f\t' % now + line.rstrip(b'\r') + b'\n')
                    pending = bytearray(rest)
                times.flush()
                if until is not None and until.search(buf.decode('latin-1')):
                    break
    s.close()

if __name__ == '__main__':
    main()
