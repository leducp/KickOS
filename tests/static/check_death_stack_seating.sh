#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A slain thread's stub is seated on its own kernel block: every arch arch/Kconfig says carves
# kernel blocks rebuilds the slain context through arch_ctx_redirect_to_block.
#
# Run from the repo root, no arguments: tests/static/check_death_stack_seating.sh
#
# The fault and slay stubs assert their own seat, so on the arches an emulator runs, listed in
# EMULATED, the fault and slay gates witness it and this reads nothing. The rest have no run
# that slays a thread, and their source is what gets read.
#
# Comments and literals are blanked before anything is read, so no claim can be met by prose.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root

scratch_dir

EMULATED="armv6m armv7m armv8a rv32imac rv64imac x86_64"

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
BODY="$(dirname "$0")/fn_body.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"
[ -r "$BODY" ] || fail "tests/static/fn_body.awk is unreadable; no function body can be extracted"

rc=0

rebuilds() { # <file>
    awk -f "$STRIP" "$1" | awk -v FN=arch_ctx_redirect -f "$BODY" 2>/dev/null \
        | grep -q 'arch_ctx_redirect_to_block('
}

printf 'void arch_ctx_redirect(void)\n{\n    arch_ctx_redirect_to_block(0, 0, 0, 0);\n}\n' \
    > "$TMP/pos.cc"
printf 'void arch_ctx_redirect(void)\n{\n    /* arch_ctx_redirect_to_block(0, 0, 0, 0); */\n    arch_context_init(0, 0, 0, 0, 0, 1);\n}\n' \
    > "$TMP/neg.cc"
printf 'void kickos_arch_ctx_redirect(void)\n{\n    arch_ctx_redirect_to_block(0, 0, 0, 0);\n}\n' \
    > "$TMP/suffix.cc"
rebuilds "$TMP/pos.cc" || fail "a planted redirect through arch_ctx_redirect_to_block is refused"
if rebuilds "$TMP/neg.cc"; then
    fail "a planted redirect onto the user stack, the helper named only in a comment, passes"
fi
if rebuilds "$TMP/suffix.cc"; then
    fail "fn_body.awk reads kickos_arch_ctx_redirect as arch_ctx_redirect, so a suffix-named body
      would answer for a backend's"
fi

awk '/^config ARCH_/ { a = tolower(substr($2, 6)); next } /^config / { a = "" }
    a != "" && $1 == "select" && $2 == "ARCH_HAS_KERNEL_STACKS" { print a }' arch/Kconfig \
    > "$TMP/carving"
for _a in $EMULATED; do
    grep -qx "$_a" "$TMP/carving" || fail "$_a is listed as emulated but arch/Kconfig no longer
  says it carves kernel blocks; the list is stale"
done
: > "$TMP/read"
while IFS= read -r _a; do
    case " $EMULATED " in
        *" $_a "*) continue ;;
    esac
    printf '%s\n' "$_a" >> "$TMP/read"
    _f="$(git ls-files -- "arch/*/$_a/*.cc" | xargs grep -l "^void arch_ctx_redirect(")"
    if [ -z "$_f" ]; then
        bad "no arch/*/$_a/*.cc defines arch_ctx_redirect, so that arch's slay rebuild is unread"
        continue
    fi
    rebuilds "$_f" || bad "$_f: arch_ctx_redirect does not rebuild through
      arch_ctx_redirect_to_block, so a slain thread's stub is not seated on its kernel block"
done < "$TMP/carving"
require_nonempty "$TMP/read" "every block-carving arch is emulated, so this gate reads nothing
  and should go"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: arch_ctx_redirect rebuilds a slain thread through arch_ctx_redirect_to_block on
  $(tr '\n' ' ' < "$TMP/read" | sed 's/ $//')"
