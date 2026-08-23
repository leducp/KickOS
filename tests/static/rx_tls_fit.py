#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Replay the RX emutls bump allocator OFFLINE, against a linked image, and refuse one whose
# thread_local set does not fit the block arch/rx/rxv3/emutls.cc reserves.
#
# WHY THIS EXISTS. rxv3 is the one backend whose thread pointer cannot be witnessed: there
# is no RX hardware on this bench and QEMU has no rxv3 machine. Exhaustion there is a
# runtime PANIC on the first thread_local access, on silicon nobody here can run, so without
# this a too-small block is a green build and a dead board. Every input the allocator uses
# is a link-time constant, which is what makes the replay exact rather than a model: the
# control blocks are gathered by the linker script into __kickos_emutls_v, and the block size
# is the .tbss the override reserves.
#
# WHAT IT CANNOT SEE: whether the emitted code still bounds-checks the anchor. Removing those
# checks builds clean and passes every gate in the tree, which is stated here because the
# mutation was tried.
#
# usage: rx_tls_fit.py <nm> <objdump> <image.elf>...

import subprocess
import sys

HDR = 4          # uint16_t brk + pad, emutls.cc
SLOT = 2         # uint16_t per object
PAD = 8          # the block's own alignment floor
VREC = 16        # sizeof(struct __emutls_object)


def syms(nm, elf):
    out = {}
    for line in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
    return out


def image_bytes(objdump, elf):
    mem = {}
    out = subprocess.run([objdump, "-s", "-j", ".appdata", "-j", ".data", elf],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        if not all(c in "0123456789abcdef" for c in parts[0]):
            continue
        if not 4 <= len(parts[0]) <= 8:
            continue
        addr = int(parts[0], 16)
        hexs = "".join(x for x in "".join(parts[1:5]) if x in "0123456789abcdef")
        for i in range(0, len(hexs), 2):
            mem[addr + i // 2] = int(hexs[i:i + 2], 16)
    return mem


def rd32(mem, a):
    return mem[a] | mem[a + 1] << 8 | mem[a + 2] << 16 | mem[a + 3] << 24


def check(nm, objdump, elf):
    s = syms(nm, elf)
    need = ("___kickos_emutls_v_start", "___kickos_emutls_v_end",
            "___kickos_tbss_start", "___kickos_tbss_end")
    for n in need:
        if n not in s:
            return [f"{elf}: {n} is undefined, so the replay would pass vacuously"]
    vstart, vend = s["___kickos_emutls_v_start"], s["___kickos_emutls_v_end"]
    block = s["___kickos_tbss_end"] - s["___kickos_tbss_start"]
    count = (vend - vstart) // VREC
    if count == 0:
        return []
    if block == 0:
        return [f"{elf}: {count} thread_local object(s) and a ZERO-byte block. "
                f"--gc-sections drops the reservation when nothing anchors it."]
    mem = image_bytes(objdump, elf)
    brk = (HDR + SLOT * count + PAD - 1) & ~(PAD - 1)
    findings = []
    for i in range(count):
        o = vstart + VREC * i
        if o not in mem:
            findings.append(f"{elf}: control block {i} at 0x{o:08x} is not in a loaded "
                            f"section, so the replay cannot read it")
            return findings
        size, align = rd32(mem, o), rd32(mem, o + 4)
        at = (brk + align - 1) & ~(align - 1)
        if at > block or size > block - at:
            findings.append(
                f"{elf}: thread_local object {i} (size {size}, align {align}) wants bytes "
                f"{at}..{at + size} of a {block}-byte block. Raise KICKOS_RX_TLS_BLOCK in "
                f"arch/rx/rxv3/emutls.cc; on RX this is a first-touch PANIC and not a link "
                f"error, on a board nothing here can run.")
            return findings
        brk = at + size
    print(f"    {elf.rsplit('/', 1)[-1]}: {count} object(s), peak {brk} of {block}, "
          f"headroom {block - brk}")
    return findings


def main():
    if len(sys.argv) < 4:
        print("usage: rx_tls_fit.py <nm> <objdump> <image.elf>...", file=sys.stderr)
        return 2
    nm, objdump = sys.argv[1], sys.argv[2]
    findings = []
    for elf in sys.argv[3:]:
        findings += check(nm, objdump, elf)
    for f in findings:
        print(f"FAIL: {f}")
    if findings:
        return 1
    print(f"PASS: {len(sys.argv) - 3} RX image(s) fit their thread_local block")
    return 0


sys.exit(main())
