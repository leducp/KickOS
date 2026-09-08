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
# TWO CLAIMS. 1: every field of struct Slot that a take SPENDS is an Atomic, so a load is a
# load and the project's atomic rule covers the field. 2: take_reply and take_call each read
# each of those fields exactly once, through .load(), and read neither as a plain field.
#
# Comments and literals are blanked before anything is read, so no claim can be met by prose.

set -u
set -f
. "$(dirname "$0")/../lib/gate.sh"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"

export LC_ALL=C
scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
BODY="$(dirname "$0")/fn_body.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"
[ -r "$BODY" ] || fail "tests/static/fn_body.awk is unreadable; no function body can be extracted"

HDR=kernel/include/kickos/ampwindow.h
SRC=kernel/amp/ampwindow.cc
[ -f "$HDR" ] || fail "$HDR is missing; struct Slot cannot be read"
[ -f "$SRC" ] || fail "$SRC is missing; the takes cannot be read"

rc=0
bad() { echo "FAIL: $*" >&2; rc=1; }

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

# The records of struct Slot's body, "<line>:<text>", off already-stripped input.
slot_body() { # <strippedfile> <outfile>
    awk '
        BEGIN { inside = 0; depth = 0 }
        {
            if (!inside) {
                if ($0 ~ /(^|[^A-Za-z_0-9])struct[[:space:]]+Slot[[:space:]]*$/) { inside = 1 }
                next
            }
            n = length($0)
            for (i = 1; i <= n; i++) {
                c = substr($0, i, 1)
                if (c == "{") { depth++ }
                else if (c == "}") { depth-- }
            }
            if (depth > 0) { printf("%d:%s\n", NR, $0) }
            if (depth <= 0 && seen) { exit }
            if (depth > 0) { seen = 1 }
        }' "$1" > "$2"
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

# --- self-test: prove both readers before reading the tree --------------------
cat > "$TMP/pos.h" <<'EOF'
        struct Slot
        {
            Atomic<uint32_t, Order::RELAXED> len;
            Atomic<uint32_t, Order::RELAXED> port;
            ReplyTag tag;
            uint8_t payload[SLOT_BYTES];
        };
EOF
strip_to "$TMP/pos.h" "$TMP/pos.stripped"
slot_body "$TMP/pos.stripped" "$TMP/pos.slot"
require_nonempty "$TMP/pos.slot" \
    "the planted struct Slot came back empty, so a plain field in the tree would read as absent"
for f in $FIELDS; do
    grep -qE "Atomic<[^>]*>[[:space:]]+$f[[:space:]]*;" "$TMP/pos.slot" \
        || fail "the planted positive does not show $f as an Atomic; that claim would pass vacuously"
done

cat > "$TMP/neg.h" <<'EOF'
        struct Slot
        {
            uint32_t len;
            uint32_t port;
            ReplyTag tag;
            uint8_t payload[SLOT_BYTES];
        };
EOF
strip_to "$TMP/neg.h" "$TMP/neg.stripped"
slot_body "$TMP/neg.stripped" "$TMP/neg.slot"
for f in $FIELDS; do
    if grep -qE "Atomic<[^>]*>[[:space:]]+$f[[:space:]]*;" "$TMP/neg.slot"; then
        fail "a plain '$f' satisfies the Atomic claim, so claim 1 checks nothing"
    fi
done

# One load each, and the reload shape the fix removed, both planted.
cat > "$TMP/pos.cc" <<'EOF'
Verdict take_probe(uint32_t from)
{
    Slot const& s = r.slot[tail & RING_MASK];
    uint32_t const len = s.len.load();
    uint32_t const port = s.port.load();
    Verdict const v = slot_ok(Class::REPLY, me, len, port);
    count_up(g_counts[me].length);
    kmemcpy(out, s.payload, len);
    *out_port = port;
    return v;
}
EOF
cat > "$TMP/reload.cc" <<'EOF'
Verdict take_probe(uint32_t from)
{
    Slot const& s = r.slot[tail & RING_MASK];
    Verdict const v = slot_ok(Class::REPLY, me, s.len.load(), s.port.load());
    uint32_t const len = s.len.load();
    kmemcpy(out, s.payload, len);
    *out_port = s.port.load();
    return v;
}
EOF
cat > "$TMP/plain.cc" <<'EOF'
Verdict take_probe(uint32_t from)
{
    Slot const& s = r.slot[tail & RING_MASK];
    uint32_t const len = s.len;
    uint32_t const port = s.port;
    count_up(g_counts[me].length);
    kmemcpy(out, s.payload, len);
    return TOOK;
}
EOF
for probe in pos reload plain; do
    strip_to "$TMP/$probe.cc" "$TMP/$probe.stripped"
    awk -v FN=take_probe -f "$BODY" "$TMP/$probe.stripped" > "$TMP/$probe.body" \
        || fail "the extractor found no take_probe in the planted $probe; it would find none in the tree"
    require_nonempty "$TMP/$probe.body" \
        "the planted $probe body came back empty, so every take would read as clean"
done
for f in $FIELDS; do
    [ "$(count_loads "$TMP/pos.body" "$f")" -eq 1 ] \
        || fail "the planted single-load body counts $(count_loads "$TMP/pos.body" "$f") load(s) of $f; the counter is wrong"
    [ "$(count_plain "$TMP/pos.body" "$f")" -eq 0 ] \
        || fail "the planted single-load body counts a PLAIN read of $f; .length or .load() is being miscounted"
    [ "$(count_loads "$TMP/reload.body" "$f")" -gt 1 ] \
        || fail "the planted reload body counts $(count_loads "$TMP/reload.body" "$f") load(s) of $f; claim 2 would accept a reload"
    [ "$(count_plain "$TMP/plain.body" "$f")" -ge 1 ] \
        || fail "the planted plain-field body shows no plain read of $f; claim 2 would accept one"
done

# --- claim 1 ------------------------------------------------------------------
strip_to "$HDR" "$TMP/hdr.stripped"
slot_body "$TMP/hdr.stripped" "$TMP/hdr.slot"
if [ ! -s "$TMP/hdr.slot" ]; then
    fail "$HDR: struct Slot could not be read, so its verdict is UNKNOWN"
fi
for f in $FIELDS; do
    grep -qE "Atomic<[^>]*>[[:space:]]+$f[[:space:]]*;" "$TMP/hdr.slot" \
        || bad "$HDR: Slot::$f is not an Atomic. It is written cross-node and spent as a
      length or an index by the consumer, so the project's atomic rule names it and the
      compiler is otherwise free to re-load it between the check and the copy"
done

# --- claim 2 ------------------------------------------------------------------
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

echo "PASS: struct Slot's spent fields are atomic, and each of $TAKES loads each of them once"
