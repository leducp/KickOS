#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# In each of the three shared-kernel backends, `kickos_irq_route_service()` must appear AFTER
# that body's request snapshot and BEFORE its answer stores. kernel/irq/irq_route.cc
# (line_op_ask) states the protocol.
#
# The backends' own barriers owe the same order and are not checked here; what each owes it for
# is stated in those bodies.
#
# AN ABSENT BODY IS A FAILURE, not a pass.
#
# usage: check_route_service_order.sh [repo-root]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

LC_ALL=C
export LC_ALL

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"

# <file>:<service function> for every backend a shared kernel is declarable on.
BODIES='arch/xtensa/lx6/klock_lx6.cc:kickos_lx6_doorbell_service
arch/arm64/armv8a/klock_armv8a.cc:kickos_arm64_doorbell_service
arch/riscv/rv64imac/klock_rv64imac.cc:kickos_rv64_doorbell_service'

scratch_dir

# --- the reader ---------------------------------------------------------------
# Ordinals inside one function body. Emits exactly one record:
#
#   NOBODY | NOSNAP <n> | NODRAIN <n> | NOANSWER <n> | ORDER <snap> <drain> <answer> <n>
#
# An answer store is g_answer without a `.load()`: the snapshot's own comparison reads g_answer
# through .load(), so the discriminator is the call and not the presence of `=`.
cat > "$TMP/reader.awk" <<'AWK'
BEGIN { inbody = 0; depth = 0; n = 0; snap = 0; drain = 0; answer = 0; seen = 0 }
{
    line = $0
    sub(/\/\/.*$/, "", line)
    gsub(/"[^"]*"/, "", line)
}
!inbody {
    if (line ~ ("^(void|static void)[ \t]+" fn "\\(")) { inbody = 1; seen = 1; depth = 0 }
    next
}
{
    n++
    if (line ~ /g_request/ && snap == 0) { snap = n }
    if (line ~ /kickos_irq_route_service[ \t]*\(/ && drain == 0) { drain = n }
    if (line ~ /g_answer/ && line !~ /\.load\(/ && answer == 0) { answer = n }
    o = gsub(/\{/, "{", line)
    c = gsub(/\}/, "}", line)
    depth += o - c
    if (depth <= 0 && n > 1) { inbody = 0 }
}
END {
    if (!seen) { print "NOBODY"; exit }
    if (snap == 0) { print "NOSNAP " n; exit }
    if (drain == 0) { print "NODRAIN " n; exit }
    if (answer == 0) { print "NOANSWER " n; exit }
    print "ORDER " snap " " drain " " answer " " n
}
AWK

read_body() { # <file> <fn>
    awk -v fn="$2" -f "$TMP/reader.awk" "$1"
}

# --- the reader's controls, before any backend is read -----------------------
cat > "$TMP/ctl_ok.cc" <<'EOF'
void planted_service(void)
{
    uint32_t asked = g_request[0].seq[me].load();
    kickos_irq_route_service();
    g_answer[me].seq[0] = asked;
}
EOF
cat > "$TMP/ctl_before.cc" <<'EOF'
void planted_service(void)
{
    kickos_irq_route_service();
    uint32_t asked = g_request[0].seq[me].load();
    g_answer[me].seq[0] = asked;
}
EOF
cat > "$TMP/ctl_after.cc" <<'EOF'
void planted_service(void)
{
    uint32_t asked = g_request[0].seq[me].load();
    g_answer[me].seq[0] = asked;
    kickos_irq_route_service();
}
EOF
cat > "$TMP/ctl_nodrain.cc" <<'EOF'
void planted_service(void)
{
    uint32_t asked = g_request[0].seq[me].load();
    g_answer[me].seq[0] = asked;
}
EOF
cat > "$TMP/ctl_loadonly.cc" <<'EOF'
void planted_service(void)
{
    uint32_t asked = g_request[0].seq[me].load();
    if (asked != g_answer[me].seq[0].load())
    {
        kickos_irq_route_service();
    }
}
EOF

ctl="$(read_body "$TMP/ctl_ok.cc" planted_service)"
case "$ctl" in
    "ORDER 2 3 4 5") ;;
    *) fail "the reader answered [$ctl] for a planted body whose drain sits between the request
  snapshot and the answer store, so it cannot recognise the correct shape and every verdict
  below is meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_before.cc" planted_service)"
case "$ctl" in
    "ORDER 3 2 4 5") ;;
    *) fail "the reader answered [$ctl] for a planted body whose drain runs BEFORE the request
  snapshot. That is the race this gate exists to catch, so a reader that does not report it
  cannot go red" ;;
esac

ctl="$(read_body "$TMP/ctl_after.cc" planted_service)"
case "$ctl" in
    "ORDER 2 4 3 5") ;;
    *) fail "the reader answered [$ctl] for a planted body whose drain runs AFTER the answer
  store. That is the OTHER wrong order, and a gate that catches only one of the two would pass
  the arrangement in which every op is reported done before it is performed" ;;
esac

ctl="$(read_body "$TMP/ctl_nodrain.cc" planted_service)"
case "$ctl" in
    NODRAIN*) ;;
    *) fail "the reader answered [$ctl] for a planted body with no drain at all, so a backend
  that stopped draining would read as a clean one" ;;
esac

ctl="$(read_body "$TMP/ctl_loadonly.cc" planted_service)"
case "$ctl" in
    NOANSWER*) ;;
    *) fail "the reader answered [$ctl] for a planted body that only READS g_answer through
  .load() and never stores one. It counted a comparison as an answer store, which would place
  the boundary at the snapshot's own test and pass a drain that is genuinely too late" ;;
esac

ctl="$(read_body "$TMP/ctl_ok.cc" a_function_this_file_lacks)"
case "$ctl" in
    NOBODY) ;;
    *) fail "the reader answered [$ctl] for a function the file does not define, so a renamed
  service body would read as a clean one" ;;
esac

# --- the real bodies ----------------------------------------------------------
echo "== the route drain's position in every doorbell service body =="

nb=0
printf '%s\n' "$BODIES" | while IFS=: read -r rel fn; do
    [ -n "$rel" ] || continue
    [ -f "$ROOT/$rel" ] || fail "no $rel under $ROOT. A shared-kernel backend moved, and this
  gate would assert nothing about it while staying green"
    rec="$(read_body "$ROOT/$rel" "$fn")"
    kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
    f2="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
    f3="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
    f4="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
    case "$kind" in
        NOBODY)
            fail "$rel defines no '$fn'. It was renamed, so this gate reports an absence it
  cannot tell apart from a pass" ;;
        NOSNAP)
            fail "'$fn' in $rel never reads g_request across its $f2 line(s). It answers from
  something this gate cannot see, so where the drain must sit is UNKNOWN rather than satisfied" ;;
        NODRAIN)
            fail "'$fn' in $rel never calls kickos_irq_route_service across its $f2 line(s). A
  cross-core line-gating ask addressed to this core is then never performed, and its asking core
  waits on an answer that certifies nothing until its bound kills the image" ;;
        NOANSWER)
            fail "'$fn' in $rel stores no answer across its $f2 line(s), so this gate has no
  upper boundary to place the drain against. UNKNOWN, not a pass" ;;
        ORDER) ;;
        *)
            fail "the reader emitted [$rec] for '$fn', a record this gate does not model" ;;
    esac
    snap="$f2"; drain="$f3"; answer="$f4"
    require_number "$snap" "the request snapshot's ordinal in $fn"
    require_number "$drain" "the route drain's ordinal in $fn"
    require_number "$answer" "the first answer store's ordinal in $fn"
    if [ "$drain" -le "$snap" ]; then
        fail "in '$fn' ($rel) the route drain is statement #$drain and the request snapshot is
  #$snap, so the drain runs BEFORE the snapshot. A request published between the two is answered
  by this pass without its op having been performed: the asking core returns from arch_ipi_wait
  believing it ran, overwrites the single mailbox slot with its next op, and the first is lost.
  A lost UNMASK leaves the line masked and its driver never runs again"
    fi
    if [ "$drain" -ge "$answer" ]; then
        fail "in '$fn' ($rel) the route drain is statement #$drain and the first answer store is
  #$answer, so the drain runs AFTER the answer. Every op is then reported complete before it is
  performed, which is the same defect without the timing window"
    fi
    echo "   $fn: snapshot #$snap, drain #$drain, answer #$answer"
done

nb="$(printf '%s\n' "$BODIES" | grep -c ':' || true)"
require_number "$nb" "the declared service-body count"
if [ "$nb" -lt 3 ]; then
    fail "this gate declares $nb service body/bodies. A shared kernel is declarable on three
  backends, and one dropped from the list is one nothing checks"
fi

echo "PASS: all $nb doorbell service body/bodies drain the route mailbox after the request
  snapshot and before the answer stores, so an answer certifies the op it is waited on for"
exit 0
