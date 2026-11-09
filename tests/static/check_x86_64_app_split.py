#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""The x86_64 image's link rules (docs/design-m10-kernel-share.md section 1.2).

An application image is the kernel's PE32+ link holding programs linked on their own at the fixed
user base (<image>.kimg/image<n>.elf, tools/image-link.py). Firmware loads the PE image anywhere and
applies its DIR64 records; every space maps its program's bytes at the addresses the program was
linked at. So over every application image the tree links this refuses:

  - an embedded program whose bytes in the PE image are not the program's own loadable bytes:
    image 0's not at the program's own offsets from its first byte, or with less room than its
    data, bss and heap span, a later image's not in its text and window pages at its
    __kickos_image<n>_store in .kimgs;
  - a DIR64 record inside the embedded program, which firmware would apply to bytes every space
    maps at their link address;
  - an absolute word the kernel does not relocate, or a relocation record with no absolute word
    behind it: firmware moves exactly the DIR64 records when it loads the image away from its
    preferred base, so each R_X86_64_64 the kept, allocated input sections carry against a
    section-relative target must be one, one to one, a relocation against an ABSOLUTE symbol
    producing none because its value is the same at every base. An unpatched ld -m i386pep emits
    no record for a word naming a WEAK definition, which only a load away from the preferred
    base shows, and the KickOS toolchain's binutils patch is what gives it one
    (docs/design-m10-toolchain.md section 5.5);
  - an absolute relocation narrower than 64 bits, which no record can move, and a relocation type
    this checker does not know;
  - a kernel reference to a program address that is not an absolute word, or that
    tests/static/x86_64_apphalf_allowlist.txt does not name, and an allowlist entry no image
    references;
  - a symbol the linker script assigns whose value in the image is not the one the map states:
    ld -m i386pep drops an empty output section after assigning its symbols and re-bases them
    onto the image's first section; and one assigned in an output section holding nothing but
    image 0's bss, the later images' stores or the constructors, which a link without them
    drops.

A PE32+ symbol table holds no 64-bit value, so the program addresses the kernel's link states
are read from the fragment that states them, <image>.kimg/kickos_image0.ld, and any other symbol
the table dropped from the map, absolute where its expression is.

usage: check_x86_64_app_split.py --readelf R --objdump O --nm N --allowlist F
           (--tree <build-dir> | <image.efi>...)
       check_x86_64_app_split.py --controls
--tree takes every application image under <build-dir> that has a map beside it.
Each image's map is <image.efi>.map, which tools/x86_64-link.sh writes.
"""

import argparse
import os
import re
import struct
import subprocess
import sys

PCREL = {
    "R_X86_64_PC8", "R_X86_64_PC16", "R_X86_64_PC32", "R_X86_64_PC64", "R_X86_64_PLT32",
    "R_X86_64_GOTPCREL", "R_X86_64_GOTPCRELX", "R_X86_64_REX_GOTPCRELX",
}
ABS64 = "R_X86_64_64"
NARROW_ABS = {"R_X86_64_8", "R_X86_64_16", "R_X86_64_32", "R_X86_64_32S"}
NOOP = {"R_X86_64_NONE"}
PARTS = (".kimg0t", ".kimg0d", ".kimg0b")
STORE = ".kimgs"
PAGE = 0x1000

# ---------------------------------------------------------------------------------------------
# The rules, over plain data so the controls below can drive them without a link.


class Window:
    def __init__(self, image_lo, image_hi, embed_lo, embed_hi, prog_lo, prog_hi):
        self.image_lo, self.image_hi = image_lo, image_hi
        self.embed_lo, self.embed_hi = embed_lo, embed_hi
        self.prog_lo, self.prog_hi = prog_lo, prog_hi

    # Closed at the top: the one-past-the-end address is the program's too (_kickos_heap_limit).
    def program(self, a):
        return self.prog_lo <= a <= self.prog_hi

    def embedded(self, a):
        return self.embed_lo <= a < self.embed_hi

    def image(self, a):
        return self.image_lo <= a < self.image_hi


def judge(relocs, dir64_sites, win, allow):
    """relocs: (site, type, target, target_kind, label). target_kind is 'addr' (in the PE
    image), 'program', 'absolute' or 'undefined'. Answers the findings and the allowlist names
    used."""
    findings = []
    used = set()
    expected = set()
    for site, rtype, target, kind, label in relocs:
        if rtype in NOOP:
            continue
        if win.embedded(site):
            findings.append(f"a relocation at {site:#x} ({rtype}) sits in the embedded program")
            continue
        if rtype in NARROW_ABS:
            findings.append(f"kernel word at {site:#x} is {rtype} against {label}: no record "
                            f"can move a narrower absolute")
            continue
        if rtype != ABS64 and rtype not in PCREL:
            findings.append(f"kernel relocation at {site:#x} has type {rtype}, unknown here")
            continue
        if kind == "program":
            name = label.split(" ")[0]
            if rtype != ABS64:
                findings.append(f"kernel code at {site:#x} reaches the program address {label} "
                                f"PC-relatively, which a load away from the preferred base "
                                f"moves")
            elif name in allow:
                used.add(name)
            else:
                findings.append(f"kernel code at {site:#x} names the program address {label}, "
                                f"which the allowlist does not name")
            continue
        if rtype == ABS64 and kind == "addr":
            expected.add(site)
    actual = {s for s in dir64_sites if win.image(s)}
    for s in sorted(actual):
        if win.embedded(s):
            findings.append(f"a DIR64 record at {s:#x} is in the embedded program, which every "
                            f"space maps at its link address")
    for s in sorted(expected - actual):
        findings.append(f"the kernel's absolute word at {s:#x} has no DIR64 record, so a load "
                        f"away from the preferred base would leave it at the link address")
    for s in sorted(actual - expected):
        if not win.embedded(s):
            findings.append(f"a DIR64 record at {s:#x} has no absolute relocation behind it")
    return findings, used


def judge_embed(parts, segments, prog_lo, prog_hi):
    """parts: {name: (vma, size, bytes or None)} of the PE image; segments: [(vaddr, filesz,
    bytes)] of the program's loadable segments, text first. Answers the findings."""
    findings = []
    missing = [p for p in PARTS[:2] if p not in parts]
    if missing:
        return [f"the image holds no {' '.join(missing)}, so it embeds no program"]
    load = parts[PARTS[0]][0]
    if len(segments) != 2:
        return [f"the program has {len(segments)} loadable segment(s), not a text and a data"]
    for (vaddr, filesz, data), name in zip(segments, PARTS[:2]):
        vma, size, raw = parts[name]
        if vma - load != vaddr - prog_lo:
            findings.append(f"{name} is at {vma - load:#x} from the image's first byte, and the "
                            f"program put that part at {vaddr - prog_lo:#x}")
        elif raw is None or raw[:filesz] != data:
            findings.append(f"{name} does not hold the program's {filesz:#x} byte(s) at "
                            f"{vaddr:#x}")
    vma, size, _ = parts.get(PARTS[2], parts[PARTS[1]])
    if vma + size - load < prog_hi - prog_lo:
        findings.append(f"the embedded program ends {vma + size - load:#x} past its first byte, "
                        f"short of the {prog_hi - prog_lo:#x} its data, bss and heap span")
    return findings


def judge_store(n, store, at, segments, spans):
    """A later image n: store is (vma, bytes) of the PE's .kimgs, `at` the image's
    __kickos_image<n>_store, segments as judge_embed's, spans its ((rom_lo, rom_hi),
    (sram_lo, sram_hi)), each laid on pages of its own from `at` in that order."""
    findings = []
    vma, blob = store
    if blob is None:
        return [f"{STORE} carries no bytes, so image {n} is not embedded"]
    chunks = []
    off = at - vma
    for lo, hi in spans:
        base = lo & ~(PAGE - 1)
        end = (hi + PAGE - 1) & ~(PAGE - 1)
        chunks.append((base, end, off))
        off += end - base
    if off > len(blob):
        return [f"image {n}'s text and window run past the end of {STORE}"]
    for vaddr, filesz, data in segments:
        for a in range(vaddr, vaddr + filesz, PAGE):
            b = min(vaddr + filesz, (a & ~(PAGE - 1)) + PAGE)
            own = [c for c in chunks if c[0] <= a < c[1]]
            if not own:
                findings.append(f"image {n} loads {a:#x}, which neither its text nor its window "
                                f"holds")
                break
            base, _, coff = own[0]
            if blob[coff + a - base:coff + b - base] != data[a - vaddr:b - vaddr]:
                findings.append(f"{STORE} does not hold image {n}'s byte(s) at {a:#x}")
                break
    return findings


# ---------------------------------------------------------------------------------------------
# Reading one image: its map, its relocation records, its symbols and its inputs' relocations.

OUT_RE = re.compile(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
IN_RE = re.compile(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
IN_NAME_RE = re.compile(r"^ (\.\S+)\s*$")
IN_CONT_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
ASSIGN_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+(\w+) = (.*)$")
ABS_EXPR_RE = re.compile(r"^(ABSOLUTE \(|0x[0-9a-f]+$)")
FRAG_ABS_RE = re.compile(r"^(\w+) = 0x([0-9a-f]+);$")


def allocated(out):
    return not out.startswith((".debug", ".zdebug", ".stab"))


def parse_map(text, absolute=None):
    """Placed input sections of allocated output sections, (file, section) -> address, and the
    script's symbol assignments; `absolute` takes the names whose expression is absolute."""
    placed = {}
    assigned = {}
    if absolute is None:
        absolute = set()
    # No heading is read: ld localises them. An input line before the first output section,
    # the discarded list's, has no section to be placed in and is skipped.
    out = None
    pending = None
    for line in text.splitlines():
        m = OUT_RE.match(line)
        if m:
            out = m.group(1)
            pending = None
            continue
        m = IN_RE.match(line)
        if m:
            pending = None
            if out and allocated(out):
                placed[(m.group(4).strip(), m.group(1))] = int(m.group(2), 16)
            continue
        m = IN_NAME_RE.match(line)
        if m:
            pending = m.group(1)
            continue
        if pending:
            m = IN_CONT_RE.match(line)
            pending_name, pending = pending, None
            if m and out and allocated(out):
                placed[(m.group(3).strip(), pending_name)] = int(m.group(1), 16)
                continue
        m = ASSIGN_RE.match(line)
        if m:
            assigned[m.group(2)] = int(m.group(1), 16)
            if ABS_EXPR_RE.match(m.group(3)):
                absolute.add(m.group(2))
    return placed, assigned


def fragment_absolutes(text):
    """The program addresses a fragment states, at its top level."""
    return {m.group(1): int(m.group(2), 16)
            for m in (FRAG_ABS_RE.match(line) for line in text.splitlines()) if m}


def run(tool, *args):
    return subprocess.run([tool, *args], capture_output=True, text=True, check=True,
                          env={"LC_ALL": "C", "PATH": "/usr/bin:/bin"}).stdout


def image_dir64(objdump, image):
    text = run(objdump, "-p", image)
    base = int(re.search(r"^ImageBase\s+([0-9a-f]+)", text, re.M).group(1), 16)
    sites = []
    in_relocs = False
    for line in text.splitlines():
        if line.startswith("PE File Base Relocations"):
            in_relocs = True
            continue
        if in_relocs:
            m = re.search(r"\[([0-9a-f]+)\] (\S+)", line)
            if m and m.group(2) == "DIR64":
                sites.append(base + int(m.group(1), 16))
            elif m and m.group(2) != "ABSOLUTE":
                raise SystemExit(f"FAIL: {image}: a base-relocation record of type "
                                 f"{m.group(2)}, which firmware need not apply")
    return sites


def image_parts(objdump, image):
    """The embedded program's PE sections: {name: (vma, size, bytes or None)}."""
    parts = {}
    with open(image, "rb") as fh:
        blob = fh.read()
    lines = run(objdump, "-h", image).splitlines()
    for i, line in enumerate(lines):
        f = line.split()
        if len(f) >= 6 and (f[1] in PARTS or f[1] == STORE):
            vma, size, off = int(f[3], 16), int(f[2], 16), int(f[5], 16)
            loaded = i + 1 < len(lines) and "CONTENTS" in lines[i + 1]
            parts[f[1]] = (vma, size, blob[off:off + size] if loaded else None)
    return parts


def program_segments(path):
    """[(vaddr, filesz, bytes)] of an ELF64 executable's PT_LOADs, each from its first
    allocated section, since a segment may also load the file's own headers."""
    with open(path, "rb") as fh:
        elf = fh.read()
    phoff, shoff = struct.unpack_from("<QQ", elf, 0x20)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", elf, 0x36)
    starts = [struct.unpack_from("<IIQQ", elf, shoff + i * shentsize)
              for i in range(shnum)]
    starts = [s[3] for s in starts if s[2] & 2]
    segs = []
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", elf, phoff + i * phentsize)
        if p_type != 1 or p_memsz == 0:
            continue
        skip = min(a for a in starts if p_vaddr <= a < p_vaddr + p_memsz) - p_vaddr
        segs.append((p_vaddr + skip, p_filesz - skip, elf[p_offset + skip:p_offset + p_filesz]))
    return segs


def image_symbols(nm, image):
    syms = {}
    for line in run(nm, image).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = (int(parts[0], 16), parts[1])
    return syms


REL_SEC_RE = re.compile(r"^Relocation section '\.rela(\S+)'")
REL_ROW_RE = re.compile(r"^([0-9a-f]+)\s+[0-9a-f]+\s+(R_X86_64_\w+)\s+[0-9a-f]+\s+(.*?)\s*"
                        r"([+-])\s*([0-9a-f]+)$")


def file_relocs(readelf, path, cache):
    """(member file as the map names it, section) -> [(offset, type, symbol, addend)]."""
    if path in cache:
        return cache[path]
    rel = {}
    member = path
    section = None
    for line in run(readelf, "-rW", path).splitlines():
        if line.startswith("File: "):
            member = line[len("File: "):].strip()
            continue
        m = REL_SEC_RE.match(line)
        if m:
            section = m.group(1)
            continue
        m = REL_ROW_RE.match(line)
        if m and section:
            addend = int(m.group(5), 16) * (-1 if m.group(4) == "-" else 1)
            rel.setdefault((member, section), []).append(
                (int(m.group(1), 16), m.group(2), m.group(3).split("@")[0], addend))
    cache[path] = rel
    return rel


def archive_of(input_name):
    return input_name.split("(")[0] if input_name.endswith(")") else input_name


def reloc_target(base, addend, rtype):
    """Where a relocation lands: the symbol plus its addend, and for a PC-relative one the end
    of the 32-bit field, which the addend counts back from."""
    return base + addend + (4 if rtype in PCREL else 0)


def image_relocs(readelf, placed, syms, programs, win, cache, assigned=None, absolute=()):
    """`assigned` and `absolute` are the map's script symbols and those of them it states
    absolute, which stand for a symbol the PE32+ symbol table dropped."""
    assigned = assigned or {}
    absolutes = {n for n in assigned if n in absolute and n not in syms}
    relatives = {n for n in assigned if n not in absolute and n not in syms}
    relocs = []
    for (inp, sec), addr in placed.items():
        # The linker's own inputs ("linker stubs", the import table's) are no file to read.
        if not os.path.isfile(archive_of(inp)):
            continue
        for off, rtype, sym, addend in file_relocs(readelf, archive_of(inp), cache).get(
                (inp, sec), []):
            site = addr + off
            if sym.startswith(".") and (inp, sym) in placed:
                target = reloc_target(placed[(inp, sym)], addend, rtype)
                relocs.append((site, rtype, target, "addr", f"{sym}+{addend:#x} of {inp}"))
            elif sym in programs:
                kind = "absolute"
                if win.program(programs[sym]):
                    kind = "program"
                relocs.append((site, rtype, programs[sym], kind, sym))
            elif sym in absolutes:
                relocs.append((site, rtype, 0, "absolute", sym))
            elif sym in syms:
                value, kind = syms[sym]
                if kind in "Aa":
                    relocs.append((site, rtype, value, "absolute", sym))
                else:
                    relocs.append((site, rtype, reloc_target(value, addend, rtype), "addr",
                                   f"{sym} {addend:+#x}"))
            elif sym in relatives:
                # Stated relative to the image and dropped from the PE32+ symbol table as no
                # 32-bit value: moved by firmware.
                relocs.append((site, rtype, win.image_lo, "addr", sym))
            else:
                relocs.append((site, rtype, 0, "undefined", sym or "(none)"))
    return relocs


def check_image(args, image, allow, cache):
    absolute = set()
    map_text = open(image + ".map", errors="replace").read()
    placed, assigned = parse_map(map_text, absolute)
    program = image + ".kimg/image0.elf"
    fragment = image + ".kimg/kickos_image0.ld"
    for path in (program, fragment):
        if not os.path.isfile(path):
            return [f"no {path}, so there is no embedded program to check"], set()
    psyms = image_symbols(args.nm, program)
    for sym in ("__kickos_app_rom_start", "__kickos_app_sram_end"):
        if sym not in psyms:
            return [f"the program states no {sym}, so its span is unknown"], set()
    for sym in ("__kickos_image0_load_start", "__kickos_image0_load_end",
                "__kickos_images_load_end"):
        if sym not in assigned:
            return [f"the map states no {sym}, so the embedded programs have no bounds"], set()
    syms = image_symbols(args.nm, image)
    moved = moved_symbols(assigned, syms) + unanchored_symbols(map_text)
    if moved:
        return moved, set()
    prog_lo, prog_hi = psyms["__kickos_app_rom_start"][0], psyms["__kickos_app_sram_end"][0]
    parts = image_parts(args.objdump, image)
    findings = judge_embed(parts, program_segments(program), prog_lo, prog_hi)
    # Every later image, in its own pages of .kimgs; the program window spans them all.
    n = 1
    while os.path.isfile(f"{image}.kimg/image{n}.elf"):
        later = f"{image}.kimg/image{n}.elf"
        lsyms = image_symbols(args.nm, later)
        names = ("__kickos_app_rom_start", "__kickos_app_rom_end", "__kickos_app_sram_start",
                 "__kickos_app_sram_end")
        at = [a for (inp, sec), a in placed.items()
              if sec == ".kickos_images" and inp.endswith(f"/kickos_image{n}.o")]
        if any(name not in lsyms for name in names) or len(at) != 1 or STORE not in parts:
            findings.append(f"image {n}, {later}, states no span or has no place in {STORE}")
        else:
            v = [lsyms[name][0] for name in names]
            prog_lo, prog_hi = min(prog_lo, v[0]), max(prog_hi, v[3])
            findings += judge_store(n, (parts[STORE][0], parts[STORE][2]), at[0],
                                    program_segments(later),
                                    ((v[0], v[1]), (v[2], v[3])))
        n += 1
    lo = min(placed.values())
    hi = max(syms.get("__end__", (lo, "t"))[0], max(placed.values()) + 1)
    win = Window(lo & ~0xfff, hi, assigned["__kickos_image0_load_start"],
                 assigned["__kickos_images_load_end"], prog_lo, prog_hi)
    programs = fragment_absolutes(open(fragment).read())
    more, used = judge(image_relocs(args.readelf, placed, syms, programs, win, cache, assigned,
                                    absolute),
                       image_dir64(args.objdump, image), win, allow)
    return findings + more, used


def moved_symbols(assigned, syms):
    """The script's section-relative symbols whose image value is not the map's."""
    return [f"the script assigns {name} {addr:#x} and the image states {syms[name][0]:#x}: "
            f"its section was dropped and the symbol re-based"
            for name, addr in sorted(assigned.items())
            if name in syms and syms[name][1] not in "Aa" and syms[name][0] != addr]


DATA_RE = re.compile(r"^\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+(QUAD|SQUAD|LONG|SHORT|BYTE)\b")
# What one link has and another lacks: image 0's bss, the later images' stores and the
# constructors.
OPTIONAL_INPUT_RE = re.compile(r"^(\.kickos_image0_bss|\.kickos_images|\.init_array(\..*)?"
                               r"|\.kickos\.kernel\.init_array)$")


def unanchored_symbols(text):
    """The script's section-relative symbols assigned in an output section that holds nothing
    but inputs some link lacks: that link drops the section and re-bases them, so a window
    bound would not mean the same where its window is empty."""
    absolute = set()
    out = None
    held = {}
    where = {}
    pending = False
    for line in text.splitlines():
        m = OUT_RE.match(line)
        if m:
            out = m.group(1)
            held.setdefault(out, False)
            pending = False
            continue
        if line.startswith("/DISCARD/"):
            out = None
            continue
        if out is None:
            continue
        if DATA_RE.match(line):
            held[out] = True
            continue
        m = IN_RE.match(line)
        if m:
            pending = False
            if int(m.group(3), 16) and not OPTIONAL_INPUT_RE.match(m.group(1)):
                held[out] = True
            continue
        m = IN_NAME_RE.match(line)
        if m:
            pending = not OPTIONAL_INPUT_RE.match(m.group(1))
            continue
        m = IN_CONT_RE.match(line)
        if m and pending:
            pending = False
            if int(m.group(2), 16):
                held[out] = True
            continue
        pending = False
        m = ASSIGN_RE.match(line)
        if m and not ABS_EXPR_RE.match(m.group(3)):
            where[m.group(2)] = out
    return [f"the script assigns {name} in {sec}, which holds only inputs some link lacks: "
            f"that link drops {sec} and re-bases {name}"
            for name, sec in sorted(where.items()) if not held[sec]]


def load_allow(path):
    allow = {}
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if line:
            name, _, reason = line.partition(" ")
            if not reason.strip():
                raise SystemExit(f"FAIL: {path}: {name} carries no reason")
            allow[name] = reason.strip()
    return allow


# ---------------------------------------------------------------------------------------------
# Controls: each a minimal pair, the planted case refused and its clean twin passed.

def controls():
    win = Window(0x400000, 0x500000, 0x480000, 0x490000, 0x7f0000000000, 0x7f0000010000)
    prog = 0x7f0000000100
    cases = [
        ("a kernel pointer with no record",
         [(0x402000, ABS64, 0x401100, "addr", "kfn")], [], True),
        ("a kernel pointer with its record",
         [(0x402000, ABS64, 0x401100, "addr", "kfn")], [0x402000], False),
        ("a record with no kernel pointer", [], [0x402008], True),
        ("a record in the embedded program", [], [0x488000], True),
        ("a relocation in the embedded program",
         [(0x488000, ABS64, 0x401100, "addr", "kfn")], [0x488000], True),
        ("a kernel pointer to an absolute symbol, no record",
         [(0x402000, ABS64, 0, "absolute", "delta")], [], False),
        ("a narrow absolute in the kernel",
         [(0x401010, "R_X86_64_32S", 0x401100, "addr", "kfn")], [], True),
        ("an unknown type in the kernel",
         [(0x401010, "R_X86_64_TPOFF32", 0x401100, "addr", "kfn")], [], True),
        ("a kernel word naming an unlisted program address",
         [(0x402000, ABS64, prog, "program", "stranger")], [], True),
        ("a kernel word naming a listed program address",
         [(0x402000, ABS64, prog, "program", "listed")], [], False),
        ("kernel code reaching a listed program address PC-relatively",
         [(0x401010, "R_X86_64_PC32", prog, "program", "listed")], [], True),
        ("a kernel word naming the program's one-past-the-end",
         [(0x402000, ABS64, 0x7f0000010000, "program", "listed")], [], False),
    ]
    bad = 0
    for name, relocs, dir64, refused in cases:
        findings, _ = judge(relocs, dir64, win, {"listed": "control"})
        if bool(findings) != refused:
            print(f"FAIL: control '{name}': expected {'a refusal' if refused else 'a pass'}, "
                  f"got {findings or 'none'}")
            bad += 1
    text, data = b"T" * 0x30, b"D" * 0x10
    segs = [(0x7f0000000000, len(text), text), (0x7f0000001000, len(data), data)]
    good = {".kimg0t": (0x480000, 0x1000, text + b"\0" * (0x1000 - len(text))),
            ".kimg0d": (0x481000, 0x1000, data + b"\0" * (0x1000 - len(data))),
            ".kimg0b": (0x482000, 0xe000, None)}
    embeds = [
        ("the program at its own offsets", good, False),
        ("a data part moved off the program's offset",
         dict(good, **{".kimg0d": (0x482000, 0x1000, good[".kimg0d"][2])}), True),
        ("a text part holding other bytes",
         dict(good, **{".kimg0t": (0x480000, 0x1000, b"X" + good[".kimg0t"][2][1:])}), True),
        ("a bss part short of the heap",
         dict(good, **{".kimg0b": (0x482000, 0xd000, None)}), True),
        ("no bss part", {k: v for k, v in good.items() if k != ".kimg0b"}, True),
    ]
    for name, parts, refused in embeds:
        findings = judge_embed(parts, segs, 0x7f0000000000, 0x7f0000010000)
        if bool(findings) != refused:
            print(f"FAIL: control '{name}': expected {'a refusal' if refused else 'a pass'}, "
                  f"got {findings or 'none'}")
            bad += 1
    wrapped = (" .text._ZN6kickos4longE\n                0x0000000000465006       0x51 /x/a.o\n"
               " .text.short    0x0000000000465057       0x51 /x/lib.a(b.o)\n")
    placed, _ = parse_map(" .text.gone 0x0 0x10 /x/c.o\n.text 0x465000 0x1200\n" + wrapped +
                          ".debug_info 0x0 0x10\n .debug_info 0x0 0x10 /x/a.o\n")
    want = {("/x/a.o", ".text._ZN6kickos4longE"): 0x465006,
            ("/x/lib.a(b.o)", ".text.short"): 0x465057}
    if placed != want:
        print(f"FAIL: control 'map parse': got {placed}")
        bad += 1
    if fragment_absolutes(".kimg0t BLOCK(x) :\n{\n    __kickos_app_load_delta = . - 0x7f00;\n}\n"
                          "__kickos_app_rom_start = 0x7f0000000000;\n") \
            != {"__kickos_app_rom_start": 0x7f0000000000}:
        print("FAIL: control 'fragment parse': a relative symbol read as absolute, or the "
              "absolute one missed")
        bad += 1
    if not moved_symbols({"__kickos_image0_load_start": 0x468000},
                         {"__kickos_image0_load_start": (0x401000, "T")}):
        print("FAIL: control 'a re-based symbol': not refused")
        bad += 1
    if moved_symbols({"__kickos_image0_load_start": 0x468000, "__kickos_frame_pool_delta": 0},
                     {"__kickos_image0_load_start": (0x468000, "T"),
                      "__kickos_frame_pool_delta": (0, "A")}):
        print("FAIL: control 'a symbol where the map put it': refused")
        bad += 1
    stores = (".kimgs          0x466000     0x8000\n"
              " .kickos_images\n"
              "                0x466000     0x8000 /x/kickos_image1.o\n")
    if not unanchored_symbols(stores + "                0x46e000                "
                              "__kickos_images_load_end = ALIGN (0x1000)\n"):
        print("FAIL: control 'a window bound only the stores hold up': not refused")
        bad += 1
    if not unanchored_symbols(".kimg0b         0x465000     0x4268\n"
                              " .kickos_image0_bss\n"
                              "                0x465000     0x4268 /x/kickos_image0.o\n"
                              "                0x469268                __kickos_image0_load_end = .\n"):
        print("FAIL: control 'image 0's end only its bss holds up': not refused")
        bad += 1
    if unanchored_symbols(stores + ".rdata          0x46e000     0x2600\n"
                          "                0x46e000                __kickos_images_load_end = .\n"
                          "                0x46e000        0x8 QUAD 0x7f0000000000\n"):
        print("FAIL: control 'a window bound the image table holds up': refused")
        bad += 1
    # A named symbol's addend counts: a kernel pointer to a kernel symbol plus an offset that
    # lands in the embedded program still asks a record, and its twin as well.
    inp = os.path.abspath(__file__)
    for name, sym, value, refused in (("an absolute word naming an unlisted program symbol",
                                       "app_sym", 0x7f0000000100, True),
                                      ("an absolute word naming a kernel symbol",
                                       "k_sym", 0x401100, False)):
        cache = {inp: {(inp, ".data.p"): [(0, ABS64, sym, 0)]}}
        programs = {}
        syms = {sym: (value, "D")}
        if value >= 0x7f0000000000:
            programs, syms = {sym: value}, {}
        relocs = image_relocs(None, {(inp, ".data.p"): 0x402000}, syms, programs, win, cache)
        findings, _ = judge(relocs, [0x402000] if not programs else [], win, {})
        if bool(findings) != refused:
            print(f"FAIL: control '{name}': expected {'a refusal' if refused else 'a pass'}, "
                  f"got {findings or 'none'}")
            bad += 1
    total = len(cases) + len(embeds) + 9
    print(f"== controls: {total - bad} of {total} as expected ==")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("--readelf")
    ap.add_argument("--objdump")
    ap.add_argument("--nm")
    ap.add_argument("--allowlist")
    ap.add_argument("--tree")
    ap.add_argument("images", nargs="*")
    args = ap.parse_args()
    if args.tree:
        for root, _, files in os.walk(args.tree):
            args.images += sorted(os.path.join(root, f) for f in files
                                  if f.endswith(".efi") and f + ".map" in files)
    bad = controls()
    if args.controls:
        return 1 if bad else 0
    if not args.images:
        print("FAIL: no image named, so nothing was checked")
        return 1
    allow = load_allow(args.allowlist)
    cache = {}
    used_all = set()
    findings_all = 0
    for image in args.images:
        findings, used = check_image(args, image, allow, cache)
        used_all |= used
        for f in findings:
            print(f"FAIL: {image}: {f}")
        findings_all += len(findings)
    for name in sorted(set(allow) - used_all):
        print(f"FAIL: {args.allowlist}: {name} is referenced by no kernel code in any image "
              f"checked; drop it")
        findings_all += 1
    print(f"== {len(args.images)} image(s), {findings_all} finding(s) ==")
    return 1 if (bad or findings_all) else 0


if __name__ == "__main__":
    sys.exit(main())
