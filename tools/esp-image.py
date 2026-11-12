# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The bootable esptool image of an ELF (kickos_emit_image in cmake/kickos.cmake):
#
#   python3 esp-image.py <esptool> <chip> <nm> <elf> <out> [<elf2image option>...]
#   python3 esp-image.py --offset <nm> <elf>
#
# --offset prints the flash offset an ELF's image is written at, the one place it is stated
# (__kickos_esp32_flash_image_offset in arch/xtensa/chip/esp32/esp32.ld).
#
# An ELF naming __kickos_esp32_flash_text_load keeps its task text in flash, where the ROM never
# loads: its program headers whose load address lies in the span __kickos_esp32_flash_text_start
# to _end are made PT_NULL in a copy esptool images, and the span's bytes, zero between its
# sections, follow that image in <out> at the load, counted from
# __kickos_esp32_flash_image_offset, the flash offset <out> is written at. A segment that runs
# elsewhere and loads in the span is a template the kernel copies from. Where the ELF also names
# __kickos_esp32_flash_window_start and _end, the addresses the cache maps, a segment loading
# there outside the span is refused: nothing would map it. The section headers are left alone:
# objcopy -R rewrites the program headers and loses the load addresses --use-segments reads.

import os
import struct
import subprocess
import sys

PT_NULL = 0
PT_LOAD = 1
SYMBOLS = ("__kickos_esp32_flash_text_start", "__kickos_esp32_flash_text_end",
           "__kickos_esp32_flash_text_load", "__kickos_esp32_flash_image_offset")
WINDOW = ("__kickos_esp32_flash_window_start", "__kickos_esp32_flash_window_end")


def fail(message):
    sys.stderr.write("esp-image: %s\n" % message)
    sys.exit(1)


def symbols(nm, elf):
    try:
        text = subprocess.run([nm, elf], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        fail("%s cannot read %s: %s" % (nm, elf, error))
    found = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 3 and (fields[2] in SYMBOLS or fields[2] in WINDOW):
            found[fields[2]] = int(fields[0], 16)
    return found


def program_headers(data):
    """[(index offset, type, offset, vaddr, paddr, filesz, memsz)] of a 32-bit little-endian ELF."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        fail("not a 32-bit little-endian ELF")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    rows = []
    for i in range(phnum):
        at = phoff + i * phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from("<IIIIII", data, at)
        rows.append((at, p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz))
    return rows


def esptool(command, args):
    try:
        subprocess.run(command + args, check=True, capture_output=True, text=True)
    except OSError as error:
        fail("%s: %s" % (command[0], error))
    except subprocess.CalledProcessError as error:
        fail("`%s` failed:\n%s%s" % (" ".join(command + args), error.stdout, error.stderr))


def main(argv):
    if len(argv) == 4 and argv[1] == "--offset":
        found = symbols(argv[2], argv[3])
        if "__kickos_esp32_flash_image_offset" not in found:
            fail("%s names no __kickos_esp32_flash_image_offset" % argv[3])
        print("0x%x" % found["__kickos_esp32_flash_image_offset"])
        return 0
    if len(argv) < 6:
        fail("usage: esp-image.py <esptool> <chip> <nm> <elf> <out> [<elf2image option>...]")
    tool, chip, nm, elf, out = argv[1:6]
    command = [tool, "--chip", chip, "elf2image"] + argv[6:]
    found = symbols(nm, elf)
    if "__kickos_esp32_flash_text_load" not in found:
        esptool(command, ["--output", out, elf])
        return 0
    if not all(name in found for name in SYMBOLS):
        fail("%s names a flash text load but not all of %s" % (elf, ", ".join(SYMBOLS)))
    start = found["__kickos_esp32_flash_text_start"]
    end = found["__kickos_esp32_flash_text_end"]
    load = found["__kickos_esp32_flash_text_load"]
    offset = found["__kickos_esp32_flash_image_offset"]
    window = None
    if any(name in found for name in WINDOW):
        if not all(name in found for name in WINDOW):
            fail("%s names a flash window but not both of %s" % (elf, ", ".join(WINDOW)))
        window = (found[WINDOW[0]], found[WINDOW[1]])
    with open(elf, "rb") as f:
        data = bytearray(f.read())
    text = bytearray(end - start)
    placed = []
    empty = []
    for at, p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz in program_headers(data):
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        if p_paddr >= end or p_paddr + p_memsz <= start:
            if window is not None and p_paddr < window[1] and p_paddr + p_memsz > window[0]:
                fail("%s: the load segment at 0x%X is in the flash window but outside its text 0x%X to 0x%X" % (elf, p_paddr, start, end))
            continue
        if p_paddr < start:
            fail("%s: the load segment at 0x%X starts below the flash text's start 0x%X" % (elf, p_paddr, start))
        if p_paddr + p_filesz > end:
            fail("%s: the load segment at 0x%X runs past the flash text's end 0x%X" % (elf, p_paddr, end))
        if p_vaddr == p_paddr and p_memsz != p_filesz:
            empty.append((p_paddr + p_filesz, p_paddr + p_memsz))
        if p_vaddr != p_paddr and p_filesz != 0:
            placed.append((p_paddr, p_paddr + p_filesz))
        text[p_paddr - start:p_paddr - start + p_filesz] = data[p_offset:p_offset + p_filesz]
        struct.pack_into("<I", data, at, PT_NULL)
    # Flash cannot be zeroed at boot: bytes a segment runs at with no file image must be a
    # template another segment loads there.
    for lo, hi in empty:
        for t_lo, t_hi in sorted(placed):
            if t_lo <= lo < t_hi:
                lo = t_hi
        if lo < hi:
            fail("%s: the load segment ending at 0x%X in the flash text has %d bytes with no file image" % (elf, hi, hi - lo))
    ram_elf = out + ".ram.elf"
    ram_bin = out + ".ram.bin"
    with open(ram_elf, "wb") as f:
        f.write(data)
    try:
        esptool(command, ["--output", ram_bin, ram_elf])
        with open(ram_bin, "rb") as f:
            image = f.read()
    finally:
        for path in (ram_elf, ram_bin):
            if os.path.exists(path):
                os.remove(path)
    if offset + len(image) > load:
        fail("%s: its boot image ends at flash 0x%X, past its text at 0x%X" % (elf, offset + len(image), load))
    with open(out, "wb") as f:
        f.write(image)
        f.write(b"\xff" * (load - offset - len(image)))
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
