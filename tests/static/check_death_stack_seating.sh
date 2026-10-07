#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The fault stub runs on the dying thread's own KERNEL BLOCK, seated at its top:
# kickos_fault_stack_top answers with ctx.kernel_sp BEFORE its user-stack fallback.
#
# Run from the repo root, no arguments: tests/static/check_death_stack_seating.sh
#
# NO RUNTIME WITNESS: a stub on the user stack still completes every death, and the host unit
# tests cannot reach the block arm, the sim's struct arch_context having no kernel_sp. The slay
# stub's half of the rule is arch_ctx_redirect_to_block (tests/unit/ctxredirect holds its body),
# and every arch arch/Kconfig says carves kernel blocks must rebuild through it.
#
# Comments and literals are blanked before anything is read, so no claim can be met by prose.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
BODY="$(dirname "$0")/fn_body.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"
[ -r "$BODY" ] || fail "tests/static/fn_body.awk is unreadable; no function body can be extracted"

rc=0

# The verdict on kickos_fault_stack_top in <file>, or the reason it cannot be read.
judge() { # <file>
    awk -f "$STRIP" "$1" > "$TMP/stripped" 2>/dev/null \
        || { echo "UNREAD: comments and literals could not be blanked"; return; }
    awk -v FN=kickos_fault_stack_top -f "$BODY" "$TMP/stripped" > "$TMP/body" 2>/dev/null \
        || { echo "UNREAD: kickos_fault_stack_top could not be extracted"; return; }
    awk -F: '
        {
            text = substr($0, index($0, ":") + 1)
            if (inarm) {
                if (text ~ /^[[:space:]]*#[[:space:]]*(else|elif|endif)/) { inarm = 0; next }
                if (text ~ /return[^;]*ctx\.kernel_sp/ && !fallback) { ok = 1 }
                next
            }
            if (text ~ /^[[:space:]]*#[[:space:]]*if[[:space:]]+KICKOS_KERNEL_STACKS[[:space:]]*$/) {
                inarm = 1
                arm = 1
                next
            }
            if (text ~ /stack_base/) { fallback = 1 }
        }
        END {
            if (!arm) { print "NOARM: carries no #if KICKOS_KERNEL_STACKS arm"; exit }
            if (!ok) { print "NOBLOCK: does not answer ctx.kernel_sp in its block arm above the user-stack fallback"; exit }
            print "OK"
        }' "$TMP/body"
}

cat > "$TMP/pos.cc" <<'EOF'
extern "C" uintptr_t kickos_fault_stack_top(void)
{
#if KICKOS_KERNEL_STACKS
    if (c->ctx.kernel_sp != 0)
    {
        return c->ctx.kernel_sp;
    }
#endif
    return reinterpret_cast<uintptr_t>(c->stack_base) + c->stack_size;
}
EOF
cat > "$TMP/below.cc" <<'EOF'
extern "C" uintptr_t kickos_fault_stack_top(void)
{
    if (c->stack_base != nullptr)
    {
        return reinterpret_cast<uintptr_t>(c->stack_base) + c->stack_size;
    }
#if KICKOS_KERNEL_STACKS
    return c->ctx.kernel_sp;
#endif
}
EOF
cat > "$TMP/prose.cc" <<'EOF'
extern "C" uintptr_t kickos_fault_stack_top(void)
{
#if KICKOS_KERNEL_STACKS
    /* return c->ctx.kernel_sp; */
#endif
    return reinterpret_cast<uintptr_t>(c->stack_base) + c->stack_size;
}
EOF
[ "$(judge "$TMP/pos.cc")" = OK ] || fail "the planted block arm reads as [$(judge "$TMP/pos.cc")]"
for _ctl in below prose; do
    case "$(judge "$TMP/$_ctl.cc")" in
        NOBLOCK*) ;;
        *) fail "the planted $_ctl body reads as [$(judge "$TMP/$_ctl.cc")], not as a stub off the block" ;;
    esac
done

FAULT=kernel/init/fault.cc
[ -f "$FAULT" ] || fail "$FAULT is missing; kickos_fault_stack_top cannot be read"
VERDICT="$(judge "$FAULT")"
case "$VERDICT" in
    OK) ;;
    UNREAD*) fail "$FAULT: ${VERDICT#UNREAD: }, so its verdict is UNKNOWN" ;;
    *) bad "$FAULT: kickos_fault_stack_top ${VERDICT#*: }, so the fault redirect aims the stub at the dying thread's USER stack" ;;
esac

printf 'void kickos_arch_ctx_redirect(void)\n{\n    arch_ctx_redirect_to_block(0, 0, 0, 0);\n}\n' \
    > "$TMP/suffix.cc"
if awk -v FN=arch_ctx_redirect -f "$BODY" "$TMP/suffix.cc" > /dev/null 2>&1; then
    fail "fn_body.awk reads kickos_arch_ctx_redirect as arch_ctx_redirect, so a suffix-named body
      would answer for a backend's"
fi
awk '/^config ARCH_/ { a = tolower(substr($2, 6)); next } /^config / { a = "" }
    a != "" && $1 == "select" && $2 == "ARCH_HAS_KERNEL_STACKS" { print a }' arch/Kconfig \
    > "$TMP/carving"
[ "$(wc -l < "$TMP/carving")" -ge 7 ] || fail "arch/Kconfig names fewer than 7 arches selecting
  ARCH_HAS_KERNEL_STACKS; the symbol moved and the backends below go unread"
while IFS= read -r _a; do
    _f="$(git ls-files -- "arch/*/$_a/*.cc" | xargs grep -l "^void arch_ctx_redirect(")"
    if [ -z "$_f" ]; then
        bad "no arch/*/$_a/*.cc defines arch_ctx_redirect, so that arch's slay rebuild is unread"
        continue
    fi
    awk -f "$STRIP" "$_f" | awk -v FN=arch_ctx_redirect -f "$BODY" \
        | grep -q 'arch_ctx_redirect_to_block(' \
        || bad "$_f: arch_ctx_redirect does not rebuild through arch_ctx_redirect_to_block, so a
      slain thread's stub is not seated on its kernel block"
done < "$TMP/carving"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: kickos_fault_stack_top answers with the kernel block before its user-stack fallback,
  and the $(wc -l < "$TMP/carving" | tr -d ' ') block-carving arches rebuild through arch_ctx_redirect_to_block"
