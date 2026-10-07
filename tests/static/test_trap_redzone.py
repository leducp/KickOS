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
#   - a ladder of twelve diamonds, too deep for the random graphs, equals the oracle.

import os
import random
import sys
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


if __name__ == "__main__":
    unittest.main()
