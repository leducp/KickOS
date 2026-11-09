# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The at= walk of trap_redzone.py (Walk.depth_to and Walk.chain_to) on planted graphs, against an
# exhaustive longest-simple-path oracle:
#   - on every acyclic graph the walk answers exactly what the oracle does;
#   - on a cyclic graph it reaches target exactly where the oracle does, and never answers less;
#   - a dense strongly connected component is walked in a bounded number of expansions, so a
#     mutual recursion is reported as a cycle instead of hanging the gate;
#   - chain_to names a cycle once and leaves it by its deepest edge, ending at the target;
#   - a ladder of twelve diamonds, too deep for the random graphs, equals the oracle;
#   - a site key's caller written `?<caller>` binds where the graph has it, is skipped where it
#     has not, and an absent caller without the `?` is refused;
#   - a link glob reads only the maps beside a file the build graph links, and a target names
#     the one file the graph builds for it;
#   - the corpus is the .ci of every compile command: a .ci beside no object the build graph
#     builds is not read, one beside a built object no command names is refused, and a command
#     whose .ci is missing is refused;
#   - a `?` caller that makes no located indirect call binds nothing, and one making another
#     number of them than its key says is refused;
#   - a stack=main class sums the deepest root of each level, refuses a level none of whose roots
#     the graph has, and its declarations refuse a root with no level, a level= anywhere else, a
#     level with no root, an undeclared level and NONE;
#   - the main stack is the smallest span the listed link maps give, and a map without the pair
#     is refused; a stack=main class may name the macro that sizes it and mark itself the stack a
#     secondary core parks on, and neither option is taken by any other class;
#   - a constructor the reset handler's array holds is refused unless a stack=main walk reaches
#     it, in an ELF32 or an ELF64 image, after a vector table of the same image too, and the image
#     read beside a map is its .elf where the link converts one; an address's bit 0 is cleared on
#     Arm alone, so off Arm a constructor a byte past a priced one is refused.
#   - every C function the assembly of the arch family names is rooted in some class or reached
#     through a calls= edge of a charged unsized body some class reaches, a callee every walk
#     cuts or one the body has no edge to being priced by nothing: one named only in a call
#     neither covers is refused at the file and line the preprocessor's markers give, an
#     included .inc's line included; each arch's comment character, a string or the file's own
#     label naming it is not a call, while a '#' mid-line is an immediate on arm, aarch64 and
#     rx; the target's label prefix is stripped; only the branch the unit's own compile command
#     selects is read, and a unit it cannot preprocess is refused; an asmcall record places one
#     off every trap path, is refused when it is also a root or a priced edge or when no branch
#     of the raw assembly names it, and is reported when this posture compiles its call out; an
#     arch family whose compile database holds no assembly is refused, and nothing outside it is
#     read.

import json
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trap_redzone  # noqa: E402

sys.setrecursionlimit(20000)

SEED = 20261005
EXPANSION_BOUND = 5000


class Graph(object):
    def __init__(self, edges, size):
        self.edges = edges
        self.size = size
        self.label = {}


class TooMany(Exception):
    pass


def walk_of(edges, size):
    return trap_redzone.Walk(Graph(edges, size), frozenset(), {})


def bounded_walk_of(edges, size):
    w = walk_of(edges, size)
    plain = w.out
    w.expansions = 0

    def counted(fn):
        w.expansions += 1
        if w.expansions > EXPANSION_BOUND:
            raise TooMany()
        return plain(fn)
    w.out = counted
    return w


def oracle(w, fn, target, path):
    if fn == target:
        return w.weight(fn)
    best = None
    for t in w.out(fn):
        if t in path:
            continue
        d = oracle(w, t, target, path | {t})
        if d is not None and (best is None or d > best):
            best = d
    if best is None:
        return None
    return best + w.weight(fn)


def random_graph(rng, n, p, acyclic):
    edges = {}
    size = {}
    for i in range(n):
        size[i] = rng.randrange(0, 65)
        edges[i] = set()
        for j in range(n):
            if acyclic and j <= i:
                continue
            if rng.random() < p:
                edges[i].add(j)
    return edges, size


class Walk(unittest.TestCase):
    def test_acyclic_equals_oracle(self):
        rng = random.Random(SEED)
        for _ in range(300):
            edges, size = random_graph(rng, 10, 0.35, True)
            w = walk_of(edges, size)
            for target in range(10):
                for fn in range(10):
                    self.assertEqual(w.depth_to(fn, target), oracle(w, fn, target, {fn}),
                                     "acyclic graph %r: depth_to(%d, %d)" % (edges, fn, target))

    def test_cyclic_reaches_and_bounds(self):
        rng = random.Random(SEED)
        for _ in range(300):
            edges, size = random_graph(rng, 8, 0.3, False)
            w = walk_of(edges, size)
            for target in range(8):
                for fn in range(8):
                    got = w.depth_to(fn, target)
                    want = oracle(w, fn, target, {fn})
                    what = "cyclic graph %r: depth_to(%d, %d)" % (edges, fn, target)
                    self.assertEqual(got is None, want is None, what + ": reachability changed")
                    if got is not None:
                        self.assertGreaterEqual(got, want, what + ": below the simple path")
                    w.chain_to(fn, target)

    def test_ring(self):
        ring = {'r': {'a'}, 'a': {'b'}, 'b': {'c'}, 'c': {'a', 't'}}
        w = walk_of(ring, {'r': 1, 'a': 2, 'b': 4, 'c': 8, 't': 16})
        self.assertEqual(w.depth_to('r', 't'), 31)
        self.assertEqual(oracle(w, 'r', 't', {'r'}), 31)
        self.assertEqual(w.chain_to('r', 't'), ['r[1]', 'a[2] (CYCLE of 3, left at c[8])', 't[16]'])

    def test_diamond_ladder(self):
        rng = random.Random(SEED)
        rungs = 12
        ladder = {}
        size = {}
        for i in range(rungs):
            s_i, a_i, b_i = 's%02d' % i, 'a%02d' % i, 'b%02d' % i
            ladder[s_i] = {a_i, b_i}
            ladder[a_i] = {'s%02d' % (i + 1)}
            ladder[b_i] = {'s%02d' % (i + 1)}
            for n in (s_i, a_i, b_i):
                size[n] = rng.randrange(0, 65)
        end = 's%02d' % rungs
        ladder[end] = set()
        size[end] = 7
        w = walk_of(ladder, size)
        for fn in sorted(ladder):
            self.assertEqual(w.depth_to(fn, end), oracle(w, fn, end, {fn}), fn)

    def test_dense_component_bounded(self):
        dense = 28
        names = ['k%02d' % i for i in range(dense)]
        edges = {'r': {'k00'}, 'k%02d' % (dense - 1): {'t'}}
        size = {'r': 3, 't': 5}
        for i, n in enumerate(names):
            edges.setdefault(n, set()).update(m for m in names if m != n)
            size[n] = 1 + i
        w = bounded_walk_of(edges, size)
        try:
            got = w.depth_to('r', 't')
            w.chain_to('r', 't')
        except TooMany:
            self.fail('a %d-node strongly connected component took more than %d expansions; a '
                      'mutual recursion would hang the gate' % (dense, EXPANSION_BOUND))
        self.assertEqual(got, 3 + 5 + sum(size[n] for n in names))


class SiteGraph(object):
    """A graph whose node `f` makes one indirect call and whose node `z` makes none."""

    def match(self, spec):
        if spec in ('f', 'g'):
            return ['f']
        if spec == 'z':
            return ['z']
        return []

    def caller_spec(self, src):
        return src

    def located(self, src):
        if src == 'z':
            return []
        return ['f.cc:1:1']


class Sites(unittest.TestCase):
    def test_optional_caller_binds_where_the_graph_has_it(self):
        raw = {'?f@1/1': ([('g', False)], 'where')}
        self.assertEqual(trap_redzone.resolve_bindings(SiteGraph(), raw), {'f@1/1': [('g', False)]})

    def test_optional_caller_is_skipped_where_the_graph_lacks_it(self):
        raw = {'?absent@1/1': ([('g', False)], 'where')}
        self.assertEqual(trap_redzone.resolve_bindings(SiteGraph(), raw), {})

    def test_optional_caller_with_only_absent_optional_callees_is_skipped(self):
        raw = {'?f@1/1': ([('?absent', True)], 'where')}
        self.assertEqual(trap_redzone.resolve_bindings(SiteGraph(), raw), {})

    def test_absent_caller_without_the_mark_is_refused(self):
        raw = {'absent@1/1': ([('g', False)], 'where')}
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.resolve_bindings(SiteGraph(), raw)

    def test_optional_caller_making_no_call_is_skipped(self):
        raw = {'?z@1/4': ([('g', False)], 'where')}
        self.assertEqual(trap_redzone.resolve_bindings(SiteGraph(), raw), {})

    def test_caller_making_no_call_without_the_mark_is_refused(self):
        raw = {'z@1/4': ([('g', False)], 'where')}
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.resolve_bindings(SiteGraph(), raw)

    def test_optional_caller_with_another_count_is_refused(self):
        raw = {'?f@1/4': ([('g', False)], 'where')}
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.resolve_bindings(SiteGraph(), raw)


class Nested(unittest.TestCase):
    def test_sums_the_deepest_root_of_each_level(self):
        edges = {'boot': {'leaf'}, 'a': {'leaf'}, 'b': set(), 'c': set(), 'leaf': set()}
        size = {'boot': 100, 'a': 30, 'b': 50, 'c': 7, 'leaf': 10}
        total, steps = trap_redzone.nested_depth(
            walk_of(edges, size), ['boot', 'low', 'high'],
            {'boot': ['boot'], 'low': ['a', 'b'], 'high': ['c']})
        self.assertEqual(total, 110 + 50 + 7)
        self.assertEqual(steps, [('boot', 110, 'boot'), ('low', 50, 'b'), ('high', 7, 'c')])


HEADER = '''/*
 * class OTHER frame=F_FRAME depth=F_DEPTH
 * class MAIN frame=M_FRAME depth=M_DEPTH stack=main
 */
#define F_FRAME 0
#define F_DEPTH 0
#define M_FRAME 0
#define M_DEPTH 0
'''

ROOTS = '''arch    toy header=toy.h
floor   toy files=1 nodes=1 reason: planted
root    toy OTHER other
level   toy MAIN boot reason: planted
level   toy MAIN irq reason: planted
root    toy MAIN start level=boot
root    toy MAIN isr level=irq
root    toy MAIN ?gone level=irq
'''


class LevelGraph(object):
    def resolve(self, sym):
        if sym == 'gone':
            return None
        return sym


class Levels(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        with open(os.path.join(self.tmp, 'toy.h'), 'w') as f:
            f.write(HEADER)

    def decl(self, roots):
        path = os.path.join(self.tmp, 'roots.txt')
        with open(path, 'w') as f:
            f.write(roots)
        return trap_redzone.Decl(path, 'toy', 1, self.tmp)

    def refused(self, roots):
        with self.assertRaises(trap_redzone.Bad):
            self.decl(roots)

    def test_levels_parse_in_order(self):
        d = self.decl(ROOTS)
        self.assertEqual(d.main_stack, {'MAIN'})
        self.assertEqual(list(d.levels['MAIN']), ['boot', 'irq'])
        self.assertEqual(d.root_levels['MAIN'], ['boot', 'irq', 'irq'])
        self.assertIn('MAIN', d.off_thread())

    def test_root_with_no_level_is_refused(self):
        self.refused(ROOTS.replace('isr level=irq', 'isr'))

    def test_level_on_another_class_is_refused(self):
        self.refused(ROOTS.replace('OTHER other', 'OTHER other level=boot'))

    def test_level_with_no_root_is_refused(self):
        self.refused(ROOTS + 'level   toy MAIN fault reason: planted\n')

    def test_undeclared_level_is_refused(self):
        self.refused(ROOTS + 'root    toy MAIN fault_report level=fault\n')

    def test_none_is_refused(self):
        self.refused(ROOTS.replace('root    toy MAIN start level=boot\n',
                                   'root    toy MAIN NONE reason: planted\n'
                                   'root    toy MAIN start level=boot\n'))

    def test_level_whose_roots_are_all_absent_is_refused(self):
        d = self.decl(ROOTS.replace('root    toy MAIN isr level=irq\n', ''))
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.root_keys(LevelGraph(), d, 'MAIN', [], {})

    def test_present_roots_are_keyed_by_level(self):
        d = self.decl(ROOTS)
        levels = {}
        trap_redzone.root_keys(LevelGraph(), d, 'MAIN', [], levels)
        self.assertEqual(levels, {'boot': ['start'], 'irq': ['isr']})


class MainStack(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def map(self, name, start, end, prefix='__'):
        path = os.path.join(self.tmp, name)
        with open(path, 'w') as f:
            f.write('                0x%08x                        %skickos_kernel_stack_start = '
                    'ABSOLUTE (__kickos_ram_end)\n' % (start, prefix))
            f.write('                0x%08x                        %skickos_kernel_stack_end = '
                    'ABSOLUTE (_estack)\n' % (end, prefix))
        return path

    def listing(self, paths):
        with open(os.path.join(self.tmp, 'kickos-image-maps.txt'), 'w') as f:
            f.write(';'.join(paths).replace(';', '\n'))

    def test_smallest_listed_span_binds(self):
        a = self.map('a.map', 0x20007800, 0x20008000)
        b = self.map('b.map', 0x20007000, 0x20008000, prefix='___')
        self.listing([a, b, os.path.join(self.tmp, 'unbuilt.map')])
        self.assertEqual(trap_redzone.main_stack_size(self.tmp), (2048, 2, a))

    def test_map_without_the_pair_is_refused(self):
        a = os.path.join(self.tmp, 'a.map')
        with open(a, 'w') as f:
            f.write('                0x20007800                        __kickos_ram_end = .\n')
        self.listing([a])
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.main_stack_size(self.tmp)

    def test_no_listed_map_is_refused(self):
        self.listing([os.path.join(self.tmp, 'unbuilt.map')])
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.main_stack_size(self.tmp)


class MainOptions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def decl(self, header):
        with open(os.path.join(self.tmp, 'toy.h'), 'w') as f:
            f.write(header)
        with open(os.path.join(self.tmp, 'roots.txt'), 'w') as f:
            f.write(ROOTS + 'level   toy CORE boot reason: planted\n'
                    'root    toy CORE park level=boot\n')
        return trap_redzone.Decl(os.path.join(self.tmp, 'roots.txt'), 'toy', 1, self.tmp)

    def header(self, core):
        return HEADER.replace(' */', ' * class CORE ' + core + '\n */') + '#define C_DEPTH 0\n'

    def test_size_and_secondary_parse(self):
        d = self.decl(self.header('frame=F_FRAME depth=C_DEPTH stack=main size=C_SIZE'
                                  ' core=secondary'))
        self.assertEqual(d.main_stack, {'MAIN', 'CORE'})
        self.assertEqual(d.main_size, {'CORE': 'C_SIZE'})
        self.assertEqual(d.secondary, {'CORE'})

    def test_size_off_the_main_stack_is_refused(self):
        with self.assertRaises(trap_redzone.Bad):
            self.decl(self.header('frame=F_FRAME depth=C_DEPTH size=C_SIZE'))

    def test_secondary_off_the_main_stack_is_refused(self):
        with self.assertRaises(trap_redzone.Bad):
            self.decl(self.header('frame=F_FRAME depth=C_DEPTH stack=idle core=secondary'))


def elf(path, wide, ctors, table=False, machine=0, addrs=(0x1000, 0x1004), infos=(0x12, 0x12)):
    """A `machine` image with two code symbols at `addrs` of `infos` and an .init_array of
    `ctors`; with `table`, a sized `vectors` table over that text naming both."""
    word = 4
    if wide:
        word = 8
    names = ['', 'ctor_a', 'ctor_b', '__init_array_start', '__init_array_end', 'vectors']
    strtab = b'\0'.join(n.encode() for n in names) + b'\0'
    offs = [0]
    for n in names[1:]:
        offs.append(offs[-1] + len(names[len(offs) - 1]) + 1)
    syms = [(0, 0, 0, 0), (offs[1], addrs[0], infos[0], 1), (offs[2], addrs[1], infos[1], 1),
            (offs[3], 0x2000, 0x10, 2), (offs[4], 0x2000 + word * len(ctors), 0x10, 2)]
    text = b'\0' * 8
    if table:
        syms.append((offs[5], 0x1000, 0x11, 1))
        text = struct.pack('<II', addrs[0], addrs[1])
    array = b''.join(struct.pack({4: '<I', 8: '<Q'}[word], c) for c in ctors)
    symtab = b''
    for name, value, info, shndx in syms:
        size = 0
        if info == 0x11:
            size = len(text)
        if wide:
            symtab += struct.pack('<IBBHQQ', name, info, 0, shndx, value, size)
        else:
            symtab += struct.pack('<IIIBBH', name, value, size, info, 0, shndx)
    ehsize = 52
    if wide:
        ehsize = 64
    body = text + array + symtab + strtab
    at = [ehsize, ehsize + len(text), ehsize + len(text) + len(array),
          ehsize + len(text) + len(array) + len(symtab)]
    shoff = ehsize + len(body)
    secs = [(0, 0, 0, 0, 0), (1, 0x1000, at[0], len(text), 0), (14, 0x2000, at[1], len(array), 0),
            (2, 0, at[2], len(symtab), 4), (3, 0, at[3], len(strtab), 0)]
    shdrs = b''
    for typ, addr, off, size, link in secs:
        if wide:
            shdrs += struct.pack('<IIQQQQIIQQ', 0, typ, 0, addr, off, size, link, 0, 0, 0)
        else:
            shdrs += struct.pack('<IIIIIIIIII', 0, typ, 0, addr, off, size, link, 0, 0, 0)
    ident = b'\x7fELF' + bytes([1 + int(wide), 1, 1]) + b'\0' * 9
    if wide:
        head = ident + struct.pack('<HHIQQQIHHHHHH', 2, machine, 1, 0, 0, shoff, 0, ehsize, 0, 0,
                                   64, len(secs), 0)
    else:
        head = ident + struct.pack('<HHIIIIIHHHHHH', 2, machine, 1, 0, 0, shoff, 0, ehsize, 0, 0,
                                   40, len(secs), 0)
    with open(path, 'wb') as f:
        f.write(head + body + shdrs)


class Ctors(unittest.TestCase):
    class D(object):
        tables = []
        vectors = {}
        inits = [('__init_array_start', '__init_array_end')]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def refusals(self, wide, ctors):
        path = os.path.join(self.tmp, 'image')
        elf(path, wide, ctors)
        return trap_redzone.vector_refusals(self.D(), path, set(),
                                            lambda names: 'ctor_a' in names)

    def test_a_reached_constructor_passes(self):
        for wide in (False, True):
            self.assertEqual(self.refusals(wide, [0x1000]), [])

    def test_an_empty_array_passes(self):
        for wide in (False, True):
            self.assertEqual(self.refusals(wide, []), [])

    def test_a_constructor_no_walk_reaches_is_refused(self):
        for wide in (False, True):
            out = self.refusals(wide, [0x1000, 0x1004])
            self.assertEqual(len(out), 1)
            self.assertIn('ctor_b', out[0][1])

    def test_a_constructor_a_byte_past_a_priced_one_is_refused_off_arm(self):
        path = os.path.join(self.tmp, 'image')
        elf(path, True, [0x1000, 0x1001], machine=62, addrs=(0x1000, 0x1001))
        out = trap_redzone.vector_refusals(self.D(), path, set(), lambda names: 'ctor_a' in names)
        self.assertEqual(len(out), 1)
        self.assertIn('0x1001 (ctor_b)', out[0][1])

    def test_an_arm_entry_names_the_code_its_thumb_bit_clears(self):
        path = os.path.join(self.tmp, 'image')
        elf(path, False, [0x1001, 0x1005], machine=40, addrs=(0x1000, 0x1005), infos=(0x10, 0x12))
        out = trap_redzone.vector_refusals(self.D(), path, set(), lambda names: 'ctor_a' in names)
        self.assertEqual(len(out), 1)
        self.assertIn('0x1005 (ctor_b)', out[0][1])

    def test_a_table_read_before_the_array_leaves_its_word_size(self):
        class T(self.D):
            tables = [('vectors', 0, '', False)]
        path = os.path.join(self.tmp, 'image')
        elf(path, False, [0x1000, 0x1004], table=True)
        out = trap_redzone.vector_refusals(T(), path, {'ctor_a', 'ctor_b'},
                                           lambda names: 'ctor_a' in names)
        self.assertEqual(len(out), 1)
        self.assertIn('ctor_b', out[0][1])

    def test_the_elf_beside_a_converted_map_is_read(self):
        for name in ('a.efi.map', 'a.efi', 'a.efi.elf', 'b.map', 'b'):
            open(os.path.join(self.tmp, name), 'w').close()
        with open(os.path.join(self.tmp, 'kickos-image-maps.txt'), 'w') as f:
            f.write('%s\n%s\n' % (os.path.join(self.tmp, 'a.efi.map'),
                                   os.path.join(self.tmp, 'b.map')))
        self.assertEqual(trap_redzone.linked_images(self.tmp),
                         [os.path.join(self.tmp, 'a.efi.elf'), os.path.join(self.tmp, 'b')])


class LinkMaps(unittest.TestCase):
    def setUp(self):
        ninja = shutil.which('ninja')
        self.assertIsNotNone(ninja, 'no ninja on PATH, and the gate asks the build graph')
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.write('CMakeCache.txt', 'CMAKE_MAKE_PROGRAM:FILEPATH=%s\n' % ninja)
        self.write('build.ninja', 'rule link\n  command = true\n'
                   'build app/app.efi: link\nbuild app: phony app/app.efi\nbuild top: link\n')
        self.write('app/app.efi.map', 'LOAD /linked.o\n')

    def write(self, rel, text):
        path = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write(text)

    def test_a_map_beside_no_linked_file_is_not_read(self):
        self.write('gone/gone.efi.map', 'LOAD /stale.o\n')
        self.assertEqual(trap_redzone.Graph.load_set(self.tmp, '**/*.efi.map'),
                         {os.path.realpath('/linked.o')})

    def test_only_unlinked_maps_are_refused(self):
        os.remove(os.path.join(self.tmp, 'app/app.efi.map'))
        self.write('gone/gone.efi.map', 'LOAD /stale.o\n')
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.Graph.load_set(self.tmp, '**/*.efi.map')

    def test_a_target_names_its_file(self):
        self.assertEqual(trap_redzone.target_file(self.tmp, 'app'),
                         os.path.join(self.tmp, 'app/app.efi'))
        self.assertEqual(trap_redzone.target_file(self.tmp, 'top'), os.path.join(self.tmp, 'top'))
        self.assertIsNone(trap_redzone.target_file(self.tmp, 'gone'))


class Corpus(unittest.TestCase):
    def setUp(self):
        ninja = shutil.which('ninja')
        self.assertIsNotNone(ninja, 'no ninja on PATH, and the gate asks the build graph')
        self.tmp = os.path.realpath(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.write('CMakeCache.txt', 'CMAKE_MAKE_PROGRAM:FILEPATH=%s\n' % ninja)
        self.write('build.ninja', 'rule cc\n  command = true\n'
                   'build a.cc.o: cc\nbuild b.cc.o: cc\n')
        self.write('compile_commands.json', json.dumps(
            [{'directory': self.tmp, 'file': '/src/a.cc', 'output': 'a.cc.o',
              'command': 'g++ -c /src/a.cc -o a.cc.o'}]))
        self.write('a.cc.ci', 'graph: { title: "/src/a.cc"\n')

    def write(self, rel, text):
        path = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write(text)

    def test_a_ci_beside_no_built_object_is_not_read(self):
        self.write('gone.cc.ci', 'graph: { title: "/src/gone.cc"\n')
        self.write('CMakeFiles/4.3.4/CompilerIdCXX/CMakeCXXCompilerId.ci', '')
        self.assertEqual(trap_redzone.corpus(self.tmp), [os.path.join(self.tmp, 'a.cc.ci')])

    def test_a_ci_beside_a_built_object_no_command_names_is_refused(self):
        self.write('b.cc.ci', 'graph: { title: "/src/b.cc"\n')
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.corpus(self.tmp)

    def test_a_command_with_no_ci_is_refused(self):
        os.remove(os.path.join(self.tmp, 'a.cc.ci'))
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.corpus(self.tmp)


ASM = r"""# 1 "arch/toy/switch.S"
    .global own_entry
own_entry:
    la      t0, g_data
    call    landed
    call    rooted
    tail    own_entry
    .error  "landed in a string"
"""


class AsmCalls(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.realpath(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def write(self, rel, text):
        path = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write(text)
        return path

    def names(self, text, arch='rv32imac', prefix=''):
        refs, _files = trap_redzone.asm_references(text, prefix, trap_redzone.asm_comment(arch))
        return [sym for _where, _n, sym in refs]

    def refused(self, text, rooted, prefix='', asmcalls=(), priced=(), raw=None):
        refs, _files = trap_redzone.asm_references(text, prefix, '#')
        if raw is None:
            raw = set(sym for _where, _n, sym in refs)
        return trap_redzone.asm_refusals(refs, raw, {'landed', 'rooted', 'own_entry'}, rooted,
                                         set(priced), asmcalls, [])

    def test_an_unrooted_call_is_refused_once_at_its_line(self):
        out = self.refused(ASM, {'rooted'})
        self.assertEqual(len(out), 1)
        self.assertIn('arch/toy/switch.S:4 names landed', out[0])

    def test_the_line_markers_place_an_included_file_s_call(self):
        text = '# 1 "a.S"\n    nop\n# 7 "b.inc" 1\n    nop\n    call landed\n# 3 "a.S" 2\n'
        out = self.refused(text, {'rooted'})
        self.assertEqual(len(out), 1)
        self.assertIn('b.inc:8 names landed', out[0])

    def test_a_rooted_call_passes(self):
        self.assertEqual(self.refused(ASM, {'rooted', 'landed'}), [])

    def test_a_call_a_priced_edge_reaches_passes(self):
        self.assertEqual(self.refused(ASM, {'rooted'}, priced=('landed',)), [])

    def test_strings_and_own_labels_are_not_calls(self):
        text = ASM.replace('    call    landed\n', '')
        self.assertEqual(self.refused(text, {'rooted'}), [])

    def test_each_arch_s_comment_character_hides_a_name(self):
        self.assertNotIn('landed', self.names('    bl rooted @ landed\n', 'armv7m'))
        self.assertNotIn('landed', self.names('    bl rooted @ landed\n', 'armv6m'))
        self.assertNotIn('landed', self.names('    bsr.a rooted ; landed\n', 'rxv3'))
        self.assertNotIn('landed', self.names('    call4 rooted # landed\n', 'lx6'))
        self.assertNotIn('landed', self.names('    call rooted # landed\n', 'rv64imac'))
        self.assertNotIn('landed', self.names('    call rooted # landed\n', 'x86_64'))
        self.assertNotIn('landed', self.names('    bl rooted // landed\n', 'armv8a'))
        self.assertNotIn('landed', self.names('# landed at the start of a line\n', 'armv7m'))

    def test_a_hash_mid_line_is_an_immediate_where_it_is_not_a_comment(self):
        self.assertIn('landed', self.names('    mov.l #landed, r1\n', 'rxv3'))
        self.assertIn('landed', self.names('    ldr r0, =landed\n    mov r1, #1\n', 'armv7m'))
        self.assertIn('landed', self.names('    mov x0, #1\n    bl landed\n', 'armv8a'))
        self.assertIn('landed', self.names('    adr x0, landed + #8\n', 'armv8a'))

    def test_an_arch_with_no_known_comment_syntax_is_refused(self):
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.asm_comment('toy')

    def test_the_label_prefix_is_stripped(self):
        text = '    bsr.a   _landed\n    bsr.a   rooted\n'
        self.assertEqual(len(self.refused(text, set(), prefix='_')), 1)
        self.assertEqual(self.refused(text, {'landed'}, prefix='_'), [])

    def test_an_asmcall_record_places_a_call_off_every_trap(self):
        self.assertEqual(self.refused(ASM, {'rooted'}, asmcalls=('landed',)), [])

    def test_an_asmcall_that_is_also_a_root_is_refused(self):
        self.assertEqual(len(self.refused(ASM, {'rooted', 'landed'}, asmcalls=('landed',))), 1)

    def test_an_asmcall_that_is_also_a_priced_edge_is_refused(self):
        self.assertEqual(len(self.refused(ASM, {'rooted'}, asmcalls=('landed',),
                                          priced=('landed',))), 1)

    def test_an_asmcall_no_branch_of_the_assembly_names_is_refused(self):
        text = ASM.replace('    call    landed\n', '')
        self.assertEqual(len(self.refused(text, {'rooted'}, asmcalls=('landed',))), 1)

    def test_an_asmcall_this_posture_compiles_out_is_not_stale(self):
        text = ASM.replace('    call    landed\n', '')
        self.assertEqual(self.refused(text, {'rooted'}, asmcalls=('landed',),
                                      raw={'rooted', 'landed'}), [])

    def test_raw_names_read_every_branch_and_no_comment(self):
        path = self.write('arch/toy/x.inc', '#if 0\n    call landed\n#endif\n'
                          '    call rooted @ own_entry\n/* g_data */\n')
        self.assertEqual(trap_redzone.asm_raw_names({path}, '', '@'), {'call', 'landed', 'rooted'})
        hdr = self.write('arch/toy/x.h', '    call landed\n')
        self.assertEqual(trap_redzone.asm_raw_names({hdr}, '', '@'), set())

    def test_the_arch_family_directory_is_read_and_nothing_outside_it(self):
        own = self.write('arch/toy/rv/switch.S', '    call landed\n')
        chip = self.write('arch/toy/chip/startup.S', '    call landed\n')
        other = self.write('arch/other/startup.S', '    call landed\n')
        user = self.write('user/src/arch/rv/syscall.S', '    ecall\n')
        self.write('build/compile_commands.json', json.dumps(
            [{'directory': self.tmp, 'file': f} for f in (own, chip, other, user)]
            + [{'directory': self.tmp, 'file': os.path.join(self.tmp, 'arch/toy/rv/c.cc')}]))
        units = trap_redzone.asm_units(os.path.join(self.tmp, 'build'), self.tmp,
                                       'arch/toy/rv/include/kickos/arch/toy_trap_stack.h')
        self.assertEqual([u['file'] for u in units], [chip, own])

    def test_no_assembly_under_the_arch_family_is_refused(self):
        self.write('build/compile_commands.json', json.dumps(
            [{'directory': self.tmp, 'file': os.path.join(self.tmp, 'arch/toy/rv/c.cc')}]))
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.asm_units(os.path.join(self.tmp, 'build'), self.tmp,
                                   'arch/toy/rv/include/kickos/arch/toy_trap_stack.h')

    def test_the_preprocessor_reads_includes_and_only_the_compiled_branch(self):
        cc = shutil.which('cc') or shutil.which('gcc')
        self.assertIsNotNone(cc, 'the host compiler preprocesses the planted unit')
        self.write('arch/toy/rv/include/hook.inc', '    .macro HOOK\n    call rooted\n'
                   '    .endm\n')
        unit = self.write('arch/toy/rv/switch.S', '#include "hook.inc"\n#if WITH_LANDED\n'
                          '    call landed\n#endif\n    HOOK\n')
        entry = {'directory': self.tmp, 'file': unit,
                 'command': '%s -DWITH_LANDED=0 -Iarch/toy/rv/include -o switch.o -c %s'
                            % (cc, unit)}
        refs, files = trap_redzone.asm_references(trap_redzone.asm_preprocess(entry, self.tmp),
                                                  '', '#')
        named = [(os.path.basename(w), n, sym) for w, n, sym in refs]
        self.assertIn(('hook.inc', 2, 'rooted'), named)
        self.assertNotIn('landed', [sym for _w, _n, sym in refs])
        self.assertIn(os.path.join(self.tmp, 'arch/toy/rv/include/hook.inc'),
                      set(os.path.realpath(os.path.join(self.tmp, f)) for f in files))
        entry['command'] = entry['command'].replace('-DWITH_LANDED=0', '-DWITH_LANDED=1')
        refs, _files = trap_redzone.asm_references(
            trap_redzone.asm_preprocess(entry, self.tmp), '', '#')
        self.assertIn(('switch.S', 3, 'landed'),
                      [(os.path.basename(w), n, sym) for w, n, sym in refs])

    def test_a_unit_the_preprocessor_refuses_is_refused(self):
        unit = self.write('arch/toy/rv/switch.S', '#include "missing.inc"\n')
        cc = shutil.which('cc') or shutil.which('gcc')
        entry = {'directory': self.tmp, 'file': unit, 'command': '%s -c %s' % (cc, unit)}
        with self.assertRaises(trap_redzone.Bad):
            trap_redzone.asm_preprocess(entry, self.tmp)


class PricedEdges(unittest.TestCase):
    class G(object):
        def __init__(self, edges):
            self.edges = edges

        def resolve(self, sym):
            if sym in self.edges or any(sym in v for v in self.edges.values()):
                return sym
            return None

    class D(object):
        unsized_calls = {'body': [('hook', True), ('gone', True)]}

    def test_a_callee_of_a_reached_unsized_body_is_priced(self):
        g = self.G({'root': {'body'}, 'body': {'hook'}})
        self.assertEqual(trap_redzone.priced_edges(g, self.D(), {'body': 8},
                                                   {'root', 'body', 'hook'}), {'hook'})

    def test_a_body_no_class_reaches_prices_nothing(self):
        g = self.G({'body': {'hook'}})
        self.assertEqual(trap_redzone.priced_edges(g, self.D(), {'body': 8}, {'root'}), set())

    def test_a_callee_every_walk_cuts_is_not_priced(self):
        g = self.G({'root': {'body'}, 'body': {'hook'}})
        self.assertEqual(trap_redzone.priced_edges(g, self.D(), {'body': 8}, {'root', 'body'}),
                         set())

    def test_a_declared_callee_the_body_has_no_edge_to_is_not_priced(self):
        g = self.G({'root': {'body'}, 'body': set(), 'other': {'hook'}})
        self.assertEqual(trap_redzone.priced_edges(g, self.D(), {'body': 8},
                                                   {'root', 'body', 'hook'}), set())

    def test_a_body_that_is_not_charged_prices_nothing(self):
        g = self.G({'root': {'body'}, 'body': {'hook'}})
        self.assertEqual(trap_redzone.priced_edges(g, self.D(), {}, {'root', 'body', 'hook'}),
                         set())


if __name__ == "__main__":
    unittest.main()
