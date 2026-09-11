#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A console baud divisor must be DERIVED from the rate its port actually landed on, never
# from a frequency the port merely hopes for.
#
# Run from the repo root, no arguments: tests/static/check_chip_divisor_rate.sh
#
# WHY A STATIC GATE AND NOT A TEST. A divisor fixed at one frequency is correct on the happy
# path and silent on every other, and the thing it breaks is the console, the only reporter
# a board has. A host unit test on the divisor formula passes, because the formula was never
# wrong; the defect is in the CALL SITE'S ARGUMENT. The arm that would catch it is a boot with
# the crystal removed, and no emulated board in this tree models a crystal that fails to start.
#
# WHAT IT ASKS. A port that re-derives a rate at runtime is a port with a degrade path. For
# every one of those, every site that commits a console divisor must reach that rate, through
# the value it stores or through a condition that SELECTS between values on it. The reader
# (chip_divisor_rate.py) is a backward slice, so the plausible wrong fixes still redden it: a
# literal lifted into a named constant is still one value, a rate read and then ignored never
# reaches the store, and a constant wrapped in a live guard is the same constant. Neither can a
# comment satisfy it: prose is stripped before anything is read.
#
# SCOPE. Console divisor registers, per the arch_console_reclaim audit in
# docs/reference/invariants.md, plus the helpers that commit one without naming a register.
# OUTSIDE IT: timer and watchdog prescalers, which are written as zero across this fleet and
# whose error is loud rather than silent because time is then wrong everywhere; the RP2xxx XIP
# SPI divisor, whose drift under clk_sys boot2.S documents on purpose; and a port that never
# moves a rate, which has no degrade path for a constant to be wrong on.
#
# THE CORPUS IS THE PORT AND WHAT THE PORT MERGES, not a root this file names: every tracked
# .cc, .c and .h under the port directory, at any depth, plus the family directory the port
# opts into through its own family.cmake. Two boundaries follow, and the first is ENFORCED
# rather than described: every body of arch_console_reclaim in the tree must fall inside that
# corpus, so a reclaim written in a sibling directory reddens here instead of going unread.
# The second is a ruling. A divisor committed under system/driver/ is a USERSPACE driver's,
# whose rate arrives as a request in its cfg and which reads its own clock back; it is not a
# port landing on a rate it may degrade from, which is the whole predicate below.
#
# THE ALLOWLIST IS A DECISION, NOT A DIAL. Each entry names a site whose constant is a fixed
# hardware fact rather than a system clock, and every entry must still match a live site: a
# stale one is a failure, so it cannot outlive the code it was written for.
#
# THE CORPUS COMES FROM `git ls-files`, so an untracked file is invisible: stage before gating.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole fleet, so set -e must stay off.

require_repo_root
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

READER="$(dirname "$0")/chip_divisor_rate.py"
[ -r "$READER" ] || fail "$READER is unreadable; nothing below can classify a divisor"
ALLOW="$(dirname "$0")/chip_divisor_allowlist.txt"
[ -r "$ALLOW" ] || fail "$ALLOW is unreadable; every waived site would read as a finding"

PY=python3
command -v "$PY" >/dev/null 2>&1 || fail "python3 not found; the reader cannot run"

# A fleet below this is a misread, not a small fleet.
PORT_FLOOR=15

scratch_dir

read_port() { # <port-dir> <file>...
    _pd="$1"
    shift
    "$PY" "$READER" "$_pd" "$@"
}

# --- the reader's controls, planted before any tree is read -------------------
# Each arm plants one shape and states what a wrong verdict would cost. A reader that
# classified everything DERIVED, or that could not see a store at all, would report this
# whole fleet clean, so both directions are proven here first.
mkdir -p "$TMP/ctl"

# The port shape every arm below shares: a rate the port moves, so it has a degrade path.
RATE='    uint32_t SystemCoreClock = 8000000u;
    void clock_init(void) { if (pll_ok()) { SystemCoreClock = 72000000u; } }'

cat > "$TMP/ctl/literal.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    void uart_init(void) { reg32(USART_BRR) = 625u; }
EOF

cat > "$TMP/ctl/derived.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    void uart_init(void) { reg32(USART_BRR) = usart_brr(SystemCoreClock, 115200u); }
EOF

# WRONG FIX 1: the literal lifted into a named constant. Reads like a fix and is the same
# number, so a reader keyed on "is there a bare literal here" would call this clean.
cat > "$TMP/ctl/named.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    constexpr uint32_t BRR_115200_72MHZ = 625u;
    void uart_init(void) { reg32(USART_BRR) = BRR_115200_72MHZ; }
EOF

# WRONG FIX 2: the rate read, then ignored. Every token a reader might look for is present
# in the function, and the store still carries a constant.
cat > "$TMP/ctl/ignored.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    void uart_init(void)
    {
        uint32_t const hz = SystemCoreClock;
        (void)hz;
        reg32(USART_BRR) = 625u;
    }
EOF

# The liveness can sit in the GUARD rather than the value, which is how a port that reads its
# clock mux back does it. Refusing this would force a real port to be allowlisted. What makes
# it derived is that the condition SELECTS: the same register takes a different number on each
# arm, so the port is choosing on the rate it landed on rather than merely being enclosed.
cat > "$TMP/ctl/guarded.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    constexpr uint32_t BRR_PLL = 625u;
    constexpr uint32_t BRR_HSI = 69u;
    void uart_init(void)
    {
        if (clk_on_pll())
        {
            reg32(USART_BRR) = BRR_PLL;
        }
        else
        {
            reg32(USART_BRR) = BRR_HSI;
        }
    }
EOF

# WRONG FIX 3: the same one number, WRITTEN INSIDE A GUARD. This is the whole defect wearing
# the previous arm's clothes, and it is what a reader that fell back to any enclosing condition
# accepted: the guard is live, so the store inherits its liveness, and not one bit of the
# number written changes on any path.
cat > "$TMP/ctl/wrapped.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    void uart_init(void)
    {
        if (SystemCoreClock != 0u)
        {
            reg32(USART_BRR) = 625u;
        }
    }
EOF

# WRONG FIX 4: an `else` added under a live condition that stores THE SAME number, which is
# what someone shown finding 3 reaches for. Two branches and one value select nothing.
cat > "$TMP/ctl/fakeelse.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    void uart_init(void)
    {
        if (clk_on_pll())
        {
            reg32(USART_BRR) = 625u;
        }
        else
        {
            reg32(USART_BRR) = 625u;
        }
    }
EOF

# A guard reaches its BLOCK and stops there. The trailing store is outside the if/else and
# carries no condition at all, so a reader that kept the guard past the closing brace would
# hand this one the mux read that governs the two above it.
cat > "$TMP/ctl/leak.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    constexpr uint32_t BRR_PLL = 625u;
    constexpr uint32_t BRR_HSI = 69u;
    void uart_init(void)
    {
        if (clk_on_pll())
        {
            reg32(USART_BRR) = BRR_PLL;
        }
        else
        {
            reg32(USART_BRR) = BRR_HSI;
        }
        reg32(USART_BRR) = 625u;
    }
EOF

# The OTHER selecting spelling. A switch over a clock source is as correct as the if/else
# above, so the case arms have to open branches or a real port would have to be allowlisted.
cat > "$TMP/ctl/switched.cc" <<EOF
$RATE
    constexpr uint32_t USART_BRR = 0x40013808u;
    void uart_init(void)
    {
        switch (clk_source())
        {
            case 0:
            {
                reg32(USART_BRR) = 625u;
                break;
            }
            default:
            {
                reg32(USART_BRR) = 69u;
                break;
            }
        }
    }
EOF

# A port that never moves a rate has no degrade path, so its constant is a fact.
cat > "$TMP/ctl/norate.cc" <<'EOF'
    uint32_t SystemCoreClock = 396000000u;
    constexpr uint32_t USART_BRR = 0x40013808u;
    void uart_init(void) { reg32(USART_BRR) = 625u; }
EOF

# Prose naming the register must not be a site: a gate that reddened on the comment
# explaining the rule is one nobody can keep green honestly.
cat > "$TMP/ctl/prose.cc" <<EOF
$RATE
    // reg32(USART_BRR) = 625u; is what this used to do
    /* another mention of reg32(USART_BRR) = 625u across
       two lines */
    char const* s = "reg32(USART_BRR) = 625u";
EOF

ctl_count() { # <file> <verdict>
    read_port "$TMP/ctl" "$1" | grep -c "^$2 " || true
}

n="$(ctl_count "$TMP/ctl/literal.cc" LITERAL)"
[ "$n" = "1" ] || fail "the reader called a planted constant divisor $n LITERAL site(s)
  instead of 1. It cannot see the defect this gate exists for, so every verdict below it
  is meaningless"

n="$(ctl_count "$TMP/ctl/derived.cc" DERIVED)"
[ "$n" = "1" ] || fail "the reader called a divisor derived from the port's own moved rate
  $n DERIVED site(s) instead of 1. It would redden on correct code, and the only way green
  would be to allowlist every port in the fleet"

n="$(ctl_count "$TMP/ctl/named.cc" LITERAL)"
[ "$n" = "1" ] || fail "the reader accepted a literal divisor that had been lifted into a
  named constant. That is the FIRST thing someone reaches for on being shown this finding,
  and it changes nothing: the store still carries one number for one frequency"

n="$(ctl_count "$TMP/ctl/ignored.cc" LITERAL)"
[ "$n" = "1" ] || fail "the reader accepted a divisor store whose function READS the rate
  and then ignores it. It is matching on the presence of a rate token rather than on the
  value reaching the store, so the fix it would accept is a decoy"

n="$(ctl_count "$TMP/ctl/guarded.cc" DERIVED)"
[ "$n" = "2" ] || fail "the reader called $n of 2 divisor stores DERIVED where the liveness
  is in the enclosing condition rather than in the value. A port that reads its clock mux
  back is correct and would have to be allowlisted, which is how an allowlist stops meaning
  anything"

n="$(ctl_count "$TMP/ctl/wrapped.cc" LITERAL)"
[ "$n" = "1" ] || fail "the reader accepted a constant divisor written inside a live guard.
  A condition that merely encloses a store lends it liveness and changes no bit of the number
  written, so every finding this gate has ever raised could be answered by wrapping it"

n="$(ctl_count "$TMP/ctl/fakeelse.cc" LITERAL)"
[ "$n" = "2" ] || fail "the reader called $n of 2 stores LITERAL where a live condition picks
  between two arms that write THE SAME number. Two branches carrying one value select
  nothing, and this is the shape someone reaches for on being shown the guarded finding"

n="$(read_port "$TMP/ctl" "$TMP/ctl/leak.cc" | grep -c '^LITERAL ' || true)"
[ "$n" = "1" ] || fail "the reader found $n of 1 constant store standing AFTER a selecting
  if/else rather than inside it. A guard that survives its own closing brace hands its mux
  read to every store below it in the function"

n="$(ctl_count "$TMP/ctl/switched.cc" DERIVED)"
[ "$n" = "2" ] || fail "the reader called $n of 2 divisor stores DERIVED where a switch over
  the clock source selects between them. It is the same selection an if/else makes, so a port
  spelling it this way would have to be allowlisted"

n="$(read_port "$TMP/ctl" "$TMP/ctl/norate.cc" | grep -c '^SKIP ' || true)"
[ "$n" = "1" ] || fail "the reader did not skip a port that never moves a rate. Such a port
  has no degrade path, so its constant divisor is a hardware fact and reddening on it would
  make this gate an allowlist of correct code"

n="$(read_port "$TMP/ctl" "$TMP/ctl/prose.cc" | grep -c '^LITERAL ' || true)"
[ "$n" = "0" ] || fail "the reader found $n site(s) in a file whose only mentions of a
  divisor store are inside comments and a string literal. It reads prose as code"

# THE SINK SET IS THE AUDIT'S, and this leg is what makes the citation load-bearing rather
# than decorative. It GREPS PROSE, so rewrapping the audit's divisor list breaks this leg on
# purpose rather than shrinking the set in silence.
"$PY" "$READER" --sinks | sort -u > "$TMP/sinks"
require_nonempty "$TMP/sinks" "the reader answered no sink registers at all, so every store
  in the fleet is invisible to it and the fleet reads clean"
sed -n 's/.*rewrite of the divisor (\([^)]*\)).*/\1/p' docs/reference/invariants.md \
    | tr ',/' '\n\n' | tr -d " \`" | grep -v '^$' | sort -u > "$TMP/audit"
naudit="$(wc -l < "$TMP/audit" | tr -d ' ')"
if [ "$naudit" -lt 8 ]; then
    fail "$naudit divisor register(s) read out of the arch_console_reclaim audit in
  docs/reference/invariants.md, which lists more than that. The sentence moved or was
  rewrapped, so this leg is comparing the reader against nothing"
fi
: > "$TMP/missing"
while IFS= read -r r; do
    [ -n "$r" ] || continue
    grep -Fxq "$r" "$TMP/sinks" || printf '  %s\n' "$r" >> "$TMP/missing"
done < "$TMP/audit"
if [ -s "$TMP/missing" ]; then
    cat "$TMP/missing" >&2
    fail "$(wc -l < "$TMP/missing" | tr -d ' ') divisor register(s) above are named by the
  audit this reader cites and are not in its sink set, so a constant written to one of them is
  not a site and the port carrying it reads clean"
fi

# The sink helpers are named by the reader, so a rename must redden here rather than
# silently shrink what it looks at.
for h in set_baud usart_brr baud_select baud_sbr baud_divider; do
    git grep -l -E "[^A-Za-z0-9_]$h[[:space:]]*\(" 'arch/*/chip/*' > "$TMP/h" 2>/dev/null
    require_nonempty "$TMP/h" "the reader treats '$h' as a divisor helper and no chip port
  mentions it any more. It was renamed or removed, and the reader is now watching a name
  nothing uses while whatever replaced it goes unread"
done

# --- the corpus ---------------------------------------------------------------
tool_out "$TMP/tracked" "" git ls-files 'arch/*/chip/*'
require_nonempty "$TMP/tracked" "git ls-files matched no chip source at all, so the fleet is
  UNKNOWN rather than empty and every verdict below would be vacuous"
sed 's|\(arch/[^/]*/chip/[^/]*\)/.*|\1|' "$TMP/tracked" | sort -u > "$TMP/ports"
nports="$(wc -l < "$TMP/ports" | tr -d ' ')"
if [ "$nports" -lt "$PORT_FLOOR" ]; then
    fail "$nports chip port(s), below the floor of $PORT_FLOOR. A fleet that small is a
  pathspec misread and not a small fleet"
fi

: > "$TMP/verdicts"
: > "$TMP/read_all"
while IFS= read -r d; do
    # A chip that opts into a family unit compiles that unit into ITS OWN archive
    # (family.cmake), so the family's divisor stores are this port's stores. Taking the
    # merge from family.cmake rather than from a list here means a new opt-in is read
    # without this gate being edited.
    fam=""
    if [ -f "$d/family.cmake" ]; then
        fam="$(sed -n 's|.*KICKOS_CHIP_FAMILY_DIR "${CMAKE_CURRENT_LIST_DIR}/\.\./\([^"]*\)".*|\1|p' "$d/family.cmake")"
    fi
    git ls-files "$d/*.cc" "$d/*.c" "$d/*.h" > "$TMP/pf"
    if [ -n "$fam" ]; then
        git ls-files "$(dirname "$d")/$fam/*.cc" "$(dirname "$d")/$fam/*.c" \
                     "$(dirname "$d")/$fam/*.h" >> "$TMP/pf"
    fi
    nf="$(wc -l < "$TMP/pf" | tr -d ' ')"
    if [ "$nf" -lt 1 ]; then
        fail "port $d has no tracked source. It is a directory this gate would report
  clean without opening a single line of it"
    fi
    cat "$TMP/pf" >> "$TMP/read_all"
    # shellcheck disable=SC2046
    read_port "$d" $(cat "$TMP/pf") >> "$TMP/verdicts" \
        || fail "the reader failed on port $d, so that port's verdict is UNKNOWN, not clean"
done < "$TMP/ports"

# THE CORPUS COVERS THE CLASS, and not only the directories this gate happens to walk. Every
# reclaim body rewrites a divisor (the audit in docs/reference/invariants.md holds over all of
# them with no exception), so one living outside the port directories is a divisor site read
# by nothing here, and the gate would report the fleet clean without it.
sort -u "$TMP/read_all" > "$TMP/read_sorted"
git grep -l -E '^void arch_console_reclaim\(void\)[^;]*$' > "$TMP/reclaim" || true
require_nonempty "$TMP/reclaim" "no file in the tree defines arch_console_reclaim. The
  definition was respelled, so the coverage leg below matches nothing and this gate no longer
  knows which files commit a divisor"
: > "$TMP/unread"
while IFS= read -r f; do
    [ -n "$f" ] || continue
    grep -Fxq "$f" "$TMP/read_sorted" || printf '  %s\n' "$f" >> "$TMP/unread"
done < "$TMP/reclaim"
if [ -s "$TMP/unread" ]; then
    cat "$TMP/unread" >&2
    fail "$(wc -l < "$TMP/unread" | tr -d ' ') file(s) above define arch_console_reclaim and
  are outside every port directory this gate reads, so the divisor each of them writes was
  classified by nothing. Move the body into its chip port, or widen the corpus above"
fi

sort -u "$TMP/verdicts" > "$TMP/all"
nderived="$(grep -c '^DERIVED ' "$TMP/all" || true)"
nliteral="$(grep -c '^LITERAL ' "$TMP/all" || true)"

echo "== a console divisor follows the rate its port landed on =="
echo "   fleet: $nports chip port(s); $nderived derived site(s), $nliteral constant site(s)"

# Proves the reader read THIS fleet and not just the planted input: most ports do derive
# their divisor, so a run finding none has read no code.
if [ "$nderived" -lt 1 ]; then
    fail "not one derived divisor site across $nports port(s). Ports in this tree do derive
  them, so a residue of zero means the reader saw no code and every LITERAL count below is
  a count of nothing"
fi

# --- the allowlist ------------------------------------------------------------
grep -v '^[[:space:]]*#' "$ALLOW" | grep -v '^[[:space:]]*$' > "$TMP/allow" || true
grep '^LITERAL ' "$TMP/all" > "$TMP/lits" || true
: > "$TMP/findings"
: > "$TMP/hit_keys"
while IFS= read -r rec; do
    [ -n "$rec" ] || continue
    f="$(printf '%s' "$rec" | awk '{ print $2 }')"
    sink="$(printf '%s' "$rec" | awk '{ print $3 }')"
    key="$(printf '%s' "$f" | sed 's/:[0-9]*$//'):$sink"
    printf '%s\n' "$key" >> "$TMP/hit_keys"
    if grep -Fxq "$key" "$TMP/allow"; then
        continue
    fi
    printf '  %s  %s\n' "$f" "$sink" >> "$TMP/findings"
done < "$TMP/lits"

# A waiver that no longer names a live site is a decision about code that has moved on.
: > "$TMP/stale"
while IFS= read -r key; do
    [ -n "$key" ] || continue
    grep -Fxq "$key" "$TMP/hit_keys" || printf '  %s\n' "$key" >> "$TMP/stale"
done < "$TMP/allow"

if [ -s "$TMP/stale" ]; then
    cat "$TMP/stale" >&2
    fail "$(wc -l < "$TMP/stale" | tr -d ' ') allowlist entry/entries above name no constant
  divisor site any more. The waiver outlived the code it was written for: either the site now
  derives its divisor, in which case delete the entry, or it was renamed and the waiver is
  silently covering whatever took the name"
fi

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    fail "$(wc -l < "$TMP/findings" | tr -d ' ') console divisor site(s) above carry a
  constant in a port that re-derives its rate at runtime. On the rate the constant was
  computed for the console is right, and on every degrade path the board's only reporter
  comes out at a rate nothing programmed, which is the one failure a board cannot tell you
  about. Derive it from the rate the port landed on, or from the register it can read that
  rate back out of. A site whose constant is a fixed hardware fact rather than a system clock
  is a decision to record in $ALLOW with its reason, and one that is neither is a finding
  to raise"
fi

nallow="$(wc -l < "$TMP/allow" | tr -d ' ')"
echo "PASS: every console divisor in $nports chip port(s) follows a rate its port derives at
  runtime, $nderived site(s) directly and $nallow by a recorded waiver over a fixed
  hardware reference"
exit 0
