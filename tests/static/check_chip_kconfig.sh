#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Kconfig's chip selection against every chip file under platform/: a chip that selects HAS_MPU
# states a region unit a build drives, a chip states a region unit no build drives exactly when it
# does not select HAS_MPU, saying so with `driven: false`, and a chip states `mmu` exactly when it
# selects HAS_ASPACE, so a chip whose file states only `none` selects neither. Each chip file names
# a chip Kconfig declares.
#
# Run from the repo root:
#   tests/static/check_chip_kconfig.sh <kconfig-python> <build-dir>
#
#   kconfig   the selects are read by kconfiglib from the Kconfig tree, under <kconfig-python>,
#             as `<chip> <HAS_MPU> <HAS_ASPACE>` for every value KICKOS_CHIP takes.
#   files     every tracked platform/<chip>/chip.yaml, from `git ls-files`, read by tools/compose
#             under uv, its environment in <build-dir>.
#   arms      each rule is run again on a selection with one fact flipped, and must refuse it, so a
#             rule that stopped firing fails the gate.

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 2 ] || fail "usage: check_chip_kconfig.sh <kconfig-python> <build-dir>"
PY="$1"
BUILD="$2"
case "$BUILD" in
    /*) ;;
    *) fail "<build-dir> must be absolute, not $BUILD" ;;
esac
require_repo_root
ROOT="$(pwd)"
TOOL="$ROOT/tools/compose"
[ -x "$PY" ] || fail "no python interpreter at $PY"
scratch_dir

cat > "$TMP/selects.py" <<'PYEOF'
import sys

try:
    import kconfiglib
except ImportError as exc:
    sys.stderr.write("kconfiglib is not importable: %s\n" % exc)
    raise SystemExit(2)

kconf = kconfiglib.Kconfig("Kconfig", warn=False)
chip = kconf.syms["KICKOS_CHIP"]
for value, condition in chip.defaults:
    if value.str_value == "":
        continue
    if not isinstance(condition, kconfiglib.Symbol):
        sys.stderr.write("KICKOS_CHIP has a default not keyed on one CHIP_ symbol\n")
        raise SystemExit(2)
    for target, cond in condition.selects:
        if target.name in ("HAS_MPU", "HAS_ASPACE") and cond is not kconf.y:
            sys.stderr.write("%s selects %s under a condition this gate cannot judge\n"
                             % (condition.name, target.name))
            raise SystemExit(2)
    selected = set(target.name for target, cond in condition.selects)
    print("%s %d %d" % (value.str_value, "HAS_MPU" in selected, "HAS_ASPACE" in selected))
PYEOF
"$PY" "$TMP/selects.py" > "$TMP/selects" || fail "kconfiglib could not read the chip selection"
[ -s "$TMP/selects" ] || fail "kconfiglib listed no chip; KICKOS_CHIP has no defaults"

corpus "$TMP/files" "chip file under platform/" 'platform/*/chip.yaml'

compose_env "$TOOL" "$BUILD/compose/chip_kconfig"

cat > "$TMP/agree.py" <<'PYEOF'
import os
import sys

from kickos_compose.descriptions import check_chip
from kickos_compose.subset import Report

REGION_UNITS = ("pmsav6", "pmsav7", "pmsav8", "pmp", "rxmpu", "sysmpu", "mprotect")


def units(path):
    """{(unit, whether a build drives it)} the chip file states."""
    report = Report()
    with open(path, encoding="utf-8") as stream:
        chip = check_chip(path, stream.read(), report)
    if report.refusals or chip is None:
        for refusal in report.refusals:
            print(refusal, file=sys.stderr)
        raise SystemExit(2)
    return set((protection.unit, protection.driven is not False) for protection in chip.protection.values())


def driven_regions(stated):
    return set(unit for unit, driven in stated if driven and unit in REGION_UNITS)


def undriven_regions(stated):
    return set(unit for unit, driven in stated if not driven and unit in REGION_UNITS)


def names(stated):
    return set(unit for unit, driven in stated)


def findings(selects, files):
    found = []
    for path in files:
        chip = os.path.basename(os.path.dirname(path))
        if chip not in selects:
            found.append("%s: Kconfig declares no chip `%s`" % (path, chip))
            continue
        mpu, aspace = selects[chip]
        stated = units(path)
        if mpu and not driven_regions(stated):
            found.append("%s: CHIP_%s selects HAS_MPU and the file states no region unit a build drives"
                         % (path, chip.upper()))
        if mpu and undriven_regions(stated) and not driven_regions(stated):
            found.append("%s: CHIP_%s selects HAS_MPU and the file says `driven: false`, so the port carves "
                         "no window for its unit" % (path, chip.upper()))
        if not mpu and driven_regions(stated):
            found.append("%s: CHIP_%s does not select HAS_MPU and the file states a region unit without "
                         "`driven: false`" % (path, chip.upper()))
        stated = names(stated)
        if aspace and "mmu" not in stated:
            found.append("%s: CHIP_%s selects HAS_ASPACE and the file does not state `mmu`"
                         % (path, chip.upper()))
        if "mmu" in stated and not aspace:
            found.append("%s: CHIP_%s does not select HAS_ASPACE and the file states `mmu`"
                         % (path, chip.upper()))
    return found


selects = {}
with open(sys.argv[1], encoding="ascii") as stream:
    for line in stream:
        chip, mpu, aspace = line.split()
        selects[chip] = (mpu == "1", aspace == "1")
with open(sys.argv[2], encoding="ascii") as stream:
    files = [line.strip() for line in stream if line.strip()]

found = findings(selects, files)
for line in found:
    print("FAIL: %s" % line, file=sys.stderr)
if found:
    raise SystemExit(1)

ARMS = (
    ("HAS_MPU over no region unit", lambda stated: not names(stated) & set(REGION_UNITS),
     lambda mpu, aspace: (True, aspace)),
    ("HAS_MPU over a unit no build drives", lambda stated: undriven_regions(stated) and not driven_regions(stated),
     lambda mpu, aspace: (True, aspace)),
    ("a driven region unit without HAS_MPU", lambda stated: driven_regions(stated) and "mmu" not in names(stated),
     lambda mpu, aspace: (False, aspace)),
    ("mmu without HAS_ASPACE", lambda stated: "mmu" in names(stated), lambda mpu, aspace: (mpu, False)),
    ("HAS_ASPACE without mmu", lambda stated: "mmu" not in names(stated), lambda mpu, aspace: (mpu, True)),
)
for label, applies, flip in ARMS:
    path = next((path for path in files if applies(units(path))), None)
    if path is None:
        print("FAIL: no chip file can carry the arm `%s`" % label, file=sys.stderr)
        raise SystemExit(1)
    chip = os.path.basename(os.path.dirname(path))
    if not findings(dict(selects, **{chip: flip(*selects[chip])}), [path]):
        print("FAIL: the arm `%s` on %s was not refused" % (label, path), file=sys.stderr)
        raise SystemExit(1)
print("chip_kconfig: %d chip file(s) agree with Kconfig, %d arm(s) refused" % (len(files), len(ARMS)))
PYEOF

compose_python "$TMP/agree.py" "$TMP/selects" "$TMP/files" \
    || fail "a chip file disagrees with Kconfig, or the gate could not run, see above"
echo "PASS: chip_kconfig"
