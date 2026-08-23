#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Replay the RX emutls bump allocator OFFLINE, against a linked image, and refuse one whose
# thread_local set does not fit the block arch/rx/rxv3/emutls.cc reserves.
#
# WHY OFFLINE. QEMU has no rxv3 machine, so exhaustion is a runtime PANIC on the first
# thread_local access on silicon and a too-small block is otherwise a green build and a dead
# board. Every input the allocator uses is a link-time constant, which is what makes the replay
# exact rather than a model: the control blocks are gathered by the linker script into
# __kickos_emutls_v, and the block size is the .tbss the override reserves.
#
# WHAT IT CANNOT SEE: whether the emitted code still bounds-checks the anchor. Removing those
# checks builds clean and passes every gate in the tree.
#
# usage: rx_tls_fit.py <nm> <objdump> <image-or-directory>...

import os
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


def uses_emutls(nm, elf):
    out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
    return "___emutls_get_address" in out


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
        # An empty gather and an ungathered one look identical from here. The image telling
        # us it calls the override is what separates them.
        if uses_emutls(nm, elf):
            return [f"{elf}: reaches __emutls_get_address but the gathered control-block "
                    f"range is EMPTY. The linker rule that collects .data.__emutls_v is "
                    f"missing, so the override's index is meaningless and this replay would "
                    f"have passed on nothing."]
        return []
    if block == 0:
        return [f"{elf}: {count} thread_local object(s) and a ZERO-byte block. "
                f"--gc-sections drops the reservation when nothing anchors it."]
    mem = image_bytes(objdump, elf)
    base = (HDR + SLOT * count + PAD - 1) & ~(PAD - 1)
    want = base
    findings = []
    for i in range(count):
        o = vstart + VREC * i
        if o not in mem:
            findings.append(f"{elf}: control block {i} at 0x{o:08x} is not in a loaded "
                            f"section, so the replay cannot read it")
            return findings
        size, align = rd32(mem, o), rd32(mem, o + 4)
        # The same two the override refuses at runtime. Accepting them here would call a
        # layout fine that panics on its first touch.
        if align == 0 or (align & (align - 1)) != 0:
            findings.append(f"{elf}: thread_local object {i} has alignment {align}, which is "
                            f"not a power of two. The override refuses it at first touch.")
            return findings
        if align > PAD:
            findings.append(f"{elf}: thread_local object {i} has alignment {align}, above the "
                            f"{PAD} the override accepts. It would panic on first touch.")
            return findings
        # ORDER-INDEPENDENT BOUND. First touch order is the program's, not the index order,
        # and padding makes the peak depend on it. Charging every object its full run-up is
        # >= any order, so a pass here holds whatever order the program uses.
        want += (size + align - 1) & ~(align - 1)
    if want > block:
        findings.append(
            f"{elf}: {count} thread_local object(s) need up to {want} bytes of a {block}-byte "
            f"block in the worst first-touch order. Raise KICKOS_RX_TLS_BLOCK in "
            f"arch/rx/rxv3/emutls.cc; on RX this is a first-touch PANIC and not a link error, "
            f"on a board nothing here can run.")
        return findings
    print(f"    {elf.rsplit('/', 1)[-1]}: {count} object(s), worst-order peak {want} of "
          f"{block}, headroom {block - want}")
    return findings


def main():
    if len(sys.argv) < 4:
        print("usage: rx_tls_fit.py <nm> <objdump> <image.elf>...", file=sys.stderr)
        return 2
    nm, objdump = sys.argv[1], sys.argv[2]
    targets = []
    for arg in sys.argv[3:]:
        if os.path.isdir(arg):
            # EVERY RX image, not one. A consumer app declaring more thread_locals than the
            # witness is exactly the case a single-image gate would miss.
            for root, _dirs, names in os.walk(arg):
                for n in names:
                    f = os.path.join(root, n)
                    if os.access(f, os.X_OK) and not os.path.isdir(f):
                        with open(f, "rb") as fh:
                            if fh.read(4) == b"\x7fELF":
                                targets.append(f)
        else:
            targets.append(arg)
    if not targets:
        print("FAIL: no images given, so every check below would pass vacuously")
        return 1
    findings = []
    for elf in sorted(targets):
        findings += check(nm, objdump, elf)
    for f in findings:
        print(f"FAIL: {f}")
    if findings:
        return 1
    print(f"PASS: {len(targets)} RX image(s) fit their thread_local block")
    return 0


sys.exit(main())
