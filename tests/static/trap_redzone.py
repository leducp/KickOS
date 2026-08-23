# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Re-measures the kernel C descent a trap entry's reservation has to cover, on whichever
# stack that entry builds on, and fails when the measurement exceeds what it enforces.
#
# INPUT is the .ci files gcc -fcallgraph-info=su,da leaves next to every object: one VCG
# graph per translation unit, carrying each function's own frame size ("N bytes (static)"),
# its alloca/VLA count, its call edges with file:line:col, and an edge to the literal node
# `__indirect_call` for every call through a pointer. The whole tree's .ci files are merged
# and the longest weighted path from the declared roots is the answer.
#
# EVERY figure comes from the declaration files or from the caller; nothing is defaulted,
# because a red zone derived from a silent under-approximation is worse than none at all.
# Every way this tool refuses to answer is listed in check_trap_redzone.sh.
#
# NODE TITLE SCOPING. An internal-linkage function is titled "<path>:<mangled>", so titles
# are keyed by basename plus mangled tail: two TUs each with a static `helper` do not
# merge into one node, and the same static seen from two .ci files does. A node with no
# "bytes (static)" is defined outside the C/C++ the compiler saw (assembly, libgcc) and
# must be declared in the unsized-allowance list or the run fails.

import collections
import glob
import os
import re
import sys

NODE_RE = re.compile(r'^node:\s*\{\s*title:\s*"([^"]*)"\s*label:\s*"([^"]*)"')
EDGE_RE = re.compile(
    r'^edge:\s*\{\s*sourcename:\s*"([^"]*)"\s*targetname:\s*"([^"]*)"'
    r'(?:\s*label:\s*"([^"]*)")?')
INDIRECT = '__indirect_call'
SITE_PREFIX = '!site '


class Bad(Exception):
    pass


def die(msg):
    raise Bad(msg)


# --- declaration files ---------------------------------------------------------

def records(path):
    """Yields (lineno, [fields], reason) with '#' comments and trailing '\\' joins gone."""
    try:
        raw = open(path).read().splitlines()
    except OSError as e:
        die('cannot read declaration file %s: %s' % (path, e))
    joined = []
    pending = ''
    first = 0
    for n, line in enumerate(raw, 1):
        line = line.split('#', 1)[0].rstrip()
        if not line.strip():
            if pending:
                die('%s:%d: continuation line runs into a blank line' % (path, n))
            continue
        if not pending:
            first = n
        if line.endswith('\\'):
            pending += line[:-1] + ' '
            continue
        joined.append((first, pending + line))
        pending = ''
    if pending:
        die('%s: file ends inside a continuation' % path)
    for n, line in joined:
        reason = None
        if ' reason:' in line:
            line, reason = line.split(' reason:', 1)
            reason = reason.strip()
        yield n, line.split(), reason


class Decl(object):
    def __init__(self, roots_path, arch):
        self.arch = arch
        self.header = None
        self.presets = []
        self.classes = []                            # class names, declaration order
        self.macros = {}                             # class -> (frame macro, depth macro)
        self.trap_stack = set()                      # classes declared stack=trap
        self.kernel_stack = set()                    # classes declared stack=kernel
        self.roots = collections.OrderedDict()        # class -> [(symbol, optional)]
        self.rootless = {}                            # class -> declared reason
        self.excludes = []                           # [(mangled, reason, optional)]
        self.unsized = collections.OrderedDict()      # symbol -> (bytes, reason)
        seen_arch = set()
        for n, f, reason in records(roots_path):
            where = '%s:%d' % (roots_path, n)
            kind = f[0]
            if kind == 'arch':
                seen_arch.add(f[1])
            if len(f) < 2:
                die('%s: record "%s" has no arch' % (where, kind))
            if f[1] != arch:
                continue
            if kind == 'arch':
                if len(f) != 3 or not f[2].startswith('header='):
                    die('%s: arch record wants exactly header=<path>' % where)
                self.header = f[2][len('header='):]
            elif kind == 'preset':
                self.presets.append(f[2])
            elif kind == 'class':
                name = f[2]
                frame = None
                depth = None
                on_trap = False
                on_kernel = False
                for opt in f[3:]:
                    if opt.startswith('frame='):
                        frame = opt[len('frame='):]
                    elif opt.startswith('depth='):
                        depth = opt[len('depth='):]
                    elif opt == 'stack=trap':
                        on_trap = True
                    elif opt == 'stack=kernel':
                        on_kernel = True
                    elif opt in ('kstacks=0', 'kstacks=1'):
                        # Read by check_trap_redzone.sh, which owns the live posture knob and
                        # passes --not-compiled for a class this image does not contain. Only
                        # accepted here so a marked record parses; this tool never resolves it
                        # itself, there being no board config in a .ci tree to resolve it from.
                        pass
                    else:
                        die('%s: unknown class option "%s"' % (where, opt))
                if frame is None or depth is None:
                    die('%s: class %s needs both frame= and depth=' % (where, name))
                if on_trap and on_kernel:
                    die('%s: class %s names two stacks; a frame goes on one of them'
                        % (where, name))
                if name in self.macros:
                    die('%s: class %s declared twice' % (where, name))
                if on_trap:
                    self.trap_stack.add(name)
                if on_kernel:
                    self.kernel_stack.add(name)
                self.classes.append(name)
                self.macros[name] = (frame, depth)
                self.roots[name] = []
            elif kind == 'root':
                cls = f[2]
                if cls not in self.roots:
                    die('%s: root names class %s, declared nowhere for %s'
                        % (where, cls, arch))
                sym = f[3]
                # NONE is the deliberate twin of an omitted record: the class runs no C
                # on this stack, so 0 is the answer rather than a silence. It carries a
                # reason because the claim is structural and only prose can carry it.
                if sym == 'NONE':
                    if reason is None:
                        die('%s: root NONE carries no "reason:"; a class that measures'
                            ' nothing has to say why nothing is the right answer' % where)
                    if self.roots[cls]:
                        die('%s: class %s already has a named root, so NONE would'
                            ' contradict it' % (where, cls))
                    self.rootless[cls] = reason
                    continue
                if cls in self.rootless:
                    die('%s: class %s is declared NONE; a named root cannot be added'
                        ' beside it' % (where, cls))
                optional = sym.startswith('?')
                self.roots[cls].append((sym.lstrip('?'), optional))
            elif kind == 'exclude':
                if reason is None:
                    die('%s: exclude %s carries no "reason:"; an undocumented exclusion is'
                        ' how a margin goes quiet' % (where, f[2]))
                # '?' as on a root: the symbol is absent from SOME boards' graphs, so its
                # absence contributes nothing instead of failing as a stale declaration.
                self.excludes.append((f[2].lstrip('?'), reason, f[2].startswith('?')))
            elif kind == 'unsized':
                if reason is None:
                    die('%s: unsized %s carries no "reason:"; the byte cost has to say'
                        ' where it was measured' % (where, f[2]))
                if len(f) != 4:
                    die('%s: unsized wants <arch> <symbol> <bytes>' % where)
                try:
                    cost = int(f[3])
                except ValueError:
                    die('%s: unsized %s cost "%s" is not a number' % (where, f[2], f[3]))
                self.unsized[f[2]] = (cost, reason)
            else:
                die('%s: unknown record "%s"' % (where, kind))
        if arch not in seen_arch:
            die('%s declares nothing for arch %s' % (roots_path, arch))
        if not self.classes:
            die('%s declares no class for arch %s' % (roots_path, arch))
        for cls in self.classes:
            if not self.roots[cls] and cls not in self.rootless:
                die('%s: class %s has no root, so it would measure 0 and always pass.'
                    ' Declare `root %s %s NONE reason: ...` if that is the honest answer'
                    % (roots_path, cls, arch, cls))

    def off_thread(self):
        """Classes whose descent spends no unprivileged thread stack."""
        return self.trap_stack | self.kernel_stack


def read_bindings(path, arch, preset):
    """site key -> [(callee spec, optional)], with [] for an explicit NONE."""
    out = collections.OrderedDict()
    for n, f, _reason in records(path):
        where = '%s:%d' % (path, n)
        if f[0] != 'site':
            die('%s: unknown record "%s"' % (where, f[0]))
        if len(f) < 5:
            die('%s: site wants <arch> <scope> <site> <callee>...' % where)
        if f[1] != arch:
            continue
        if f[2] != '*' and f[2] != preset:
            continue
        site = f[3]
        if site.count(':') != 2:
            die('%s: site "%s" is not <basename>:<line>:<col>' % (where, site))
        if site in out:
            die('%s: site %s bound twice for %s/%s' % (where, site, arch, preset))
        if f[4:] == ['NONE']:
            out[site] = []
            continue
        callees = []
        for spec in f[4:]:
            if spec == 'NONE':
                die('%s: NONE cannot be mixed with a named callee' % where)
            callees.append((spec.lstrip('?'), spec.startswith('?')))
        out[site] = callees
    return out


# --- graph ---------------------------------------------------------------------

def node_key(title):
    """"<path>:<mangled>" -> "<basename>:<mangled>"; a bare symbol stays itself."""
    if ':' in title and not title.startswith('_'):
        path, mangled = title.rsplit(':', 1)
        return os.path.basename(path) + ':' + mangled
    return title


def site_key(loc):
    """A .ci edge label, "<path>:<line>:<col>" -> "<basename>:<line>:<col>"."""
    parts = loc.rsplit(':', 2)
    if len(parts) != 3:
        return loc
    return os.path.basename(parts[0]) + ':' + parts[1] + ':' + parts[2]


class Graph(object):
    """Every .ci under the build dir, merged, MINUS the ones the link threw away.

    COMPILED IS NOT LINKED: a .ci file is written by the compiler, so the corpus on disk
    includes translation units that never entered the image. KickOS resolves an optional
    arch/chip seam by archive-member extraction, so beside every backend sits an
    unextracted <symbol>_default.cc fallback, and reading it both inflates depths and
    invents recursion: arch/common/arch_console_write_sync_default.cc calls
    arch_console_write, which on a board with a real backend goes through console_tx and
    back, so the merged graph shows a console cycle the image cannot contain.
    dropped_tus() applies the tree's own extraction rule to skip them.
    """

    def __init__(self, ci_dir):
        self.size = {}
        self.dynobj = {}
        self.label = {}
        self.edges = collections.defaultdict(set)
        self.sites = collections.defaultdict(set)     # source key -> {site key}
        self.definers = collections.defaultdict(set)  # global key -> {defining TU path}
        self.unbound = set()
        self.dropped = []
        self.files = 0
        found = sorted(glob.glob(os.path.join(ci_dir, '**', '*.ci'), recursive=True))
        if not found:
            die('no .ci file under %s; -fcallgraph-info did not reach this build' % ci_dir)
        tu_of = {}
        defined = collections.defaultdict(set)        # TU path -> {global key}
        for ci in found:
            tu = None
            for line in open(ci):
                line = line.strip()
                if line.startswith('graph:'):
                    m = re.search(r'title:\s*"([^"]*)"', line)
                    if m:
                        tu = m.group(1)
                    continue
                m = NODE_RE.match(line)
                if m and 'bytes (static)' in m.group(2):
                    key = node_key(m.group(1))
                    if ':' in key:
                        continue                      # file-scoped, cannot collide
                    if tu is None:
                        die('%s: a node before any graph title, so its TU is unknown' % ci)
                    defined[tu].add(key)
                    self.definers[key].add(tu)
            if tu is None:
                die('%s: no graph title, so the .ci file is not the shape this tool parses'
                    % ci)
            tu_of[ci] = tu
        skip = self.dropped_tus(defined)
        for ci in found:
            if tu_of[ci] in skip:
                continue
            self.files += 1
            self._read(ci)
        for tu in sorted(skip):
            self.dropped.append(tu)
            for key in defined[tu]:
                self.definers[key].discard(tu)

    def dropped_tus(self, defined):
        """The seam fallbacks the link cannot have extracted.

        arch/CMakeLists.txt states the rule and tests/static/check_seam_defaults.sh enforces
        it: a fallback lives ALONE in a <name>_default.cc translation unit, so the member is
        pulled in only when nothing else defines its symbol. A _default.cc whose every
        global is also defined elsewhere is therefore, by that rule, not in the image.
        Restricted to the _default.cc naming on purpose: two rival non-fallback definitions
        (spi_mock.cc against spi_proxy.cc, one main.cc per app) are ALSO mutually exclusive
        at link time, and there the rule says nothing about which one won, so they are left
        in and refused later if the measurement actually reaches them.
        """
        skip = set()
        for tu, keys in defined.items():
            if not os.path.basename(tu).endswith('_default.cc'):
                continue
            if not keys:
                continue
            if all(len(self.definers[k]) > 1 for k in keys):
                skip.add(tu)
        return skip

    def _read(self, ci):
        for line in open(ci):
            line = line.strip()
            m = NODE_RE.match(line)
            if m:
                key = node_key(m.group(1))
                label = m.group(2)
                self.label[key] = label.split('\\n')[0]
                s = re.search(r'(\d+) bytes \(static\)', label)
                if s:
                    self.size[key] = max(self.size.get(key, 0), int(s.group(1)))
                d = re.search(r'(\d+) dynamic objects', label)
                if d and int(d.group(1)) > 0:
                    self.dynobj[key] = max(self.dynobj.get(key, 0), int(d.group(1)))
                continue
            m = EDGE_RE.match(line)
            if m:
                src = node_key(m.group(1))
                tgt = node_key(m.group(2))
                loc = m.group(3)
                if tgt == INDIRECT:
                    if loc is None:
                        die('%s: an __indirect_call edge from %s carries no call site, so'
                            ' it cannot be bound' % (ci, src))
                    self.sites[src].add(site_key(loc))
                    continue
                self.edges[src].add(tgt)

    def universe(self):
        keys = set(self.label)
        keys.update(self.size)
        return keys

    def all_sites(self):
        out = set()
        for s in self.sites.values():
            out |= s
        return out

    def resolve(self, spec):
        """One graph key, or None. Ambiguity is refused, never guessed at."""
        keys = self.universe()
        if ':' in spec:
            base, needle = spec.rsplit(':', 1)
            hits = [k for k in keys
                    if k.startswith(base + ':') and needle in k.split(':', 1)[1]]
        else:
            hits = [k for k in keys if k == spec or k.endswith(':' + spec)]
        if len(hits) > 1:
            die('"%s" matches %d graph nodes (%s); make the declaration exact'
                % (spec, len(hits), ', '.join(sorted(hits)[:4])))
        if not hits:
            return None
        return hits[0]

    def bind_indirect(self, bindings):
        """Replaces every __indirect_call edge with a per-site pseudo-node.

        The pseudo-node weighs 0 and its out-edges are exactly the callees bound to THAT
        site. An unbound site becomes a pseudo-node with no out-edge, recorded in
        self.unbound, so the reachability walk can still see it and refuse it.
        """
        for src in sorted(self.sites):
            for site in sorted(self.sites[src]):
                pseudo = SITE_PREFIX + site
                self.size[pseudo] = 0
                self.label[pseudo] = 'indirect call at ' + site
                self.edges[src].add(pseudo)
                if site not in bindings:
                    self.unbound.add(pseudo)
                    continue
                for spec, optional in bindings[site]:
                    key = self.resolve(spec)
                    if key is None:
                        if optional:
                            continue
                        die('binding for %s names callee "%s", which is not in the graph;'
                            ' a stale binding rots like a stale margin' % (site, spec))
                    self.edges[pseudo].add(key)


# --- longest weighted path -----------------------------------------------------

class Walk(object):
    def __init__(self, graph, excluded):
        self.g = graph
        self.excluded = excluded
        self.memo = {}
        self.stack = []
        self.onstack = set()
        self.cycles = []

    def out(self, fn):
        for t in sorted(self.g.edges.get(fn, ())):
            if t in self.excluded:
                continue
            yield t

    def depth(self, fn):
        if fn in self.onstack:
            at = self.stack.index(fn)
            self.cycles.append(list(self.stack[at:]) + [fn])
            return 0
        if fn in self.memo:
            return self.memo[fn]
        self.stack.append(fn)
        self.onstack.add(fn)
        best = 0
        for t in self.out(fn):
            best = max(best, self.depth(t))
        self.stack.pop()
        self.onstack.discard(fn)
        self.memo[fn] = self.g.size.get(fn, 0) + best
        return self.memo[fn]

    def chain(self, fn, seen=()):
        if fn in seen:
            return ['%s (CYCLE)' % fn]
        best = -1
        nxt = None
        for t in self.out(fn):
            d = self.depth(t)
            if d > best:
                best = d
                nxt = t
        step = '%s[%d]' % (self.g.label.get(fn, fn), self.g.size.get(fn, 0))
        if nxt is None or best == 0:
            return [step]
        return [step] + self.chain(nxt, seen + (fn,))

    def reach(self, roots):
        seen = set()
        todo = list(roots)
        while todo:
            fn = todo.pop()
            if fn in seen:
                continue
            seen.add(fn)
            todo.extend(self.out(fn))
        return seen


def root_keys(graph, decl, cls, report):
    keys = []
    for sym, optional in decl.roots[cls]:
        key = graph.resolve(sym)
        if key is None:
            if optional:
                report.append('  %-6s root %s: ABSENT, declared optional, contributes 0'
                              % (cls, sym))
                continue
            die('%s root %s is not in the graph and is not declared optional; a root'
                ' nobody measured is a path nobody bounded' % (cls, sym))
        keys.append(key)
    # A class whose roots are ALL optional and all absent has a non-empty declaration and an
    # empty root set, so it measures 0 and passes forever. NONE is the record that says so
    # deliberately.
    if not keys and cls not in decl.rootless:
        die('%s has %d declared root(s) and not one of them is in the graph, so it would'
            ' measure 0 and always pass. Every root it names is optional; if the class truly'
            ' runs no C on its stack, say so with `root <arch> %s NONE reason: ...`'
            % (cls, len(decl.roots[cls]), cls))
    return keys


# --- main ----------------------------------------------------------------------

def usage():
    sys.stderr.write(
        'usage: trap_redzone.py --ci-dir <dir> --arch <arch> --preset <preset>\n'
        '                       --roots <file> --indirect <file>\n'
        '                       --enforced <CLASS>=<frame>,<depth> [--enforced ...]\n'
        '                       [--not-compiled <CLASS>]...\n')
    return 2


def parse_argv(argv):
    want = {'--ci-dir', '--arch', '--preset', '--roots', '--indirect'}
    opt = {}
    enforced = collections.OrderedDict()
    # Classes this image does not compile, per the caller's read of the live posture knob.
    # Still measured and still printed; only the figure comparison is dropped, because the
    # depth measures the same C landing on a stack this design does not put it on.
    not_compiled = set()
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--not-compiled':
            if i + 1 >= len(argv):
                die('--not-compiled wants <CLASS>')
            not_compiled.add(argv[i + 1])
            i += 2
            continue
        if a == '--enforced':
            if i + 1 >= len(argv):
                die('--enforced wants <CLASS>=<frame>,<depth>')
            spec = argv[i + 1]
            if '=' not in spec or ',' not in spec:
                die('--enforced "%s" is not <CLASS>=<frame>,<depth>' % spec)
            cls, nums = spec.split('=', 1)
            frame, depth = nums.split(',', 1)
            try:
                enforced[cls] = (int(frame), int(depth))
            except ValueError:
                die('--enforced %s carries a non-numeric figure' % cls)
            i += 2
            continue
        if a in want:
            if i + 1 >= len(argv):
                die('%s wants a value' % a)
            opt[a.lstrip('-')] = argv[i + 1]
            i += 2
            continue
        die('unknown argument "%s"' % a)
    for a in sorted(want):
        if a.lstrip('-') not in opt:
            die('missing %s' % a)
    if not enforced:
        die('no --enforced figure; there would be nothing to compare against')
    if not_compiled and set(enforced) <= not_compiled:
        die('every class was passed --not-compiled, so nothing would be compared at all;'
            ' a posture that enforces no class of an arch is a declaration bug, not a run')
    return opt, enforced, not_compiled


def run(argv):
    opt, enforced, not_compiled = parse_argv(argv)
    arch = opt['arch']
    preset = opt['preset']
    decl = Decl(opt['roots'], arch)
    if preset not in decl.presets:
        die('preset %s is not declared for arch %s in %s (declared: %s)'
            % (preset, arch, opt['roots'], ' '.join(decl.presets)))
    for cls in decl.classes:
        if cls not in enforced:
            die('class %s is declared for %s but no --enforced figure was passed for it'
                % (cls, arch))
    for cls in enforced:
        if cls not in decl.macros:
            die('--enforced names class %s, which %s declares nowhere for %s'
                % (cls, opt['roots'], arch))
    for cls in sorted(not_compiled):
        if cls not in decl.macros:
            die('--not-compiled names class %s, which %s declares nowhere for %s'
                % (cls, opt['roots'], arch))

    graph = Graph(opt['ci-dir'])
    bindings = read_bindings(opt['indirect'], arch, preset)
    present = graph.all_sites()
    for site in bindings:
        if site not in present:
            die('binding for site %s is stale: no __indirect_call edge in the graph'
                ' carries that file:line:col' % site)
    graph.bind_indirect(bindings)

    report = []
    roots = collections.OrderedDict()
    for cls in decl.classes:
        roots[cls] = root_keys(graph, decl, cls, report)
        if cls in decl.rootless:
            report.append('  %-8s root NONE, so its measured depth is 0 by declaration: %s'
                          % (cls, decl.rootless[cls]))

    excluded = set()
    for spec, reason, optional in decl.excludes:
        key = graph.resolve(spec)
        if key is None:
            if optional:
                report.append('  exclusion %s: not in this graph, unused' % spec)
                continue
            die('declared exclusion %s is not in the graph; the declaration is stale and'
                ' the printed without-exclusions figure would be a duplicate' % spec)
        excluded.add(key)

    # Charge the declared cost to every allowance that IS in the graph. An allowance the
    # graph does not have is reported, not failed: the list spans the fleet's boards and a
    # given image need not pull every libgcc helper.
    unsized_keys = {}
    for sym, (cost, _reason) in decl.unsized.items():
        key = graph.resolve(sym)
        if key is None:
            report.append('  unsized allowance %s: not in this graph, unused' % sym)
            continue
        if key in graph.size:
            report.append('  unsized allowance %s: the graph sizes it at %d bytes, so the'
                          ' allowance is unused' % (sym, graph.size[key]))
            continue
        unsized_keys[key] = cost
        graph.size[key] = cost

    walk = Walk(graph, excluded)
    bare = Walk(graph, set())

    # A stack=trap or stack=kernel class is measured with NOTHING excluded: the exclusion set
    # exists only to keep a THREAD's red zone under the spawn floor, and neither class spends
    # a thread stack. So both walk `bare`, and every hard check below runs over the set that
    # walk reaches, not over the post-exclusion one.
    def walk_for(cls):
        if cls in decl.off_thread():
            return bare
        return walk

    print('trap_redzone: arch=%s preset=%s (%d .ci files read, %d unextracted seam'
          ' fallbacks skipped, %d nodes, %d indirect sites)'
          % (arch, preset, graph.files, len(graph.dropped), len(graph.universe()),
             len(present)))
    for line in report:
        print(line)

    measured = {}
    for cls in decl.classes:
        w = walk_for(cls)
        best = 0
        winner = None
        for key in roots[cls]:
            d = w.depth(key)
            if winner is None or d > best:
                best = d
                winner = key
        measured[cls] = best
        frame, enf_depth = enforced[cls]
        macro_frame, macro_depth = decl.macros[cls]
        print()
        where = 'on the interrupted thread stack'
        if cls in decl.trap_stack:
            where = 'on the arch trap stack, exclusions NOT applied'
        if cls in decl.kernel_stack:
            where = 'on the per-thread kernel stack, exclusions NOT applied'
        if cls in not_compiled:
            where += ', NOT ENFORCED: this image does not compile the entry design this' \
                     ' class describes'
        print('%s: measured depth %d bytes, %s enforces %d bytes (%s)'
              % (cls, best, macro_depth, enf_depth, where))
        print('  red zone = %s %d + %s %d = %d bytes'
              % (macro_frame, frame, macro_depth, enf_depth, frame + enf_depth))
        if winner is not None:
            print('  deepest root %s:' % winner)
            print('    ' + ' -> '.join(w.chain(winner)))

    # Report-only: the same measurement with nothing excluded, so a deliberately excluded
    # tail cannot grow unwatched.
    if decl.excludes:
        print()
        print('WITHOUT the declared exclusions (report only, never a failure):')
        for spec, reason, _optional in decl.excludes:
            print('  excluded %s' % spec)
            print('    reason: %s' % reason)
        for cls in decl.classes:
            if cls in decl.off_thread():
                print('  %-6s already measured that way, see above' % cls)
                continue
            b = 0
            for key in roots[cls]:
                b = max(b, bare.depth(key))
            frame, enf_depth = enforced[cls]
            note = ''
            if cls in not_compiled:
                note = ' (not enforced here at all)'
            print('  %-6s %d bytes measured, %d over the enforced %d, red zone would be %d%s'
                  % (cls, b, b - enf_depth, enf_depth, frame + b, note))

    # Every hard check runs over the reachable set the ENFORCED figure claims to cover,
    # which is the set after exclusions: a node only the excluded tail reaches is outside
    # the claim and reporting it would make the gate unfixable. A stack=trap class claims
    # the tail too, so its own reachable set is the one with nothing excluded.
    reach = set()
    for cls in decl.classes:
        reach |= walk_for(cls).reach(roots[cls])

    def show(key):
        if key.startswith(SITE_PREFIX):
            return 'indirect call at ' + key[len(SITE_PREFIX):]
        return key

    fails = []
    for cls in decl.classes:
        if cls in not_compiled:
            continue
        frame, enf_depth = enforced[cls]
        if measured[cls] > enf_depth:
            fails.append(
                'DEPTH EXCEEDS RED ZONE: %s measures %d bytes, %s reserves %d. Raise %s'
                ' to at least %d (red zone %d) or shorten the winning chain above.'
                % (cls, measured[cls], decl.macros[cls][1], enf_depth,
                   decl.macros[cls][1], measured[cls], frame + measured[cls]))

    for pseudo in sorted(graph.unbound & reach):
        site = pseudo[len(SITE_PREFIX):]
        fails.append(
            'UNBOUND INDIRECT SITE: %s is reachable from a trap root and is not in %s.'
            ' Until it is bound, every figure above is a lower bound and not a bound.'
            % (site, opt['indirect']))

    for key in sorted(k for k in graph.dynobj if k in reach):
        fails.append(
            'DYNAMIC STACK OBJECT: %s allocates %d alloca/VLA object(s), which have no'
            ' static size. Nothing below it can be bounded.'
            % (key, graph.dynobj[key]))

    seen_cycles = set()
    cycles = list(walk.cycles)
    if decl.off_thread():
        cycles += bare.cycles
    for cyc in cycles:
        if not (set(cyc) & reach):
            continue
        rot = tuple(cyc[:-1])
        canon = min(tuple(rot[i:] + rot[:i]) for i in range(len(rot)))
        if canon in seen_cycles:
            continue
        seen_cycles.add(canon)
        ring = list(canon) + [canon[0]]
        fails.append('REACHABLE CYCLE: %s. Recursion has no static bound.'
                     % ' -> '.join(show(k) for k in ring))

    for key in sorted(reach):
        if len(graph.definers.get(key, ())) > 1:
            fails.append(
                'AMBIGUOUS DEFINITION: %s is defined with a frame in %d translation units'
                ' (%s) and the measurement reaches it. Only one of them linked, and the'
                ' merged graph cannot say which, so the depth below it is not a bound.'
                % (key, len(graph.definers[key]),
                   ', '.join(os.path.basename(t) for t in sorted(graph.definers[key]))))
    for key in sorted(reach):
        if key.startswith(SITE_PREFIX) or key in graph.size:
            continue
        fails.append(
            'UNSIZED REACHABLE NODE: %s has no "bytes (static)" (assembly, or a prebuilt'
            ' library) and no entry in the unsized-allowance list of %s. Measure its frame'
            ' from the linked ELF and declare it.' % (key, opt['roots']))

    print()
    for key in sorted(unsized_keys):
        print('charged unsized %s at the declared %d bytes' % (key, unsized_keys[key]))
    print('reachable nodes after exclusions: %d' % len(reach))

    if fails:
        print()
        for f in fails:
            sys.stderr.write('FAIL: %s\n' % f)
        return 1
    print('trap_redzone: depths OK, %s' % ', '.join(
        '%s %d <= %d' % (c, measured[c], enforced[c][1])
        for c in decl.classes if c not in not_compiled))
    if not_compiled:
        print('trap_redzone: measured and NOT enforced here, %s' % ', '.join(
            '%s %d' % (c, measured[c]) for c in sorted(not_compiled)))
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
