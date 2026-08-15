#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ESP reset-into-run + capture on ONE serial handle (so no boot output is lost to a
# separate reset step). Usage: cap_esp.py <port> <out> <secs> [until-regex]
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
    t0 = time.monotonic()
    with open(out, 'wb') as f:
        while time.monotonic() - t0 < secs:
            c = s.read(4096)
            if c:
                f.write(c)
                f.flush()
                buf += c
                if until is not None and until.search(buf.decode('latin-1')):
                    break
    s.close()

if __name__ == '__main__':
    main()
