#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Controls for tests/static/app_stack.py on a planted corpus: two images, a compile database,
# hand-written .ci files and link maps. Each arm states what must hold:
#   - a thread whose need equals its stack passes, and one byte less fails;
#   - the image's thread-local carve comes off the stack, read from the image's own map where
#     the final map loads its object and from no other;
#   - a reachable node with no frame and no unsized record fails, as does an indirect call no
#     site record binds; a bound site is charged its callee, and the trap roots' unsized
#     records size a node too, never one the declarations also size;
#   - a deeper definition in a unit only the other image links is not charged, and fails as
#     ambiguous once both are linked;
#   - the unit's own flags reach the figures: a header that selects its frame on a macro the
#     compile command's -f flag predefines answers the wider one;
#   - an image not linked is named and not bounded, a map named for an executable suffix is the
#     image's, and an arch with no thread whose tree links an image another arch bounds fails;
#   - a map is the image's only beside the file the build graph builds for the image's target: an
#     earlier build's map is not read, and a target built with no map beside it fails;
#   - an image linked on its own first is the inputs its own map loads, beside the final map, which
#     names it by its object: read so it bounds as before, and refused when that map is missing or
#     loads nothing;
#   - a token after a record's fixed fields that is no key its kind takes, or a key stated twice,
#     fails the parse naming the record, while the reason may say anything;
#   - the real app_stack_roots.txt parses, every image it names is one an app CMakeLists.txt
#     names, and every arch it declares threads for has a trap-stack header;
#   - each arch's real need record, read through its own header, passes a planted thread at
#     exactly its need and fails one a byte deeper;
#   - a need's via= frame is charged below the root, and one no loaded unit sizes fails;
#   - a thread's above= frames are charged below the root the same way.

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 1 ] || { echo "usage: $0 <c++ compiler>" >&2; exit 2; }
TOOL="$(dirname "$0")/app_stack.py"
[ -r "$TOOL" ] || fail "$TOOL is unreadable"
command -v python3 >/dev/null 2>&1 || fail "python3 not found"
scratch_dir

python3 -B - "$TOOL" "$1" "$TMP" <<'PYEOF'
import glob
import json
import os
import shutil
import subprocess
import sys

tool, cxx, tmp = sys.argv[1], sys.argv[2], sys.argv[3]
src = os.path.join(tmp, 'src')
build = os.path.join(tmp, 'build')
ninja = shutil.which('ninja')
if ninja is None:
    sys.stderr.write('FAIL: no ninja on PATH, and app_stack.py asks the build graph which file'
                     ' an image is\n')
    sys.exit(1)


def die(msg):
    sys.stderr.write('FAIL: %s\n' % msg)
    sys.exit(1)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def node(title, label, frame):
    return ('node: { title: "%s" label: "%s\\n%s:1:1\\n%d bytes (static)\\n0 dynamic objects" }\n'
            % (title, label, title, frame))


def ref(title, label):
    return 'node: { title: "%s" label: "%s\\n%s:1:1" shape : ellipse }\n' % (title, label, title)


def edge(a, b, loc):
    return 'edge: { sourcename: "%s" targetname: "%s" label: "%s" }\n' % (a, b, loc)


def plant(stack='(PLANT_BASE + 278)', carve=0, sys_sized=True, indirect=False, both=False,
          fflag='', map1=True, bound=False, roots_sys=None, need=None, map_suffix='', via=None, above=None,
          split=None, linked=None, stale=False, carve_at='final'):
    if linked is None:
        linked = map1
    for d in (src, build):
        subprocess.run(['rm', '-rf', d])
    write(os.path.join(src, 'hdr.h'),
          '#ifdef __FAST_MATH__\n#define PLANT_FRAME 104\n#else\n#define PLANT_FRAME 32\n#endif\n'
          '#define PLANT_ZONE_A 600\n#define PLANT_ZONE (PLANT_ZONE_A + 96u)\n'
          '/*\n * class T frame=PLANT_FRAME depth=PLANT_ZONE\n */\n')
    write(os.path.join(src, 'a.cc'), '#define PLANT_BASE 600\n#define PLANT_STACK %s\n' % stack)
    write(os.path.join(src, 'b.cc'), '\n')
    write(os.path.join(src, 'c.cc'), '\n')
    db = []
    for unit in ('a', 'b', 'c'):
        cmd = '%s %s -DPLANT=1 -c %s/%s.cc -o %s/%s.cc.o' % (cxx, fflag, src, unit, build, unit)
        db.append({'directory': build, 'file': '%s/%s.cc' % (src, unit), 'command': cmd})
    write(os.path.join(build, 'compile_commands.json'), json.dumps(db))
    a = ('graph: { title: "%s/a.cc"\n' % src
         + node('%s/a.cc:_ZL5entryPv' % src, 'void entry(void*)', 100)
         + node('helper', 'void helper()', 50)
         + node('start', 'void start()', 24)
         + ref('libfn', 'void libfn()')
         + ref('sys', 'void sys()')
         + edge('%s/a.cc:_ZL5entryPv' % src, 'helper', '%s/a.cc:3:3' % src)
         + edge('%s/a.cc:_ZL5entryPv' % src, 'libfn', '%s/a.cc:4:3' % src)
         + edge('helper', 'sys', '%s/a.cc:7:3' % src))
    if indirect:
        a += edge('helper', '__indirect_call', '%s/a.cc:8:3' % src)
    write(os.path.join(build, 'a.cc.ci'), a)
    write(os.path.join(build, 'b.cc.ci'),
          'graph: { title: "%s/b.cc"\n' % src + node('libfn', 'void libfn()', 20))
    write(os.path.join(build, 'c.cc.ci'),
          'graph: { title: "%s/c.cc"\n' % src + node('libfn', 'void libfn()', 900))
    carve_line = '                0x%08x                __kickos_tls_carve = 0x0\n' % carve
    final_carve = carve_line
    if carve_at == 'image':
        final_carve = ''
    loads = 'LOAD a.cc.o\nLOAD b.cc.o\n'
    if both:
        loads += 'LOAD c.cc.o\n'
    if map1 and split is None:
        write(os.path.join(build, 'app1', 'app1%s.map' % map_suffix), loads + final_carve)
        if carve_at == 'image':
            write(os.path.join(build, 'app1', 'app1%s.kimg' % map_suffix, 'image0.map'),
                  loads + carve_line)
    elif map1:
        write(os.path.join(build, 'app1', 'app1%s.map' % map_suffix),
              'LOAD app1/app1%s.kimg/kickos_image0.o\nLOAD libkernel.a\n' % map_suffix
              + final_carve)
        if split != 'missing':
            image_loads = loads
            if split == 'empty':
                image_loads = ''
            write(os.path.join(build, 'app1', 'app1%s.kimg' % map_suffix, 'image0.map'),
                  image_loads + carve_line)
    if stale:
        write(os.path.join(build, 'app1', 'app1.image0.map'), 'LOAD c.cc.o\n' + carve_line)
        if not map1:
            write(os.path.join(build, 'app1', 'app1.map'), loads + carve_line)
    write(os.path.join(build, 'CMakeCache.txt'), 'CMAKE_MAKE_PROGRAM:FILEPATH=%s\n' % ninja)
    graph = 'rule link\n  command = true\nbuild app2/app2: link\nbuild app2: phony app2/app2\n'
    if linked:
        graph += ('build app1/app1%s: link\nbuild app1: phony app1/app1%s\n'
                  % (map_suffix, map_suffix))
    write(os.path.join(build, 'build.ninja'), graph)
    write(os.path.join(build, 'app2', 'app2.map'), 'LOAD a.cc.o\nLOAD c.cc.o\n' + carve_line)
    if need is None:
        need = ('hdr.h', 'PLANT_FRAME', 'PLANT_ZONE')
    decl = 'need plant header=%s frame=%s zone=%s' % need
    if via is not None:
        decl += ' via=%s' % via
    decl += ' reason: planted\n'
    thread_above = ''
    if above is not None:
        thread_above = ' above=%s' % above
    decl += 'thread plant app1 t root=a.cc:entry stack=PLANT_STACK%s reason: planted\n' % thread_above
    decl += 'need other header=hdr.h frame=PLANT_FRAME zone=PLANT_ZONE reason: planted\n'
    decl += 'thread other app3 t root=a.cc:entry stack=PLANT_STACK reason: planted\n'
    if sys_sized:
        decl += 'unsized plant sys 0 reason: planted\n'
    write(os.path.join(tmp, 'decl.txt'), decl)
    roots = ('arch plant header=%s\n' % os.path.join(src, 'hdr.h')
             + 'floor plant files=1 nodes=1 reason: planted\n'
             'root plant T NONE reason: planted\n')
    if roots_sys is not None:
        roots += 'unsized plant sys %d reason: planted\n' % roots_sys
    write(os.path.join(tmp, 'roots.txt'), roots)
    sites = ''
    if bound:
        sites = 'site plant * helper@1/1 libfn reason: planted\n'
    write(os.path.join(tmp, 'indirect.txt'), sites)


def run(preset, src_dir=None, arch='plant'):
    if src_dir is None:
        src_dir = src
    r = subprocess.run(['python3', '-B', tool, '--ci-dir', build, '--src', src_dir,
                        '--arch', arch, '--preset', preset, '--decl', os.path.join(tmp, 'decl.txt'),
                        '--roots', os.path.join(tmp, 'roots.txt'),
                        '--indirect', os.path.join(tmp, 'indirect.txt'), '--kernel-cores', '1'],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode, r.stdout.decode('utf-8', 'replace')


def expect(what, want_rc, words, preset='p1', arch='plant', **kw):
    plant(**kw)
    rc, out = run(preset, arch=arch)
    if rc != want_rc:
        die('%s: exit %d, want %d\n%s' % (what, rc, want_rc, out))
    for w in words:
        if w not in out:
            die('%s: output lacks "%s"\n%s' % (what, w, out))
    print('app_stack control: %s' % what)


# entry 100 + helper 50 = 150 deep, plus 32 + 696 below it.
expect('a need equal to its stack passes', 0, ['= 878 bytes', 'headroom 0', 'app_stack: OK'])
expect('one byte less fails', 1, ['STACK BELOW ITS THREAD'], stack='877')
expect('the tls carve comes off the stack', 0, ['tls carve 16', 'headroom 0'],
       stack='894', carve=16)
expect('one carve byte more fails', 1, ['tls carve 17', 'STACK BELOW ITS THREAD'],
       stack='894', carve=17)
expect('an unsized reachable node fails', 1, ['UNSIZED REACHABLE NODE', 'reaches sys'],
       sys_sized=False)
expect('an indirect call fails', 1, ['UNBOUND INDIRECT SITE'], indirect=True)
expect('an indirect call bound to its callee is charged it', 1,
       ['= 898 bytes', 'STACK BELOW ITS THREAD'], indirect=True, bound=True)
expect('the trap roots size a node the declarations leave unsized', 1,
       ['= 888 bytes', 'STACK BELOW ITS THREAD'], sys_sized=False, roots_sys=10)
expect('a node sized in both files is refused', 2, ['declared in both'], roots_sys=10)
expect('both images linking libfn is ambiguous', 1, ['AMBIGUOUS DEFINITION'], both=True)
expect('the -f flag reaches the header', 1, ['PLANT_FRAME 104', 'STACK BELOW ITS THREAD'],
       fflag='-ffast-math')
expect('an image not linked is named', 0, ['app1/t: not bounded on p1'], map1=False)
expect('an arch with no thread whose tree links a bounded image fails', 2,
       ['UNBOUNDED', 'no app thread for bare', 'links app1'], arch='bare')
expect('an arch with no thread whose tree links no bounded image passes', 0,
       ['no app thread for bare'], arch='bare', map1=False)
expect('a map named for an executable suffix is the image\'s', 0, ['app_stack: OK'],
       map_suffix='.efi')
expect('an earlier build\'s map beside the image\'s is not read', 0,
       ['= 878 bytes', 'app_stack: OK'], stale=True)
expect('a map no target of the build graph builds is not read', 0,
       ['app1/t: not bounded on p1'], map1=False, stale=True)
expect('a target the build graph builds with no map beside it fails', 2,
       ['NO LINK MAP', 'no map of it is beside it'], map1=False, linked=True)
expect('a carve the image\'s own link assigns is read from its map', 0,
       ['tls carve 16', 'headroom 0'], stack='894', carve=16, split='loaded', carve_at='image')
expect('a program map the final map does not load assigns no carve', 2,
       ['assigns no __kickos_tls_carve'], carve=16, carve_at='image')
expect('an image linked on its own first is the inputs its own map loads', 0,
       ['= 878 bytes', 'headroom 0', 'app_stack: OK'], split='loaded')
expect('an image object with no map of its own beside it fails', 2,
       ['loads image 0 and no map of it is beside it'], split='missing')
expect('an image map loading nothing fails', 2, ['app1.kimg/image0.map carries no LOAD line'],
       split='empty')
expect('the frame every entry is called from is charged below it', 1,
       ['via start 24', '= 902 bytes', 'STACK BELOW ITS THREAD'], via='start')
expect('a via no loaded unit sizes fails', 2, ['via nosuch is no sized node'], via='nosuch')
expect('the frames above the root are charged below it', 1,
       ['above start 24', '= 902 bytes', 'STACK BELOW ITS THREAD'], above='start')
expect('a frame above the root no loaded unit sizes fails', 2, ['above nosuch is no sized node'],
       above='nosuch')

# The real declarations parse, for every arch they name, on a host that builds none of them.
sys.path.insert(0, os.path.dirname(os.path.abspath(tool)))
import app_stack  # noqa: E402
import trap_redzone  # noqa: E402

NEED = 'need plant header=hdr.h frame=F zone=Z reason: planted\n'


def parsed(what, text, words, want_refused=True):
    path = os.path.join(tmp, 'tokens.txt')
    write(path, text)
    try:
        app_stack.Decl(path, 'plant')
    except trap_redzone.Bad as e:
        if not want_refused:
            die('%s: refused\n%s' % (what, e))
        for w in words:
            if w not in str(e):
                die('%s: refusal lacks "%s"\n%s' % (what, w, e))
    else:
        if want_refused:
            die('%s: parsed' % what)
    print('app_stack control: %s' % what)


THREAD = 'thread plant app1 t root=a.cc:entry stack=S %s reason: planted %s\n'
parsed('a key=value in the reason is the reason', NEED + THREAD % ('', 'presets=p9 frob=1'),
       [], want_refused=False)
parsed('a thread naming presets= is refused', NEED + THREAD % ('presets=p2', ''),
       ['tokens.txt:2', 'thread plant app1 t', '"presets=p2"'])
parsed('a thread naming an unknown key is refused', NEED + THREAD % ('frob=1', ''),
       ['tokens.txt:2', '"frob=1"'])
parsed('a thread stating root= twice is refused', NEED + THREAD % ('root=b.cc:entry', ''),
       ['tokens.txt:2', 'root= twice'])
parsed('a bare token on a thread is refused', NEED + THREAD % ('presets', ''),
       ['tokens.txt:2', '"presets"'])
parsed('a need naming an unknown key is refused',
       'need plant header=hdr.h frame=F zone=Z depth=D reason: planted\n',
       ['tokens.txt:1', '"depth=D"'])
parsed('an unsized naming call= is refused',
       NEED + 'unsized plant sys 0 call=x reason: planted\n', ['tokens.txt:2', '"call=x"'])

real = os.path.join(os.path.dirname(os.path.abspath(tool)), 'app_stack_roots.txt')
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(tool))))
arches = set(f[1] for _n, f, _r in trap_redzone.records(real) if len(f) > 1)
if not arches:
    die('%s declares nothing' % real)


# Whether trap_redzone, which runs app_stack.py, is registered on <arch>'s presets.
def gated(arch):
    return bool(glob.glob(os.path.join(repo, 'arch', '*', arch, 'include', 'kickos', 'arch',
                                       '*_trap_stack.h')))


ctl = os.path.join(tmp, 'ctl')
write(os.path.join(ctl, 'user', 'apps', 'a', 'CMakeLists.txt'),
      'add_subdirectory(gone) # gone\nset(_t named)\n')
if app_stack.unnamed_images(ctl, ['named', 'gone']) != ['gone'] or gated('plant'):
    die('a planted image no app names, or a planted arch with no trap-stack header, passes')
for arch in sorted(arches):
    try:
        d = app_stack.Decl(real, arch)
    except trap_redzone.Bad as e:
        die(str(e))
    if not d.threads:
        die('%s: %s has records and no thread' % (real, arch))
    if not gated(arch):
        die('%s: %s declares app threads and has no trap-stack header, so no trap_redzone run'
            ' bounds them' % (real, arch))
gone = app_stack.unnamed_images(repo, d.images)
if gone:
    die('%s declares threads of %s, which no user/apps CMakeLists.txt names, so no tree links'
        ' them and no run bounds them' % (real, ', '.join(gone)))
print('app_stack control: %s parses for %s' % (os.path.basename(real), ', '.join(sorted(arches))))

# Each arch's real need record, read through its own header, bounds a planted thread: one at
# exactly its need passes and one a byte deeper than its stack fails.
for arch in sorted(arches):
    need = app_stack.Decl(real, arch).need
    # Every unit of the arch compiles with its include root, which a header may include from.
    inc = '-I' + os.path.join(repo, need[0].split('/include/', 1)[0], 'include')
    plant(stack='(PLANT_BASE + 100000)', need=need, fflag=inc)
    rc, out = run('p1', repo)
    figure = None
    for line in out.splitlines():
        if line.startswith('app_stack: app1/t: depth '):
            figure = int(line.split(' = ')[1].split(' ')[0])
    if rc != 0 or figure is None:
        die('%s: the need record does not bound a planted thread\n%s' % (arch, out))
    plant(stack=str(figure), need=need, fflag=inc)
    rc, out = run('p1', repo)
    if rc != 0 or 'headroom 0' not in out:
        die('%s: a thread at exactly its need of %d fails\n%s' % (arch, figure, out))
    plant(stack=str(figure - 1), need=need, fflag=inc)
    rc, out = run('p1', repo)
    if rc != 1 or 'STACK BELOW ITS THREAD' not in out:
        die('%s: a thread one byte deeper than its stack passes\n%s' % (arch, out))
    print('app_stack control: %s bounds a planted thread at its need of %d' % (arch, figure))
PYEOF
