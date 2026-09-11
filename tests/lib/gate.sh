# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Primitives shared by the gate scripts. SOURCED, never executed:
#   . "$(dirname "$0")/../lib/gate.sh"
# POSIX sh (dash-clean), because /bin/sh is dash on the CI images.

# THE LOCALE, FOR EVERY GATE, SET BEFORE ANY OF THEM READS ANYTHING. Two things a gate reads
# stop meaning what it expects under a UTF-8 locale. Binutils, cmake and the compilers
# TRANSLATE their headings and diagnostics, so a host readelf answers `Fichier:` where a member
# slicer keys on `File:` and the slice comes back empty. And awk's length(), substr() and
# index() count CHARACTERS under gawk, so a capture carrying one invalid sequence shifts every
# offset the byte-exact matchers below compute. Collation follows: `sort` order and a `grep`
# range are locale-dependent too.
LC_ALL=C
export LC_ALL

# Every reporter's literal dump marker, as one ERE. Case-sensitive and anchored on the
# banner shape, because a substring match on "fault" also hits "EFAULT" and "default" in
# benign output.
#
# It lives in tests/lib/panic.ere, ONE line, and has two consumers: this file and the root
# CMakeLists, which registers it as a ctest FAIL_REGULAR_EXPRESSION. Read once, and REFUSE an
# empty result: an empty ERE matches nothing, so every panic gate in the suite would silently
# stop failing.
KOS_PANIC_RE="$(cat "$(dirname "$0")/../lib/panic.ere")"
if [ -z "$KOS_PANIC_RE" ]; then
    echo "FAIL: tests/lib/panic.ere is empty or unreadable; every panic gate would pass" >&2
    exit 1
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

# A finding COLLECTED rather than fatal, so one run names them all. It sets the caller's `rc`,
# which the caller declares and exits on. A broken tool takes fail()'s hard exit instead.
bad() { echo "FAIL: $*" >&2; rc=1; }

# A literal tab, for `while IFS="$TAB" read -r ...` over tab-separated records. NOT $'\t':
# that is a bashism, and dash sets IFS to the three characters $ \ t instead, so every field
# splits on those. `dash -n` passes it and a gate whose records mis-split goes VACUOUS rather
# than loud, so only a run under dash shows it.
TAB="$(printf '\t')"

# ONE HANDLER FOR THE WHOLE LIBRARY, because a second `trap ... EXIT` REPLACES the first and
# would drop whatever the earlier owner registered. Two owners register here: the scratch
# directory and a polled image's child.
#
# THE SIGNALS ARE TRAPPED BESIDE EXIT. A ctest TIMEOUT signals the script, and under an
# EXIT-only trap the emulator outlives it and holds the console the next test opens, which
# surfaces as that test failing rather than as this one being killed. Each signal arm exits, and
# the EXIT arm then runs the handler a second time, so the handler clears every record it acts
# on.
KOS_TRASH_DIR=""
KOS_TRASH_FILE=""
KOS_CHILD_PID=""

kos_cleanup() {
    kos_stop_child
    if [ -n "$KOS_TRASH_FILE" ]; then
        _kc_f="$KOS_TRASH_FILE"
        KOS_TRASH_FILE=""
        rm -f "$_kc_f"
    fi
    if [ -n "$KOS_TRASH_DIR" ]; then
        _kc_d="$KOS_TRASH_DIR"
        KOS_TRASH_DIR=""
        rm -rf "$_kc_d"
    fi
}

kos_trap() {
    trap 'kos_cleanup' EXIT
    trap 'kos_cleanup; exit 130' INT
    trap 'kos_cleanup; exit 143' TERM
    trap 'kos_cleanup; exit 129' HUP
}

# STOP THE RECORDED CHILD WITHIN A BOUND. `wait` on a child that ignores SIGTERM never returns,
# so a plain kill-then-wait leaves the advertised timeout unenforced, which is worse than no
# timeout at all because every caller trusts it. SIGTERM first, so the emulator flushes what it
# has written; SIGKILL once the grace runs out, and that one cannot be ignored, so the `wait`
# below it terminates. By RECORDED PID: `pgrep -f` matches this script's own command line, so a
# pattern kill takes the gate down with the emulator.
KOS_STOP_TICKS=15

kos_stop_child() {
    if [ -z "$KOS_CHILD_PID" ]; then
        return
    fi
    _sc="$KOS_CHILD_PID"
    KOS_CHILD_PID=""
    kill "$_sc" 2>/dev/null
    _sc_n=0
    while [ "$_sc_n" -lt "$KOS_STOP_TICKS" ]; do
        if ! kill -0 "$_sc" 2>/dev/null; then
            break
        fi
        sleep 0.2
        _sc_n=$((_sc_n + 1))
    done
    kill -9 "$_sc" 2>/dev/null
    wait "$_sc" 2>/dev/null
}

# TMP: a fresh directory, removed on exit.
scratch_dir() {
    TMP="$(mktemp -d)" || fail "mktemp -d failed"
    KOS_TRASH_DIR="$TMP"
    kos_trap
}

# A tool that produced nothing leaves every grep below it vacuously satisfied.
require_nonempty() {
    if [ ! -s "$1" ]; then
        fail "$2"
    fi
}

# The working directory a source-tree gate needs, which ctest sets through WORKING_DIRECTORY
# and a hand run does not. <prose> names the directory in the refusal for a gate that was
# handed one rather than run in it.
require_repo_root() { # [<prose>]
    _rr="${1:-run from the repo root (see WORKING_DIRECTORY)}"
    [ -f CMakeLists.txt ] || fail "$_rr"
    # `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
    [ -d .git ] || [ -f .git ] || fail "$_rr (no .git here)"
}

# THE CORPUS, AND WHY IT IS ONE FUNCTION. `git ls-files` and not `find`: an untracked scratch
# file is neither gated nor counted, which also means a NEW file is invisible here until it is
# staged. An empty result is refused rather than walked, an empty corpus satisfying every
# absence-assertion below it; a failed `git ls-files` is refused for the stronger reason that
# the tree is then UNKNOWN and not empty.
#
# <what> completes "git ls-files matched no <what>", so it names what the pathspec selects.
corpus() { # <outfile> <what> [pathspec]...
    _co="$1"
    _cw="$2"
    shift 2
    command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"
    git ls-files -- "$@" > "$_co" \
        || fail "git ls-files failed; the corpus is UNKNOWN and not empty"
    require_nonempty "$_co" "git ls-files matched no $_cw, so this gate would pass on any tree
      at all. An untracked file is invisible here (git add first)."
}

# The three pathspecs more than one gate wants, so a new extension reaches every gate at once.
# A gate needing a narrower set spells its own and calls corpus() directly.
corpus_all() { # <outfile>
    corpus "$1" "tracked file"
}

corpus_sources() { # <outfile>
    corpus "$1" "C/C++ or assembler source" \
        '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' '*.S'
}

corpus_headers() { # <outfile>
    corpus "$1" "header" '*.h' '*.hh' '*.hpp'
}

# The same trap one level up: a binutils invocation that FAILED also produces nothing, so
# every absence-assertion reading its output concludes "clean". Route every invocation
# through here. The landmark is a positive control (a section, a symbol shape) that a healthy
# run cannot lack, so a tool that merely emitted a banner is caught too. Exits on the spot,
# and not through fail(), which a gate may legitimately redefine to accumulate.
tool_out() { # <outfile> <landmark-ere, empty for success-only> <tool> <arg>...
    _o="$1"
    _mark="$2"
    shift 2
    # Inside the condition so a `set -e` caller does not abort before $? is read.
    if "$@" > "$_o" 2>"$_o.err"; then
        _rc=0
    else
        _rc=$?
    fi
    if [ "$_rc" -ne 0 ]; then
        echo "FAIL: exit $_rc from: $*" >&2
        sed -n '1,3p' "$_o.err" >&2
        exit 1
    fi
    if [ -n "$_mark" ] && ! grep -qE "$_mark" "$_o"; then
        echo "FAIL: nothing matching /$_mark/ came out of: $*" >&2
        exit 1
    fi
}

# THE OBJDUMP READER'S SCOPE, shared so a listing-shape fix is made once. The reader awk a
# caller passes carries the per-arch mnemonic match and the END verdict, which are bespoke;
# what it does NOT carry is finding the body, and getting that wrong reads as a clean body.
# tests/lib/objdump_scope.awk documents what the reader inherits and why the -f order matters.
#
# Extra `-v` assignments go after the three fixed arguments, before the program files, POSIX
# putting every assignment ahead of the first -f.
KOS_OBJDUMP_SCOPE="$(dirname "$0")/../lib/objdump_scope.awk"

scoped_body() { # <reader.awk> <listing> <symbol> [-v name=value]...
    _sb_prog="$1"
    _sb_list="$2"
    _sb_sym="$3"
    shift 3
    [ -r "$KOS_OBJDUMP_SCOPE" ] || fail "$KOS_OBJDUMP_SCOPE is unreadable, so no reader can
      find a body and every one of them would report the symbol as absent"
    [ -r "$_sb_prog" ] || fail "$_sb_prog is unreadable; the reader has no verdict half"
    awk -v sym="$_sb_sym" "$@" -f "$KOS_OBJDUMP_SCOPE" -f "$_sb_prog" "$_sb_list"
}

# THE DEAD-READER CONTROL, which no planted body can stand in for: a reader handed a symbol
# the listing does not carry must say NOSYM, or a renamed, inlined or static body reads as a
# clean one and the gate goes green on an image it never decoded. <prose> completes "so ...".
ctl_dead_reader() { # <verdict> <prose>
    case "$1" in
        NOSYM) ;;
        *) fail "the reader answered [$1] for a symbol the listing does not carry, so $2" ;;
    esac
}

# The -D arguments an installed KickOS package puts on a consumer's compile line, read back
# off a BUILT out-of-tree app rather than restated. The exported target carries them as
# generator expressions, so the app's own compile_commands.json is the only place they exist
# resolved, and a set written by hand is right for the arch it was written on and wrong
# elsewhere.
#
# PER TRANSLATION UNIT, never a union of the whole corpus: a union can hand
# check_public_headers.sh a combination no single consumer TU ever compiles with, and the
# probe cannot tell that apart from real agreement. Every TU in the corpus must carry the
# SAME set; a disagreement is refused by name (tests/lib/package_defs.py) rather than merged
# or resolved by picking one TU's file, which would hardcode a consumer's layout into a gate
# helper shared by every arch.
package_defs() { # <compile_commands.json> <outfile>
    [ -f "$1" ] || fail "no compile_commands.json at $1, so what a consumer inherits from
      this package cannot be read"
    command -v python3 >/dev/null 2>&1 || fail "python3 not found; package_defs cannot read $1"
    _pd_reader="$(dirname "$0")/../lib/package_defs.py"
    [ -r "$_pd_reader" ] || fail "$_pd_reader is unreadable; the per-TU -D set cannot be read"
    python3 "$_pd_reader" "$1" > "$2" 2>"$2.err" || {
        sed -n '1,20p' "$2.err" >&2
        fail "the out-of-tree corpus's translation units do not agree on their -D set (see
      above); the public-header probe has no honest basis for picking one TU's set over
      another's"
    }
    require_nonempty "$2" "the out-of-tree app's compile line carries no -D at all, so
      either the exported target lost its usage requirements or $1 is not a compile database"
}

# HOW AN IMAGE IS HANDED TO THE EMULATOR, per board, into KOS_BOOT_ARGS as a word list the
# two runners below leave unquoted. The default is `-semihosting -kernel <elf>`.
#
# KICKOS_BOOT=uefi-pe is x86_64, and -kernel cannot start that image at all: firmware loads a
# PE32+ UEFI application, so the image goes into an EFI system partition and boots off the
# removable-media fallback path. Three things are per run. The ESP is rebuilt from the image
# UNDER TEST, because a stale BOOTX64.EFI left in a reused volume boots instead and prints the
# same banner. The variable store is copied, because the shipped one is root-owned and
# read-only and firmware writes to it. And -no-reboot is not cosmetic: an absent or malformed
# interrupt table triple-faults, which RESETS the machine, so without it the run loops instead
# of ending.
#
# The scratch lives beside the image and not under /tmp: it is tens of megabytes, and the
# gates that use it also call scratch_dir(), whose EXIT trap would replace any trap set here.
KOS_OVMF_CODE="${KICKOS_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
KOS_OVMF_VARS="${KICKOS_OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"

boot_args() { # <image>
    if [ "${KICKOS_BOOT:-kernel}" != "uefi-pe" ]; then
        KOS_BOOT_ARGS="-semihosting -kernel $1"
        return
    fi
    for _t in dd mformat mmd mcopy mdir; do
        if ! command -v "$_t" >/dev/null 2>&1; then
            echo "SKIP: $_t not found (Debian: mtools, coreutils)"
            exit 77
        fi
    done
    if [ ! -f "$KOS_OVMF_CODE" ] || [ ! -f "$KOS_OVMF_VARS" ]; then
        echo "SKIP: no UEFI firmware at $KOS_OVMF_CODE / $KOS_OVMF_VARS (Debian: ovmf)"
        exit 77
    fi
    KOS_BOOT_DIR="$1.boot"
    rm -rf "$KOS_BOOT_DIR"
    mkdir -p "$KOS_BOOT_DIR" || fail "cannot create $KOS_BOOT_DIR"
    "$(dirname "$0")/../../tools/esp-x86_64.sh" "$1" "$KOS_BOOT_DIR/esp.img" >/dev/null \
        || fail "could not build the EFI system partition for $1"
    cp "$KOS_OVMF_VARS" "$KOS_BOOT_DIR/vars.fd" || fail "cannot copy $KOS_OVMF_VARS"
    chmod u+w "$KOS_BOOT_DIR/vars.fd" || fail "cannot make $KOS_BOOT_DIR/vars.fd writable"
    KOS_BOOT_ARGS="-m 512 -net none -no-reboot \
-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
-drive format=raw,file=$KOS_BOOT_DIR/esp.img \
-drive if=pflash,format=raw,unit=0,readonly=on,file=$KOS_OVMF_CODE \
-drive if=pflash,format=raw,unit=1,file=$KOS_BOOT_DIR/vars.fd"
}

# The status line an image prints for itself, as a sed BRE with the number in \1. Restated from
# arch/x86/chip/q35/chip_q35.cc on purpose: a parse derived from the emitter would assert
# nothing about it. Anchored, because an unanchored match would also take a line quoting it.
# Deliberately not banner-shaped: tests/static/check_panic_banners.sh reads a `=== x ===`
# literal as a fault reporter's marker and would demand panic.ere match an ordinary exit.
KOS_EXIT_LINE_RE='KICKOS-EXIT status \([0-9][0-9]*\)'

# KOS_STATUS: the image's OWN exit status, out of whatever the machine reports.
#
# THE WHOLE MECHANISM IS GATED ON KICKOS_BOOT=uefi-pe. Only the x86_64 posture has a status
# channel too narrow to carry a byte: isa-debug-exit reports (status << 1) | 1 into an 8-bit
# process exit code, so only 0 through 127 round-trip and 139 arrives as 11. Every other board's
# emulator reports the image's status directly, and below this gate they take the raw status
# untouched.
#
# THE TWO CHANNELS CROSS-CHECK, and neither is trusted alone. On the gated posture the console is
# polled and the image runs an unprivileged thread that can write kernel memory, so a printed
# line is a claim an application could make about itself; the device write is privileged and its
# code comes from the emulator. So the printed value is used only where the device corroborated
# it: the recovery must equal the printed status modulo 128. A printed line with NO device report
# is refused rather than believed, which also catches a run configured without the device.
#
# The timeout code is never overridden: an image killed for making no progress must read as
# killed, and a status line it printed before the kill is exactly what would hide that.
#
# NEITHER CHANNEL MOVES THE RAW STATUS ALONE, and the console must speak first: arch_shutdown
# prints the line before it writes the device, so a device report with no line on the wire is
# not this image exiting. An emptiness test does not catch that: run_image folds QEMU's own
# stderr into this argument, so only a SILENT failure ever had an empty one.
#
# Sets a variable rather than printing: a caller's `$(boot_status ...)` would confine fail()
# to a subshell, so a disagreement would be reported and then discarded.
boot_status() { # <raw status> <output>
    KOS_STATUS="$1"
    if [ "$1" -eq 124 ]; then
        return
    fi
    if [ "${KICKOS_BOOT:-kernel}" != "uefi-pe" ]; then
        return
    fi
    _printed="$(printf '%s\n' "$2" | sed -n "s/^$KOS_EXIT_LINE_RE\$/\1/p" | tail -n1)"
    if [ -z "$_printed" ]; then
        return
    fi
    # The device encodes (status << 1) | 1, so every report it makes is odd.
    if [ $(($1 % 2)) -ne 1 ]; then
        fail "the image printed status $_printed and the exit device reported nothing (raw $1):
  the printed line is not corroborated, so it is not used"
    fi
    _recovered="$((($1 - 1) / 2))"
    if [ "$_recovered" -ne "$((_printed % 128))" ]; then
        fail "the image printed status $_printed, whose low seven bits are $((_printed % 128)),
  but the exit device reported $_recovered (raw $1): one of the two is broken
  (arch_shutdown's status line, or this recovery)"
    fi
    KOS_STATUS="$_printed"
}

# QEMU_BIN: the emulator this board needs. Exit 77 -> CTest SKIP (not PASS), so a
# QEMU-less box cannot green-light a boot gate. Not a command substitution: the exit
# has to leave the SCRIPT, not a subshell.
need_qemu() {
    QEMU_BIN="${QEMU:-qemu-system-arm}"
    if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
        echo "SKIP: $QEMU_BIN not found"
        exit 77
    fi
}

# For a gate that only ever runs under QEMU. kickos_add_qemu_test always exports
# QEMU_MACHINE, so an unset one means the script was run bare, and a per-script default
# would silently target the wrong core.
need_qemu_machine() {
    if [ -z "${QEMU_MACHINE:-}" ]; then
        fail "QEMU_MACHINE is unset; this gate boots on QEMU only"
    fi
}

# OUT: the image's combined output, CR-stripped (a KICKOS_CONSOLE_CRLF board emits CR,
# which defeats end-anchored parses). RC: its exit status, which a pipeline here would
# replace with tr's. QEMU when QEMU_MACHINE is set (kickos_add_qemu_test always exports
# it), native otherwise.
run_image() {
    if [ -n "${QEMU_MACHINE:-}" ]; then
        need_qemu
        boot_args "$1"
        # QEMU_EXTRA and KOS_BOOT_ARGS are word lists (e.g. `-bios none`), so they must split.
        # shellcheck disable=SC2086
        OUT="$(timeout "${QEMU_TIMEOUT:-20}" "$QEMU_BIN" -M "$QEMU_MACHINE" ${QEMU_EXTRA:-} \
                 -nographic ${KOS_BOOT_ARGS} 2>&1)"
    else
        OUT="$(timeout "${SIM_TIMEOUT:-20}" "$1" 2>&1)"
    fi
    RC=$?
    # Every capture-parsing pattern in tests/ rests on this line: the console lowers '\n' to
    # CR+LF on every board but the sim (KICKOS_CONSOLE_CRLF), so the wire carries
    # `ok 149 - amp_window\r\n`, and stripping the CR is what makes a '$' anchor and a
    # whole-line `grep -c` mean what the gate author expects. The break is ASYMMETRIC: GNU
    # grep's '$' does not match before a CR while the ugrep that shadows `grep` on an
    # interactive shell does, so it fails in CI and passes by hand. The bench chain keeps the
    # CR on purpose, a line ending being evidence there (tools/bench/bench-capture.sh).
    OUT="$(printf '%s\n' "$OUT" | tr -d '\r')"
    boot_status "$RC" "$OUT"
    RC="$KOS_STATUS"
    printf '%s\n' "$OUT"
}

# For an app that never terminates on its own: boot it in the background and poll its
# output until EVERY pattern has appeared, then stop it. POLL_OK is 1 when they all
# landed, 0 when the poll ran out or the image died first; OUT carries the whole run
# either way. QEMU_TIMEOUT bounds only the no-progress path, and kos_stop_child bounds the
# stop that follows it. POLL_MS is how long the poll ran for, at the resolution of its own
# tick, and POLL_ALIVE is 0 when the image ended before the poll did: a bound that ran out and
# an image that stopped early are different findings.
#
# KOS_POLL_UNTIL names a shell FUNCTION the poll re-evaluates on every tick beside the
# patterns, satisfied when it returns 0; the poll stops when the patterns AND the function
# are both satisfied. POLL_UNTIL_OK carries its final verdict SEPARATELY from POLL_OK: a
# caller whose bound expires has to say which of the two it was still waiting for, and the
# function is what knows what is outstanding.
#
# A caller's function is called in THIS shell, so what it records stays readable after the
# poll; it must not exit, a poll tick being no place to reach a verdict.
KOS_POLL_UNTIL=""

poll_image() { # <elf> <ere>...
    _elf="$1"
    shift
    _log="$(mktemp)" || fail "mktemp failed"
    KOS_TRASH_FILE="$_log"
    if [ -n "${QEMU_MACHINE:-}" ]; then
        need_qemu
        boot_args "$_elf"
        # QEMU_EXTRA and KOS_BOOT_ARGS are word lists (e.g. `-bios none`), so they must split.
        # shellcheck disable=SC2086
        "$QEMU_BIN" -M "$QEMU_MACHINE" ${QEMU_EXTRA:-} \
            -nographic ${KOS_BOOT_ARGS} >"$_log" 2>&1 &
    else
        "$_elf" >"$_log" 2>&1 &
    fi
    _qpid=$!
    # Recorded and trapped BEFORE the poll runs: a caller's until-function, a signal or a
    # fail() inside the loop all leave this shell without reaching the stop below.
    KOS_CHILD_PID="$_qpid"
    kos_trap
    _n=0
    POLL_ALIVE=1
    while [ "$_n" -lt $(( ${QEMU_TIMEOUT:-8} * 5 )) ]; do   # poll at 5 Hz
        if _poll_matched "$_log" "$@" && _poll_until; then
            break
        fi
        if ! kill -0 "$_qpid" 2>/dev/null; then             # the image exited on its own
            POLL_ALIVE=0
            break
        fi
        sleep 0.2
        _n=$((_n + 1))
    done
    kos_stop_child
    POLL_MS=$((_n * 200))
    # Judged on the FINAL log: an image that exited between the last poll and the
    # liveness check has everything on the wire and must not read as no-progress. Both
    # conditions are evaluated, and not short-circuited: each one's verdict is reported on
    # its own, and the second is what records what it is still short of.
    POLL_OK=0
    if _poll_matched "$_log" "$@"; then
        POLL_OK=1
    fi
    POLL_UNTIL_OK=0
    if _poll_until; then
        POLL_UNTIL_OK=1
    fi
    OUT="$(tr -d '\r' < "$_log")"
    KOS_TRASH_FILE=""
    rm -f "$_log"
    printf '%s\n' "$OUT"
}

_poll_matched() { # <log> <ere>...
    _l="$1"
    shift
    for _p in "$@"; do
        grep -qE "$_p" "$_l" || return 1
    done
    return 0
}

# An unset KOS_POLL_UNTIL is SATISFIED, so a caller that names no function polls on the
# patterns alone.
_poll_until() {
    if [ -z "${KOS_POLL_UNTIL:-}" ]; then
        return 0
    fi
    "$KOS_POLL_UNTIL"
}

# grep OUT as a predicate, with `set -e` kept out of the way.
has() { printf '%s\n' "$OUT" | grep -q "$1"; }
has_e() { printf '%s\n' "$OUT" | grep -qE "$1"; }

# ABOVE ONE CORE THE WIRE HAS NO PER-LINE ATOMICITY: arch_console_write is a byte-at-a-time
# device loop under no lock, so one core's line arrives shuffled INTO another's, character by
# character, and a byte-exact grep stops meaning what its author expects. Measured over 1440
# four-core captures of one image: a third of them carried such a collision.
#
# wire_has answers whether <literal> reached the wire with its own bytes in order, allowing
# foreign bytes among them across a bounded span. The bound is what ONE status line can
# contribute, so a match stitched out of bytes scattered over the capture is still refused.
# WIRE_SPAN carries the span the match occupied, and equals the literal's length when nothing
# was interleaved.
#
# POSITIVE ASSERTIONS ONLY. A literal that must be ABSENT is not made safe by this: a shuffle
# hides it from grep, and answering the reverse question here would take the tolerance for a
# sighting instead. Corroborate an absence against a positive assertion that the same defect
# would also break.
#
# THE SLACK IS ONE FOREIGN LINE'S WORTH OF BYTES, AND WHICH WRITER CONTRIBUTES IT IS NOT FIXED:
# a status line lands inside a thread's line as readily as the reverse, and an app's line is
# routinely the wider of the two. So the slack is read off the capture rather than fixed, and
# KOS_WIRE_SLACK is only the floor under it: the longest status line,
# `# doorbell: <n> core(s) answered, rounds 0x<8 hex>` with its newline.
#
# THE SLACK COMES FROM THE WINDOW THE MATCH SITS IN AND NEVER FROM THE WIDEST LINE OF THE WHOLE
# CAPTURE. One interleave leaves the literal across at most TWO ADJACENT physical lines, because
# the foreign line brings its own newline in with it and that newline is what ends the first of
# the two; a fragment that arrives without one keeps the literal on a single line. So a candidate
# is searched inside a window of one line, or of two adjacent lines, and its bound is that
# window's own widest line. The line that broke the literal is IN the window by construction,
# which is why the tolerance survives; an unrelated wide line elsewhere in the capture no longer
# widens the bound, which is the direction that let a presence check pass on bytes it stitched
# together from somewhere else.
KOS_WIRE_SLACK=64

wire_has() { # <literal>; reads OUT, sets WIRE_SPAN
    WIRE_SPAN="$(printf '%s\n' "$OUT" \
        | KOS_WIRE_PAT="$1" awk -v slack="$KOS_WIRE_SLACK" '
            # In order and nothing more, so a window that cannot carry the literal at all is
            # rejected on the pattern length rather than on the window length.
            function reaches(w, pat,    m, i, j, p) {
                m = length(pat)
                i = 1
                for (j = 1; j <= m; j++) {
                    p = index(substr(w, i), substr(pat, j, 1))
                    if (p == 0) { return 0 }
                    i = i + p
                }
                return 1
            }
            # The SHORTEST span in <w> holding <pat> in order, 0 for none. Each match found
            # left to right is shrunk from its end back to its own latest possible start, so a
            # span that fits the bound is not missed because an earlier start stretched it.
            function minspan(w, pat,    L, m, i, j, k, e, s, best) {
                L = length(w)
                m = length(pat)
                best = 0
                i = 1
                j = 1
                while (i <= L) {
                    if (substr(w, i, 1) == substr(pat, j, 1)) {
                        j++
                        if (j > m) {
                            e = i
                            k = m
                            while (k >= 1) {
                                if (substr(w, i, 1) == substr(pat, k, 1)) { k-- }
                                i--
                            }
                            i++
                            s = e - i + 1
                            if (best == 0 || s < best) { best = s }
                            j = 1
                        }
                    }
                    i++
                }
                return best
            }
            { line[NR] = $0 }
            END {
                pat = ENVIRON["KOS_WIRE_PAT"]
                m = length(pat)
                for (k = 1; k <= NR; k++) {
                    w = line[k]
                    wide = length(line[k])
                    if (k < NR) {
                        w = w "\n" line[k + 1]
                        if (length(line[k + 1]) > wide) { wide = length(line[k + 1]) }
                    }
                    lim = m + slack + 1
                    if (m + wide + 1 > lim) { lim = m + wide + 1 }
                    if (reaches(w, pat) == 0) { continue }
                    s = minspan(w, pat)
                    if (s > 0 && s <= lim) {
                        print s
                        exit
                    }
                }
                print 0
            }')"
    # An awk that produced nothing would leave every caller vacuously satisfied.
    if [ -z "$WIRE_SPAN" ]; then
        fail "wire_has: no answer from awk for: $1"
    fi
    [ "$WIRE_SPAN" -gt 0 ]
}

# How many cores the image reported online, 1 when it reported none. Read from the FIRST
# status line and not the last: the first lands before any peer runs a thread and was intact in
# every one of those captures, where the last, emitted while peers already run, is the one that
# collides.
wire_cores() {
    _wc="$(printf '%s\n' "$OUT" \
        | sed -n 's/^# smp: \([0-9]\{1,\}\) core(s) online$/\1/p' | tail -n1)"
    if [ -z "$_wc" ]; then
        _wc=1
    fi
    printf '%s' "$_wc"
}

# A LITERAL A GATE REQUIRES ON THE WIRE. Strict first, so a one-writer capture stays byte-exact
# and no split goes unreported; above one core a split is tolerated through wire_has alone, so a
# literal absent for any other reason still fails. Every tolerated split is REPORTED.
require_on_wire() { # <literal> <prose>
    require_literal "$1" "the literal required on the wire"
    if printf '%s\n' "$OUT" | grep -qF -- "$1"; then
        return
    fi
    if [ "$(wire_cores)" -le 1 ]; then
        fail "$2"
    fi
    if ! wire_has "$1"; then
        fail "$2"
    fi
    echo "   TOLERATED A SPLIT: \"$1\" reached the wire across $WIRE_SPAN bytes, broken by a
   kernel status line, which two harts on one unlocked device wire may do at any byte"
}

# A WEAK CHECK BY NATURE ABOVE ONE CORE, and it cannot be made otherwise: a shuffle hides text
# from grep, so every way this can be wrong ends in a pass. A caller on a multi-writer posture
# owes it a positive assertion the same panic would also break (an exit status, a line the run
# only reaches by not panicking); one that has none says so in its own header.
assert_no_panic() {
    if has_e "$KOS_PANIC_RE"; then
        fail "$1"
    fi
}

# The faulting address the reporters recorded, as bare hex digits, from whichever spelling
# this backend uses. Every one of them is printed only when the address is live, so a match
# is never a stale register.
reported_fault_addr() {
    printf '%s\n' "$OUT" \
        | sed -n -e 's/.*MMFAR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*BFAR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*ADDR=0x\([0-9a-fA-F]*\).*/\1/p' \
                 -e 's/.*attempted [a-z]* at 0x\([0-9a-fA-F]*\).*/\1/p' \
        | head -n1
}

# The thread-kill dump's banner for a named thread, as an ERE; kernel/init/fault.cc owns
# the wording, and four gates pin it through here.
thread_fault_re() { # <thread-name>
    printf "=== THREAD FAULT === thread '%s' killed" "$1"
}

# WHAT THE RV64 FAULT GATES SHARE. Each caller keeps its own markers, its own scause constant,
# its own address and the prose every refusal here prints.
#
# The image is expected to fault, so an `ERROR:` line is the image failing to arrange the fault
# rather than the fault under test. OUT and RC carry the run, as after run_image.
run_faulting_image() { # <image>
    need_qemu_machine
    run_image "$1"
    if has "ERROR:"; then
        printf '%s\n' "$OUT" | grep 'ERROR:'
        fail "the image reported a failure instead of faulting"
    fi
}

# WHAT THE THREE HELPERS BELOW REFUSE BEFORE THEY ASSERT ANYTHING. `[ "" -ne 139 ]` is an ERROR
# in test(1) and not a false condition, and `grep -F -e ""` matches every line, so an empty
# marker and an empty count each turn a refusal into a pass.
require_number() { # <value> <what>
    case "$1" in
        ''|*[!0-9]*) fail "$2 must be a decimal number, got [$1]" ;;
    esac
}

require_literal() { # <value> <what>
    if [ -z "$1" ]; then
        fail "$2 is empty, so every line of the run would match it"
    fi
}

# KOS_LITERAL_N: OCCURRENCES of a LITERAL in <text>, counted left to right and
# non-overlapping. NOT `grep -c`, which counts the LINES that carry a match: above one core the
# console has no per-line atomicity, so two markers reach the wire on one physical line and a
# line count answers 1 where a reader tallies 2. Every exactly-once assertion built on this
# then passes on a doubled fault, and an interleaved line is precisely how two markers come to
# share one.
#
# An awk that produced nothing would leave the caller judging an empty count, so the answer
# goes through require_number before it is believed.
literal_count() { # <text> <literal>
    require_literal "$2" "the literal to count"
    _lc_n="$(printf '%s\n' "$1" | KOS_COUNT_PAT="$2" awk '
        BEGIN { pat = ENVIRON["KOS_COUNT_PAT"]; m = length(pat); n = 0 }
        {
            s = $0
            p = index(s, pat)
            while (p > 0) {
                n++
                s = substr(s, p + m)
                p = index(s, pat)
            }
        }
        END { print n }')"
    require_number "$_lc_n" "the count of '$2'"
    KOS_LITERAL_N="$_lc_n"
}

# The same count over OUT.
count_literal() { # <literal>
    literal_count "$OUT" "$1"
    KOS_COUNT="$KOS_LITERAL_N"
}

# KOS_FIELD_N: OCCURRENCES of a record `<name>=<value>` in <text>, the value WHOLE. A substring
# count reads `scause=0xdead` as a hit for `scause=0xd` and `ADDR=0x80201000` as a hit for
# `ADDR=0x8020100`. The value ends at the first character that could not continue it, or at end
# of line.
#
# NOT `grep -c`, for the reason literal_count is not: it counts the LINES carrying a match, so
# two records that reached the wire on one physical line are one hit, and an exactly-once
# reading of a field passes on a doubled record. The boundary characters are TESTED and not
# consumed, so two records sharing one separator are both counted.
#
# A name or value carrying anything but an identifier character REFUSES rather than being
# escaped, which is also what makes the record's single `=` the only one: two occurrences of the
# record cannot overlap, so advancing past a hit cannot skip another.
field_count() { # <text> <name> <value>
    require_literal "$2" "the field name"
    require_literal "$3" "the field value"
    case "$2$3" in
        *[!0-9A-Za-z_]*)
            fail "the field record '$2=$3' holds a character this matcher does not model, so
      its count is UNKNOWN and not zero" ;;
    esac
    _fc_n="$(printf '%s\n' "$1" | KOS_FIELD_PAT="$2=$3" awk '
        # index() over a spelled-out set, not a range compare: a range rests on the locale
        # collation, where `_` can sort among the letters. An empty character is not a word
        # character, and index() answers 1 for the empty string, so it is refused first.
        function word(c) {
            if (c == "") { return 0 }
            return index("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_", c)
        }
        BEGIN { pat = ENVIRON["KOS_FIELD_PAT"]; m = length(pat); n = 0 }
        {
            L = length($0)
            at = 1
            p = index(substr($0, at), pat)
            while (p > 0) {
                b = at + p - 1
                bc = ""
                if (b > 1) { bc = substr($0, b - 1, 1) }
                ac = ""
                if (b + m - 1 < L) { ac = substr($0, b + m, 1) }
                if (word(bc) == 0 && word(ac) == 0) { n++ }
                at = b + m
                p = index(substr($0, at), pat)
            }
        }
        END { print n }')"
    require_number "$_fc_n" "the count of '$2=$3'"
    KOS_FIELD_N="$_fc_n"
}

# The same count over OUT.
count_field() { # <name> <value>
    field_count "$OUT" "$1" "$2"
    KOS_COUNT="$KOS_FIELD_N"
}

# The matcher, before it is asked to report an absence. Both directions, because only one of
# them is the defect: a planted record must count ONCE for its own value and NOT AT ALL for a
# value it merely begins with. A prefix matcher passes the first control and fails the second.
# THE OCCURRENCE COUNTER, before an exactly-once refusal rests on it. THE CONTROL IS A
# SAME-LINE DUPLICATE, because that is the reading a line counter gets wrong and an interleaved
# wire is what produces it: three markers over two lines, two of them sharing one line, so a
# counter that answers 2 is counting lines and every doubled fault below it reads as a single.
# The absent marker is the other direction: a counter that reports one is unattributable.
literal_matcher_control() {
    _lmc="=== THREAD FAULT === thread 'a' killed=== THREAD FAULT === thread 'b' killed
=== THREAD FAULT === thread 'c' killed"
    literal_count "$_lmc" "=== THREAD FAULT ==="
    [ "$KOS_LITERAL_N" -eq 3 ] \
        || fail "the literal counter answers $KOS_LITERAL_N for a planted record carrying the
      marker three times over two lines, two of them on ONE line: it counts lines and not
      occurrences, so a doubled fault dump passes as a single one"
    literal_count "$_lmc" "=== BUS FAULT ==="
    [ "$KOS_LITERAL_N" -eq 0 ] \
        || fail "the literal counter reports a marker the planted record does not carry, so
      every count it takes is unattributable"
}

field_matcher_control() {
    _fmc="  PC=0x80001234 scause=0xdead
  ADDR=0x80201000"
    field_count "$_fmc" ADDR 0x80201000
    [ "$KOS_FIELD_N" -eq 1 ] \
        || fail "the field matcher counted $KOS_FIELD_N hit(s) for the address a planted
      record names, so it cannot find the record the assertions below rest on"
    field_count "$_fmc" ADDR 0x8020100
    [ "$KOS_FIELD_N" -eq 0 ] \
        || fail "the field matcher counts a planted ADDR=0x80201000 as a hit for
      ADDR=0x8020100, so a fault at a longer address passes as the address asserted"
    field_count "$_fmc" scause 0xdead
    [ "$KOS_FIELD_N" -eq 1 ] \
        || fail "the field matcher counted $KOS_FIELD_N hit(s) for the cause a planted record
      names"
    field_count "$_fmc" scause 0xd
    [ "$KOS_FIELD_N" -eq 0 ] \
        || fail "the field matcher counts a planted scause=0xdead as a hit for scause=0xd, so
      a fault with a longer cause code passes as the cause asserted"
    field_count "$_fmc" ADDR 0x80201001
    [ "$KOS_FIELD_N" -eq 0 ] \
        || fail "the field matcher reports an address the planted record does not carry"
    # THE SAME-LINE DUPLICATE, and the arm above is the near miss it needs: the planted record
    # there carries the address ONCE, so a matcher that answers 1 to both cannot be told from
    # one that counts occurrences. Three records over two lines, two of them on ONE line, is
    # what a wire with no per-line atomicity delivers and what a line count reads as two.
    _fmc_dup="  ADDR=0x80201000 PC=0x1 ADDR=0x80201000
  ADDR=0x80201000"
    field_count "$_fmc_dup" ADDR 0x80201000
    [ "$KOS_FIELD_N" -eq 3 ] \
        || fail "the field matcher answers $KOS_FIELD_N for a planted record carrying the
      address three times over two lines, two of them on ONE line: it counts lines and not
      occurrences, so a doubled fault record passes as a single one"
    field_count "$_fmc_dup" ADDR 0x8020100
    [ "$KOS_FIELD_N" -eq 0 ] \
        || fail "the field matcher counts a doubled ADDR=0x80201000 as a hit for
      ADDR=0x8020100, so a fault at a longer address passes as the address asserted"
}

# Exactly ONE occurrence of a gate's fault-dump marker. The absence prose is the caller's,
# because what a missing dump means is the whole of what that gate asserts; a repeat prose is
# given where a second dump means something more than a repeated fault.
#
# A REPEAT IS STILL A REPEAT, AND ONLY THE ABSENCE IS A READING A SPLIT EXPLAINS: an interleave
# hides a marker from grep and can never manufacture a second one, so the count above one is
# judged byte-exact and the zero goes through require_on_wire.
#
# Never a control marker: a control's absence and its repetition are two different findings.
# check_aspace_ufault_rv64.sh asserts its control on its own lines.
require_single_marker() { # <marker> <absence-prose> [repeat-prose]
    require_literal "$1" "the fault-dump marker"
    literal_matcher_control
    count_literal "$1"
    if [ "$KOS_COUNT" -gt 1 ]; then
        fail "fault-dump marker '$1' appeared $KOS_COUNT times${3:+: $3}"
    fi
    require_on_wire "$1" "fault-dump marker '$1' missing: $2"
}

# A FAULT RECORD'S FIELD, WHOLE, AND THE ONE READ ABOVE ONE CORE THAT MAY NOT BE TOLERATED.
# require_on_wire answers whether a LITERAL reached the wire and never which VALUE it carried:
# foreign bytes are permitted among the pattern's own, so one digit landing inside a record
# satisfies a shorter value with a longer one, and `ADDR=0x8020100` is already a subsequence of
# `ADDR=0x80201000` with nothing interleaved at all. Tolerating that would credit a fault at
# another address, so the match stays byte-exact, through field_count, which pins the value's
# END. Same reason the realized soak size in check_smp_threads.sh is read strictly.
#
# A LITERAL NOT ON THE WIRE AT ALL AND ONE ONLY A BOUNDED MATCH FINDS ARE REPORTED APART, since
# they send a reader to different places, but the second is NOT resolved further: a record whose
# value is longer and one a peer's line broke into satisfy the bounded match alike, and telling
# those two apart is the very thing a subsequence cannot do. Both are refused, and the record
# <context-ere> selects is printed above the refusal so the reader can see which it was.
require_field_on_wire() { # <name> <value> <context-ere> <prose>
    count_field "$1" "$2"
    if [ "$KOS_COUNT" -gt 0 ]; then
        return
    fi
    if [ -n "$3" ]; then
        printf '%s\n' "$OUT" | grep -E "$3" || :
    fi
    if [ "$(wire_cores)" -gt 1 ]; then
        if wire_has "$1=$2"; then
            fail "$4.
  No record carries '$1=$2' byte-exact and a bounded in-order match finds it across $WIRE_SPAN
  bytes, so either a record names a LONGER value or a peer's line broke into one and left it
  UNREADABLE. A subsequence cannot separate those, and refuses both"
        fi
    fi
    fail "$4"
}

# The three assertions an RV64 fault record carries: the address the caller computed, the cause
# the caller spells out as a scause constant, and the image's exit status. Every refusal prints
# the record's own lines first. Each field is matched WHOLE, through field_count above, whose
# control runs here before any of the three is judged.
#
# scause is the whole of what this architecture publishes about the access: no fault-status
# field and no level field sits beside it (RISC-V Privileged ISA, Supervisor Cause Register).
#
# Both fields go through require_field_on_wire, so above one hart a record a peer's line broke
# into is reported as UNREADABLE rather than as a fault at another address. Neither is tolerated:
# they carry VALUES, and a bounded in-order match cannot say which value a record named.
require_rv64_fault_at() { # <addr-hex> <addr-prose> <scause-hex> <cause-prose> <expect-status>
    require_literal "$1" "the faulting address"
    require_literal "$3" "the scause constant"
    require_number "$5" "the expected exit status"
    require_number "$RC" "the status the run reported"
    field_matcher_control
    require_field_on_wire ADDR "0x$1" 'ADDR=|scause=' \
        "the record faults somewhere other than 0x$1, $2"
    require_field_on_wire scause "$3" 'scause=' "the cause is not $4"
    if [ "$RC" -ne "$5" ]; then
        fail "expected exit $5, got $RC"
    fi
}
