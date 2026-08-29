# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Reachability-only clause on the fault-record console route: no kpanic from kvprintf_route. It
# walks the callgraph tests/static/trap_redzone.py builds, out of the .ci files
# gcc -fcallgraph-info=su,da leaves next to every object, importing that module.
#
# From the declared roots, following direct edges and the indirect edges bound in the bindings
# file, no path reaches kpanic or kpanic_at.

import collections
import os
import sys

# The .ci parser, the node-key scoping, the site keys and the binding reader all live in the
# trap-stack tool. Running this file as a script already puts its directory first on the path;
# the insert is for the case where it is imported instead.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trap_redzone as tz

Bad = tz.Bad
die = tz.die
SITE_PREFIX = tz.SITE_PREFIX


class Decl(object):
    def __init__(self, path, arch, preset):
        self.arch = arch
        self.preset = preset
        self.presets = []                 # (arch, preset) pairs declared for THIS arch
        self.floor_files = None
        self.floor_nodes = None
        self.roots = []                   # [(symbol, optional, reason)]
        self.witnesses = []               # [(symbol, reason)]
        self.forbid = []                  # [(symbol, optional, reason)]
        self.opaque = collections.OrderedDict()   # symbol -> reason
        for n, f, reason in tz.records(path):
            where = '%s:%d' % (path, n)
            kind = f[0]
            if len(f) < 2:
                die('%s: record "%s" has no arch' % (where, kind))
            if kind == 'preset':
                if len(f) != 3:
                    die('%s: preset wants <arch> <configure-preset>' % where)
                if f[1] == arch:
                    self.presets.append(f[2])
                continue
            if kind == 'floor':
                if len(f) != 5:
                    die('%s: floor wants <arch> <preset> files=<n> nodes=<n>' % where)
                if reason is None:
                    die('%s: floor carries no "reason:"; a figure with no measurement behind'
                        ' it is a guess this clause would then trust' % where)
                if f[1] != arch or f[2] != preset:
                    continue
                self.floor_files = self._number(where, f[3], 'files=')
                self.floor_nodes = self._number(where, f[4], 'nodes=')
                continue
            if f[1] != arch and f[1] != '*':
                continue
            if reason is None:
                die('%s: %s %s carries no "reason:"; an undocumented declaration is how a'
                    ' clause goes quiet' % (where, kind, f[2]))
            if len(f) != 3:
                die('%s: %s wants <arch> <symbol> and one reason' % (where, kind))
            sym = f[2]
            optional = sym.startswith('?')
            sym = sym.lstrip('?')
            if kind == 'root':
                self.roots.append((sym, optional, reason))
            elif kind == 'witness':
                if optional:
                    die('%s: witness %s is marked optional. A witness that may be absent'
                        ' proves nothing about the root set' % (where, sym))
                self.witnesses.append((sym, reason))
            elif kind == 'forbid':
                self.forbid.append((sym, optional, reason))
            elif kind == 'opaque':
                if optional:
                    die('%s: opaque %s is marked optional; the list is consulted only for a'
                        ' node the walk actually reached' % (where, sym))
                self.opaque[sym] = reason
            else:
                die('%s: unknown record "%s"' % (where, kind))
        if not self.presets:
            die('%s declares no preset for arch %s, so this arch is not one this clause'
                ' gates at all' % (path, arch))
        if preset not in self.presets:
            die('preset %s is not declared for arch %s in %s (declared: %s)'
                % (preset, arch, path, ' '.join(self.presets)))
        if self.floor_files is None:
            die('%s declares no floor record for %s/%s. Without one the clause reports the'
                ' same clean answer over a full build and over an empty directory'
                % (path, arch, preset))
        for name, got in (('root', self.roots), ('witness', self.witnesses),
                          ('forbid', self.forbid)):
            if not got:
                die('%s declares no %s record for arch %s' % (path, name, arch))

    @staticmethod
    def _number(where, field, key):
        if not field.startswith(key):
            die('%s: expected %s<n>, got "%s"' % (where, key, field))
        try:
            return int(field[len(key):])
        except ValueError:
            die('%s: %s"%s" is not a number' % (where, key, field[len(key):]))


def show(key):
    if key.startswith(SITE_PREFIX):
        return 'indirect call at ' + key[len(SITE_PREFIX):]
    return key


class Reach(object):
    """Breadth-first, so a reported path is the shortest one and reads as an explanation."""

    def __init__(self, graph, roots):
        self.g = graph
        self.roots = list(roots)
        self.parent = {}
        self.dist = {}
        self.order = []
        todo = collections.deque()
        for r in self.roots:
            if r in self.dist:
                continue
            self.dist[r] = 0
            self.order.append(r)
            todo.append(r)
        while todo:
            fn = todo.popleft()
            for t in sorted(self.g.edges.get(fn, ())):
                if t in self.dist:
                    continue
                self.dist[t] = self.dist[fn] + 1
                self.parent[t] = fn
                self.order.append(t)
                todo.append(t)

    def seen(self):
        return set(self.dist)

    def path(self, key):
        if key not in self.dist:
            return None
        out = [key]
        while out[-1] in self.parent:
            out.append(self.parent[out[-1]])
        out.reverse()
        return ' -> '.join(show(k) for k in out)

    def deepest(self, exclude):
        """The reachable real function furthest from the roots, ties broken by name."""
        best = None
        for key in self.order:
            if key.startswith(SITE_PREFIX) or key in exclude:
                continue
            if key not in self.g.size:
                continue                       # opaque: it has no out-edge to hang one on
            if best is None or self.dist[key] > self.dist[best]:
                best = key
            elif self.dist[key] == self.dist[best] and key < best:
                best = key
        return best


def resolve_all(graph, decl_list, kind):
    """[(key, symbol, optional)] for the ones the graph has, plus the absent optional ones."""
    found = []
    absent = []
    for sym, optional, reason in decl_list:
        key = graph.resolve(sym)
        if key is None:
            if optional:
                absent.append(sym)
                continue
            die('%s %s is not in this graph and is not declared optional. Either it was'
                ' renamed or the compiler folded it away, and a clause walking a graph that'
                ' no longer contains it proves nothing' % (kind, sym))
        found.append((key, sym, optional))
    return found, absent


def usage():
    sys.stderr.write(
        'usage: console_reach.py --ci-dir <dir> --arch <arch> --preset <preset>\n'
        '                        --decl <file> --indirect <file>\n')
    return 2


def parse_argv(argv):
    want = {'--ci-dir', '--arch', '--preset', '--decl', '--indirect'}
    opt = {}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a not in want:
            die('unknown argument "%s"' % a)
        if i + 1 >= len(argv):
            die('%s wants a value' % a)
        opt[a.lstrip('-')] = argv[i + 1]
        i += 2
    for a in sorted(want):
        if a.lstrip('-') not in opt:
            die('missing %s' % a)
    return opt


def run(argv):
    opt = parse_argv(argv)
    arch = opt['arch']
    preset = opt['preset']
    decl = Decl(opt['decl'], arch, preset)

    graph = tz.Graph(opt['ci-dir'])
    nodes = len(graph.universe())
    print('console_reach: arch=%s preset=%s (%d .ci files read, %d unextracted seam fallbacks'
          ' skipped, %d nodes, %d indirect sites)'
          % (arch, preset, graph.files, len(graph.dropped), nodes, len(graph.all_sites())))

    # --- the corpus floor, before anything is asserted about an absence ---------
    if graph.files < decl.floor_files:
        die('CORPUS FLOOR: %d .ci file(s) under %s, and %s declares a floor of %d for %s/%s.'
            ' This is a partial or interrupted build, not a clean route: the walk below would'
            ' find no panic because there is nothing to walk'
            % (graph.files, opt['ci-dir'], opt['decl'], decl.floor_files, arch, preset))
    if nodes < decl.floor_nodes:
        die('CORPUS FLOOR: %d graph node(s), and %s declares a floor of %d for %s/%s. A graph'
            ' this small is not this image'
            % (nodes, opt['decl'], decl.floor_nodes, arch, preset))

    # --- the roots, and the proof that they are the route -----------------------
    # The walk starts at the REQUIRED roots only. An optional root that IS in this graph is
    # then checked for reachability from them below, which is what turns "gcc inlined it into
    # its callers" from an assumption into a measurement on the boards where it did not.
    roots, roots_absent = resolve_all(graph, decl.roots, 'root')
    for sym in roots_absent:
        print('  root %s: ABSENT, declared optional' % sym)
    root_keys = [k for k, _s, o in roots if not o]
    covered = [(k, s) for k, s, o in roots if o]
    if not root_keys:
        die('every REQUIRED root is absent from this graph, so the walk would start nowhere'
            ' and report the route clean')
    for key, sym, o in roots:
        where = 'root'
        if o:
            where = 'root (optional, checked as covered)'
        print('  %s %s -> %s' % (where, sym, key))

    forbid, forbid_absent = resolve_all(graph, decl.forbid, 'forbid')
    for sym in forbid_absent:
        print('  forbid %s: ABSENT from this image, declared optional' % sym)
    if not forbid:
        die('not one forbidden symbol is in this graph. There is nothing for the clause to'
            ' look for, so a clean answer would say only that the panic terminal is gone')

    # A symbol nothing calls anywhere in the corpus cannot be found on this route either, so
    # its absence here would be a property of the graph and not of the route.
    for key, sym, _optional in forbid:
        callers = [n for n in graph.edges if key in graph.edges[n]]
        if not callers:
            die('CONTROL: %s is in the graph and NOTHING in the corpus calls it. The edges'
                ' this clause looks for are not being emitted, so its answer about this route'
                ' would be an artefact of the graph' % sym)
        print('  forbid %s -> %s, called from %d place(s) in the corpus'
              % (sym, key, len(callers)))

    # --- indirect edges ---------------------------------------------------------
    bindings = tz.read_bindings(opt['indirect'], arch, preset)
    present = graph.all_sites()
    for site in bindings:
        if site not in present:
            die('binding for site %s is stale: no __indirect_call edge in the graph carries'
                ' that file:line:column' % site)
    graph.bind_indirect(bindings)

    walk = Reach(graph, root_keys)
    reach = walk.seen()

    fails = []

    # --- the root set still covers the route ------------------------------------
    for sym, _reason in decl.witnesses:
        key = graph.resolve(sym)
        if key is None:
            die('witness %s is not in this graph. The route this clause walks no longer'
                ' contains the call it is defined by, so the root set is stale' % sym)
        if key not in reach:
            die('witness %s (%s) is NOT reachable from the declared roots. The roots no longer'
                ' cover the routing body, so an absence found from them says nothing about'
                ' the route' % (sym, key))
        print('  witness %s reached at distance %d' % (sym, walk.dist[key]))

    for key, sym in covered:
        if key not in reach:
            die('%s is a node of its own in this graph and is NOT reachable from the required'
                ' roots. It is declared optional because a board that inlines it has no node,'
                ' and a board that does not must reach it: neither holds here, so the root set'
                ' no longer covers the function this clause is named for' % sym)
        print('  covered %s reached at distance %d' % (sym, walk.dist[key]))

    # --- the positive control ---------------------------------------------------
    # One synthetic edge, from the reachable function furthest from the roots to the first
    # forbidden symbol, which is the shape the defect takes: an assert written into something
    # the route already calls. The clause has to name that path, and the edge is dropped again
    # before the verdict below.
    probe_key, probe_sym, _optional = forbid[0]
    # EVERY REAL EDGE INTO THE TARGET IS LIFTED FIRST, or the search would answer with whatever
    # path already exists on a tree that is failing anyway. The function to plant on is chosen
    # on the LIFTED graph, a node whose only way in was through the panic tail not being
    # reachable once the tail is lifted.
    lifted = [n for n in graph.edges if probe_key in graph.edges[n]]
    for n in lifted:
        graph.edges[n].discard(probe_key)
    probe_from = Reach(graph, root_keys).deepest(exclude={probe_key})
    if probe_from is None:
        for n in lifted:
            graph.edges[n].add(probe_key)
        die('CONTROL: the reachable set holds no function to plant a control edge on, so the'
            ' search below has never been shown to find anything')
    graph.edges[probe_from].add(probe_key)
    control = Reach(graph, root_keys)
    control_path = control.path(probe_key)
    control_via = control.parent.get(probe_key)
    graph.edges[probe_from].discard(probe_key)
    for n in lifted:
        graph.edges[n].add(probe_key)
    if control_path is None:
        die('CONTROL: a %s edge was planted on %s, which is reachable from the roots, and the'
            ' search did not report it. The search is broken and its clean answers mean'
            ' nothing' % (probe_sym, probe_from))
    if control_via != probe_from:
        die('CONTROL: the planted %s edge on %s was reported through %s instead. A real edge'
            ' survived the lift, so the control proves nothing about a NEW one'
            % (probe_sym, probe_from, control_via))
    print('  control: with every real edge into %s lifted and one planted on %s, the clause'
          ' reports' % (probe_sym, probe_from))
    print('    ' + control_path)

    # --- the verdict ------------------------------------------------------------
    for key, sym, _optional in forbid:
        if key in reach:
            fails.append(
                'REACHABLE PANIC: %s is reachable from the fault-record console route.\n'
                '    %s\n'
                '    kpanic prints through kprintf, which re-enters this same route, so the'
                ' recursion has no static bound.\n'
                '    REMEDY: the function on that path answers its caller instead of'
                ' asserting. Do NOT widen this declaration.'
                % (sym, walk.path(key)))

    for pseudo in sorted(graph.unbound & reach):
        site = pseudo[len(SITE_PREFIX):]
        fails.append(
            'UNBOUND INDIRECT SITE: %s is reachable from the console route and is not in %s.'
            ' Until it is bound, the walk stops there and the answer is a lower bound and not'
            ' a bound.' % (site, opt['indirect']))

    reached_opaque = []
    for key in sorted(reach):
        if key.startswith(SITE_PREFIX) or key in graph.size:
            continue
        if key in decl.opaque:
            reached_opaque.append(key)
            continue
        fails.append(
            'OPAQUE REACHABLE NODE: %s has no "bytes (static)", so it was not compiled from'
            ' C or C++ here and this graph carries no out-edge for it. Everything it calls is'
            ' outside the claim. Declare it in %s with the reason its callees do not matter,'
            ' or the clause is walking around it in silence.' % (key, opt['decl']))

    for key in sorted(reach):
        if len(graph.definers.get(key, ())) > 1:
            fails.append(
                'AMBIGUOUS DEFINITION: %s is defined in %d translation units (%s) and the'
                ' route reaches it. Only one of them linked and the merged graph cannot say'
                ' which, so the edges below it are not this image edges.'
                % (key, len(graph.definers[key]),
                   ', '.join(os.path.basename(t) for t in sorted(graph.definers[key]))))

    print()
    for key in reached_opaque:
        print('reached opaque node %s: %s' % (key, decl.opaque[key]))
    print('reachable nodes from the console route: %d of %d' % (len(reach), nodes))

    if fails:
        print()
        for f in fails:
            sys.stderr.write('FAIL: %s\n' % f)
        return 1
    print('console_reach: no panic terminal is reachable from the console route (%s)'
          % ', '.join(s for _k, s, _o in forbid))
    return 0


def main():
    if len(sys.argv) < 2:
        return usage()
    try:
        return run(sys.argv[1:])
    except Bad as e:
        sys.stderr.write('FAIL: %s\n' % e)
        return 2


if __name__ == '__main__':
    sys.setrecursionlimit(20000)
    sys.exit(main())
