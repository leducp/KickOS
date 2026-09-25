#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The link refuses a default stack whose TLS carve leaves less than its floor above it.
#
# KICKOS_TLS_CARVE_ASSERT (arch/common/sections.ld.h) holds the idle, user and root stacks to
# their floor PLUS the carve, and a link that passes proves only that the defaults clear it. This
# configures the preset again with each stack AT its floor and expects the link to refuse by the
# named text: a carve term dropped from the assert, or an assert that no longer expands, links
# there and passes every board.
#
# Registered where the carve is never zero (KICKOS_REENT_IN_TCB, a seating arch), so `hello`,
# which declares no thread_local, reaches the control-block clause of all three stacks.
# `tlsprobe` reaches the thread-local clause of idle's; a user or root stack at its floor cannot
# build it, its caller stack being half the stride that stack sets.
#
# The floors are read out of the preset's own generated script, which is what the link compares:
# the assert's `(size) >= (floor)` form after cpp. The messages are read from there too, by the
# prefixes below, so a reworded message moves this gate with it and a removed one refuses here.
#
# usage: check_tls_carve_link.sh <src-dir> <cmake> <preset> <generated linker script>

set -u
set -f
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <src-dir> <cmake> <preset> <generated linker script>" >&2
    exit 2
fi
SRC="$1"
CMAKE="$2"
PRESET="$3"
LDS="$4"

case "$SRC$CMAKE$PRESET$LDS" in
    *[[:space:]]*) fail "an argument contains whitespace; every path here is re-split" ;;
esac
[ -d "$SRC" ] || fail "source dir does not exist: $SRC"
[ -r "$LDS" ] || fail "cannot read the generated linker script $LDS"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"
command -v python3 >/dev/null 2>&1 || fail "python3 not found; it parses the script's asserts"

MSG_IDLE_TCB="KickOS: KICKOS_IDLE_STACK_SIZE cannot hold idle's TLS control block"
MSG_IDLE_TLS="KickOS: KICKOS_IDLE_STACK_SIZE cannot hold idle's thread-local block"
MSG_USER_TCB="KickOS: KICKOS_USER_STACK_SIZE cannot hold a thread's TLS control block"
MSG_ROOT_TCB="KickOS: KICKOS_ROOT_STACK_SIZE cannot hold root's TLS control block"

# <script> <message prefix> -> the floor that message's assert compares its stack against, or
# nothing when no assert carrying that message has the `|| (size) >= (floor) +` form.
floor_of() {
    python3 - "$1" "$2" <<'PYEOF'
import re
import sys

text = open(sys.argv[1]).read()
prefix = sys.argv[2]
for body in re.findall(r'ASSERT\((.*?"[^"]*")\)', text, re.S):
    if prefix not in body:
        continue
    m = re.search(r'\|\|\s*\((\d+)\)\s*>=\s*\((.+?)\)\s*\+', body, re.S)
    if m is None:
        continue
    expr = m.group(2)
    if not re.fullmatch(r'[\d\s()+*-]+', expr):
        continue
    print(eval(expr, {'__builtins__': {}}))
    break
PYEOF
}

# <script> <message prefix> -> that message whole, as the link prints it.
message_of() {
    python3 - "$1" "$2" <<'PYEOF'
import re
import sys

text = open(sys.argv[1]).read()
for m in re.findall(r'"([^"]*)"', text):
    if m.startswith(sys.argv[2]):
        print(m)
        break
PYEOF
}

# --- the readers, on planted text before the real script ----------------------
KOS_TRASH_DIR="$(mktemp -d "/var/tmp/kickos-tls-carve-$PRESET.XXXXXX")" \
    || fail "mktemp -d under /var/tmp failed"
TMP="$KOS_TRASH_DIR"
kos_trap

cat > "$TMP/ctl.ld" <<'EOF'
ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) != 0 || (960) >= ((256 + 576 + 64)) + 1 * ALIGN(8, 16), "KickOS: KICKOS_IDLE_STACK_SIZE cannot hold idle's TLS control block above the floor.")
ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) != 0 || (4096) >= (896) + 1 * ALIGN(8, 16), "KickOS: KICKOS_USER_STACK_SIZE cannot hold a thread's TLS control block above the floor.")
ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) != 0 || 4096 >= 896, "KickOS: KICKOS_ROOT_STACK_SIZE cannot hold root's TLS control block above the floor.")
EOF
_c="$(floor_of "$TMP/ctl.ld" "$MSG_IDLE_TCB")"
[ "$_c" = "896" ] || fail "the floor reader answers [$_c] for a planted (256 + 576 + 64) floor"
_c="$(floor_of "$TMP/ctl.ld" "$MSG_USER_TCB")"
[ "$_c" = "896" ] || fail "the floor reader answers [$_c] for a planted (896) floor"
_c="$(floor_of "$TMP/ctl.ld" "$MSG_ROOT_TCB")"
[ -z "$_c" ] || fail "the floor reader answers [$_c] for an assert with no (size) >= (floor) form,
    so a reshaped assert would hand this gate a figure it never compares"
_c="$(floor_of "$TMP/ctl.ld" "$MSG_IDLE_TLS")"
[ -z "$_c" ] || fail "the floor reader answers [$_c] for a message the planted script lacks"
_c="$(message_of "$TMP/ctl.ld" "$MSG_USER_TCB")"
[ "$_c" = "$MSG_USER_TCB above the floor." ] \
    || fail "the message reader answers [$_c] for a planted user-stack message"
[ -z "$(message_of "$TMP/ctl.ld" "$MSG_IDLE_TLS")" ] \
    || fail "the message reader finds a message the planted script lacks"
echo "== control: the floor reader parses the assert's own form and refuses any other; the message reader finds only what is there =="

# --- the positive control: every assert expands on this preset ----------------
for m in "$MSG_IDLE_TCB" "$MSG_IDLE_TLS" "$MSG_USER_TCB" "$MSG_ROOT_TCB"; do
    [ -n "$(message_of "$LDS" "$m")" ] || fail "$LDS carries no [$m...] assert, so this preset
    links with that stack unchecked and nothing below could refuse it"
done
FLOOR_IDLE="$(floor_of "$LDS" "$MSG_IDLE_TCB")"
FLOOR_MIN="$(floor_of "$LDS" "$MSG_USER_TCB")"
require_number "$FLOOR_IDLE" "the idle floor in $LDS"
require_number "$FLOOR_MIN" "the spawn floor in $LDS"
_r="$(floor_of "$LDS" "$MSG_ROOT_TCB")"
[ "$_r" = "$FLOOR_MIN" ] || fail "$LDS compares root's stack against [$_r] and a thread's against
    $FLOOR_MIN; both are KICKOS_MIN_STACK_SIZE"
echo "tls_carve_link: $PRESET idle floor $FLOOR_IDLE, spawn floor $FLOOR_MIN (from $LDS)"

# <label> <target> <cmake -D>... : configures the preset in its own tree and builds one image,
# leaving the build's output in $TMP/<label>.log. The build must fail.
refused_link() {
    _rl_label="$1"
    _rl_target="$2"
    shift 2
    _rl_dir="$TMP/$_rl_label"
    "$CMAKE" -S "$SRC" -B "$_rl_dir" --preset "$PRESET" "$@" > "$TMP/$_rl_label.cfg.log" 2>&1 \
        || { sed -n '1,40p' "$TMP/$_rl_label.cfg.log" >&2
             fail "$_rl_label: configure of $PRESET with $* failed, so no link was asked"; }
    if "$CMAKE" --build "$_rl_dir" --target "$_rl_target" > "$TMP/$_rl_label.log" 2>&1; then
        fail "$_rl_label: $PRESET links $_rl_target with $*, a stack whose carve leaves less than
    its floor above it"
    fi
}

# <label> <message prefix> : the refused build printed that assert's own message.
refused_by() {
    _rb_msg="$(message_of "$LDS" "$2")"
    if ! grep -qF "$_rb_msg" "$TMP/$1.log"; then
        grep -E 'error|Error|ld: ' "$TMP/$1.log" | sed -n '1,20p' >&2
        fail "$1: the link was refused, but not by [$2...]; whatever refused it is not the assert
    this gate is about"
    fi
}

refused_link at_floor hello -DKICKOS_IDLE_STACK_SIZE="$FLOOR_IDLE" \
    -DKICKOS_USER_STACK_SIZE="$FLOOR_MIN" -DKICKOS_ROOT_STACK_SIZE="$FLOOR_MIN"
refused_by at_floor "$MSG_IDLE_TCB"
refused_by at_floor "$MSG_USER_TCB"
refused_by at_floor "$MSG_ROOT_TCB"

refused_link idle_tls tlsprobe -DKICKOS_IDLE_STACK_SIZE="$FLOOR_IDLE"
refused_by idle_tls "$MSG_IDLE_TLS"

echo "PASS: $PRESET refuses to link idle at $FLOOR_IDLE and a user and root stack at $FLOOR_MIN by name, and idle at $FLOOR_IDLE under a thread_local"
