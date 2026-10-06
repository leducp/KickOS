#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Controls for the at= walk of trap_redzone.py (Walk.depth_to and Walk.chain_to), on planted
# graphs and against an exhaustive longest-simple-path oracle:
#   - on every acyclic graph the walk answers exactly what the oracle does;
#   - on a cyclic graph it reaches target exactly where the oracle does, and never answers
#     less than the oracle;
#   - a dense strongly connected component is walked in a bounded number of expansions, so
#     a mutual recursion is reported as a cycle instead of hanging the gate;
#   - chain_to names a cycle once and leaves it by its deepest edge, ending at the target;
#   - a ladder of twelve diamonds, too deep for the random graphs, equals the oracle.

set -u
. "$(dirname "$0")/../lib/gate.sh"

TOOL="$(dirname "$0")/trap_redzone.py"
[ -r "$TOOL" ] || fail "$TOOL is unreadable"
command -v python3 >/dev/null 2>&1 || fail "python3 not found; the walk cannot run"

python3 -B - "$TOOL" <<'PYEOF'
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[1])))
import trap_redzone

sys.setrecursionlimit(20000)

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


def die(msg):
    sys.stderr.write('FAIL: %s\n' % msg)
    sys.exit(1)


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


rng = random.Random(20261005)

acyclic = 0
for _ in range(300):
    edges, size = random_graph(rng, 10, 0.35, True)
    w = walk_of(edges, size)
    for target in range(10):
        for fn in range(10):
            got = w.depth_to(fn, target)
            want = oracle(w, fn, target, {fn})
            if got != want:
                die('acyclic graph %r: depth_to(%d, %d) is %r, the longest path is %r'
                    % (edges, fn, target, got, want))
            acyclic += 1

cyclic = 0
for _ in range(300):
    edges, size = random_graph(rng, 8, 0.3, False)
    w = walk_of(edges, size)
    for target in range(8):
        for fn in range(8):
            got = w.depth_to(fn, target)
            want = oracle(w, fn, target, {fn})
            if (got is None) != (want is None):
                die('cyclic graph %r: depth_to(%d, %d) is %r where the oracle says %r;'
                    ' reachability must not change' % (edges, fn, target, got, want))
            if got is not None and got < want:
                die('cyclic graph %r: depth_to(%d, %d) is %r, below the simple path of %r'
                    % (edges, fn, target, got, want))
            w.chain_to(fn, target)
            cyclic += 1

ring = {'r': {'a'}, 'a': {'b'}, 'b': {'c'}, 'c': {'a', 't'}}
ring_size = {'r': 1, 'a': 2, 'b': 4, 'c': 8, 't': 16}
w = walk_of(ring, ring_size)
got = w.depth_to('r', 't')
if got != 31 or got != oracle(w, 'r', 't', {'r'}):
    die('ring r->a->b->c->a, c->t: depth_to is %r, want 31' % got)
chain = w.chain_to('r', 't')
if chain != ['r[1]', 'a[2] (CYCLE of 3, left at c[8])', 't[16]']:
    die('ring: chain_to is %r, not the cycle named once and left for the target' % chain)

rungs = 12
ladder = {}
ladder_size = {}
for i in range(rungs):
    s_i, a_i, b_i = 's%02d' % i, 'a%02d' % i, 'b%02d' % i
    ladder[s_i] = {a_i, b_i}
    ladder[a_i] = {'s%02d' % (i + 1)}
    ladder[b_i] = {'s%02d' % (i + 1)}
    ladder_size[s_i] = rng.randrange(0, 65)
    ladder_size[a_i] = rng.randrange(0, 65)
    ladder_size[b_i] = rng.randrange(0, 65)
end = 's%02d' % rungs
ladder[end] = set()
ladder_size[end] = 7
w = walk_of(ladder, ladder_size)
for fn in sorted(ladder):
    got = w.depth_to(fn, end)
    want = oracle(w, fn, end, {fn})
    if got != want:
        die('diamond ladder: depth_to(%s) is %r, the longest path is %r' % (fn, got, want))

dense = 28
names = ['k%02d' % i for i in range(dense)]
edges = {'r': {'k00'}, 'k%02d' % (dense - 1): {'t'}}
size = {'r': 3, 't': 5}
for i, n in enumerate(names):
    edges.setdefault(n, set()).update(m for m in names if m != n)
    size[n] = 1 + i
want = 3 + 5 + sum(size[n] for n in names)
w = bounded_walk_of(edges, size)
try:
    got = w.depth_to('r', 't')
    w.chain_to('r', 't')
except TooMany:
    die('a %d-node strongly connected component took more than %d expansions; the walk is'
        ' exponential in it and a mutual recursion would hang the gate'
        % (dense, EXPANSION_BOUND))
if got != want:
    die('dense component: depth_to is %r, want %d' % (got, want))

print('PASS: %d acyclic pairs equal the oracle, %d cyclic pairs reach where it does and'
      ' bound it, a %d-node component walked in %d expansions'
      % (acyclic, cyclic, dense, w.expansions))
PYEOF
