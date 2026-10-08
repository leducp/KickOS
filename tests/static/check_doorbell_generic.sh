#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The doorbell carries a rendezvous and a reschedule over one raise, and this reads the LINKED
# IMAGE for the two halves of the boundary between them the types cannot hold.
#
# The reschedule's publisher is file-local to kernel/sync/klock.cc and its consumer enters the
# scheduler only through kickos_kernel_core_resched_if_owed, so no backend can publish an ask or
# enter the scheduler unguarded. That fused entry has to be visible to the dispatch, though, so:
#
#   - the send and the rendezvous may not branch to it: a rendezvous the caller wanted no switch
#     out of would cost each target a scheduler entry and a contended kernel lock;
#   - on lx6 the dispatch must branch to it. Elsewhere an emulator run reddens when it does not;
#     this part has none, and an ask left standing starves the core it names.
#
# usage: check_doorbell_generic.sh <elf> <nm> <objdump> <arch>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_doorbell_generic.sh <elf> <nm> <objdump> <arch>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"
arch="${4:?$_usage}"

SEND=arch_ipi_send
FUSED=kickos_kernel_core_resched_if_owed
DISPATCH=
# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

# Per arch: the rendezvous body where one is linked, and the mnemonics a direct branch is spelled
# with. A backend not named here is a refusal: it would otherwise read as a gate that passed.
case "$arch" in
    armv8a)
        RDV=kickos_arm64_instruction_side_rendezvous
        BRX='^(b|bl|b\\..*)$'
        B_CALL="bl 2000"; B_TAIL="b 2200" ;;
    rv64imac)
        RDV=kickos_rv64_translation_rendezvous
        BRX='^(j|jal|call|tail)$'
        B_CALL="jal 2000"; B_TAIL="j 2200" ;;
    lx6)
        # No rendezvous body on this part.
        RDV=
        DISPATCH=kickos_lx6_dispatch_l1
        # callx*/jx are register-indirect and name no symbol.
        BRX='^(j|call0|call4|call8|call12)$'
        B_CALL="call8 2000"; B_TAIL="j 2200" ;;
    *)
        fail "check_doorbell_generic.sh knows no backend '$arch'. The symbol names and the
  branch mnemonics are both per arch, so an unlisted one would pass without asserting anything" ;;
esac

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# The direct branch targets inside one body, one per line, or NOSYM / NOINSN.
# HALF A PROGRAM: `seen` and the body scope come from gate.sh's scoped_body.
cat > "$TMP/calls.awk" <<'AWK'
{
    text = $0
    sub(/^[^:]*:[ \t]*/, "", text)
    n++
    mnem = text
    sub(/[ \t].*$/, "", mnem)
    if (mnem !~ brx) { next }
    if (text !~ /</) { next }
    tgt = text
    sub(/^[^<]*</, "", tgt)
    sub(/>.*$/, "", tgt)
    sub(/\+0x[0-9a-f]+$/, "", tgt)
    if (tgt == sym) { next }
    if (!(tgt in got)) { got[tgt] = 1; order[++k] = tgt }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    for (i = 1; i <= k; i++) { print order[i] }
}
AWK

body_calls() { # <listing> <symbol>
    scoped_body "$TMP/calls.awk" "$1" "$2" -v brx="$BRX"
}

# Planted listings in this arch's --no-show-raw-insn syntax.
cat > "$TMP/ctl_clean" <<EOF
0000000000001000 <planted_send>:
    1000: $B_CALL <arch_cpu_id>
    1004: $B_TAIL <planted_raise>
EOF
cat > "$TMP/ctl_dirty" <<EOF
0000000000001000 <planted_send>:
    1000: $B_CALL <$FUSED>
    1004: $B_TAIL <planted_raise>
EOF

ctl="$(body_calls "$TMP/ctl_clean" planted_send | tr '\n' ' ')"
[ "$ctl" = "arch_cpu_id planted_raise " ] || fail "the branch reader answered [$ctl] for a
  planted send, so it does not see the targets a body branches to"
ctl="$(body_calls "$TMP/ctl_dirty" planted_send | grep -c -x "$FUSED" || true)"
[ "$ctl" = "1" ] || fail "the branch reader found $ctl branch(es) to '$FUSED' in a planted body
  that makes one, so neither a send reaching it nor a dispatch dropping it would be reported"
ctl_dead_reader "$(body_calls "$TMP/ctl_clean" a_symbol_no_listing_carries)" \
    "a renamed body would read as a clean one"

image_listing "$elf" "$nm" "$objdump" "$SYM_FLOOR" --no-show-raw-insn

# The branch targets of a body the image defines, into $TMP/targets.
targets() { # <symbol>
    image_body "$1" "$elf"
    rec="$(body_calls "$TMP/dis" "$1")"
    case "$rec" in
        NOSYM) fail "'$1' is a sized symbol in $elf but the disassembly carries no body for it" ;;
        NOINSN) fail "the body of '$1' in $elf disassembles to no instruction at all" ;;
        "") fail "the body of '$1' in $elf branches to no symbol at all, so this reader is
  looking at the wrong body" ;;
    esac
    printf '%s\n' "$rec" > "$TMP/targets"
}

image_body "$FUSED" "$elf"
for sym in "$SEND" ${RDV:+"$RDV"}; do
    targets "$sym"
    if grep -q -x -F -e "$FUSED" "$TMP/targets"; then
        fail "'$sym' branches to '$FUSED' in $elf: every raise it makes enters the target's
  scheduler, so a rendezvous costs each target a scheduler entry and a contended kernel lock"
    fi
done
if [ -n "$DISPATCH" ]; then
    targets "$DISPATCH"
    grep -q -x -F -e "$FUSED" "$TMP/targets" || fail "'$DISPATCH' never branches to '$FUSED'
  in $elf: an ask standing against this core is never consumed, and the core it names starves
  and re-raises a doorbell at itself on every release"
fi

echo "PASS: no branch to '$FUSED' from '$SEND'${RDV:+ or '$RDV'}${DISPATCH:+, and one from '$DISPATCH'}"
exit 0
