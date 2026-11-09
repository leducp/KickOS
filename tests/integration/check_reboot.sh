#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The shutdown outcomes past halt, riding rebootdemo. Native run for the sim; QEMU when
# QEMU_MACHINE is set; with KOS_CAPTURE a silicon capture.
#
#   check_reboot.sh <rebootdemo.elf> declined|reset|refused
#   KOS_CAPTURE=<log> check_reboot.sh <board-build> <kickos-source> <cmake> reset|bootloader|refused
#
#   declined   both requests come back -KOS_ENOSYS and the app shuts down cleanly.
#   reset      the bootloader request comes back -KOS_ENOSYS and the reset restarts the chip: the
#              image boots again and asks again. In a capture every boot between the first and
#              the last declines and resets, and a complete boot of the same image follows a
#              declining one.
#   bootloader the chip goes to its loader after the countdown: one boot, no decline line, and
#              the device the capture selected for every presence row the board declares reads
#              present once the window is over (<log>.bus, `<vid:pid>|<state>|<identity>|<what>`
#              per row, tools/bench/bench-capture.sh). Only a board whose presence rows leave the
#              bus while it runs (tools/bench/board-rows.sh, board_unseen) has a loader to witness.
#   refused    an AMP node that does not own the chip's reset: both requests come back
#              -KOS_EPERM and the app shuts down cleanly.
#
# A capture's first boot may be cut by the capture's own reset and its last by the window's end,
# so each of those two is read as a prefix of the app's lines. A last line cut before its newline
# is not read at all.
#
# rc=-38 is -KOS_ENOSYS and rc=-1 -KOS_EPERM (system/include/kickos/sys/errno.h).

set -u
. "$(dirname "$0")/../lib/gate.sh"

ANNOUNCE='[rebootdemo] KickOS reboot-to-bootloader demo'
COUNTDOWN='[rebootdemo] handing the chip to its bootloader in 3s'
DECLINED='[rebootdemo] reboot declined: rc=-38'
CLEAN='the app shut down cleanly with status 0'

_usage="usage: check_reboot.sh <rebootdemo.elf> declined|reset|refused
       KOS_CAPTURE=<log> check_reboot.sh <board-build> <kickos-source> <cmake> reset|bootloader|refused"
if judging_capture; then
    build="${1:?$_usage}"
    src="${2:?$_usage}"
    shift 3
    expect="${1:?$_usage}"
    case "$expect" in
        reset | bootloader | refused) ;;
        *) fail "unknown expectation '$expect'" ;;
    esac
    capture_whole "$KOS_CAPTURE"
else
    elf="${1:?$_usage}"
    expect="${2:?$_usage}"
    # A machine that resets loops until this bound: three boots fit inside it.
    : "${QEMU_TIMEOUT:=12}"
    # The machine must be let reset rather than stop on the request (gate.sh, boot_args).
    KOS_ALLOW_RESET=1
    run_image "$elf"
fi

# The first line of OUT carrying <literal>.
first_with() { # <literal>
    printf '%s\n' "$OUT" | grep -m1 -F -- "$1"
}

assert_no_panic "panic/fault during rebootdemo"
if has_f '=== THREAD FAULT ==='; then
    cfail fault "a thread was killed during rebootdemo: $(first_with '=== THREAD FAULT ===')"
fi
if has_f 'arch_reboot: '; then
    cfail fault "a reboot request fell through: $(first_with 'arch_reboot: ')"
fi
has_f "$ANNOUNCE" || cfail announce "the app never announced itself"
has_f "$COUNTDOWN" || cfail countdown "the app never printed its countdown"

if [ "$expect" = refused ]; then
    if judging_capture; then
        n="$(boot_starts | grep -c .)"
        [ "$n" -eq 1 ] || jfail reboot "the image booted $n times on a node that does not own the reset"
    fi
    has_f "reboot declined: rc=-1" || cfail decline "the bootloader request's -KOS_EPERM line is missing"
    has_f "reset declined: rc=-1" || cfail reset "the reset request's -KOS_EPERM line is missing"
    status_clause "$CLEAN" 0
    echo "PASS: both outcomes refused -KOS_EPERM on a node that does not own the reset"
    exit 0
fi

if [ "$expect" = bootloader ]; then
    n="$(boot_starts | grep -c .)"
    [ "$n" -eq 1 ] || jfail reboot "the image booted $n times: the request did not leave the chip in its loader"
    if has_f 'reboot declined'; then
        jfail decline "the bootloader request returned: $(first_with 'reboot declined')"
    fi
    if has_f 'reset declined'; then
        jfail reset "the app went on to ask a reset: $(first_with 'reset declined')"
    fi
    jafter announce 1 "$ANNOUNCE" "announcement"
    jafter order "$AT" "$COUNTDOWN" "countdown after the announcement"
    bus="$KOS_CAPTURE.bus"
    rows=""
    if [ -f "$bus" ]; then
        # `#` lines are a planted record's header.
        rows="$(tr -d '\r' < "$bus" | grep -v -e '^$' -e '^#' || true)"
    fi
    [ -n "$rows" ] || jfail no-bus "no record of the bus after the window at $bus"
    board="$(sed -n 's/^KICKOS_BOARD:[A-Z]*=//p' "$build/CMakeCache.txt" 2>/dev/null)"
    [ -n "$board" ] || jfail no-board "$build/CMakeCache.txt states no KICKOS_BOARD, so no loader is known"
    declared="$(bash -c '. "$1/tools/bench/board-rows.sh" && board_probe_rows "$2"' _ "$src" "$board" \
        2>/dev/null | awk -F '|' 'NF && $2 != "seen"')"
    unseen="$(bash -c '. "$1/tools/bench/board-rows.sh" && board_unseen "$2"' _ "$src" "$board" 2>/dev/null)"
    if [ -z "$declared" ] || [ -z "$unseen" ]; then
        jfail no-loader "tools/bench/board-rows.sh declares no row for $board that leaves the bus while it runs"
    fi
    verdict="$(printf '%s\n' "$rows" | KOS_DECLARED="$declared" awk -F '|' '
        BEGIN {
            n = split(ENVIRON["KOS_DECLARED"], d, "\n")
            for (i = 1; i <= n; i++) {
                split(d[i], f, "|")
                id[i] = f[1]
                kind[f[1]] = f[2]
                what[f[1]] = f[3]
            }
        }
        bad { next }
        !($1 in kind) {
            bad = sprintf("unrelated|[%s] is no presence row the board declares", $0)
            next
        }
        NF != 4 {
            bad = sprintf("format|[%s] is not <vid:pid>|<state>|<identity>|<what>", $0)
            next
        }
        $1 in state {
            bad = sprintf("ambiguous|%s is recorded twice", $1)
            next
        }
        { state[$1] = $2; ident[$1] = $3 }
        END {
            if (bad != "") {
                print bad
                exit
            }
            for (i = 1; i <= n; i++) {
                k = id[i]
                if (!(k in state)) {
                    printf "no-bus|%s (%s) is not recorded\n", what[k], k
                    exit
                }
                by = "port"
                if (kind[k] == "sn") {
                    by = "serial"
                }
                if (index(ident[k], by " ") != 1 || length(ident[k]) <= length(by) + 1) {
                    printf "identity|%s (%s) is recorded as [%s], not by its %s\n", what[k], k, ident[k], by
                    exit
                }
                if (state[k] == "ambiguous") {
                    printf "ambiguous|%s matches more than one %s\n", ident[k], k
                    exit
                }
                if (state[k] != "present") {
                    printf "loader|%s (%s, %s) is %s after the window\n", what[k], k, ident[k], state[k]
                    exit
                }
            }
            print "ok|"
        }')"
    [ -n "$verdict" ] || fail "the bus reader printed no verdict"
    token="${verdict%%|*}"
    [ "$token" = ok ] || jfail "$token" "${verdict#*|}"
    echo "PASS: the countdown was the image's last line and $board is in its loader:" \
        "$(printf '%s\n' "$rows" | cut -d '|' -f 1,3 | paste -sd ' ' -)"
    exit 0
fi

# A capture that ends inside a line has no newline there: that last line is no evidence of anything.
lines="$OUT"
if judging_capture && [ -n "$(tail -c 1 "$KOS_CAPTURE")" ]; then
    lines="$(printf '%s\n' "$OUT" | sed '$d')"
fi

other="$(printf '%s\n' "$lines" | grep -F 'reboot declined: rc=' | grep -m1 -vF -- "$DECLINED" || true)"
[ -z "$other" ] || cfail decline "the bootloader request answered other than -KOS_ENOSYS: $other"
printf '%s\n' "$lines" | grep -qF -- "$DECLINED" \
    || cfail decline "the bootloader request's -KOS_ENOSYS line is missing"

case "$expect" in
    declined)
        status_clause "$CLEAN" 0
        has_f "reset declined: rc=-38" || fail "the reset request's -KOS_ENOSYS line is missing"
        echo "PASS: both outcomes declined with -KOS_ENOSYS and the app shut down cleanly"
        exit 0
        ;;
    reset)
        if has_f "reset declined"; then
            cfail reset "the reset request returned: $(first_with 'reset declined')"
        fi
        ;;
    *)
        fail "unknown expectation '$expect'"
        ;;
esac

if ! judging_capture; then
    boots=$(printf '%s\n' "$OUT" | grep -cF -- "$ANNOUNCE")
    [ "$boots" -ge 2 ] || fail "the image booted $boots time(s): the reset did not restart it"
    echo "PASS: the bootloader declined -KOS_ENOSYS and the reset booted the image $boots times"
    exit 0
fi

# Per boot: A, C and D for the announcement, the countdown and the decline in arrival order, and
# the banner's build, app and commit rows.
verdict="$(printf '%s\n' "$lines" | KOS_A="$ANNOUNCE" KOS_C="$COUNTDOWN" KOS_D="$DECLINED" awk '
    function prefix(s) { return substr("ACD", 1, length(s)) == s }
    /^   KickOS [^ ]+  -  microkernel RTOS$/ || /^K [0-9]/ { n++; seq[n] = ""; next }
    n == 0 { next }
    index($0, ENVIRON["KOS_A"]) == 1 { seq[n] = seq[n] "A"; next }
    index($0, ENVIRON["KOS_C"]) == 1 { seq[n] = seq[n] "C"; next }
    index($0, ENVIRON["KOS_D"]) == 1 { seq[n] = seq[n] "D"; next }
    /^   (build|app|commit) / {
        k = $1
        if (!((n, k) in row)) { row[n, k] = $0 }
        if (!(k in ref)) { ref[k] = $0; refboot[k] = n }
    }
    END {
        for (i = 1; i <= n; i++) {
            s = seq[i]
            if (gsub(/A/, "A", s) > 1) {
                printf "second-boot|boot %d announced itself twice under one banner\n", i
                exit
            }
            if (i == 1 || i == n) {
                if (!prefix(seq[i])) {
                    printf "order|boot %d printed [%s], not the announcement, countdown and decline in order\n", i, seq[i]
                    exit
                }
                continue
            }
            if (index(seq[i], "A") == 0) {
                printf "announce|boot %d of %d never announced itself\n", i, n
                exit
            }
            if (index(seq[i], "C") == 0) {
                printf "countdown|boot %d of %d restarted without its countdown\n", i, n
                exit
            }
            if (index(seq[i], "D") == 0) {
                printf "decline|boot %d of %d restarted without declining the bootloader request\n", i, n
                exit
            }
            if (seq[i] != "ACD") {
                printf "order|boot %d printed [%s], not the announcement, countdown and decline in order\n", i, seq[i]
                exit
            }
        }
        cycle = 0
        for (i = 1; i < n; i++) {
            if (seq[i] == "ACD" && substr(seq[i + 1], 1, 1) == "A") {
                cycle = i
                break
            }
        }
        if (cycle == 0) {
            printf "second-boot|no boot that declined is followed by a boot of the image that reached main (%d boot(s))\n", n
            exit
        }
        for (i = 1; i <= n; i++) {
            if (substr(seq[i], 1, 1) == "A" && !((i, "commit") in row)) {
                printf "same-image|boot %d reached main with no commit row, so nothing says which image it is\n", i
                exit
            }
            for (k in ref) {
                if ((i, k) in row && row[i, k] != ref[k]) {
                    printf "same-image|boot %d is another image: [%s] where boot %d has [%s]\n", i, row[i, k], refboot[k], ref[k]
                    exit
                }
            }
        }
        printf "ok|boot %d declined and reset, boot %d came back, %d boot(s) in all\n", cycle, cycle + 1, n
    }')"
[ -n "$verdict" ] || fail "the boot reader printed no verdict"
token="${verdict%%|*}"
[ "$token" = ok ] || jfail "$token" "${verdict#*|}"
echo "PASS: the bootloader declined -KOS_ENOSYS and the reset booted the image again: ${verdict#*|}"
exit 0
