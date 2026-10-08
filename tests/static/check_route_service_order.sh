#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The doorbell service's route drain, read out of arch/common/doorbell_protocol.cc:
# kickos_irq_route_service() is called by the drain step and by nothing else in that file.
#
# The step types order the steps: drain needs the fence's token, and the answers need drain's.
# What a step's body does is not typed, so a drain call deleted from drain, or moved into another
# step or the service body, compiles. Moved ahead of the snapshot it answers an ask it never ran;
# moved behind the answers it reports every op done before running it (kernel/irq/irq_route.cc,
# line_op_ask).
#
# usage: check_route_service_order.sh [repo-root]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
FILE="$ROOT/arch/common/doorbell_protocol.cc"

scratch_dir

# Emits NOBODY, or IN <calls inside the drain step> OUT <calls anywhere else>. A declaration
# (`void kickos_irq_route_service(`) is not a call.
cat > "$TMP/reader.awk" <<'AWK'
BEGIN { inbody = 0; depth = 0; seen = 0; in_n = 0; out_n = 0 }
{
    line = $0
    sub(/\/\/.*$/, "", line)
    gsub(/"[^"]*"/, "", line)
}
!inbody && line ~ /Drained[ \t]+Service::drain[ \t]*\(/ { inbody = 1; seen = 1; depth = 0; opened = 0 }
{
    if (line ~ /kickos_irq_route_service[ \t]*\(/ && line !~ /void[ \t]+kickos_irq_route_service/) {
        if (inbody) { in_n++ } else { out_n++ }
    }
    if (inbody) {
        o = gsub(/\{/, "{", line)
        c = gsub(/\}/, "}", line)
        if (o > 0) { opened = 1 }
        depth += o - c
        if (opened && depth <= 0) { inbody = 0 }
    }
}
END {
    if (!seen) { print "NOBODY"; exit }
    print "IN " in_n " OUT " out_n
}
AWK

read_file() { # <file>
    awk -f "$TMP/reader.awk" "$1"
}

cat > "$TMP/ctl_ok.cc" <<'EOF'
    extern "C" void kickos_irq_route_service(void);
    inline Drained Service::drain(Fenced)
    {
        kickos_irq_route_service();
        return Drained();
    }
EOF
cat > "$TMP/ctl_nodrain.cc" <<'EOF'
    inline Drained Service::drain(Fenced)
    {
        return Drained();
    }
EOF
cat > "$TMP/ctl_moved.cc" <<'EOF'
    inline Drained Service::drain(Fenced)
    {
        return Drained();
    }
void kickos_doorbell_service(void)
{
    kickos::doorbell::kickos_irq_route_service();
}
EOF
cat > "$TMP/ctl_nobody.cc" <<'EOF'
void kickos_doorbell_service(void)
{
    kickos::doorbell::kickos_irq_route_service();
}
EOF

ctl="$(read_file "$TMP/ctl_ok.cc")"
[ "$ctl" = "IN 1 OUT 0" ] || fail "the reader answered [$ctl] for a planted drain step that calls
  the route drain once, so every verdict below is meaningless"
ctl="$(read_file "$TMP/ctl_nodrain.cc")"
[ "$ctl" = "IN 0 OUT 0" ] || fail "the reader answered [$ctl] for a planted drain step with no
  call, so a drain deleted from the step would read as present"
ctl="$(read_file "$TMP/ctl_moved.cc")"
[ "$ctl" = "IN 0 OUT 1" ] || fail "the reader answered [$ctl] for a drain moved out of its step
  into the service body, so a drain run before the snapshot or after the answers would pass"
ctl="$(read_file "$TMP/ctl_nobody.cc")"
[ "$ctl" = "NOBODY" ] || fail "the reader answered [$ctl] for a file with no drain step, so a
  renamed step would read as a clean one"

[ -f "$FILE" ] || fail "no $FILE: the service body moved, and this gate would assert nothing"
rec="$(read_file "$FILE")"
case "$rec" in
    NOBODY)
        fail "$FILE defines no 'Drained Service::drain(' step: it was renamed, so this gate
  reports an absence it cannot tell apart from a pass" ;;
    "IN 1 OUT 0") ;;
    "IN 0 "*)
        fail "the drain step in $FILE never calls kickos_irq_route_service. A cross-core
  line-gating ask addressed to this core is answered and never performed: a lost UNMASK leaves
  the line masked and its driver never runs again" ;;
    IN*)
        fail "kickos_irq_route_service is called [$rec] in $FILE: inside the drain step once
  and nowhere else is the only placement the step types order between the snapshot and the
  answers" ;;
    *)
        fail "the reader emitted [$rec], a record this gate does not model" ;;
esac

echo "PASS: the drain step is the one caller of kickos_irq_route_service in $FILE"
exit 0
