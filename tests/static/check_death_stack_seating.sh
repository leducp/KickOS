#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The death path runs on the dying thread's own KERNEL BLOCK, seated at its TOP. Every arch
# that carves blocks carries that rule as its own copy of the same code, and this gate is
# what keeps the copies from drifting apart.
#
# Run from the repo root, no arguments: tests/static/check_death_stack_seating.sh
#
# NEITHER OF TWO CLAIMS BELOW HAS A RUNTIME WITNESS, which is why this is a source gate.
# ctx.stack_lo and ctx.stack_hi have no consumer in kernel/ at all and no switch.S guard can
# refuse the rebuilt stub over them; and a missing `return` at the end of the block arm falls
# through and re-runs arch_context_init on the USER stack, so every image still boots and every
# death still completes. The host unit tests cannot cover either: the sim resolves
# KICKOS_KERNEL_STACKS 0 and its struct arch_context has no kernel_sp, stack_lo or stack_hi.
#
# THREE CLAIMS. 1: every backend that defines arch_ctx_redirect carries the block arm, and the
# corpus is not empty. 2: in that arm, the block base is kernel_sp minus
# KICKOS_KERNEL_STACK_SIZE, arch_context_init is handed that base and size with privileged 1,
# stack_lo, stack_hi and kernel_sp are all restored AFTER that call, and the arm RETURNS. 3:
# kickos_fault_stack_top answers with ctx.kernel_sp BEFORE the user-stack fallback.
#
# Comments and literals are blanked before anything is read, so no claim can be met by prose.

set -u
# Findings accumulate over every backend, so one run names all of them.
set -f
. "$(dirname "$0")/../lib/gate.sh"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

export LC_ALL=C
scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
BODY="$(dirname "$0")/fn_body.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"
[ -r "$BODY" ] || fail "tests/static/fn_body.awk is unreadable; no function body can be extracted"

rc=0
bad() { echo "FAIL: $*" >&2; rc=1; }

# The body of <fn> in <file>, comments and literals blanked, as "<line>:<text>" records.
extract() { # <file> <fn> <outfile>
    if ! awk -f "$STRIP" "$1" > "$TMP/stripped" 2> "$TMP/striperr"; then
        sed 's/^/      /' "$TMP/striperr" >&2
        fail "$1: comments and literals could not be blanked, so its verdict is UNKNOWN"
    fi
    if ! awk -v FN="$2" -f "$BODY" "$TMP/stripped" > "$3" 2> "$TMP/bodyerr"; then
        sed 's/^/      /' "$TMP/bodyerr" >&2
        return 1
    fi
    return 0
}

# The records between `#if KICKOS_KERNEL_STACKS` and its matching `#endif`, and NOT past a
# top-level `#else` or `#elif`: what follows one of those is the arm compiled when the knob is
# 0, so folding it in would let the fallback branch satisfy a claim about the block branch.
arm_records() { # <bodyfile> <outfile>
    awk -F: '
        BEGIN { depth = 0; inarm = 0 }
        {
            text = substr($0, index($0, ":") + 1)
            if (inarm) {
                if (text ~ /^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef)/) { depth++ }
                if (depth == 1 && text ~ /^[[:space:]]*#[[:space:]]*(else|elif)/) {
                    inarm = 0
                    next
                }
                if (text ~ /^[[:space:]]*#[[:space:]]*endif/) {
                    depth--
                    if (depth == 0) { inarm = 0; next }
                }
                print
                next
            }
            if (text ~ /^[[:space:]]*#[[:space:]]*if[[:space:]]+KICKOS_KERNEL_STACKS[[:space:]]*$/) {
                inarm = 1
                depth = 1
            }
        }' "$1" > "$2"
}

# Joined into one logical line: the block base and the arch_context_init call each span two
# source lines, so a per-line match would report both absent.
collapse() { # <recordfile>
    awk -F: '{ text = substr($0, index($0, ":") + 1); printf("%s ", text) }' "$1" \
        | tr -s '[:space:]' ' '
}

# The first line number whose text matches <ere>, or empty.
line_of() { # <recordfile> <ere>
    awk -F: -v re="$2" '{ text = substr($0, index($0, ":") + 1); if (text ~ re) { print $1; exit } }' "$1"
}

# An ASSIGNMENT and not a comparison: `=[^=]` refuses `==`, which the bare `=` forms matched
# and which would have let a test of the field stand in for a write to it.
BASE_RE='kernel_sp\)? *- *KICKOS_KERNEL_STACK_SIZE'
INIT_RE='arch_context_init *\([^;]*KICKOS_KERNEL_STACK_SIZE[^;]*, *1 *\)'
LO_RE='ctx->stack_lo *=[^=]'
HI_RE='ctx->stack_hi *=[^=]'
KSP_RE='ctx->kernel_sp *=[^=] *kernel_sp'
RET_RE='^[[:space:]]*return[[:space:]]*;'

# --- self-test: prove the scanner both ways before reading the tree -----------
# A body extractor that returned nothing and an ERE that matched nothing each report every
# backend clean, so both are proven on planted input first.
cat > "$TMP/pos.cc" <<'EOF'
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    uint32_t const kernel_sp = ctx->kernel_sp;
#if KICKOS_KERNEL_STACKS
    if (kernel_sp != 0)
    {
        uint32_t const lo = ctx->stack_lo;
        uint32_t const hi = ctx->stack_hi;
        void* const block = reinterpret_cast<void*>(
            static_cast<uintptr_t>(kernel_sp) - KICKOS_KERNEL_STACK_SIZE);
        arch_context_init(ctx, entry, nullptr, block, KICKOS_KERNEL_STACK_SIZE, 1);
        ctx->stack_lo = lo;
        ctx->stack_hi = hi;
        ctx->kernel_sp = kernel_sp;
        return;
    }
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
    ctx->kernel_sp = kernel_sp;
}
EOF
# The prose twin: every token the claims look for, in comments alone.
cat > "$TMP/neg.cc" <<'EOF'
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    uint32_t const kernel_sp = ctx->kernel_sp;
    /* The block base is kernel_sp - KICKOS_KERNEL_STACK_SIZE and the rebuild runs
       arch_context_init(ctx, entry, nullptr, block, KICKOS_KERNEL_STACK_SIZE, 1),
       after which ctx->stack_lo = lo; ctx->stack_hi = hi; ctx->kernel_sp = kernel_sp;
       and then return; */
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
    ctx->kernel_sp = kernel_sp;
}
EOF
extract "$TMP/pos.cc" arch_ctx_redirect "$TMP/posbody" \
    || fail "the extractor found no arch_ctx_redirect in the planted positive; it would find none in the tree"
arm_records "$TMP/posbody" "$TMP/posarm"
require_nonempty "$TMP/posarm" \
    "the planted block arm came back empty, so every backend would read as carrying none"
POSBLOB="$(collapse "$TMP/posarm")"
for _re in "$BASE_RE" "$INIT_RE" "$LO_RE" "$HI_RE" "$KSP_RE"; do
    printf '%s' "$POSBLOB" | grep -qE "$_re" \
        || fail "the planted positive does not match /$_re/; that claim would pass vacuously on every backend"
done
[ -n "$(line_of "$TMP/posarm" "$RET_RE")" ] \
    || fail "the planted positive shows no return in its block arm; that claim would pass vacuously"

extract "$TMP/neg.cc" arch_ctx_redirect "$TMP/negbody" \
    || fail "the extractor refused the planted prose twin, so a real backend could read as UNKNOWN"
arm_records "$TMP/negbody" "$TMP/negarm"
if [ -s "$TMP/negarm" ]; then
    fail "the prose twin yielded a block arm; comments are being read as code and every claim is satisfiable by a comment"
fi

# --- the corpus ---------------------------------------------------------------
# WHICH ARCHES OWE THE ARM IS DERIVED FROM arch/Kconfig, never listed here. ARCH_SIM and
# ARCH_LX6 define arch_ctx_redirect and select neither kernel-stack symbol, so an arm would be
# dead code there.
ARCHKC=arch/Kconfig
[ -f "$ARCHKC" ] || fail "$ARCHKC is missing; which arches carve blocks cannot be derived"
awk '
    /^config[[:space:]]+ARCH_/ { sym = $2; next }
    /^config[[:space:]]/       { sym = ""; next }
    sym != "" && $1 == "select" && $2 == "ARCH_HAS_KERNEL_STACKS" {
        name = tolower(substr(sym, 6))
        print name
        sym = ""
    }' "$ARCHKC" | sort -u > "$TMP/kstack_arches"
require_nonempty "$TMP/kstack_arches" \
    "$ARCHKC declares no arch selecting ARCH_HAS_KERNEL_STACKS; the symbol was renamed and
      every backend below would be excused"
NARCH="$(wc -l < "$TMP/kstack_arches" | tr -d ' ')"

git ls-files -- 'arch/*.cc' > "$TMP/archsrc" || fail "git ls-files failed"
require_nonempty "$TMP/archsrc" "git ls-files matched no arch source; every check below would pass vacuously"

: > "$TMP/backends"
: > "$TMP/excused"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    # STRIPPED, not raw: enrolment decided off prose would let a file that merely MENTIONS
    # arch_ctx_redirect join the corpus, and one defining it inside a commented-out block leave it.
    awk -f "$STRIP" "$f" > "$TMP/disc" 2>/dev/null || fail "strip failed on $f"
    grep -qE '^[[:space:]]*(void|extern)?[^;]*arch_ctx_redirect[[:space:]]*\(' "$TMP/disc" || continue
    grep -qE 'arch_ctx_redirect[^;]*\)[[:space:]]*;' "$TMP/disc" && continue
    _owes=""
    while IFS= read -r a; do
        case "$f" in
            */"$a"/*) _owes="$a" ;;
        esac
    done < "$TMP/kstack_arches"
    if [ -n "$_owes" ]; then
        printf '%s\t%s\n' "$_owes" "$f" >> "$TMP/backends"
    else
        printf '%s\n' "$f" >> "$TMP/excused"
    fi
done < "$TMP/archsrc"
require_nonempty "$TMP/backends" \
    "no arch source defines arch_ctx_redirect under an arch that carves blocks; the seam was
      renamed, or the arch directory names no longer follow their Kconfig symbols"
NBE="$(wc -l < "$TMP/backends" | tr -d ' ')"

# Every arch that selected the symbol must have been FOUND, or a renamed directory silently
# shrinks the corpus to the ones that still match and the gate still says PASS.
while IFS= read -r a; do
    awk -F"$TAB" -v a="$a" '$1 == a { found = 1 } END { exit !found }' "$TMP/backends" \
        || bad "$ARCHKC declares $a as carving kernel blocks, but no arch/*/$a/*.cc defines
      arch_ctx_redirect; this gate reads nothing for that arch"
done < "$TMP/kstack_arches"

echo "== $NARCH arch(es) carve kernel blocks; $NBE of them define arch_ctx_redirect =="
if [ -s "$TMP/excused" ]; then
    echo "== excused, no block on these arches: $(tr '\n' ' ' < "$TMP/excused")=="
fi

# --- claims 1 and 2, per backend ----------------------------------------------
while IFS="$TAB" read -r a f; do
    if ! extract "$f" arch_ctx_redirect "$TMP/body"; then
        bad "$f: arch_ctx_redirect could not be extracted, so its verdict is UNKNOWN"
        continue
    fi
    arm_records "$TMP/body" "$TMP/arm"
    if [ ! -s "$TMP/arm" ]; then
        bad "$f: arch_ctx_redirect carries no '#if KICKOS_KERNEL_STACKS' arm, so on a board
      that carves blocks its slay stub is rebuilt on the thread's USER stack"
        continue
    fi
    BLOB="$(collapse "$TMP/arm")"
    printf '%s' "$BLOB" | grep -qE "$BASE_RE" \
        || bad "$f: the block arm does not derive its base as kernel_sp - KICKOS_KERNEL_STACK_SIZE"
    printf '%s' "$BLOB" | grep -qE "$INIT_RE" \
        || bad "$f: the block arm does not hand arch_context_init the block size and privileged 1"

    INIT_LN="$(line_of "$TMP/arm" 'arch_context_init')"
    if [ -z "$INIT_LN" ]; then
        bad "$f: the block arm calls no arch_context_init, so nothing seats a frame on the block"
        continue
    fi
    # arch_context_init DERIVES stack_lo and stack_hi from what it is handed and clears
    # kernel_sp, so a restore placed above the call is overwritten by it.
    for _pair in "stack_lo:$LO_RE" "stack_hi:$HI_RE" "kernel_sp:$KSP_RE"; do
        _name="${_pair%%:*}"
        _re="${_pair#*:}"
        _ln="$(line_of "$TMP/arm" "$_re")"
        if [ -z "$_ln" ]; then
            bad "$f: the block arm never restores ctx->$_name, so the TCB describes kernel .bss
      as this thread's stack from the redirect on"
            continue
        fi
        if [ "$_ln" -le "$INIT_LN" ]; then
            bad "$f: ctx->$_name is restored at line $_ln, above the arch_context_init at line
      $INIT_LN, which overwrites it"
        fi
    done

    RET_LN="$(line_of "$TMP/arm" "$RET_RE")"
    if [ -z "$RET_LN" ]; then
        bad "$f: the block arm does not return, so it falls through to the user-stack
      arch_context_init below and the rebuild lands on the USER stack after all"
    elif [ "$RET_LN" -le "$INIT_LN" ]; then
        bad "$f: the block arm returns at line $RET_LN, above its own arch_context_init at
      line $INIT_LN"
    fi
done < "$TMP/backends"

# --- claim 3: the top, and before the fallback --------------------------------
FAULT=kernel/init/fault.cc
[ -f "$FAULT" ] || fail "$FAULT is missing; kickos_fault_stack_top cannot be read"
if ! extract "$FAULT" kickos_fault_stack_top "$TMP/topbody"; then
    fail "$FAULT: kickos_fault_stack_top could not be extracted, so its verdict is UNKNOWN"
fi
arm_records "$TMP/topbody" "$TMP/toparm"
if [ ! -s "$TMP/toparm" ]; then
    bad "$FAULT: kickos_fault_stack_top carries no '#if KICKOS_KERNEL_STACKS' arm, so the
      fault redirect aims every stub at the dying thread's USER stack"
else
    printf '%s' "$(collapse "$TMP/toparm")" | grep -qE 'return[^;]*ctx\.kernel_sp' \
        || bad "$FAULT: the block arm of kickos_fault_stack_top does not answer with
      ctx.kernel_sp, so the stub is not seated at the block top"
    KSP_LN="$(line_of "$TMP/toparm" 'ctx[.]kernel_sp')"
    # The user-stack fallback returns first for any thread that has one, so a block arm
    # placed below it is dead code on every thread the pool seats.
    FALLBACK_LN="$(line_of "$TMP/topbody" 'stack_base')"
    if [ -n "$KSP_LN" ] && [ -n "$FALLBACK_LN" ] && [ "$KSP_LN" -gt "$FALLBACK_LN" ]; then
        bad "$FAULT: the block arm is at line $KSP_LN, below the user-stack fallback at line
      $FALLBACK_LN, so a seated thread never reaches it"
    fi
fi

if [ "$rc" -ne 0 ]; then
    echo "" >&2
    echo "      The death path is the last site where privileged C runs on memory an" >&2
    echo "      unprivileged thread can write. See kernel/init/fault.cc and the four" >&2
    echo "      arch_ctx_redirect bodies." >&2
    exit 1
fi

echo "PASS: $NBE backend(s) seat the slay stub at their block top and keep the user bounds,"
echo "      and kickos_fault_stack_top answers with the block before its user-stack fallback"
