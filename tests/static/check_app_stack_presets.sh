#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the presets= lists of tests/static/app_stack_roots.txt, through
# tests/static/app_stack_presets.py: each listed preset's tree links the thread's image, and
# every trap_redzone preset of that arch whose tree links it is listed. Builds nothing.
#
# Controls first, each stating what must hold:
#   - the flattener resolves a preset's KICKOS_* cache variables through `inherits` in order,
#     a null unsetting what a parent gives and a boolean reading as a BOOL;
#   - a preset's KICKOS_CONSOLE and KICKOS_ENABLE_SELFTEST reach the generated configuration;
#   - a planted app's guard, its selection macro and the if() around that are read as its
#     conditions, and a selection or a guard of any other shape is refused;
#   - a listed preset that does not link the image fails, on either condition; a gated preset
#     that links it and is not listed fails; a gated preset of an arch with no thread that links
#     it fails; a judge that reads nothing fails.

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 3 ] || fail "usage: $0 <kconfig python> <cmake> <src-dir>"
PY="$1"
CMAKE="$2"
SRC="$(cd "$3" && pwd)" || fail "no source tree at $3"
TOOL="$SRC/tests/static/app_stack_presets.py"
FLATTEN="$SRC/tests/static/preset_boards.cmake"
[ -x "$PY" ] || fail "no python interpreter at $PY"
[ -r "$TOOL" ] || fail "$TOOL is unreadable"
[ -r "$FLATTEN" ] || fail "$FLATTEN is unreadable"
scratch_dir

# --- the flattener's cache resolution, on planted presets ---------------------
mkdir -p "$TMP/presets"
cat >"$TMP/presets/CMakePresets.json" <<'EOF'
{
  "version": 6,
  "configurePresets": [
    { "name": "base", "hidden": true,
      "cacheVariables": { "KICKOS_BOARD": "b", "KICKOS_CONSOLE": "rtt", "OTHER": "x" } },
    { "name": "mute", "hidden": true, "cacheVariables": { "KICKOS_CONSOLE": null } },
    { "name": "flag", "hidden": true,
      "cacheVariables": { "KICKOS_ENABLE_SELFTEST": { "type": "BOOL", "value": false },
                          "KICKOS_CONSOLE": "chip" } },
    { "name": "kept", "inherits": "base" },
    { "name": "unset", "inherits": ["mute", "base"] },
    { "name": "first", "inherits": ["flag", "mute", "base"],
      "cacheVariables": { "KICKOS_TELEMETRY": true } }
  ]
}
EOF
"$CMAKE" -DSRC="$TMP/presets" -DOUT="$TMP/presets/table.tsv" -DCACHE="$TMP/presets/cache.tsv" \
    -P "$FLATTEN" >"$TMP/flatten.log" 2>&1 || fail "the flattener refused planted presets:
    $(tail -3 "$TMP/flatten.log")"
printf '%s\t%s\t%s\t%s\n' \
    first KICKOS_BOARD UNINITIALIZED b \
    first KICKOS_CONSOLE UNINITIALIZED chip \
    first KICKOS_ENABLE_SELFTEST BOOL FALSE \
    first KICKOS_TELEMETRY BOOL TRUE \
    kept KICKOS_BOARD UNINITIALIZED b \
    kept KICKOS_CONSOLE UNINITIALIZED rtt \
    unset KICKOS_BOARD UNINITIALIZED b | sort >"$TMP/presets/want.tsv"
sort "$TMP/presets/cache.tsv" >"$TMP/presets/got.tsv"
cmp -s "$TMP/presets/want.tsv" "$TMP/presets/got.tsv" || fail "the flattener resolves planted
    presets wrongly: $(diff "$TMP/presets/want.tsv" "$TMP/presets/got.tsv")"
echo "app_stack_presets control: the flattener resolves cache variables through inherits"

# --- the reader and the judge, on a planted tree -----------------------------
"$PY" -B - "$TOOL" "$SRC" "$PY" "$TMP" <<'PYEOF' || exit 1
import os
import sys

tool, src, py, tmp = sys.argv[1:5]
sys.path.insert(0, os.path.dirname(tool))
import app_stack_presets as P  # noqa: E402
import trap_redzone as T  # noqa: E402


def die(msg):
    sys.stderr.write('FAIL: %s\n' % msg)
    sys.exit(1)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


MACRO = ('macro(diag_apps)\n  if(SELFTEST)\n    foreach(_d ${ARGN})\n'
         '      add_subdirectory(${_d})\n    endforeach()\n  endif()\nendmacro()\n')
SELECT = 'if(HAVE)\n  diag_apps(other app)\nendif()\n'
GUARD = 'if(CONSOLE STREQUAL "none" OR CONSOLE STREQUAL "rtt")\n  return()\nendif()\n'


def plant(macro=MACRO, select=SELECT, guard=GUARD):
    root = os.path.join(tmp, 'tree%d' % plant.n)
    plant.n += 1
    write(os.path.join(root, 'CMakeLists.txt'), 'set(HAVE 1)\nif(X)\n  set(Y 1)\nendif()\n')
    write(os.path.join(root, 'user/apps/CMakeLists.txt'), macro + 'add_subdirectory(common)\n')
    write(os.path.join(root, 'user/apps/common/CMakeLists.txt'), select)
    write(os.path.join(root, 'user/apps/common/app/CMakeLists.txt'),
          '# img\n' + guard + 'add_executable(img main.cc)\n')
    write(os.path.join(root, 'user/apps/common/other/CMakeLists.txt'),
          'add_executable(other main.cc)\n')
    return root


plant.n = 0


def conds_of(root):
    path = P.image_file(root, 'img')
    return P.guards(path) + P.selection(root, os.path.dirname(path))


def refused(what, fn, words):
    try:
        fn()
    except T.Bad as e:
        for w in words:
            if w not in str(e):
                die('%s: refusal lacks "%s"\n%s' % (what, w, e))
    else:
        die('%s: not refused' % what)
    print('app_stack_presets control: %s' % what)


root = plant()
got = [c.text() for c in conds_of(root)]
want = ['NOT (CONSOLE STREQUAL "none" OR CONSOLE STREQUAL "rtt")', 'SELFTEST', 'HAVE']
if got != want:
    die('the planted conditions read as %s, want %s' % (got, want))
if P.root_constants(root) != {'HAVE': '1'}:
    die('the root constants read as %s' % P.root_constants(root))
print('app_stack_presets control: a guard, a selection macro and its if() are the conditions')

refused('an image entered in an else() branch is refused',
        lambda: conds_of(plant(select='if(HAVE)\nelse()\n  diag_apps(app)\nendif()\n')),
        ['else() branch'])
refused('an image entered inside a foreach() is refused',
        lambda: conds_of(plant(select='foreach(x a)\n  diag_apps(app)\nendforeach()\n')),
        ['foreach()'])
refused('a return() nested below depth 0 is refused',
        lambda: conds_of(plant(guard='if(A)\n  if(B)\n    return()\n  endif()\nendif()\n')),
        ['cannot read as a guard'])
refused('a selection macro of another shape is refused',
        lambda: conds_of(plant(macro='macro(diag_apps)\n  add_subdirectory(${ARGN})\n'
                                     'endmacro()\n')),
        ['no if() around a foreach()'])
refused('a directory named twice is refused',
        lambda: conds_of(plant(select='diag_apps(app)\nadd_subdirectory(app)\n')),
        ['2 calls name the directory app'])
refused('a variable neither generated nor a root constant is refused',
        lambda: P.links(conds_of(plant()), {'CONSOLE': 'chip', 'HAVE': '1'}),
        ['SELFTEST is neither'])
write(os.path.join(tmp, 'empty.cmake'), '\n')
refused('a fragment carrying no board is refused',
        lambda: P.read_fragment(os.path.join(tmp, 'empty.cmake')), ['sets no KICKOS_BOARD'])

conds = {'img': conds_of(plant())}
on = {'SELFTEST': 'ON', 'CONSOLE': 'chip', 'HAVE': '1'}
off = {'SELFTEST': 'OFF', 'CONSOLE': 'chip', 'HAVE': '1'}
rtt = {'SELFTEST': 'ON', 'CONSOLE': 'rtt', 'HAVE': '1'}
envs = {'on': on, 'on2': on, 'off': off, 'rtt': rtt}


def verdict(what, listed, gated, words, threads=None, others=None):
    if threads is None:
        threads = {'plant': [('img', 't', frozenset(listed))]}
    arches = {'plant': gated}
    arches.update(others or {})
    findings, confirmed, _absent = P.judge(threads, arches, conds, envs)
    text = '\n'.join(findings)
    if not words and (findings or not confirmed):
        die('%s: %s' % (what, text or 'nothing confirmed'))
    for w in words:
        if w not in text:
            die('%s: findings lack "%s"\n%s' % (what, w, text))
    print('app_stack_presets control: %s' % what)


verdict('a list naming exactly the presets that link passes', ['on'], ['on', 'off', 'rtt'], [])
verdict('a listed preset with no self-test is refused', ['on', 'off'], ['on', 'off'],
        ['LISTED, NOT LINKED: img/t names off', 'if(SELFTEST)'])
verdict('a listed preset with an rtt console is refused', ['on', 'rtt'], ['on', 'rtt'],
        ['LISTED, NOT LINKED: img/t names rtt', 'CONSOLE STREQUAL "rtt"'])
verdict('a gated preset that links and is not listed is refused', ['on'], ['on', 'on2'],
        ['LINKED, NOT LISTED: img/t leaves out on2'])
verdict('a listed preset with no configuration read is refused', ['on', 'gone'], ['on'],
        ['no configuration was read for gone'])
verdict('an arch with threads and no gated preset is refused', ['on'], [],
        ['gates no preset'])
verdict('judging nothing is refused', [], ['off'], ['nothing was judged'], threads={})
verdict('an arch with no thread whose gated preset links the image is refused', ['on'], ['on'],
        ['UNBOUNDED: bare declares no thread, and 1 of its trap_redzone preset(s) link img: on2'],
        others={'bare': ['on2', 'off']})
verdict('an arch with no thread whose gated presets link no image passes', ['on'], ['on'], [],
        others={'bare': ['off', 'rtt']})

# The preset's own console and self-test reach the configuration the conditions are read from.
board = {'KICKOS_BOARD': ('UNINITIALIZED', 'qemu')}
env = P.configure(src, py, os.path.join(tmp, 'gen'), 'console',
                  dict(board, KICKOS_CONSOLE=('UNINITIALIZED', 'rtt')))
if env.get('KICKOS_CONSOLE') != 'rtt':
    die('a preset asking KICKOS_CONSOLE=rtt resolves %s' % env.get('KICKOS_CONSOLE'))
env = P.configure(src, py, os.path.join(tmp, 'gen'), 'selftest',
                  dict(board, KICKOS_ENABLE_SELFTEST=('UNINITIALIZED', 'OFF')))
if env.get('KICKOS_ENABLE_SELFTEST') != 'OFF':
    die('a preset asking KICKOS_ENABLE_SELFTEST=OFF resolves %s'
        % env.get('KICKOS_ENABLE_SELFTEST'))
env = P.configure(src, py, os.path.join(tmp, 'gen'), 'selftest-on',
                  dict(board, KICKOS_ENABLE_SELFTEST=('UNINITIALIZED', 'ON')))
if env.get('KICKOS_ENABLE_SELFTEST') != 'ON':
    die('a preset asking KICKOS_ENABLE_SELFTEST=ON resolves %s'
        % env.get('KICKOS_ENABLE_SELFTEST'))
print('app_stack_presets control: a preset\'s console and self-test reach the configuration')
PYEOF

# --- the real declarations ---------------------------------------------------
"$CMAKE" -DSRC="$SRC" -DOUT="$TMP/presets.tsv" -DCACHE="$TMP/cache.tsv" -P "$FLATTEN" \
    >"$TMP/flatten.log" 2>&1 || fail "the flattener refused: $(tail -1 "$TMP/flatten.log")"
require_nonempty "$TMP/cache.tsv" "the flattener resolved no preset cache variable"
mkdir -p "$TMP/real"
"$PY" -B "$TOOL" "$SRC" "$PY" "$SRC/tests/static/app_stack_roots.txt" \
    "$SRC/tests/static/trap_redzone_roots.txt" "$TMP/cache.tsv" "$TMP/real" \
    || fail "app_stack_presets refused the real declarations"
