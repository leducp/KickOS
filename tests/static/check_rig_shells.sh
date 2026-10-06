#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/bench/rig.sh's fitting list and judge arguments under every shell that sources it: sh,
# bash and zsh, which splits no unquoted parameter. Exits 77, saying so, where zsh is absent,
# with the other shells already held.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0

cat > "$TMP/judge.sh" <<'EOF2'
for a in "$@"; do
    printf '[%s]\n' "$a"
done
printf 'capture=[%s]\n' "$KOS_CAPTURE"
EOF2
cat > "$TMP/drive" <<EOF2
. "$PWD/tools/bench/rig.sh"
eval "RIG_WIRED_\$(printf '%s' "\$1" | tr 'a-z' 'A-Z')=\"\\\$2\""
rig_wired "\$1" || exit \$?
echo ---
rig_judge "\$1" "$TMP/log" "$TMP/build" "$TMP/judge.sh" "\$3"
EOF2

# <shell>: bad for each way rig.sh's lists come out wrong under it.
check_shell() {
    "$1" "$TMP/drive" frdmk64f 'dspi0-loopback' 'a b;;*;' > "$TMP/out.$1" 2>&1 \
        || bad "$1: rig_judge fails: $(cat "$TMP/out.$1")"
    printf '%s\n' dspi0-loopback --- "[$TMP/build]" "[$PWD]" '[cmake]' '[a b]' '[]' '[*]' \
        '[@dspi0-loopback]' "capture=[$TMP/log]" > "$TMP/want"
    cmp -s "$TMP/want" "$TMP/out.$1" \
        || bad "$1: rig_judge passes the judge [$(tr '\n' ' ' < "$TMP/out.$1")], not [$(tr '\n' ' ' < "$TMP/want")]"
    "$1" "$TMP/drive" frdmk64f '' '' > "$TMP/out.$1" 2>&1 || bad "$1: rig_judge fails with no fitting: $(cat "$TMP/out.$1")"
    printf '%s\n' '' --- "[$TMP/build]" "[$PWD]" '[cmake]' "capture=[$TMP/log]" > "$TMP/want"
    cmp -s "$TMP/want" "$TMP/out.$1" \
        || bad "$1: rig_judge with no argument or fitting passes [$(tr '\n' ' ' < "$TMP/out.$1")]"
    "$1" "$TMP/drive" f411disco 'spi1-loopback lan9252' ';x' > "$TMP/out.$1" 2>&1 \
        || bad "$1: rig_judge fails with two fittings: $(cat "$TMP/out.$1")"
    printf '%s\n' 'spi1-loopback lan9252' --- "[$TMP/build]" "[$PWD]" '[cmake]' '[]' '[x]' \
        '[@spi1-loopback]' '[@lan9252]' "capture=[$TMP/log]" > "$TMP/want"
    cmp -s "$TMP/want" "$TMP/out.$1" \
        || bad "$1: rig_judge with two fittings passes [$(tr '\n' ' ' < "$TMP/out.$1")]"
    if "$1" "$TMP/drive" frdmk64f 'lan9252 dspi0-loopback' '' > "$TMP/out.$1" 2>&1; then
        bad "$1: rig_wired accepts two fittings that occupy the same pins"
    fi
    grep -q '^REFUSING: RIG_WIRED_FRDMK64F declares lan9252 dspi0-loopback, ' "$TMP/out.$1" \
        || bad "$1: the exclusive-fitting refusal says: $(cat "$TMP/out.$1")"
}

for shell in sh bash; do
    command -v "$shell" > /dev/null || fail "no $shell on PATH"
    check_shell "$shell"
done
if ! command -v zsh > /dev/null; then
    [ "$rc" -eq 0 ] || exit 1
    echo "SKIPPED: no zsh on PATH, so rig.sh under zsh is not witnessed" >&2
    exit 77
fi
check_shell zsh

[ "$rc" -eq 0 ] || exit 1
echo "PASS: rig.sh lists fittings and judge arguments alike under sh, bash and zsh"
