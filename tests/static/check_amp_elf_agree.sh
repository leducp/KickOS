#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Checks that every image of an AMP partition agrees. Each node links on its own, so images
# configured from different partition descriptions build cleanly and boot, and disagree only at
# run time as a HANG (docs/design-multicore.md N6b), which nothing in any build can report.
#
# usage: check_amp_elf_agree.sh <cmake> <node0-build-dir> <readelf> <node0.elf> <peer-root>
#                              <nodes> <peer-target>
#
# Checked over the partition, per docs/design-multicore.md N6h:
#
#   1. `.amp_shared` sits at the same address, with the same size, in every node's image.
#   2. `.amp_shared` is NOBITS in every one of them, so no image carries initialised bytes
#      into memory a peer may already be using.
#   3. The loaded spans of every PAIR of nodes are disjoint.
#   4. Every node's base is distinct; two nodes at one base boot as one node twice over (N6c).
#
# Clauses 1 and 2 reduce to a sweep against node 0, being equalities. Clauses 3 and 4 do NOT:
# nodes 1 and 2 can overlap each other, or share a base, while each is disjoint from node 0 and
# distinct from its base. The planted controls below therefore plant a defect at the pair not
# involving node 0.
#
# The clause reader goes through those controls, the failing shapes and the passing one, before
# any image is read. They need no build, so a broken reader is named before a node is compiled.

set -eu
here="$(dirname "$0")"
. "$here/../lib/gate.sh"

CMAKE="${1:?usage: check_amp_elf_agree.sh <cmake> <build> <readelf> <node0.elf> <peer-root> <nodes> <peer-target>}"
BUILD="${2:?}"
READELF="${3:?}"
ELF0="${4:?}"
PEER_ROOT="${5:?}"
NODES="${6:?}"
PEER_TARGET="${7:?}"

require_number "$NODES" "the partition's node count"
[ "$NODES" -ge 2 ] || fail "KICKOS_AMP_NODES is $NODES: a partition of two is the smallest
  useful one (docs/design-multicore.md N6c), so there is no second image to agree with."

# --- the clause reader, over a table one line per node: <node> <addr> <size> <type> <lo> <hi>
#
# Findings go to stdout one per line, and the count on a FINDINGS line of its own, NOT the exit
# status: a process status is eight bits, so a table with exactly 256 findings would exit 0 and
# read as a partition that agrees. Addresses arrive as the hex readelf prints (no 0x) for the
# string clauses and as decimal for the span clauses.
agree_awk='
BEGIN { n = 0 }
{ node[n] = $1; addr[n] = $2; size[n] = $3; type[n] = $4; lo[n] = $5 + 0; hi[n] = $6 + 0; n++ }
END {
    findings = 0
    if (n < 2)
    {
        printf "FINDING: the table holds %d node image(s); nothing can be compared\n", n
        findings++
    }
    # Clauses 1 and 2, per node. Equality against node 0 is transitive, so the sweep is right.
    for (i = 1; i < n; i++)
    {
        if (addr[i] != addr[0])
        {
            printf "FINDING: nodes %s and %s place .amp_shared at different addresses, 0x%s against 0x%s. Each node derives it from the partition constants, so they were configured from different partition descriptions and would never see one another writes\n", node[0], node[i], addr[0], addr[i]
            findings++
        }
        if (size[i] != size[0])
        {
            printf "FINDING: nodes %s and %s size .amp_shared differently, 0x%s against 0x%s. The window is the partition own, so a node believing it wider indexes past what the others reserved\n", node[0], node[i], size[0], size[i]
            findings++
        }
    }
    for (i = 0; i < n; i++)
    {
        if (type[i] != "NOBITS")
        {
            printf "FINDING: node %s LOADS .amp_shared (%s). The region every node writes must carry no initialised bytes: loading it lets the last node to boot overwrite what the others published\n", node[i], type[i]
            findings++
        }
    }
    # Clauses 3 and 4, per PAIR. These do NOT reduce to a sweep against node 0: two peers can
    # overlap each other, or share a base, while each is disjoint from node 0 and distinct from
    # its base.
    for (i = 0; i < n; i++)
    {
        for (j = i + 1; j < n; j++)
        {
            if (lo[i] < hi[j] && lo[j] < hi[i])
            {
                printf "FINDING: the images of nodes %s and %s OVERLAP in physical memory, [%#x, %#x) against [%#x, %#x). Each node is linked at the partition base plus its own index times the node share, so an overlap means one of those constants differs between two configures, and the later image to load would silently replace part of the earlier\n", node[i], node[j], lo[i], hi[i], lo[j], hi[j]
                findings++
            }
            if (lo[i] == lo[j])
            {
                printf "FINDING: nodes %s and %s are linked at the same base %#x, so one is a second copy of the other. Every arm that runs on the node owning that base passes on such a partition, which is what makes this the collapse docs/design-multicore.md N6c names\n", node[i], node[j], lo[i]
                findings++
            }
        }
    }
    printf "FINDINGS %d\n", findings
}'

# Runs the reader over a table and prints what it found. Sets AGREE_FOUND off the reader's own
# FINDINGS line rather than off its exit status. That line is also what says the reader RAN: an
# awk that refused the program prints nothing, and is named here instead of counting as
# agreement.
agree_check() { # <table>
    _agree_all="$(awk "$agree_awk" "$1")" \
        || fail "the clause reader did not run over $1, so nothing about that table is known"
    AGREE_FOUND="$(printf '%s\n' "$_agree_all" \
                   | sed -n 's/^FINDINGS \([0-9]\{1,\}\)$/\1/p' | tail -n1)"
    [ -n "$AGREE_FOUND" ] || fail "the clause reader printed no FINDINGS line for $1. Its count
  is what this gate reads, so without one there is no verdict to take, green or red."
    AGREE_OUT="$(printf '%s\n' "$_agree_all" | grep -v '^FINDINGS ' || true)"
    [ -z "$AGREE_OUT" ] || printf '%s\n' "$AGREE_OUT"
}

# --- the planted controls, run before any image is read -----------------------------------
#
# Seven tables. Six plant exactly one defect each and must be reported; the seventh is sound
# and must be reported clean, because a reader that reddens on everything witnesses nothing.
# The pairs covered are (0,1), (0,2) and (1,2); the last is the one a sweep against node 0
# misses.
scratch_dir
CTRL="$TMP/controls"
mkdir -p "$CTRL"

# A sound partition of three: one window, NOBITS everywhere, three disjoint ascending spans.
cat >"$CTRL/sound" <<'EOF'
0 48000000 200000 NOBITS 1073741824 1074790400
1 48000000 200000 NOBITS 1140850688 1141899264
2 48000000 200000 NOBITS 1207959552 1209008128
EOF

# (0,1) overlap: node 1 starts inside node 0's span.
sed 's/^1 \(.*\) 1140850688 1141899264$/1 \1 1074266112 1075314688/' \
    "$CTRL/sound" >"$CTRL/overlap01"
# (0,2) overlap: node 2 starts inside node 0's span.
sed 's/^2 \(.*\) 1207959552 1209008128$/2 \1 1074266112 1075314688/' \
    "$CTRL/sound" >"$CTRL/overlap02"
# (1,2) overlap: node 2 starts inside node 1's span, and BOTH stay clear of node 0.
sed 's/^2 \(.*\) 1207959552 1209008128$/2 \1 1141374976 1142423552/' \
    "$CTRL/sound" >"$CTRL/overlap12"
# (1,2) collapse: node 2 is linked at node 1's base, node 0 untouched.
sed 's/^2 \(.*\) 1207959552 1209008128$/2 \1 1140850688 1141899264/' \
    "$CTRL/sound" >"$CTRL/base12"
# Clause 1 past node 1: node 2 alone disagrees about the window.
sed 's/^2 48000000 /2 49000000 /' "$CTRL/sound" >"$CTRL/window2"
# Clause 2 past node 1: node 2 alone loads the window.
sed 's/^2 \(48000000 200000\) NOBITS/2 \1 PROGBITS/' "$CTRL/sound" >"$CTRL/loads2"

ctrl_expect_red() { # <table> <what it plants> <substring the finding must name>
    agree_check "$CTRL/$1"
    [ "$AGREE_FOUND" -gt 0 ] || fail "PLANTED CONTROL '$1' WAS NOT REPORTED: a table with $2
  produced no finding, so this reader cannot go red on that defect and a green run of it
  witnesses nothing about the claim."
    printf '%s\n' "$AGREE_OUT" | grep -q "$3" || fail "planted control '$1' reddened, but no
  finding names $3. The reader reports a defect at the wrong place, which proves the reader
  reddens and proves nothing about the clause."
    echo "== control '$1' reported $AGREE_FOUND finding(s), naming $3"
}

echo "== planted controls: the clause reader before any image is read =="
ctrl_expect_red overlap01 "nodes 0 and 1 overlapping"       "nodes 0 and 1 OVERLAP"
ctrl_expect_red overlap02 "nodes 0 and 2 overlapping"       "nodes 0 and 2 OVERLAP"
ctrl_expect_red overlap12 "nodes 1 and 2 overlapping"       "nodes 1 and 2 OVERLAP"
ctrl_expect_red base12    "nodes 1 and 2 sharing a base"    "nodes 1 and 2 are linked at the same base"
ctrl_expect_red window2   "node 2 disagreeing on the window" "nodes 0 and 2 place .amp_shared at different addresses"
ctrl_expect_red loads2    "node 2 loading the window"       "node 2 LOADS .amp_shared"

# And one at the wrapping count: 256 findings must not read as 0. Every node loads the window,
# one finding each, the window being one address and one size throughout and the spans disjoint
# and ascending.
awk 'BEGIN {
    for (i = 0; i < 256; i++)
    {
        lo = 1073741824 + i * 2097152
        printf "%d 48000000 200000 PROGBITS %d %d\n", i, lo, lo + 1048576
    }
}' >"$CTRL/wrap256"

agree_check "$CTRL/wrap256" >/dev/null
[ "$AGREE_FOUND" -eq 256 ] || fail "PLANTED CONTROL 'wrap256' REPORTED $AGREE_FOUND finding(s)
  where 256 were planted, one per node loading the window. A count of 0 here is the eight-bit
  wrap this control exists to catch, and it makes a malformed partition of that shape pass."
echo "== control 'wrap256' reported $AGREE_FOUND finding(s), so the count does not wrap at 256"

agree_check "$CTRL/sound"
[ "$AGREE_FOUND" -eq 0 ] || fail "THE SOUND PLANTED CONTROL WAS REPORTED: a partition of three
  that agrees on its window, is NOBITS throughout, and holds three disjoint ascending spans
  produced $AGREE_FOUND finding(s):
$AGREE_OUT
  A reader that reddens on a sound table reddens on everything, so its red says nothing."
echo "== control 'sound' reported no finding, so the reader can go green"

# --- the partition's own images ------------------------------------------------------------

# The artefact target is what builds the peer images.
"$CMAKE" --build "$BUILD" --target amp_partition >/dev/null 2>&1 \
    || fail "could not build the partition, so there are no peer images to compare"

# addr size type, for one section, out of the section header table.
#
# readelf right-aligns the section index in a two-character field, so a single-digit section
# prints "[ 9]" and a two-digit one "[11]": splitting on whitespace puts the name in a different
# field depending on the section count. Strip the index first and the fields are fixed: name,
# type, address, offset, size.
sect() { # <elf> <name>
    "$READELF" -SW "$1" \
        | sed 's/^ *\[ *[0-9]*\] *//' \
        | awk -v n="$2" '$1 == n { print $3, $5, $2; exit }'
}

# The loaded span of an image: lowest and highest physical byte any PT_LOAD with contents
# covers. A NOBITS segment carries no bytes, so it is skipped.
#
# The arithmetic is the shell's because awk cannot read a hex field as a number without a gawk
# extension, and a non-gawk awk REFUSES the whole program, leaving every comparison below
# reading an empty string. tests/static/check_awk_portable.sh holds the rule.
#
# $(( )) is signed 64-bit; field 4 is PhysAddr, and a row at or above 2^63 is refused by name
# rather than wrapping negative.
span() { # <elf> -> "lo hi", both decimal
    "$READELF" -lW "$1" >"$TMP/phdr" \
        || fail "readelf could not read the program headers of $1"
    _sp_rows="$(awk '$1 == "LOAD" && $5 !~ /^(0[xX])?0*$/ { print $4, $5 }' < "$TMP/phdr")"
    [ -n "$_sp_rows" ] || fail "no PT_LOAD carrying contents in $1: either the image has none,
  or awk refused the program that reads them, which reads exactly the same from here"
    _sp_min=""
    _sp_max=0
    # A redirected read, never a pipe: a `while` on the right of a pipe runs in a subshell in
    # POSIX sh and the accumulation below would be discarded with it.
    printf '%s\n' "$_sp_rows" > "$TMP/rows"
    while read -r _sp_a _sp_n; do
        case "$_sp_a" in
            0[xX]*) : ;;
            *) fail "PhysAddr [$_sp_a] in $1 is not hex; the span is UNKNOWN, not zero" ;;
        esac
        _sp_lo=$((_sp_a))
        _sp_hi=$((_sp_a + _sp_n))
        if [ "$_sp_lo" -lt 0 ] || [ "$_sp_hi" -lt "$_sp_lo" ]; then
            fail "PhysAddr $_sp_a plus size $_sp_n in $1 is at or above 2^63, which this
  gate's arithmetic cannot carry. The span is UNKNOWN and not an overlap."
        fi
        if [ -z "$_sp_min" ] || [ "$_sp_lo" -lt "$_sp_min" ]; then
            _sp_min="$_sp_lo"
        fi
        if [ "$_sp_hi" -gt "$_sp_max" ]; then
            _sp_max="$_sp_hi"
        fi
    done < "$TMP/rows"
    printf '%s %s' "$_sp_min" "$_sp_max"
}

TABLE="$TMP/partition"
: >"$TABLE"

node=0
while [ "$node" -lt "$NODES" ]; do
    if [ "$node" -eq 0 ]; then
        elf="$ELF0"
        [ -f "$elf" ] || fail "no node 0 image at $elf"
    else
        # Located rather than spelled: an app's output path is the build's business.
        elf="$(find "$PEER_ROOT/node$node" -type f -name "$PEER_TARGET" -perm -u+x 2>/dev/null | head -1)"
        [ -n "$elf" ] || fail "no node $node image named '$PEER_TARGET' under $PEER_ROOT/node$node"
    fi

    w="$(sect "$elf" .amp_shared)"
    [ -n "$w" ] || fail "node $node has no .amp_shared section"
    s="$(span "$elf")"
    [ -n "$s" ] || fail "node $node carries no loaded segment"
    for v in $s; do
        require_number "$v" "a loaded-span bound of node $node"
    done

    echo "== node $node: $elf"
    printf '%s %s %s\n' "$node" "$w" "$s" >>"$TABLE"
    node=$((node + 1))
done

echo "== the partition as read =="
awk '{ printf "   node %s: .amp_shared 0x%s size 0x%s (%s), loaded span [%#x, %#x)\n",
              $1, $2, $3, $4, $5 + 0, $6 + 0 }' "$TABLE"

agree_check "$TABLE"
[ "$AGREE_FOUND" -eq 0 ] || fail "$AGREE_FOUND finding(s) across $NODES node image(s); see above."

W="$(awk 'NR == 1 { print $2, $3 }' "$TABLE")"
echo "PASS: $NODES image(s) agree on .amp_shared (0x${W% *}, 0x${W#* } bytes, NOBITS in every"
echo "      one), every pair holds disjoint spans, and every node is linked at its own base"
