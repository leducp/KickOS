#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The XMC4800 probes whose stdout is the kernel console by design print through kickos::emit,
# which waits out a full console ring, and never kos::print, which drops the line. The one
# exception is a line carrying `// a dropped beat is the measurement`. A planted copy that
# prints a burst line through kos::print must be refused.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
APPS="user/apps/xmc4800-relax/pvprobe user/apps/xmc4800-relax/xmcspi user/apps/xmc4800-relax/inprstorm"

# <file>...: each kos::print line no exception marks, as <file>:<line>.
dropping() {
    grep -nH 'kos::print(' "$@" | grep -v '// a dropped beat is the measurement$'
}

for app in $APPS; do
    [ -d "$app" ] || fail "no app at $app"
    if dropping "$app"/*.cc > "$TMP/hits"; then
        while read -r hit; do
            bad "$hit prints through kos::print, which drops a line the full console ring cannot take"
        done < "$TMP/hits"
    fi
done

awk '!done && sub(/kickos::emit\("\[inprstorm\] MARKER/, "kos::print(\"[inprstorm] MARKER") {
         done = 1
     }
     { print }' user/apps/xmc4800-relax/inprstorm/main.cc > "$TMP/planted.cc"
cmp -s user/apps/xmc4800-relax/inprstorm/main.cc "$TMP/planted.cc" \
    && fail "the planted kos::print was not written"
dropping "$TMP/planted.cc" > /dev/null || bad "a planted kos::print of a judged line is not refused"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: the kernel-console XMC4800 probes print every judged line through kickos::emit"
