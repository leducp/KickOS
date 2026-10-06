# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Bounds an application thread's stack from the same .ci corpus trap_redzone.py reads: the
# deepest the thread's own code descends from its entry, plus what the arch's trap entry
# reserves below the deepest sp, against the stack the thread is spawned on less the image's
# thread-local carve.
#
# The corpus is narrowed to the units the image's own link map LOADs, so the many apps that
# define main or a file-scoped helper of one name do not merge. Everything the walk cannot see
# fails the thread rather than shrinking it: a reachable node with no frame (assembly, newlib,
# libgcc) that neither app_stack_roots.txt nor trap_redzone_roots.txt sizes, a reachable
# indirect call trap_redzone_indirect.txt does not bind, a dynamic stack object, a cycle, a
# symbol two linked units define.
#
# The figures are read through the compiler, with the compile command of the unit that defines
# the root, so the posture is the image's: the stack macro from that unit, the arch's frame and
# zone from the header it is told to include. The ISA flags stay in, because a figure the arch
# header selects on __ARM_FP resolves to the integer posture without them.

import collections
import json
import os
import re
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trap_redzone as T  # noqa: E402

CARVE_RE = re.compile(r'^\s*0x([0-9a-fA-F]+)\s+_?__kickos_tls_carve = ')
DEFINE_RE = re.compile(r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.*)$')
TOKEN_RE = re.compile(r'\s*(?:(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*|([A-Za-z_][A-Za-z0-9_]*)|(.))')


def die(msg):
    raise T.Bad(msg)


class Decl(object):
    def __init__(self, path, arch, kernel_cores=1):
        self.need = None                            # (header, frame macro, zone macro)
        self.threads = []                           # (image, name, root spec, stack macro, presets)
        self.unsized = {}                           # symbol -> (bytes, [(callee, optional)])
        for n, f, reason in T.records(path):
            where = '%s:%d' % (path, n)
            if len(f) < 2:
                die('%s: record "%s" has no arch' % (where, f[0]))
            if f[1] != arch:
                continue
            if not reason:
                die('%s: every record carries a reason:' % where)
            kind = f[0]
            fields = dict(x.split('=', 1) for x in f[2:] if '=' in x)
            if kind == 'need':
                if self.need is not None:
                    die('%s: a second need record for %s' % (where, arch))
                for key in ('header', 'frame', 'zone'):
                    if key not in fields:
                        die('%s: need wants header=, frame= and zone=' % where)
                self.need = (fields['header'], fields['frame'], fields['zone'])
            elif kind == 'thread':
                presets = set()
                for x in f[4:]:
                    if x.startswith('presets='):
                        presets.update(p for p in x[len('presets='):].split(',') if p)
                if len(f) < 7 or 'root' not in fields or 'stack' not in fields or not presets:
                    die('%s: thread wants <arch> <image> <name> root=<symbol> stack=<macro>'
                        ' presets=<preset>[,<preset>...]...' % where)
                self.threads.append((f[2], f[3], fields['root'], fields['stack'],
                                     frozenset(presets)))
            elif kind == 'unsized':
                if len(f) < 4:
                    die('%s: unsized wants <arch> <symbol> <bytes> [cores=1|cores>1]'
                        ' [calls=...]' % where)
                try:
                    cost = int(f[3])
                except ValueError:
                    die('%s: unsized %s carries a non-numeric frame' % (where, f[2]))
                scope = [x for x in f[4:] if x.startswith('cores')]
                if scope:
                    if len(scope) > 1 or scope[0] not in (T.SCOPE_ONE_CORE, T.SCOPE_MULTI_CORE):
                        die('%s: unsized %s scope is neither %s nor %s'
                            % (where, f[2], T.SCOPE_ONE_CORE, T.SCOPE_MULTI_CORE))
                    if not T.in_scope(scope[0], None, kernel_cores):
                        continue
                calls = []
                for c in fields.get('calls', '').split(','):
                    if c:
                        calls.append((c.lstrip('?'), c.startswith('?')))
                if f[2] in self.unsized:
                    die('%s: unsized %s declared twice for this posture' % (where, f[2]))
                self.unsized[f[2]] = (cost, calls)
            else:
                die('%s: unknown record kind "%s"' % (where, kind))
        if self.threads and self.need is None:
            die('%s declares app threads for %s and no need record, so there is no trap'
                ' reservation to add below them' % (path, arch))


class ImageGraph(T.Graph):
    """The corpus narrowed to one image, remembering which .ci defines each sized node."""

    def __init__(self, ci_dir, map_path):
        self.home = {}
        self._map_path = map_path
        T.Graph.__init__(self, ci_dir, os.path.relpath(map_path, ci_dir))

    # The armv7m and RISC-V links name their inputs relative to the build directory, which is
    # where the linker ran; T.Graph reads only the absolute LOAD lines x86_64 writes.
    @staticmethod
    def load_set(ci_dir, pattern):
        path = os.path.join(ci_dir, pattern)
        loaded = set()
        for line in open(path, errors='replace'):
            if line.startswith('LOAD '):
                loaded.add(os.path.realpath(os.path.join(ci_dir, line[len('LOAD '):].strip())))
        if not loaded:
            die('NO LINK MAP: %s carries no LOAD line, so it names no input of its image' % path)
        return loaded

    def _read(self, ci):
        T.Graph._read(self, ci)
        for line in open(ci):
            m = T.NODE_RE.match(line.strip())
            if m and T.FRAME_RE.search(m.group(2)):
                self.home[T.node_key(m.group(1))] = ci


def compile_entries(build_dir):
    """.ci path -> compile database entry."""
    db = json.load(open(os.path.join(build_dir, 'compile_commands.json')))
    out = {}
    for e in db:
        src = e.get('file', '')
        if os.path.splitext(src)[1] not in T.CI_SOURCE_EXT:
            continue
        obj = os.path.realpath(os.path.join(e.get('directory', build_dir),
                                            T.entry_output(src, e)))
        out[os.path.splitext(obj)[0] + '.ci'] = e
    return out


def macro_dump(entry, header):
    """The unit's macros, its own and the header's, under the unit's own flags."""
    argv = entry.get('arguments')
    if argv is None:
        argv = shlex.split(entry['command'])
    keep = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ('-D', '-U', '-I', '-isystem', '-include') and i + 1 < len(argv):
            keep += [a, argv[i + 1]]
            i += 1
        elif a.startswith(('-D', '-U', '-I', '-isystem', '-std=', '-m', '-O', '--specs=')):
            keep.append(a)
        elif a.startswith('-f') and not a.startswith('-fcallgraph-info'):
            keep.append(a)
        i += 1
    cmd = [argv[0], '-E', '-dM'] + keep + ['-include', header, entry['file']]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       cwd=entry.get('directory'))
    if r.returncode != 0:
        die('resolving the figures failed:\n%s\nthe command was: %s'
            % (r.stderr.decode('utf-8', 'replace')[:2000], ' '.join(cmd)))
    macros = {}
    for line in r.stdout.decode('utf-8', 'replace').splitlines():
        m = DEFINE_RE.match(line)
        if m:
            macros[m.group(1)] = m.group(2).strip()
    return macros


def evaluate(name, macros, seen=()):
    """An object-like macro as an integer: literals, other such macros, + - * / and
    parentheses. Anything else is refused rather than guessed at."""
    if name in seen:
        die('macro %s expands to itself' % name)
    if name not in macros:
        die('macro %s is not defined under the image\'s flags' % name)
    text = macros[name]
    expr = []
    pos = 0
    while pos < len(text):
        m = TOKEN_RE.match(text, pos)
        if m is None or m.end() == pos:
            break
        pos = m.end()
        if m.group(1):
            expr.append(str(int(m.group(1), 0)))
        elif m.group(2):
            expr.append(str(evaluate(m.group(2), macros, seen + (name,))))
        elif m.group(3) in '+-*/()':
            expr.append(m.group(3).replace('/', '//'))
        elif not m.group(3).isspace():
            die('macro %s is "%s", which is not plain integer arithmetic' % (name, text))
    try:
        return int(eval(' '.join(expr), {'__builtins__': {}}, {}))
    except (SyntaxError, ZeroDivisionError):
        die('macro %s is "%s", which does not evaluate' % (name, text))


def tls_carve(map_path):
    for line in open(map_path, errors='replace'):
        m = CARVE_RE.match(line)
        if m:
            return int(m.group(1), 16)
    die('%s assigns no __kickos_tls_carve, so the bytes the image carves off a spawned stack'
        ' are unknown' % map_path)


def find_map(build_dir, image):
    hits = []
    for root, _dirs, files in os.walk(build_dir):
        if image + '.map' in files:
            hits.append(os.path.join(root, image + '.map'))
    if len(hits) > 1:
        die('%d link maps named %s.map under %s' % (len(hits), image, build_dir))
    if hits:
        return hits[0]
    return None


def check_thread(build_dir, src_dir, decl, bindings, entries, preset, image, name, root,
                 stack_macro, presets):
    map_path = find_map(build_dir, image)
    if map_path is None and preset in presets:
        return ['IMAGE NOT LINKED: %s/%s is bounded on %s, and the tree links no %s.map'
                % (image, name, preset, image)]
    if map_path is None:
        print('app_stack: %s/%s: not bounded on %s' % (image, name, preset))
        return []
    graph = ImageGraph(build_dir, map_path)
    # A record for a caller this image does not link binds nothing here; every other one is
    # resolved exactly as the trap gate resolves it.
    linked = collections.OrderedDict()
    for site, binding in bindings.items():
        if graph.match(T.split_site_key(site)[0]):
            linked[site] = binding
    graph.bind_indirect(T.resolve_bindings(graph, linked))
    key = graph.resolve(root)
    if key is None:
        die('%s/%s: root %s is not in the units %s loads' % (image, name, root, map_path))
    if key not in graph.home:
        die('%s/%s: root %s carries no frame, so no unit defines it' % (image, name, root))

    for sym, (cost, calls) in sorted(decl.unsized.items()):
        k = graph.resolve(sym)
        if k is None or k in graph.size:
            continue
        graph.size[k] = cost
        for callee, optional in calls:
            t = graph.resolve(callee)
            if t is None:
                if optional:
                    continue
                die('ABSENT CALLEE: unsized %s declares a call to "%s", which %s does not'
                    ' link' % (sym, callee, image))
            graph.edges[k].add(t)

    walk = T.Walk(graph, frozenset(), {})
    depth = walk.depth(key)
    reach = walk.reach([key])

    header, frame_macro, zone_macro = decl.need
    entry = entries.get(graph.home[key])
    if entry is None:
        die('%s/%s: the unit defining %s answers to no compile command' % (image, name, root))
    macros = macro_dump(entry, os.path.join(src_dir, header))
    frame = evaluate(frame_macro, macros)
    zone = evaluate(zone_macro, macros)
    stack = evaluate(stack_macro, macros)
    carve = tls_carve(map_path)
    need = depth + frame + zone
    usable = stack - carve

    print('app_stack: %s/%s: depth %d + %s %d + %s %d = %d bytes, against %s %d less the'
          ' tls carve %d = %d, headroom %d'
          % (image, name, depth, frame_macro, frame, zone_macro, zone, need, stack_macro,
             stack, carve, usable, usable - need))
    print('  ' + ' -> '.join(walk.chain(key)))

    fails = []
    if need > usable:
        fails.append('STACK BELOW ITS THREAD: %s/%s needs %d bytes and %s gives it %d. The'
                     ' entry\'s reservation below the deepest sp runs off the stack, and the'
                     ' arch either refuses that trap every time or overflows into whatever'
                     ' sits below. Raise %s to at least %d.'
                     % (image, name, need, stack_macro, usable, stack_macro, need + carve))
    for pseudo in sorted(graph.unbound & reach):
        fails.append('UNBOUND INDIRECT SITE: %s/%s reaches %s, and nothing here bounds what'
                     ' it calls' % (image, name, graph.where(pseudo[len(T.SITE_PREFIX):])))
    for k in sorted(x for x in graph.dynobj if x in reach):
        fails.append('DYNAMIC STACK OBJECT: %s/%s reaches %s, which allocates with no static'
                     ' size' % (image, name, k))
    for cyc in walk.cycles:
        if set(cyc) & reach:
            fails.append('REACHABLE CYCLE: %s/%s reaches %s' % (image, name, ' -> '.join(cyc)))
    for k in sorted(reach):
        if len(graph.definers.get(k, ())) > 1:
            fails.append('AMBIGUOUS DEFINITION: %s/%s reaches %s, defined in %d linked units'
                         % (image, name, k, len(graph.definers[k])))
        if not k.startswith(T.SITE_PREFIX) and k not in graph.size:
            fails.append('UNSIZED REACHABLE NODE: %s/%s reaches %s, which has no frame in the'
                         ' corpus and no unsized record' % (image, name, k))
    return fails


def run(argv):
    opt = {}
    i = 0
    while i < len(argv):
        if argv[i] not in ('--ci-dir', '--src', '--arch', '--preset', '--decl', '--roots',
                           '--indirect', '--kernel-cores') or i + 1 >= len(argv):
            die('usage: app_stack.py --ci-dir <dir> --src <dir> --arch <arch> --preset <preset>'
                ' --decl <file> --roots <file> --indirect <file> --kernel-cores <n>')
        opt[argv[i][2:]] = argv[i + 1]
        i += 2
    for k in ('ci-dir', 'src', 'arch', 'preset', 'decl', 'roots', 'indirect', 'kernel-cores'):
        if k not in opt:
            die('missing --%s' % k)
    try:
        cores = int(opt['kernel-cores'])
    except ValueError:
        die('--kernel-cores %s is not a number' % opt['kernel-cores'])
    decl = Decl(opt['decl'], opt['arch'], cores)
    if not decl.threads:
        print('app_stack: %s declares no app thread for %s' % (opt['decl'], opt['arch']))
        return 0
    # The trap gate's own allowances for code no .ci sizes, so one figure prices both.
    base = T.Decl(opt['roots'], opt['arch'], cores)
    for sym, (cost, _reason) in base.unsized.items():
        if sym in decl.unsized:
            die('unsized %s is declared in both %s and %s' % (sym, opt['decl'], opt['roots']))
        decl.unsized[sym] = (cost, base.unsized_calls.get(sym, []))
    bindings = T.read_bindings(opt['indirect'], opt['arch'], opt['preset'], cores)
    entries = compile_entries(opt['ci-dir'])
    fails = []
    for image, name, root, stack, presets in decl.threads:
        fails += check_thread(opt['ci-dir'], opt['src'], decl, bindings, entries, opt['preset'],
                              image, name, root, stack, presets)
    if fails:
        for f in fails:
            sys.stderr.write('FAIL: %s\n' % f)
        return 1
    print('app_stack: OK')
    return 0


def main():
    try:
        return run(sys.argv[1:])
    except T.Bad as e:
        sys.stderr.write('FAIL: %s\n' % e)
        return 2


if __name__ == '__main__':
    sys.setrecursionlimit(20000)
    sys.exit(main())
