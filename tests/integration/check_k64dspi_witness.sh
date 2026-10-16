#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of the DSPI0 slot witness (user/apps/frdmk64f/k64dspi, witness.cc): a
# transfer halted under it ends -KOS_EIO and the next one completes, a transfer at the slowest
# divider pair completes, then the slot is shut. `close`: the bus close shuts it, so a thread of
# the holder's own task holding no window is killed reading MCR while the holder lives. `death`:
# the holder returns with the bus reopened, and its end shuts it, so main's probe from a task of
# its own is killed. Either way main's probe after the holder's end is killed.
#
#   KOS_CAPTURE=<log> check_k64dspi_witness.sh <board-build> <kickos-source> <cmake> <close|death>

set -u
. "$(dirname "$0")/../lib/gate.sh"

ARM="${4:-}"
case "$ARM" in
    close|death)
        ;;
    *)
        fail "usage: KOS_CAPTURE=<log> check_k64dspi_witness.sh <build> <source> <cmake> <close|death>"
        ;;
esac
judge_capture "k64dspi_$ARM"

if has_e '^\[k64dspi\] (ERROR|FAIL)'; then
    jfail error "the witness reported a failure"
fi
if has_f 'the slot stayed open'; then
    jfail open "a thread holding no window read MCR after the slot should have shut"
fi
jafter transfer 1 '[k64dspi] transfer rc=4 (want 4)' "first transfer"
jafter halted "$AT" '[k64dspi] halted transfer rc=-5 (want -5)' "-KOS_EIO from the halted transfer"
jafter recovered "$AT" '[k64dspi] transfer after the halt rc=4 (want 4)' "transfer after the halt"
jafter slowest "$AT" '[k64dspi] slowest-pair transfer rc=2 (want 2)' "slowest-pair transfer"
jafter close "$AT" '[k64dspi] bus close rc=0 (want 0)' "bus close"
if [ "$ARM" = close ]; then
    jafter announce "$AT" '[k64dspi] after the close, a thread holding no window reads MCR' \
        "announce of the probe after the close"
    if has_f '[k64dspi] the holder outlived its probe'; then
        jfail killed "the probe after the close faulted nothing: its task lived on"
    fi
    require_killed_at k64cprb 0x4002C000
else
    jafter reopen "$AT" '[k64dspi] transfer after the reopen rc=4 (want 4)' "transfer after the reopen"
    jafter returns "$AT" '[k64dspi] the holder returns with the bus open' "the holder's return"
fi
jafter death-announce "$AT" '[k64dspi] the holder ended; a task holding no window reads MCR' \
    "announce of the probe after the holder's end"
_from="$AT"
require_killed_at k64dprb 0x4002C000
jafter verdict "$_from" "[k64dspi] PASS: the probe after the holder's end faulted" "main's verdict"
echo "PASS: k64dspi_$ARM: the halted transfer ended -KOS_EIO and DSPI0's slot shut with its $ARM"
