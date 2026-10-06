#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No boot path reads the thread pointer before an instruction seats it. From reset to its first
# switch-in a core runs with whatever its thread-pointer register held at reset (THREADPTR on lx6,
# tp on RISC-V, TPIDR_EL0 on armv8a), which names no TLS block: a __getreent or a thread_local
# reached there dereferences it, and on lx6 that is a load from address 0 that resets the chip
# before its banner.
#
# Read out of each linked image. The roots are the ELF entry, every kickos_*_secondary_entry the
# image defines, and every word of .init_array, which Reset_Handler calls through a pointer. Inside
# a function it follows the control flow from the function's start, and a path ends at the
# instruction that seats the register. It follows calls and branches into other functions, and an
# indirect call whose register an annotated constant load set earlier in the same function.
#
# The flow is followed rather than the listing read top to bottom because a linear disassembly of
# Xtensa loses its alignment after a padded jump and decodes calls that are not there. A branch
# into such a stretch is decoded again from its own address.
#
# NOT EXACT: a call through a pointer loaded from memory is not followed, so a path that leaves
# the followed graph that way is invisible here. Each such site the walk reaches is listed; the
# silicon capture of a C++ image on a board that seats its register is the witness behind them.
#
#   check_boot_tp.py <objdump> <arch> <image>...
#   check_boot_tp.py <objdump> <arch> --images <file of link maps, one per line>
#   check_boot_tp.py --self-test

import bisect
import re
import subprocess
import sys

INSN_RE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
LABEL_RE = re.compile(r'^([0-9a-f]+) <(.+)>:$')
TARGET_RE = re.compile(r'([0-9a-f]+) <([^>]+)>')
SYM_RE = re.compile(r'^([0-9a-f]+)\s+(\S+)\s+(\S*)\s*(\S+)\s+([0-9a-f]+)\s+(.+)$')
SECONDARY_RE = re.compile(r'^_?kickos_\w+_secondary_entry$')


class Bad(Exception):
    pass


class Arch(object):
    def __init__(self, read, seat, park, branch, call, indirect, term, store, value):
        self.read = read                  # (mnemonic, operands) -> reads the thread pointer
        self.seat = seat                  # (mnemonic, operands) -> writes it
        self.park = park                  # (mnemonic, operands) -> writes a value naming no block
        self.branch = re.compile(branch)  # a direct transfer whose target is annotated
        self.call = re.compile(call)      # the direct transfers that come back
        self.indirect = re.compile(indirect)
        self.term = re.compile(term)      # nothing falls through it
        self.store = re.compile(store)    # its first operand is read, not written
        self.value = re.compile(value)    # the constant an annotated load leaves in its register


def ops_of(text):
    return [o.strip() for o in re.split(r'#|//', text)[0].split(',')]


RV_STORE = r'^(c\.)?f?s[bhwd](sp)?$'


def rv_read(mn, text):
    ops = ops_of(text)
    if any(re.search(r'\(tp\)', o) for o in ops):
        return True
    if re.match(RV_STORE, mn):
        return False
    return 'tp' in ops[1:]


def rv_seat(mn, text):
    ops = ops_of(text)
    return ops[:1] == ['tp'] and not re.match(RV_STORE, mn) and not mn.startswith('b')


def rv_park(mn, text):
    return mn in ('li', 'c.li', 'mv', 'c.mv') and ops_of(text) in (['tp', '0'], ['tp', 'zero'])


RISCV = Arch(rv_read, rv_seat, rv_park,
             branch=r'^(c\.)?(jal|j|b\w*)$',
             call=r'^(c\.)?jal$',
             indirect=r'^(c\.)?(jalr|jr)$',
             term=r'^(c\.)?(j|jr|ret|mret|sret)$',
             store=RV_STORE + r'|^b',
             value=r'#\s*([0-9a-f]+) <[^>]+>\s*$')

ARCHES = {
    'lx6': Arch(lambda mn, t: mn == 'rur.threadptr' or (mn == 'rur' and 'threadptr' in t),
                lambda mn, t: mn == 'wur.threadptr' or (mn == 'wur' and 'threadptr' in t),
                lambda mn, t: False,
                branch=r'^(call(0|4|8|12)|j|b\w*|loop\w*)(\.n)?$',
                call=r'^call(0|4|8|12)$',
                indirect=r'^(callx(0|4|8|12)|jx)$',
                term=r'^(j|jx|ret|retw|rf\w+)(\.n)?$',
                store=r'^(s8i|s16i|s32i|s32e|s32c1i|s32ri|ssi|ssip|ssx|ssxp)(\.n)?$|^b',
                value=r'\(([0-9a-f]+) <[^>]+>\)\s*$'),
    'rv32imac': RISCV,
    'rv64imac': RISCV,
    'armv8a': Arch(lambda mn, t: mn == 'mrs' and re.search(r'\btpidr_el0\b', t) is not None,
                   lambda mn, t: mn == 'msr' and re.match(r'\s*tpidr_el0\b', t) is not None,
                   lambda mn, t: False,
                   branch=r'^(bl|b|b\.\w+|cbn?z|tbn?z)$',
                   call=r'^bl$',
                   indirect=r'^(blr|br)$',
                   term=r'^(b|br|ret|eret)$',
                   store=r'^st|^b|^cbn?z$|^tbn?z$',
                   value=r'//\s*([0-9a-f]+) <[^>]+>\s*$'),
}


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if p.returncode != 0:
        raise Bad('%s failed: %s' % (' '.join(cmd), p.stderr.strip()))
    return p.stdout


def parse(disasm):
    """{addr: (mnemonic, operands, next addr)}, each next taken from the same listing."""
    rows = []
    for line in disasm.splitlines():
        m = INSN_RE.match(line)
        if m:
            rows.append((int(m.group(1), 16), m.group(2), m.group(3)))
    out = {}
    for k, (a, mn, text) in enumerate(rows):
        nxt = None
        if k + 1 < len(rows):
            nxt = rows[k + 1][0]
        out[a] = (mn, text, nxt)
    return out


class Image(object):
    def __init__(self, disasm, symtab, entry, init_words, decode=None):
        self.insn = parse(disasm)
        self.decode = decode      # (start, stop) -> listing from start, or None
        starts = sorted(int(m.group(1), 16) for m in map(LABEL_RE.match, disasm.splitlines())
                        if m)
        self.funcs = {}           # start -> (name, end)
        for line in symtab.splitlines():
            m = SYM_RE.match(line)
            if not m or 'F' not in m.group(3):
                continue
            start = int(m.group(1), 16)
            size = int(m.group(5), 16)
            if size == 0:
                k = bisect.bisect_right(starts, start)
                if k < len(starts):
                    size = starts[k] - start
            if size and start not in self.funcs:
                self.funcs[start] = (m.group(6).split()[-1], start + size)
        # An assembly entry often carries no function type; it runs to the next label.
        k = bisect.bisect_right(starts, entry)
        if entry in starts and k < len(starts) and not self.covers(entry):
            self.funcs[entry] = ('entry', starts[k])
        self.fstarts = sorted(self.funcs)
        self.entry = entry
        self.init_words = init_words

    def covers(self, addr):
        return any(s <= addr < e for s, (_n, e) in self.funcs.items())

    def func_at(self, addr):
        k = bisect.bisect_right(self.fstarts, addr) - 1
        if k >= 0 and addr < self.funcs[self.fstarts[k]][1]:
            return self.fstarts[k]
        return None

    def at(self, addr, end):
        if addr not in self.insn and self.decode is not None:
            for a, row in parse(self.decode(addr, end)).items():
                self.insn.setdefault(a, row)
        return self.insn.get(addr)

    def roots(self):
        f = self.func_at(self.entry)
        if f is None:
            raise Bad('the entry 0x%x is inside no function the symbol table sizes' % self.entry)
        out = [f]
        out += [s for s in self.fstarts if SECONDARY_RE.match(self.funcs[s][0])]
        for w in self.init_words:
            f = self.func_at(w)
            if f is None:
                raise Bad('.init_array word 0x%x names no function' % w)
            out.append(f)
        return out


def flow(img, arch, f):
    """The instructions of f reachable from its start before a seat, in address order."""
    start, end = f, img.funcs[f][1]
    seen = {}
    todo = [start]
    while todo:
        a = todo.pop()
        if a in seen or not start <= a < end:
            continue
        row = img.at(a, end)
        if row is None:
            raise Bad('%s: nothing decodes at 0x%x, which its own flow reaches'
                      % (img.funcs[f][0], a))
        seen[a] = row
        mn, text, nxt = row
        if not arch.read(mn, text) and arch.seat(mn, text) and not arch.park(mn, text):
            continue
        if arch.branch.match(mn):
            m = TARGET_RE.search(text)
            if m:
                todo.append(int(m.group(1), 16))
        if not arch.term.match(mn) and nxt is not None:
            todo.append(nxt)
    return sorted(seen.items())


def walk(img, arch):
    """(reads, unresolved): each read with the call path that reaches it, each unfollowed site."""
    parent = {}
    reads = []
    unresolved = []
    todo = []
    for f in img.roots():
        if f not in parent:
            parent[f] = None
            todo.append(f)
    while todo:
        f = todo.pop()
        regs = {}
        for addr, (mn, text, _nxt) in flow(img, arch, f):
            if arch.read(mn, text):
                path = []
                p = f
                while p is not None:
                    path.append(img.funcs[p][0])
                    p = parent[p]
                reads.append('0x%x %s %s, reached %s' % (addr, mn, text.strip(),
                                                         ' <- '.join(path)))
                continue
            targets = []
            if arch.branch.match(mn):
                m = TARGET_RE.search(text)
                if m:
                    targets.append(int(m.group(1), 16))
            elif arch.indirect.match(mn):
                reg = re.sub(r'^-?\w*\((\w+)\)$', r'\1', ops_of(text)[-1])
                m = arch.value.search(text)
                if m:
                    targets.append(int(m.group(1), 16))
                elif reg in regs:
                    targets.append(regs[reg])
                elif not (mn in ('jr', 'c.jr', 'br') and reg in ('ra', 'x30')):
                    unresolved.append('0x%x %s %s in %s' % (addr, mn, text.strip(),
                                                            img.funcs[f][0]))
            ops = ops_of(text)
            if ops[0] and not arch.store.match(mn):
                regs.pop(ops[0], None)
                m = arch.value.search(text)
                if m:
                    regs[ops[0]] = int(m.group(1), 16)
            for t in targets:
                g = img.func_at(t)
                if g is not None and g != f and g not in parent:
                    parent[g] = f
                    todo.append(g)
    return reads, unresolved


def load(objdump, elf):
    head = run([objdump, '-f', elf])
    m = re.search(r'start address 0x([0-9a-f]+)', head)
    if not m:
        raise Bad('%s: objdump states no start address' % elf)
    entry = int(m.group(1), 16)
    fmt = re.search(r'file format (\S+)', head).group(1)
    wide = 4
    if '64' in fmt:
        wide = 8
    order = 'little'
    if 'big' in fmt or fmt.endswith('-be'):
        order = 'big'
    words = []
    if re.search(r'^\s*\d+\s+\.init_array\s', run([objdump, '-h', elf]), re.M):
        raw = ''
        for line in run([objdump, '-s', '-j', '.init_array', elf]).splitlines():
            m = re.match(r'^ ([0-9a-f]+) ((?:[0-9a-f]{1,8} ?){1,4})', line)
            if m:
                raw += m.group(2).replace(' ', '')
        data = bytes.fromhex(raw)
        for k in range(0, len(data) - wide + 1, wide):
            words.append(int.from_bytes(data[k:k + wide], order))

    def decode(start, stop):
        return run([objdump, '-d', '-w', '--no-show-raw-insn', '--start-address=0x%x' % start,
                    '--stop-address=0x%x' % stop, elf])

    return Image(run([objdump, '-d', '-w', '--no-show-raw-insn', elf]),
                 run([objdump, '-t', '-w', elf]), entry, words, decode)


def check(objdump, arch_name, elves):
    arch = ARCHES.get(arch_name)
    if arch is None:
        raise Bad('no thread-pointer read is spelled for arch %s' % arch_name)
    failed = False
    for elf in elves:
        reads, unresolved = walk(load(objdump, elf), arch)
        for r in reads:
            print('FAIL: %s: the thread pointer is read before it is seated: %s' % (elf, r))
            failed = True
        for u in unresolved:
            print('not followed: %s: %s' % (elf, u))
    return not failed


SYMTAB = '''
00001000 g     F .text	00000020 Reset_Handler
00001020 g     F .text	00000010 libc_malloc
00001030 g     F .text	00000010 getreent
00001040 g     F .text	00000010 kmain
00001070 g     F .text	00000010 kernel_ctor
00001080 g     F .text	00000010 kickos_lx6_secondary_entry
'''


def control(name, arch, lines, want_read, want_unresolved=0, init=()):
    symtab = '\n'.join(l for l in SYMTAB.splitlines()
                       if l and '<%s>:' % l.split()[-1] in ''.join(lines))
    img = Image('\n'.join(lines), symtab, 0x1000, list(init))
    reads, unresolved = walk(img, ARCHES[arch])
    if bool(reads) != want_read:
        raise Bad('control %s: expected %s read, got %s'
                  % (name, {True: 'a', False: 'no'}[want_read], reads or 'none'))
    if len(unresolved) != want_unresolved:
        raise Bad('control %s: expected %d unfollowed site(s), got %s'
                  % (name, want_unresolved, unresolved))


def self_test():
    lx6_tail = ['00001020 <libc_malloc>:', '    1020:\tentry\ta1, 32',
                '    1023:\tcall8\t1030 <getreent>', '    1026:\tretw.n',
                '00001030 <getreent>:', '    1030:\tentry\ta1, 32', '    1033:\trur.threadptr\ta8',
                '    1036:\tl32i.n\ta2, a8, 0', '    1038:\tretw.n',
                '00001040 <kmain>:', '    1040:\tretw.n']
    reset = ['00001000 <Reset_Handler>:', '    1000:\tentry\ta1, 32']
    end = ['    1010:\tretw.n']
    control('lx6 direct call', 'lx6',
            reset + ['    1003:\tcall8\t1020 <libc_malloc>'] + end + lx6_tail, True)
    control('lx6 no call', 'lx6', reset + ['    1003:\tcall8\t1040 <kmain>'] + end + lx6_tail,
            False)
    control('lx6 l32r callx', 'lx6',
            reset + ['    1003:\tl32r\ta8, 1018 <Reset_Handler+0x18> (1020 <libc_malloc>)',
                     '    1006:\tcallx8\ta8'] + end + lx6_tail, True)
    control('lx6 overwritten register', 'lx6',
            reset + ['    1003:\tl32r\ta8, 1018 <Reset_Handler+0x18> (1020 <libc_malloc>)',
                     '    1006:\tl32i.n\ta8, a8, 0', '    1008:\tcallx8\ta8'] + end + lx6_tail,
            False, 1)
    control('lx6 seat first', 'lx6',
            reset + ['    1003:\twur.threadptr\ta3', '    1006:\tcall8\t1020 <libc_malloc>']
            + end + lx6_tail, False)
    control('lx6 seat on one path only', 'lx6',
            reset + ['    1003:\tbeqz\ta2, 100c <Reset_Handler+0xc>',
                     '    1006:\twur.threadptr\ta3', '    1009:\tj\t1010 <Reset_Handler+0x10>',
                     '    100c:\tcall8\t1020 <libc_malloc>'] + end + lx6_tail, True)
    control('lx6 tail jump', 'lx6', reset + ['    1003:\tj\t1030 <getreent>'] + lx6_tail, True)
    control('lx6 undecoded padding after a jump', 'lx6',
            reset + ['    1003:\tj\t1010 <Reset_Handler+0x10>',
                     '    1006:\tcall12\t1030 <getreent>'] + end + lx6_tail, False)
    control('lx6 init_array', 'lx6',
            reset + end + ['00001070 <kernel_ctor>:', '    1070:\tcall8\t1020 <libc_malloc>',
                           '    1073:\tretw.n'] + lx6_tail, True, init=[0x1070])
    control('lx6 secondary entry', 'lx6',
            reset + end + ['00001080 <kickos_lx6_secondary_entry>:',
                           '    1080:\tcall8\t1030 <getreent>', '    1083:\tretw.n'] + lx6_tail,
            True)
    rv_tail = ['00001030 <getreent>:', '    1030:\tmv\ta0,tp', '    1032:\tret']
    rv_reset = ['00001000 <Reset_Handler>:', '    1000:\taddi\tsp,sp,-16']
    rv_end = ['    1010:\tret']
    control('rv jal', 'rv32imac',
            rv_reset + ['    1004:\tjal\t1030 <getreent>'] + rv_end + rv_tail, True)
    control('rv tp base', 'rv32imac', rv_reset + ['    1004:\tlw\ta0,8(tp)'] + rv_end, True)
    control('rv tp saved', 'rv32imac', rv_reset + ['    1004:\tsw\ttp,8(sp)'] + rv_end, False)
    control('rv park then read', 'rv32imac',
            rv_reset + ['    1004:\tli\ttp,0', '    1006:\tjal\t1030 <getreent>'] + rv_end
            + rv_tail, True)
    control('rv seat then read', 'rv32imac',
            rv_reset + ['    1004:\tlw\ttp,0(a0)', '    1006:\tjal\t1030 <getreent>'] + rv_end
            + rv_tail, False)
    control('rv la jalr', 'rv64imac',
            rv_reset + ['    1004:\tauipc\ta5,0x0', '    1008:\taddi\ta5,a5,44 # 1030 <getreent>',
                        '    100c:\tjalr\ta5'] + rv_end + rv_tail, True)
    control('rv auipc jalr', 'rv64imac',
            rv_reset + ['    1004:\tauipc\tra,0x0', '    1008:\tjalr\tra,44(ra) # 1030 <getreent>']
            + rv_end + rv_tail, True)
    control('rv unfollowed', 'rv64imac',
            rv_reset + ['    1004:\tld\ta5,0(a0)', '    1008:\tjalr\ta5'] + rv_end, False, 1)
    a64_tail = ['00001030 <getreent>:', '    1030:\tmrs\tx0, tpidr_el0', '    1034:\tret']
    a64_reset = ['00001000 <Reset_Handler>:', '    1000:\tstp\tx29, x30, [sp, #-16]!']
    a64_end = ['    1010:\tret']
    control('a64 bl', 'armv8a',
            a64_reset + ['    1004:\tbl\t1030 <getreent>'] + a64_end + a64_tail, True)
    control('a64 seat', 'armv8a',
            a64_reset + ['    1004:\tmsr\ttpidr_el0, x1', '    1008:\tbl\t1030 <getreent>']
            + a64_end + a64_tail, False)
    print('PASS: every planted boot read is reported and every seated or unreached one is not')


def main(argv):
    try:
        if argv[1:] == ['--self-test']:
            self_test()
            return 0
        if len(argv) < 4:
            raise Bad('usage: check_boot_tp.py <objdump> <arch> <image>... | --images <list>')
        objdump, arch = argv[1], argv[2]
        elves = argv[3:]
        if elves[0] == '--images':
            with open(elves[1]) as f:
                elves = [l.strip()[:-len('.map')] for l in f if l.strip().endswith('.map')]
        if not elves:
            raise Bad('no image to read, so the walk would pass vacuously')
        if not check(objdump, arch, elves):
            return 1
        print('PASS: %d image(s): no boot path reads the thread pointer before it is seated'
              % len(elves))
        return 0
    except Bad as e:
        print('FAIL: %s' % e)
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
