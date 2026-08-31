#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# THE KERNEL'S OWN MAPPING IS NEVER WRITABLE AND EXECUTABLE AT ONCE. The kernel window is one
# LEVEL-2 slot, filled with a level-1 table whose leaves span 2 MiB each: the leaves covering
# the KTEXT region are read-execute and every leaf above them is read-write. This reads the
# LINKED image and refuses a table that widens either side.
#
# The paging mode does not reach this gate: a mode with more levels adds tables ABOVE level 2
# and leaves the level-2 table and everything under it alone, so the symbol below names the
# same table at every mode. A level shift that DID move the boundary refuses by name below,
# the entries there no longer being 2 MiB leaves.
#
# The subject is the IMAGE. The leaves are link-time constants assembled into .mmu_boot, so
# what the hardware walks is a byte range of the ELF, and the boundary they are checked against
# is the image's own section table.
#
# EVERY VALID ENTRY OF EVERY NAMED BOOT TABLE IS CLASSIFIED and EVERY LEAF IS CHECKED. A
# non-leaf entry is skipped, and only because it holds the next table's frame number, which no
# object format can relocate: _start fills each of them at boot and the image carries zeros
# there. That reason covers a non-leaf and nothing else, the device gigapage in
# kickos_rv64_high_l2 being a LEAF: one PTE_U token there hands an unprivileged store the test
# finisher, and the machine then exits 0 on command. Every leaf is held to the same three rules
# as the kernel window's: U clear, never writable and executable at once, never write-only.
#
# Each table is reached by SYMBOL and a missing one is a failure and not a skip: collapsing any
# of them back to the single 1 GiB leaf this ladder replaced deletes the symbol rather than
# widening an entry. A table that is all zero in the image has no valid entry, which is correct
# for the ones _start fills and is reported rather than floored. What IS a corpus defect is
# finding NO leaf outside the kernel window, the device gigapage being one, so that is floored.
#
# THE CORPUS IS THE SECTION. The symbols above say where to start reading; the byte extent of
# .mmu_boot and .mmu_leaves, out of the image's own section table, says how much there is to
# read. Every named table accounts for one table's worth of bytes of the section it sits in, and
# a byte of either section that no name accounts for is refused: one page appended inside
# `.section .mmu_boot` under a new symbol is a page the hardware can walk that no symbol here
# reaches. The refusal names the section, the unaccounted extent and the offending slot.
#
# usage: riscv_kernel_wx.py <readelf> <image>...

import os
import subprocess
import sys

PTE_V = 1 << 0
PTE_R = 1 << 1
PTE_W = 1 << 2
PTE_X = 1 << 3
PTE_U = 1 << 4

# The entry format (RISC-V Privileged ISA, "Sv39 Page Table Entry"; the same in every RV64
# mode): 512 entries per table, a level-1 leaf spans 2 MiB, the output address sits in bits
# 53:10 shifted right by the 4 KiB granule, and everything above bit 53 is either a ratified
# extension's or reserved.
PTES = 512
L1_SPAN = 1 << 21
GRANULE_SHIFT = 12
PPN_MASK = 0x003FFFFFFFFFFC00
RESERVED_MASK = ~0x003FFFFFFFFFFFFF & 0xFFFFFFFFFFFFFFFF

L2_SPAN = PTES * L1_SPAN

TABLE_SYM = "kickos_rv64_kernel_l1"
LEAVES_SYM = "kickos_rv64_window_l0"

# The rest of the boot tables. One page holds exactly PTES entries, which is what makes the
# distance between the root and the level-2 table a level count.
ROOT_SYM = "kickos_rv64_root"
HIGH_L2_SYM = "kickos_rv64_high_l2"
WINDOW_L1_SYM = "kickos_rv64_window_l1"
PAGE = 4096
TABLE_BYTES = PTES * 8

# The two sections the boot tables are assembled into (startup.S, virt_rv64.ld). Their byte
# extents are the corpus every named table is accounted against.
BOOT_SECTIONS = (".mmu_boot", ".mmu_leaves")

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHF_TLS = 0x400

FLAG_BITS = {
    "W": SHF_WRITE,
    "A": SHF_ALLOC,
    "X": SHF_EXECINSTR,
    "T": SHF_TLS,
}


class Refused(Exception):
    pass


def run(readelf, args, elf):
    # LC_ALL=C: this readelf is the cross one and speaks English, but a translated binutils
    # renames the columns parsed below without failing, so the parse is pinned rather than
    # trusted.
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    try:
        out = subprocess.run([readelf] + args + [elf], capture_output=True, text=True,
                             check=True, env=env)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise Refused("%s refused %s (%s), so its verdict is UNKNOWN rather than clean"
                      % (readelf, elf, exc))
    return out.stdout.splitlines()


def sections(readelf, elf):
    # readelf -SW prints `[Nr] Name Type Address Off Size ES [Flg] Lk Inf Al`, and the Flg
    # column is ABSENT on a section with no flags. Counting tokens is what distinguishes the
    # two shapes; anything else is refused rather than guessed at.
    out = []
    for line in run(readelf, ["-SW"], elf):
        stripped = line.strip()
        if not stripped.startswith("["):
            continue
        close = stripped.find("]")
        if close < 0 or not stripped[1:close].strip().isdigit():
            continue
        tok = stripped[close + 1:].split()
        if len(tok) < 9:
            continue
        if len(tok) == 9:
            flg = ""
        elif len(tok) == 10:
            flg = tok[6]
        else:
            raise Refused("%s: unparsable section header %r" % (elf, stripped))
        flags = 0
        for ch in flg:
            if ch not in FLAG_BITS:
                continue
            flags |= FLAG_BITS[ch]
        out.append({
            "name": tok[0],
            "addr": int(tok[2], 16),
            "off": int(tok[3], 16),
            "size": int(tok[4], 16),
            "flags": flags,
        })
    if not out:
        raise Refused("%s: no section headers read" % elf)
    return out


def loads(readelf, elf):
    # `LOAD off vaddr paddr filesz memsz FLAGS align`, and the flag column is spelled with a
    # SPACE in it (`R E`), so the flags are everything between the sizes and the alignment.
    out = []
    for line in run(readelf, ["-lW"], elf):
        tok = line.split()
        if len(tok) < 8 or tok[0] != "LOAD":
            continue
        out.append({
            "vaddr": int(tok[2], 16),
            "paddr": int(tok[3], 16),
            "memsz": int(tok[5], 16),
            "flags": "".join(tok[6:-1]),
        })
    if not out:
        raise Refused("%s: no LOAD segments read" % elf)
    return out


def symbol(readelf, elf, want):
    for line in run(readelf, ["-sW"], elf):
        tok = line.split()
        if len(tok) < 8 or not tok[0].endswith(":"):
            continue
        if tok[7] == want:
            return int(tok[1], 16)
    return None


def read_table(elf, secs, addr, name):
    for sec in secs:
        if sec["size"] == 0 or sec["addr"] == 0:
            continue
        if not sec["addr"] <= addr < sec["addr"] + sec["size"]:
            continue
        need = TABLE_BYTES
        if addr + need > sec["addr"] + sec["size"]:
            raise Refused("%s: %s runs past the end of %s" % (elf, name, sec["name"]))
        with open(elf, "rb") as fh:
            fh.seek(sec["off"] + (addr - sec["addr"]))
            raw = fh.read(need)
        if len(raw) != need:
            raise Refused("%s: short read of %s" % (elf, name))
        return [int.from_bytes(raw[i * 8:i * 8 + 8], "little") for i in range(PTES)]
    raise Refused("%s: %s at 0x%x is in no section this gate can read" % (elf, name, addr))


def boot_tables(readelf, elf):
    # The tables by symbol, plus the chain above level 2, which is a page per level the mode
    # adds over Sv39 and no page at Sv39 itself: there the root IS the level-2 table and the two
    # symbols are one address. A missing symbol is a failure, per table, and not a skip.
    addr = {}
    for sym in (ROOT_SYM, HIGH_L2_SYM, TABLE_SYM, WINDOW_L1_SYM, LEAVES_SYM):
        got = symbol(readelf, elf, sym)
        if got is None:
            raise Refused("%s: no %s: this image does not carry the boot table that symbol "
                          "names, so nothing here can say what it maps" % (elf, sym))
        addr[sym] = got
    root = addr[ROOT_SYM]
    high = addr[HIGH_L2_SYM]
    if high < root or (high - root) % PAGE != 0:
        raise Refused("%s: %s at 0x%x and %s at 0x%x are not a whole number of pages apart, so "
                      "the chain between them cannot be walked"
                      % (elf, ROOT_SYM, root, HIGH_L2_SYM, high))
    out = []
    upper = (high - root) // PAGE
    for k in range(upper):
        name = ROOT_SYM
        if k != 0:
            name = "%s+0x%x" % (ROOT_SYM, k * PAGE)
        out.append((name, root + k * PAGE))
    name = HIGH_L2_SYM
    if upper == 0:
        name = "%s (= %s)" % (HIGH_L2_SYM, ROOT_SYM)
    out.append((name, high))
    out.append((TABLE_SYM, addr[TABLE_SYM]))
    out.append((WINDOW_L1_SYM, addr[WINDOW_L1_SYM]))
    out.append((LEAVES_SYM, addr[LEAVES_SYM]))
    return out


def boot_sections(elf, secs):
    # The two sections by name, out of the image's own section table. A section this gate cannot
    # find is refused rather than treated as empty, an empty extent accounting for itself.
    out = []
    for want in BOOT_SECTIONS:
        found = []
        for sec in secs:
            if sec["name"] == want:
                found.append(sec)
        if len(found) != 1:
            raise Refused("%s: %d section(s) named %s in the image's section table, so the byte "
                          "range the hardware walks cannot be read from it" % (elf, len(found),
                                                                               want))
        sec = found[0]
        if sec["size"] == 0 or sec["addr"] == 0:
            raise Refused("%s: %s is %d byte(s) at 0x%x, which holds no boot table"
                          % (elf, want, sec["size"], sec["addr"]))
        if sec["size"] % TABLE_BYTES != 0:
            raise Refused("%s: %s is 0x%x byte(s), which is not a whole number of tables"
                          % (elf, want, sec["size"]))
        out.append(sec)
    return out


def refuse_gap(elf, sec, lo, hi):
    # The unaccounted bytes, read out of the image and held to the three leaf rules, so that the
    # refusal names the slot and not only the page.
    with open(elf, "rb") as fh:
        fh.seek(sec["off"] + (lo - sec["addr"]))
        raw = fh.read(hi - lo)
    slot = ""
    for i in range(len(raw) // 8):
        pte = int.from_bytes(raw[i * 8:i * 8 + 8], "little")
        if not pte & PTE_V or pte & RESERVED_MASK:
            continue
        if not pte & (PTE_R | PTE_W | PTE_X):
            continue
        why = None
        if not pte & PTE_R:
            why = "is write-only, which the architecture reserves"
        elif pte & PTE_U:
            why = "GRANTS THE UNPRIVILEGED LEVEL"
        elif pte & PTE_W and pte & PTE_X:
            why = "is WRITABLE AND EXECUTABLE at once"
        if why is None:
            continue
        slot = ("; slot %d of it, at 0x%x (0x%016x), %s" % (i, lo + i * 8, pte, why))
        break
    raise Refused("%s: 0x%x byte(s) of %s at 0x%x (section offset 0x%x, file offset 0x%x) are "
                  "accounted for by no boot table symbol this gate names, so the hardware walks "
                  "a table no rule above has read%s"
                  % (elf, hi - lo, sec["name"], lo, lo - sec["addr"],
                     sec["off"] + (lo - sec["addr"]), slot))


def account(elf, secs, tables):
    # Every named table against the extent of the section it sits in: the tables are walked in
    # address order and every byte between the section's start and its end has to be one a name
    # covers. A table in neither section, an overlap, an overrun and a remainder each refuse.
    boot = boot_sections(elf, secs)
    placed = {}
    for name, at in tables:
        home = None
        for sec in boot:
            if sec["addr"] <= at < sec["addr"] + sec["size"]:
                home = sec
                break
        if home is None:
            raise Refused("%s: %s at 0x%x is in none of %s, so the sections accounted for below "
                          "do not hold every boot table this gate names"
                          % (elf, name, at, " ".join(BOOT_SECTIONS)))
        placed.setdefault(home["name"], []).append((at, name))
    spans = []
    for sec in boot:
        owned = sorted(placed.get(sec["name"], []))
        end = sec["addr"] + sec["size"]
        cursor = sec["addr"]
        for at, name in owned:
            if at < cursor:
                raise Refused("%s: %s at 0x%x overlaps the boot table before it in %s"
                              % (elf, name, at, sec["name"]))
            if at > cursor:
                refuse_gap(elf, sec, cursor, at)
            cursor = at + TABLE_BYTES
        if cursor > end:
            raise Refused("%s: the boot tables named in %s run 0x%x byte(s) past its end at 0x%x"
                          % (elf, sec["name"], cursor - end, end))
        if cursor < end:
            refuse_gap(elf, sec, cursor, end)
        spans.append((sec["name"], sec["addr"], sec["size"], len(owned)))
    return spans


def classify(elf, name, entries):
    # Leaf, non-leaf or invalid for every entry, and the three rules on every leaf. The output
    # ADDRESS is not checked here: what a leaf outside the kernel window may output is that
    # table's own business, and the kernel window's own order is checked below.
    leaves = 0
    nonleaf = 0
    invalid = 0
    for i, pte in enumerate(entries):
        where = "%s[%d] (0x%016x)" % (name, i, pte)
        if not pte & PTE_V:
            invalid += 1
            continue
        if pte & RESERVED_MASK:
            raise Refused("%s: %s sets a bit above the output address; this gate reads none of "
                          "the extension encodings that live there" % (elf, where))
        if not pte & (PTE_R | PTE_W | PTE_X):
            nonleaf += 1
            continue
        leaves += 1
        if not pte & PTE_R:
            raise Refused("%s: %s is write-only, which the architecture reserves" % (elf, where))
        if pte & PTE_U:
            raise Refused("%s: %s GRANTS THE UNPRIVILEGED LEVEL. Nothing a boot table maps "
                          "belongs to the unprivileged level: every space fills its own slot "
                          "with leaves of its own, so a U bit here hands every thread on the "
                          "machine this page, the test finisher and the console included, and "
                          "the supervisor may not even fetch from it" % (elf, where))
        if pte & PTE_W and pte & PTE_X:
            raise Refused("%s: %s is WRITABLE AND EXECUTABLE at once" % (elf, where))
    return leaves, nonleaf, invalid


def leaves_for(lo, hi, base, span):
    first = (lo - base) // span
    last = (hi - 1 - base) // span
    return range(first, last + 1)


def check(readelf, elf, report):
    secs = sections(readelf, elf)
    segs = loads(readelf, elf)

    # EVERY BOOT TABLE FIRST, so a widened entry in any of them reddens whatever else the
    # kernel window's own checks below would have said.
    named = boot_tables(readelf, elf)
    tables = []
    outside = 0
    for name, at in named:
        counts = classify(elf, name, read_table(elf, secs, at, name))
        tables.append((name, counts[0], counts[1], counts[2]))
        if name != TABLE_SYM:
            outside += counts[0]

    # AND THE SECTIONS THOSE NAMES HAVE TO ACCOUNT FOR, so a table appended beside them is
    # refused rather than left unread.
    spans = account(elf, secs, named)

    if outside == 0:
        raise Refused("%s: not one leaf outside %s in any boot table. The device gigapage is "
                      "one, so the corpus is wrong, not clean: the tables this gate names no "
                      "longer hold the entries it reads" % (elf, TABLE_SYM))

    addr = symbol(readelf, elf, TABLE_SYM)
    if addr is None:
        raise Refused("%s: no %s: the kernel window is not described by a level-1 table, so "
                      "nothing here can be read-execute" % (elf, TABLE_SYM))
    entries = read_table(elf, secs, addr, TABLE_SYM)

    win_pa = (entries[0] & PPN_MASK) >> 10 << GRANULE_SHIFT
    if win_pa % L2_SPAN != 0:
        raise Refused("%s: the window's first leaf outputs 0x%x, which no level-2 slot spans"
                      % (elf, win_pa))

    # The window's virtual base, taken from the load segment the tables themselves sit in
    # rather than from a constant: every kernel section carries one VMA-to-LMA delta.
    delta = None
    for seg in segs:
        if seg["memsz"] and seg["vaddr"] <= addr < seg["vaddr"] + seg["memsz"]:
            delta = seg["vaddr"] - seg["paddr"]
            break
    if delta is None:
        raise Refused("%s: %s is in no load segment" % (elf, TABLE_SYM))
    win_va = win_pa + delta
    if win_va % L2_SPAN != 0:
        raise Refused("%s: the window's virtual base 0x%x is not a level-2 boundary, so one "
                      "level-1 table cannot describe it" % (elf, win_va))

    rx = 0
    rw = 0
    for i, pte in enumerate(entries):
        where = "%s[%d] (0x%016x, va 0x%x)" % (TABLE_SYM, i, pte, win_va + i * L1_SPAN)
        if not pte & PTE_V:
            raise Refused("%s: %s is invalid: the window no longer maps its whole span"
                          % (elf, where))
        if pte & RESERVED_MASK:
            raise Refused("%s: %s sets a bit above the output address; this gate reads none of "
                          "the extension encodings that live there" % (elf, where))
        if not pte & (PTE_R | PTE_W | PTE_X):
            raise Refused("%s: %s is a pointer to a further table, not a 2 MiB leaf; the "
                          "boundary this gate checks is one level up from where it now falls"
                          % (elf, where))
        if not pte & PTE_R:
            raise Refused("%s: %s is write-only, which the architecture reserves" % (elf, where))
        if pte & PTE_U:
            raise Refused("%s: %s grants the unprivileged level, and the supervisor may not "
                          "even fetch from such a page" % (elf, where))
        if pte & PTE_W and pte & PTE_X:
            raise Refused("%s: %s is WRITABLE AND EXECUTABLE at once, which is exactly what the "
                          "kernel window's split exists to prevent" % (elf, where))
        out = (pte & PPN_MASK) >> 10 << GRANULE_SHIFT
        if out != win_pa + i * L1_SPAN:
            raise Refused("%s: %s outputs 0x%x where the window's own order wants 0x%x"
                          % (elf, where, out, win_pa + i * L1_SPAN))
        if pte & PTE_X:
            rx += 1
        else:
            rw += 1

    # WHERE THE BOUNDARY HAS TO FALL, out of the image's own section table. A section fetched
    # from needs X under it and a section stored into needs W, so a layout that moved either
    # across the boundary fails here rather than at the first access.
    execs = 0
    writes = 0
    for sec in secs:
        if not sec["flags"] & SHF_ALLOC or sec["size"] == 0:
            continue
        if not win_va <= sec["addr"] < win_va + L2_SPAN:
            continue
        span = leaves_for(sec["addr"], sec["addr"] + sec["size"], win_va, L1_SPAN)
        if sec["flags"] & SHF_EXECINSTR:
            execs += 1
            for i in span:
                if not entries[i] & PTE_X:
                    raise Refused("%s: %s is fetched from and %s[%d] under it is not executable"
                                  % (elf, sec["name"], TABLE_SYM, i))
        # A TLS section here is the TEMPLATE each thread's block is copied FROM, never written
        # through this address, so its SHF_WRITE demands nothing of the leaf.
        if sec["flags"] & SHF_WRITE and not sec["flags"] & SHF_TLS:
            writes += 1
            for i in span:
                if not entries[i] & PTE_W:
                    raise Refused("%s: %s is stored into and %s[%d] under it is read-only"
                                  % (elf, sec["name"], TABLE_SYM, i))
    if execs == 0 or writes == 0:
        raise Refused("%s: %d executable and %d writable section(s) inside the window; the "
                      "corpus is wrong, not clean" % (elf, execs, writes))

    # THE ALIAS THE KERNEL WRITES THROUGH. The app's half loads inside the window's physical
    # span while it links outside the window, and the reset path zeroes .appbss through the
    # kernel's own alias of those frames, so the leaf over them has to be writable. Nothing
    # demands X there: kernel text never fetches through this alias.
    aliases = 0
    for seg in segs:
        if "W" not in seg["flags"] or seg["memsz"] == 0:
            continue
        if not win_pa <= seg["paddr"] < win_pa + L2_SPAN:
            continue
        if win_va <= seg["vaddr"] < win_va + L2_SPAN:
            continue
        aliases += 1
        for i in leaves_for(seg["paddr"], seg["paddr"] + seg["memsz"], win_pa, L1_SPAN):
            if not entries[i] & PTE_W:
                raise Refused("%s: a writable segment loads at 0x%x and %s[%d] over the "
                              "kernel's alias of it is read-only"
                              % (elf, seg["paddr"], TABLE_SYM, i))
    if aliases == 0:
        raise Refused("%s: no writable segment loads inside the window; the corpus is wrong, "
                      "not clean" % elf)

    # THE ONE TABLE PAGE THE KERNEL WRITES AFTER BOOT. Moving it back beside its siblings puts
    # it in the read-execute leaf, and the first transient window slot then faults.
    leaves = symbol(readelf, elf, LEAVES_SYM)
    if leaves is None:
        raise Refused("%s: no %s: the transient window has no leaf table" % (elf, LEAVES_SYM))
    if not win_va <= leaves < win_va + L2_SPAN:
        raise Refused("%s: %s at 0x%x is outside the kernel window" % (elf, LEAVES_SYM, leaves))
    slot = (leaves - win_va) // L1_SPAN
    if not entries[slot] & PTE_W:
        raise Refused("%s: %s is written by the map editor and %s[%d] over it is read-only"
                      % (elf, LEAVES_SYM, TABLE_SYM, slot))

    report.append((elf, win_va, rx, rw, execs, writes, aliases, tables, outside, spans))


def main(argv):
    if len(argv) < 3:
        print("usage: riscv_kernel_wx.py <readelf> <image>...", file=sys.stderr)
        return 2
    readelf = argv[1]
    images = argv[2:]
    report = []
    try:
        for elf in images:
            check(readelf, elf, report)
    except Refused as exc:
        for line in report:
            print("checked: %s" % line[0])
        print("FAIL: %s" % exc, file=sys.stderr)
        return 1
    total = 0
    widths = set()
    for elf, win_va, rx, rw, execs, writes, aliases, tables, outside, spans in report:
        print("%s: window 0x%x, %d read-execute leaf/leaves, %d read-write, %d executable and "
              "%d writable section(s) checked, %d alias segment(s)"
              % (elf, win_va, rx, rw, execs, writes, aliases))
        for name, leaves, nonleaf, invalid in tables:
            note = ""
            if invalid == PTES:
                note = " (all zero in the image: its entries are written at run time)"
            print("  %s: %d leaf/leaves, %d non-leaf, %d invalid%s"
                  % (name, leaves, nonleaf, invalid, note))
        print("  %d leaf/leaves outside %s" % (outside, TABLE_SYM))
        for sec_name, sec_addr, sec_size, count in spans:
            print("  %s: 0x%x byte(s) at 0x%x, %d table(s) named, no unaccounted byte"
                  % (sec_name, sec_size, sec_addr, count))
        widths.add(len(tables))
        total += 1
    # The table count is printed per DISTINCT value, so two images built at different modes in
    # one run report both figures rather than the last one read.
    print("corpus: %d image(s), %s boot table(s) each, %d entries per table"
          % (total, "/".join(str(w) for w in sorted(widths)), PTES))
    print("PASS: no boot-table leaf grants the unprivileged level, and no leaf of the kernel "
          "window is writable and executable at once")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
