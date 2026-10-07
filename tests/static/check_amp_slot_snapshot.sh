#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A SLOT HEADER IS THE FAR SIDE'S WRITING, AND A TAKE MUST SPEND THE WORDS IT VALIDATED.
# `Slot::len` bounds a kmemcpy into a 256-byte interrupt-stack buffer, so a length re-read
# after it was bounded is a length nothing bounded: a peer that rewrites a slot it wrongly
# believes free (a stale tail read, exactly the malformed producer this layer defends
# against) lands between the two reads and the copy overruns.
#
# Run from the repo root, no arguments: tests/static/check_amp_slot_snapshot.sh
#
# THERE IS NO RUNTIME WITNESS, which is why this is a source gate. Nothing the host fixture
# can interpose lies between the validation and the copy: the seam under test is arch_cpu_id,
# arch_ipi_send and the endpoint layer, all of them outside the take, and kmemcpy folds to
# memcpy where no address space is enforced. A second load is also a compiler's privilege over
# a plain field, so no arm can distinguish one taken and one not.
#
# THE CLAIM. take_reply and take_call each read each field of struct Slot they SPEND exactly
# once, through .load(), and neither as a plain field: Atomic converts to T implicitly, so a
# plain read is a second load. That those fields are Atomic is the compiler's: `.load()` on a
# plain field does not compile.
#
# Comments and literals are blanked before anything is read, so no claim can be met by prose.

set -u
set -f
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
BODY="$(dirname "$0")/fn_body.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"
[ -r "$BODY" ] || fail "tests/static/fn_body.awk is unreadable; no function body can be extracted"

SRC=kernel/amp/ampwindow.cc
[ -f "$SRC" ] || fail "$SRC is missing; the takes cannot be read"

rc=0

# The fields a take spends. The tag is NOT here: it is two words, cannot be one atomic, and no
# arm of the window spends it, so a torn one is handed back torn and refused at the calling
# node's own clause.
FIELDS="len port"
TAKES="take_reply take_call"

# BLANKED AND THEN DE-INDENTED, line for line. fn_body.awk asks that a definition either open
# its line or be preceded only by a return type, and every definition here sits two namespaces
# deep; the line numbers it reports are the original file's either way.
strip_to() { # <file> <outfile>
    awk -f "$STRIP" "$1" 2> "$TMP/striperr" | sed 's/^[[:space:]]*//' > "$2" \
        || { sed 's/^/      /' "$TMP/striperr" >&2
             fail "$1: comments and literals could not be blanked, so its verdict is UNKNOWN"; }
    [ -s "$2" ] || fail "$1: stripped to nothing, so every claim below would pass vacuously"
}

# How many times <field> is loaded off a slot in <bodyfile>, and how many times it is read as
# a PLAIN field. `\.len` is bounded on the right so that `.length`, the refusal counter,
# is not read as the slot's length.
count_loads() { # <bodyfile> <field>
    grep -cE "\\.$2\\.load[[:space:]]*\\(" "$1" || true
}
count_plain() { # <bodyfile> <field>
    grep -cE "\\.$2([^A-Za-z_0-9.]|\\.[^l]|\\.l[^o])" "$1" || true
}

# The planted take: port read once as a plain field, len loaded twice.
cat > "$TMP/plant.cc" <<'EOF'
Verdict take_probe(uint32_t from)
{
    Slot const& s = r.slot[tail & RING_MASK];
    Verdict const v = slot_ok(Class::REPLY, me, s.len.load(), s.port);
    count_up(g_counts[me].length);
    kmemcpy(out, s.payload, s.len.load());
    return v;
}
EOF
strip_to "$TMP/plant.cc" "$TMP/plant.stripped"
awk -v FN=take_probe -f "$BODY" "$TMP/plant.stripped" > "$TMP/plant.body" \
    || fail "the extractor found no take_probe in the planted take; it would find none in the tree"
[ "$(count_loads "$TMP/plant.body" len)" -eq 2 ] \
    && [ "$(count_plain "$TMP/plant.body" len)" -eq 0 ] \
    && [ "$(count_loads "$TMP/plant.body" port)" -eq 0 ] \
    && [ "$(count_plain "$TMP/plant.body" port)" -eq 1 ] \
    || fail "the planted take does not count two loads of len and one plain read of port"

strip_to "$SRC" "$TMP/src.stripped"
for fn in $TAKES; do
    awk -v FN="$fn" -f "$BODY" "$TMP/src.stripped" > "$TMP/take.body" 2> "$TMP/bodyerr"
    if [ $? -ne 0 ] || [ ! -s "$TMP/take.body" ]; then
        sed 's/^/      /' "$TMP/bodyerr" >&2
        bad "$SRC: $fn could not be extracted, so its verdict is UNKNOWN"
        continue
    fi
    for f in $FIELDS; do
        _loads="$(count_loads "$TMP/take.body" "$f")"
        _plain="$(count_plain "$TMP/take.body" "$f")"
        if [ "$_plain" -ne 0 ]; then
            bad "$SRC: $fn reads a slot's $f as a plain field $_plain time(s); the field is
      the far side's writing and every read of it must be a load"
        fi
        if [ "$_loads" -eq 0 ]; then
            bad "$SRC: $fn loads no slot $f at all; either the field moved or this gate has
      stopped reading the body it names"
        elif [ "$_loads" -gt 1 ]; then
            bad "$SRC: $fn loads a slot's $f $_loads times. ONE snapshot, validated and then
      spent: a second load may answer a producer's later writing, and where the field is the
      length that is a kmemcpy bounded by a length nothing bounded"
        fi
    done
done

if [ "$rc" -ne 0 ]; then
    echo "" >&2
    echo "      The take is the only code that reads a slot header. See slot_ok and the two" >&2
    echo "      takes in $SRC." >&2
    exit 1
fi

echo "PASS: each of $TAKES loads each of struct Slot's spent fields once"
