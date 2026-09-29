#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""The x86_64 app window's link rules (docs/design-m10-kernel-share.md section 1.3).

A task reaches the app window at the loader address plus the user offset and reaches nothing
else of the image, so over every application image the tree links this refuses:

  - an app relocation whose target is in the kernel's half, instruction or data, which would
    fault at run time where the kernel sits unmapped for the task;
  - an absolute word the image does not relocate, or a relocation record with no absolute word
    behind it: firmware moves exactly the DIR64 records when it loads the image away from its
    preferred base, and the boot adds the user offset to exactly those in the app window
    (arch/x86/x86_64/apprel_x86_64.cc), so each R_X86_64_64 the kept, allocated input sections
    carry must be one, one to one, a relocation against an ABSOLUTE symbol producing none
    because its value is the same at every base. ld -m i386pep emits no record for a WEAK
    reference to a weak definition, which only a load away from the preferred base shows;
  - an absolute relocation narrower than 64 bits in the app window, which no record can move,
    and a relocation type this checker does not know;
  - a kernel reference to an app symbol that tests/static/x86_64_apphalf_allowlist.txt does not
    name, the linker answering an app symbol at the kernel's view of it, and an allowlist entry
    no image references;
  - a symbol the linker script assigns whose value in the image is not the one the map states:
    ld -m i386pep drops an empty output section after assigning its symbols and re-bases them
    onto the image's first section, which put an app window's bounds on the kernel's .text.

usage: check_x86_64_app_split.py --readelf R --objdump O --nm N --allowlist F
           (--tree <build-dir> | <image.efi>...)
       check_x86_64_app_split.py --controls
--tree takes every application image under <build-dir> that has a map beside it.
Each image's map is <image.efi>.map, which cmake/x86_64_image.cmake writes.
"""

import argparse
import os
import re
import subprocess
import sys

PCREL = {
    "R_X86_64_PC8", "R_X86_64_PC16", "R_X86_64_PC32", "R_X86_64_PC64", "R_X86_64_PLT32",
    "R_X86_64_GOTPCREL", "R_X86_64_GOTPCRELX", "R_X86_64_REX_GOTPCRELX",
}
ABS64 = "R_X86_64_64"
NARROW_ABS = {"R_X86_64_8", "R_X86_64_16", "R_X86_64_32", "R_X86_64_32S"}
NOOP = {"R_X86_64_NONE"}

# ---------------------------------------------------------------------------------------------
# The rules, over plain data so the controls below can drive them without a link.


class Window:
    def __init__(self, image_lo, image_hi, app_lo, app_hi):
        self.image_lo, self.image_hi = image_lo, image_hi
        self.app_lo, self.app_hi = app_lo, app_hi

    # Closed at the top: the one-past-the-end address is the app's too (_kickos_heap_limit).
    def app(self, a):
        return self.app_lo <= a <= self.app_hi

    def kernel(self, a):
        return self.image_lo <= a < self.image_hi and not self.app(a)


def judge(relocs, dir64_sites, win, allow):
    """relocs: (site, type, target, target_kind, label). target_kind is 'addr', 'absolute' or
    'undefined'. Answers the findings and the allowlist names used."""
    findings = []
    used = set()
    expected = set()
    for site, rtype, target, kind, label in relocs:
        if rtype in NOOP:
            continue
        if win.app(site):
            if rtype in NARROW_ABS:
                findings.append(f"app word at {site:#x} is {rtype} against {label}: no record "
                                f"can move a narrower absolute")
                continue
            if rtype != ABS64 and rtype not in PCREL:
                findings.append(f"app relocation at {site:#x} has type {rtype}, unknown here")
                continue
            if kind == "addr" and win.kernel(target):
                findings.append(f"app relocation at {site:#x} ({rtype}) reaches {label} at "
                                f"{target:#x}, in the kernel's half")
            if rtype == ABS64 and kind == "addr":
                expected.add(site)
        elif win.kernel(site):
            if rtype == ABS64 and kind == "addr":
                expected.add(site)
            if kind == "addr" and win.app(target):
                name = label.split(" ")[0]
                if name in allow:
                    used.add(name)
                else:
                    findings.append(f"kernel code at {site:#x} names the app symbol {label}, "
                                    f"which the allowlist does not name")
    actual = {s for s in dir64_sites if win.image_lo <= s < win.image_hi}
    for s in sorted(expected - actual):
        where = "the app's" if win.app(s) else "the kernel's"
        findings.append(f"{where} absolute word at {s:#x} has no DIR64 record, so a load away "
                        f"from the preferred base would leave it at the link address")
    for s in sorted(actual - expected):
        findings.append(f"a DIR64 record at {s:#x} has no absolute relocation behind it")
    return findings, used


# ---------------------------------------------------------------------------------------------
# Reading one image: its map, its relocation records, its symbols and its inputs' relocations.

OUT_RE = re.compile(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
IN_RE = re.compile(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
IN_NAME_RE = re.compile(r"^ (\.\S+)\s*$")
IN_CONT_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
ASSIGN_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+(\w+) = ")


def allocated(out):
    return not out.startswith((".debug", ".zdebug", ".stab"))


def parse_map(text):
    """Placed input sections of allocated output sections, (file, section) -> address, and the
    script's symbol assignments."""
    placed = {}
    assigned = {}
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
    return placed, assigned


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
                                 f"{m.group(2)}, which the boot does not apply")
    return sites


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


def image_relocs(readelf, placed, syms, cache):
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
            elif sym in syms:
                value, kind = syms[sym]
                if kind in "Aa":
                    relocs.append((site, rtype, value, "absolute", sym))
                else:
                    relocs.append((site, rtype, reloc_target(value, addend, rtype), "addr",
                                   f"{sym} {addend:+#x}"))
            else:
                relocs.append((site, rtype, 0, "undefined", sym or "(none)"))
    return relocs


def check_image(args, image, allow, cache):
    placed, assigned = parse_map(open(image + ".map", errors="replace").read())
    for sym in ("__kickos_app_rom_start", "__kickos_app_sram_end"):
        if sym not in assigned:
            return [f"the map states no {sym}, so there is no app window to check"], set()
    syms = image_symbols(args.nm, image)
    moved = moved_symbols(assigned, syms)
    if moved:
        return moved, set()
    lo = min(placed.values())
    hi = max(syms.get("__end__", (lo, "t"))[0], max(placed.values()) + 1)
    win = Window(lo & ~0xfff, hi, assigned["__kickos_app_rom_start"],
                 assigned["__kickos_app_sram_end"])
    return judge(image_relocs(args.readelf, placed, syms, cache),
                 image_dir64(args.objdump, image), win, allow)


def moved_symbols(assigned, syms):
    """The script's section-relative symbols whose image value is not the map's."""
    return [f"the script assigns {name} {addr:#x} and the image states {syms[name][0]:#x}: "
            f"its section was dropped and the symbol re-based"
            for name, addr in sorted(assigned.items())
            if name in syms and syms[name][1] not in "Aa" and syms[name][0] != addr]


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
    win = Window(0x400000, 0x500000, 0x480000, 0x490000)
    cases = [
        ("an app call into the kernel",
         [(0x480010, "R_X86_64_PLT32", 0x401000, "addr", "kfn")], [], True),
        ("an app call inside the window",
         [(0x480010, "R_X86_64_PLT32", 0x480100, "addr", "afn")], [], False),
        ("an app pointer with no record",
         [(0x488000, ABS64, 0x480100, "addr", "afn")], [], True),
        ("an app pointer with its record",
         [(0x488000, ABS64, 0x480100, "addr", "afn")], [0x488000], False),
        ("a record with no app pointer", [], [0x488008], True),
        ("a kernel pointer with no record",
         [(0x402000, ABS64, 0x401100, "addr", "kfn")], [], True),
        ("a kernel pointer with its record",
         [(0x402000, ABS64, 0x401100, "addr", "kfn")], [0x402000], False),
        ("an app pointer to an absolute symbol, no record",
         [(0x488000, ABS64, 0, "absolute", "delta")], [], False),
        ("an app pointer into the kernel",
         [(0x488000, ABS64, 0x401000, "addr", "kdata")], [0x488000], True),
        ("a narrow absolute in the app window",
         [(0x480010, "R_X86_64_32S", 0x480100, "addr", "afn")], [], True),
        ("an unknown type in the app window",
         [(0x480010, "R_X86_64_TPOFF32", 0x480100, "addr", "afn")], [], True),
        ("kernel code naming an unlisted app symbol",
         [(0x401010, "R_X86_64_PC32", 0x480100, "addr", "stranger")], [], True),
        ("kernel code naming a listed app symbol",
         [(0x401010, "R_X86_64_PC32", 0x480100, "addr", "listed")], [], False),
        ("kernel code naming the window's one-past-the-end",
         [(0x401010, "R_X86_64_PC32", 0x490000, "addr", "listed")], [], False),
    ]
    bad = 0
    for name, relocs, dir64, refused in cases:
        findings, _ = judge(relocs, dir64, win, {"listed": "control"})
        if bool(findings) != refused:
            print(f"FAIL: control '{name}': expected {'a refusal' if refused else 'a pass'}, "
                  f"got {findings or 'none'}")
            bad += 1
    wrapped = (" .text._ZN6kickos4longE\n                0x0000000000465006       0x51 /x/a.o\n"
               " .text.short    0x0000000000465057       0x51 /x/lib.a(b.o)\n")
    placed, _ = parse_map(" .text.gone 0x0 0x10 /x/c.o\n.apptext 0x465000 0x1200\n" + wrapped +
                          ".debug_info 0x0 0x10\n .debug_info 0x0 0x10 /x/a.o\n")
    want = {("/x/a.o", ".text._ZN6kickos4longE"): 0x465006,
            ("/x/lib.a(b.o)", ".text.short"): 0x465057}
    if placed != want:
        print(f"FAIL: control 'map parse': got {placed}")
        bad += 1
    if not moved_symbols({"__kickos_app_sram_start": 0x468000},
                         {"__kickos_app_sram_start": (0x401000, "T")}):
        print("FAIL: control 'a re-based window symbol': not refused")
        bad += 1
    if moved_symbols({"__kickos_app_sram_start": 0x468000, "__kickos_frame_pool_delta": 0},
                     {"__kickos_app_sram_start": (0x468000, "T"),
                      "__kickos_frame_pool_delta": (0, "A")}):
        print("FAIL: control 'a symbol where the map put it': refused")
        bad += 1
    # A named symbol's addend counts: an app pointer to an app symbol plus an offset that lands
    # in the kernel is refused, and its twin landing inside the window passes.
    inp = os.path.abspath(__file__)
    for name, addend, refused in (("a named app symbol plus an addend into the kernel",
                                   -0x87f00, True),
                                  ("a named app symbol plus an addend inside the window",
                                   0x10, False)):
        cache = {inp: {(inp, ".data.p"): [(0, ABS64, "app_sym", addend)]}}
        relocs = image_relocs(None, {(inp, ".data.p"): 0x488000},
                              {"app_sym": (0x488100, "D")}, cache)
        findings, _ = judge(relocs, [0x488000], win, {})
        if bool(findings) != refused:
            print(f"FAIL: control '{name}': expected {'a refusal' if refused else 'a pass'}, "
                  f"got {findings or 'none'}")
            bad += 1
    total = len(cases) + 5
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
