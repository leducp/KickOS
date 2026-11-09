#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Copy a USB CDC console to stdout until the device leaves. Usage: cap_cdc.py <tty>
#
# A KickOS console holds its output from the open until the host sends a line coding
# (kickos/sys/usb_cdc_service.h), and cdc_acm sends one only when the speed differs from the
# last it sent. Raw is set at two speeds so that one of them always does, and the device then
# speaks only to a tty already raw: no byte is turned from CR to LF or echoed back to it.
import os
import sys
import termios


def raw(fd, speed):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = (attrs[2] & ~(termios.CSIZE | termios.PARENB | termios.HUPCL)) \
        | termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0  # lflag
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main():
    fd = os.open(sys.argv[1], os.O_RDONLY | os.O_NOCTTY)
    raw(fd, termios.B57600)
    raw(fd, termios.B115200)
    while True:
        try:
            data = os.read(fd, 4096)
        except OSError:
            break  # EIO once the device is gone
        if not data:
            break
        view = memoryview(data)
        while view:
            view = view[os.write(1, view):]


if __name__ == '__main__':
    main()
