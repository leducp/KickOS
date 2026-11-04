#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Replay the RX emutls bump allocator OFFLINE, against a linked image, and refuse one whose
# thread_local set does not fit the block user/src/arch/rxv3/emutls.cc reserves, or whose image
# header states a thread-local span past the segment, which the boot check panics on, or a
# template the image does not store.
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
# The images given must replay one thread_local object at least, or the fit was shown for nothing.
#
# usage: rx_tls_fit.py <nm> <objdump> <image-or-directory>...

import os
import re
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


def image_bytes(objdump, elf, sections):
    mem = {}
    flags = []
    for section in sections:
        flags += ["-j", section]
    out = subprocess.run([objdump, "-s"] + flags + [elf], capture_output=True, text=True).stdout
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


def spans(s):
    """[(what, control blocks' start, end, block bytes, sections holding them)]: each image's
    where every image is linked on its own first, its block its span of the thread-local
    segment; the one image's otherwise, its block the .tbss the override reserves."""
    images = sorted(int(m.group(1)) for m in
                    (re.fullmatch(r"___kickos_image(\d+)_emutls_start", k) for k in s) if m)
    if images:
        out = []
        for n in images:
            need = ("___kickos_image%d_emutls_end" % n, "___kickos_image%d_tls_total" % n)
            for k in need:
                if k not in s:
                    return None, f"{k} is undefined, so the replay would pass vacuously"
            out.append(("image %d" % n, s["___kickos_image%d_emutls_start" % n],
                        s["___kickos_image%d_emutls_end" % n], s["___kickos_image%d_tls_total" % n],
                        [".kickos_image%d_data" % n]))
        return out, None
    need = ("___kickos_emutls_v_start", "___kickos_emutls_v_end",
            "___kickos_tbss_start", "___kickos_tbss_end")
    for n in need:
        if n not in s:
            return None, f"{n} is undefined, so the replay would pass vacuously"
    return [("the image", s["___kickos_emutls_v_start"], s["___kickos_emutls_v_end"],
             s["___kickos_tbss_end"] - s["___kickos_tbss_start"], [".appdata", ".data"])], None


def segment_findings(elf, s):
    """Each image's span [tls_offset, tls_offset + tls_total) inside the thread-local segment,
    the bound kernel/task/image_check.cc refuses an image header for at boot."""
    seg = ("___kickos_tls_segment_start", "___kickos_tls_segment_end")
    images = sorted(int(m.group(1)) for m in
                    (re.fullmatch(r"___kickos_image(\d+)_tls_offset", k) for k in s) if m)
    if not images:
        return []
    for k in seg:
        if k not in s:
            return [f"{elf}: {k} is undefined, so no image's span can be held to the segment"]
    segment = s[seg[1]] - s[seg[0]]
    findings = []
    for n in images:
        off = s["___kickos_image%d_tls_offset" % n]
        total = s.get("___kickos_image%d_tls_total" % n, 0)
        if off > segment or total > segment - off:
            findings.append(f"{elf}: image {n}'s thread-local span 0x{off:x}+0x{total:x} is past "
                            f"the {segment}-byte segment, which the boot check refuses at "
                            f"tls_offset")
    return findings


def template_findings(objdump, elf, s):
    """Each image's template, the tls_init bytes the kernel copies into every thread's block,
    in a thread-local section the image stores. A .tdata emitted with no contents is not
    written, and the sections ld places over it are what the copy then reads."""
    out = subprocess.run([objdump, "-h", elf], capture_output=True, text=True).stdout
    lines = out.splitlines()
    secs = []
    for i, line in enumerate(lines):
        p = line.split()
        if len(p) >= 4 and p[0].isdigit() and i + 1 < len(lines):
            if "THREAD_LOCAL" in lines[i + 1]:
                secs.append((int(p[3], 16), int(p[2], 16), "CONTENTS" in lines[i + 1]))
    findings = []
    for k in sorted(s):
        m = re.fullmatch(r"___kickos_image(\d+)_tls_init", k)
        if not m or s[k] == 0:
            continue
        n = int(m.group(1))
        at = s.get("___kickos_image%d_tls_template" % n)
        if at is None:
            findings.append(f"{elf}: image {n} states {s[k]} template bytes and no template")
            continue
        held = [c for vma, size, c in secs if vma <= at and at + s[k] <= vma + size]
        if not any(held):
            findings.append(f"{elf}: image {n}'s {s[k]}-byte thread-local template at 0x{at:08x} "
                            f"is in no thread-local section the image stores, so every "
                            f"thread's block is seated from whatever lies at that address")
    return findings


def check(nm, objdump, elf, replayed):
    s = syms(nm, elf)
    found, refused = spans(s)
    if refused is not None:
        return [f"{elf}: {refused}"]
    findings = segment_findings(elf, s) + template_findings(objdump, elf, s)
    gathered = 0
    for what, vstart, vend, block, sections in found:
        gathered += (vend - vstart) // VREC
        findings += check_span(objdump, elf, what, vstart, vend, block, sections, replayed)
    if gathered == 0 and uses_emutls(nm, elf):
        # An empty gather and an ungathered one look identical from here. The image telling
        # us it calls the override is what separates them.
        findings.append(f"{elf}: reaches __emutls_get_address but the gathered control-block "
                        f"range is EMPTY. The linker rule that collects .data.__emutls_v is "
                        f"missing, so the override's index is meaningless and this replay would "
                        f"have passed on nothing.")
    return findings


def check_span(objdump, elf, what, vstart, vend, block, sections, replayed):
    count = (vend - vstart) // VREC
    if count == 0:
        return []
    if block == 0:
        return [f"{elf}: {what}: {count} thread_local object(s) and a ZERO-byte block. "
                f"--gc-sections drops the reservation when nothing anchors it."]
    mem = image_bytes(objdump, elf, sections)
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
        # and padding makes the peak depend on it. One object costs its size plus the run-up
        # its alignment can demand, which is align - 1 bytes BEFORE it: rounding the size up
        # to a multiple of align instead charges {1,1} then {8,8} nine bytes where the
        # reverse order needs sixteen, and the gate then passes a layout that panics.
        want += size + align - 1
    if want > block:
        findings.append(
            f"{elf}: {count} thread_local object(s) need up to {want} bytes of a {block}-byte "
            f"block in the worst first-touch order. Raise KICKOS_RX_TLS_BLOCK in "
            f"user/src/arch/rxv3/emutls.cc; on RX this is a first-touch PANIC and not a link error, "
            f"on a board nothing here can run.")
        return findings
    print(f"    {elf.rsplit('/', 1)[-1]}, {what}: {count} object(s), worst-order peak {want} of "
          f"{block}, headroom {block - want}")
    replayed.append(count)
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
    replayed = []
    for elf in sorted(targets):
        findings += check(nm, objdump, elf, replayed)
    if not findings and not replayed:
        findings.append(f"not one of {len(targets)} image(s) gathers a thread_local control "
                        f"block, so no fit was replayed")
    for f in findings:
        print(f"FAIL: {f}")
    if findings:
        return 1
    print(f"PASS: {len(targets)} RX image(s) fit their thread_local block, "
          f"{sum(replayed)} object(s) replayed in {len(replayed)}")
    return 0


sys.exit(main())
