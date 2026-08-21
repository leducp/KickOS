#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the HAND-MAINTAINED forwarding lists in the root CMakeLists.txt. Kconfig owns
# the knobs, but a `cmake -DKICKOS_X=...` only becomes a CONFIG_X= request if X is named
# in one of those lists. A prompted symbol missing from all of them is SILENT: CMake reports
# it under "Manually-specified variables were not used", a warning, the configure goes green,
# and the generated board_config.h carries the symbol's DEFAULT, which can be the opposite of
# what was asked for. A forwarded knob with a BAD VALUE is loud instead, a FATAL_ERROR, since
# genconfig.py reads every request back.
#
# The type leg makes a symbol forwarded through the WRONG list visible: the three lists emit
# three request syntaxes (`=N`, `="text"`, `=y|n`), so an int in the bool list forwards
# `CONFIG_X=y` and is refused at configure time on a value nobody typed.
#
# SCOPE. Read are the prompted non-choice symbols kconfiglib resolves, and the forwarding
# lists parsed out of the region between `set(_kcfg_req "")` and the kickos_kconfig_generate()
# call, which is refused if the parse cannot find it. Outside it:
#   - Choice members (BOARD_*, CONSOLE_*, TELEMETRY_*, MEMORY_MODEL_*). They are prompted
#     but are not set by a -D of their own name: the board defconfig picks them, and the
#     two the command line does reach go through the bespoke KICKOS_CONSOLE and
#     KICKOS_TELEMETRY blocks, which project a string onto a choice member. Those two
#     blocks are read only as "this name is forwarded"; their projection is not checked.
#   - Whether a forwarded knob's value survives resolution, which is check_kconfig_gen.sh.
#   - -D knobs that are not Kconfig symbols at all (KICKOS_RX_MPU_TRACE and friends). They
#     live outside the parsed region and are a different mechanism.
#
# Every prompted non-choice symbol in the tree is forwarded, so one that legitimately must
# not be belongs here as a named exemption WITH its reason, not as a loosened leg.

set -eu
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 2 ]; then
    fail "usage: check_kconfig_forwarding.sh <python> <srcdir>"
fi

PY="$1"
SRC="$(cd "$2" && pwd)" || fail "no source tree at $2"
CML="$SRC/CMakeLists.txt"

[ -x "$PY" ] || fail "no python interpreter at $PY"
[ -f "$SRC/Kconfig" ] || fail "no Kconfig at $SRC/Kconfig"
[ -f "$CML" ] || fail "no CMakeLists.txt at $CML"

# The symbol names and the CMake tokens are matched with [A-Z] classes.
export LC_ALL=C

scratch_dir

# --- The prompted set, from kconfiglib ---------------------------------------
# INT and HEX fold to `int`, BOOL and TRISTATE to `bool`: the forwarding lists distinguish
# the three request syntaxes, not the Kconfig types.
cat > "$TMP/prompted.py" <<'PYEOF'
import os
import sys

try:
    import kconfiglib
except ImportError as exc:
    sys.stderr.write("kconfiglib is not importable: %s\n" % exc)
    raise SystemExit(2)

os.chdir(sys.argv[1])
kconf = kconfiglib.Kconfig("Kconfig", warn=False)

CLASS = {
    kconfiglib.BOOL: "bool",
    kconfiglib.TRISTATE: "bool",
    kconfiglib.STRING: "string",
    kconfiglib.INT: "int",
    kconfiglib.HEX: "int",
}

for sym in kconf.unique_defined_syms:
    prompted = "derived"
    for node in sym.nodes:
        if node.prompt:
            prompted = "prompted"
    kind = "choice"
    if sym.choice is None:
        kind = "plain"
    sys.stdout.write("%s %s %s %s\n"
                     % (prompted, kind, CLASS.get(sym.type, "unknown"), sym.name))
PYEOF

# A Kconfig that resolved to nothing leaves leg 1 with an empty left-hand side and passes
# over zero symbols, so the landmark refuses an empty or unparseable read.
tool_out "$TMP/syms" '^prompted plain (int|bool|string) KICKOS_' \
         "$PY" "$TMP/prompted.py" "$SRC"

if grep -q ' unknown ' "$TMP/syms"; then
    fail "kconfiglib reports a symbol type this gate cannot classify: $(grep ' unknown ' "$TMP/syms" | awk '{ print $4 }' | tr '\n' ' ')"
fi

awk '$1 == "prompted" && $2 == "plain" { print $3, $4 }' "$TMP/syms" \
    | sort > "$TMP/want"
declared_n="$(wc -l < "$TMP/syms" | tr -d ' ')"
want_n="$(wc -l < "$TMP/want" | tr -d ' ')"
[ "$want_n" -gt 0 ] \
    || fail "no prompted non-choice symbol found in $SRC/Kconfig (leg 1 would pass vacuously)"

# --- The forwarding lists, parsed out of CMakeLists.txt ----------------------
# `foreach(<var> NAME NAME ...)` wraps, so the name list accumulates until the closing paren.
# The bespoke rule below requires an uppercase letter immediately after DEFINED, which keeps
# `if(DEFINED ${_knob})` in a loop body out of it.
awk '
/set\(_kcfg_req ""\)/            { inreg = 1; next }
/kickos_kconfig_generate\(/      { if (inreg) { print "ENDREG"; inreg = 0 } }
!inreg                           { next }
{
    line = $0
    sub(/#.*/, "", line)
}
collecting {
    buf = buf " " line
    if (index(line, ")") > 0) {
        sub(/\).*/, "", buf)
        emit(var, buf)
        collecting = 0
    }
    next
}
match(line, /foreach\([A-Za-z_][A-Za-z0-9_]*/) {
    head = substr(line, RSTART, RLENGTH)
    sub(/foreach\(/, "", head)
    var = head
    buf = substr(line, RSTART + RLENGTH)
    print "LIST", var
    if (index(buf, ")") > 0) {
        sub(/\).*/, "", buf)
        emit(var, buf)
    } else {
        collecting = 1
    }
    next
}
# The bespoke projections are NAMED, never matched by shape: a bare if(DEFINED X) is an
# ordinary thing to write in this region, and reading one as a forwarding would satisfy leg 1
# for a knob no list forwards.
match(line, /if\(DEFINED [A-Z][A-Z0-9_]*\)/) {
    tok = substr(line, RSTART, RLENGTH)
    sub(/if\(DEFINED /, "", tok)
    sub(/\)/, "", tok)
    if (tok == "KICKOS_CONSOLE" || tok == "KICKOS_TELEMETRY") {
        print "FWD", "@bespoke", tok
    }
}
function emit(v, text,   i, n, parts, tok) {
    n = split(text, parts, /[ \t]+/)
    for (i = 1; i <= n; i++) {
        tok = parts[i]
        if (tok ~ /^(CONFIG_)?[A-Z][A-Z0-9_]*$/) {
            print "FWD", v, tok
        }
    }
}
END { if (inreg) { print "OPENREG" } }
' "$CML" > "$TMP/parse"

grep -q '^ENDREG$' "$TMP/parse" \
    || fail "$CML: no forwarding region between \`set(_kcfg_req \"\")\` and kickos_kconfig_generate(); the gate reads nothing and would pass vacuously"
if grep -q '^OPENREG$' "$TMP/parse"; then
    fail "$CML: the forwarding region never closes; the parse ran past it"
fi

# The lists themselves are parsed out of CMakeLists.txt; this map is the only copy the gate
# holds. A list whose type is not named here is refused, never given a silent pass.
list_class() {
    case "$1" in
        _knob) echo int ;;
        _str)  echo string ;;
        _flag) echo bool ;;
        *)     echo "?" ;;
    esac
}

unknown_list=""
for var in $(awk '/^LIST /{ print $2 }' "$TMP/parse" | sort -u); do
    if [ "$(list_class "$var")" = "?" ]; then
        unknown_list="$unknown_list $var"
    fi
done
if [ -n "$unknown_list" ]; then
    fail "$CML forwards through a list this gate does not know the type of:$unknown_list
      Add it to list_class() in $0 with the Kconfig type it emits, or the type leg
      silently stops covering every name in it."
fi

# <class> <SYMBOL>, with the CONFIG_ prefix stripped the way the _flag block strips it.
awk '/^FWD /{
    cls = $2
    name = $3
    sub(/^CONFIG_/, "", name)
    print cls, name
}' "$TMP/parse" | sort -u > "$TMP/fwd_raw"

while read -r var name; do
    if [ "$var" = "@bespoke" ]; then
        echo "@bespoke $name"
    else
        echo "$(list_class "$var") $name"
    fi
done < "$TMP/fwd_raw" | sort -u > "$TMP/fwd"

fwd_n="$(wc -l < "$TMP/fwd" | tr -d ' ')"
[ "$fwd_n" -gt 0 ] \
    || fail "no forwarded name parsed out of $CML (both legs would pass vacuously)"
awk '{ print $2 }' "$TMP/fwd" | sort -u > "$TMP/fwd_names"

# --- Leg 1: every prompted non-choice symbol is forwarded --------------------
awk '{ print $2 }' "$TMP/want" | sort > "$TMP/want_names"
missing="$(comm -23 "$TMP/want_names" "$TMP/fwd_names")"
if [ -n "$missing" ]; then
    echo "FAIL: prompted Kconfig symbol(s) that no forwarding list in CMakeLists.txt names:" >&2
    for m in $missing; do
        echo "        $m ($(awk -v n="$m" '$2 == n { print $1 }' "$TMP/want"))" >&2
    done
    echo "      cmake -D<name>=... for these is DROPPED with only a non-fatal" >&2
    echo "      \"Manually-specified variables were not used\" warning, and the build takes the" >&2
    echo "      Kconfig DEFAULT instead. Add each to the list of its type around $CML:76." >&2
    exit 1
fi

# --- Leg 2: a forwarded name is a prompted symbol of that list's type --------
# @bespoke is exempt: KICKOS_CONSOLE and KICKOS_TELEMETRY are promptless projections onto a
# choice, so neither the prompt nor the type test applies. A third belongs above WITH its
# reason.
awk '$1 != "@bespoke" { print $1, $2 }' "$TMP/fwd" | sort > "$TMP/fwd_typed"
typed_n="$(wc -l < "$TMP/fwd_typed" | tr -d ' ')"
[ "$typed_n" -gt 0 ] || fail "every forwarded name parsed is bespoke; the typed lists were not read"

bad=""
while read -r cls name; do
    rec="$(awk -v n="$name" '$4 == n { print $1, $3 }' "$TMP/syms")"
    if [ -z "$rec" ]; then
        bad="$bad
        $name: named in the $cls list, declared by no Kconfig symbol (dead entry; -D can never reach it)"
        continue
    fi
    state="${rec%% *}"
    kind="${rec##* }"
    if [ "$state" != "prompted" ]; then
        bad="$bad
        $name: named in the $cls list, but it carries no prompt (derived value; a request on it is refused)"
        continue
    fi
    if [ "$kind" != "$cls" ]; then
        bad="$bad
        $name: declared $kind, forwarded through the $cls list (the request syntax will not match)"
    fi
done < "$TMP/fwd_typed"

if [ -n "$bad" ]; then
    echo "FAIL: forwarding list entries that do not match their Kconfig declaration:$bad" >&2
    exit 1
fi

echo "PASS: $want_n prompted non-choice symbol(s) of $declared_n declared, all forwarded;"
echo "      $typed_n typed forwarding entr(ies) match their declared type, $(( fwd_n - typed_n )) bespoke"
