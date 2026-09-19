#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A commit programs only the descriptors whose words changed, against a RECORD of what the
# hardware holds (`mpu-commit-writes-what-changed` in docs/reference/invariants.md). That is
# sound only while every write to an MPU descriptor goes through the commit that keeps the
# record, or clears it. The failure mode this gate exists for: a NEW site programs or
# suspends a descriptor (a chip bring-up, a power-management resume, a fault path), the
# record still claims the old words, and the next commit SKIPS a descriptor the hardware no
# longer holds. Nothing else in the tree can see it: the record is file-static, every board
# test still passes, and the wrong thread's regions stand until some other slot happens to
# move.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_mpu_record.sh
#
#   corpus     tracked *.c, *.cc, *.cpp, *.h, *.hh, *.hpp, *.inc, *.h.in and *.S. An
#              UNTRACKED file is invisible here, so a new backend must be `git add`ed before
#              this can see it.
#
#   comments   tests/lib/strip_comments.awk blanks `//`, `/* */` and every literal BEFORE the
#              match; this tree's headers and this gate's own prose name the registers. A
#              file whose block comment or literal is still open at EOF is REFUSED by name.
#
#   write      a line that ASSIGNS through the MMIO accessor to an MPU descriptor register,
#              or a `csrw` to a PMP CSR. Naming a register is not writing one: regs.h defines
#              the addresses and arch_mpu_encode_default.cc names the RASR bit fields, and
#              neither moves a descriptor.
#
#   claim 1    every file holding a write keeps the record (`g_mpu_held`), or is named in
#              EXEMPT below with its reason.
#   claim 2    every file naming the record both CLEARS the flag and READS it. A record
#              nothing clears can never be brought back into step with the hardware; a record
#              nothing reads is dead weight pretending to be a safeguard.
#   claim 3    every EXEMPT path is tracked and still holds a write. A stale exemption is a
#              failure, not a quiet pass: it is how a file that stopped writing keeps a
#              waiver a later edit then rides on.
#
# What the scan does NOT reach:
#   - a write through a pointer or a helper of another name (`*p = rasr`), which no backend
#     spells today and which would need this gate taught the indirection.
#   - WHERE in a file the clear sits. Claim 2 says a clear exists, not that it covers every
#     path; the shape of each backend's invalidation is the invariant's to state and the host
#     gate tests/unit/mpuskip's to witness on PMSAv7.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: every finding must be collected in one run.

require_repo_root

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can strip a comment"

# THE REGISTER SET, spelled once. A token missing here takes its backend out of the gate and
# the corpus figure printed at the end goes DOWN by one, which is the only sign.
#   ARM PMSA   MPU_RBAR / MPU_RASR (v7) / MPU_RLAR (v8) / MPU_RNR / MPU_CTRL
#   NXP SYSMPU RGD word registers, the RGDAAC alternate view and CESR (the global enable)
#   Renesas RX RSPAGE / REPAGE / MPOPI (the invalidate-all) / MPEN / MPBAC
#   RISC-V PMP pmpaddr0..n and pmpcfg0..n, written by csrw
REGS='MPU_RBAR|MPU_RASR|MPU_RLAR|MPU_RNR|MPU_CTRL|RGD_WORD[0-9]|RGDAAC[0-9]|sysmpu::CESR|MPU_RSPAGE_BASE|MPU_REPAGE_BASE|MPU_MPOPI|MPU_MPEN|MPU_MPBAC'
# An assignment through one of the two MMIO accessors whose argument names a register above,
# or a CSR write to a PMP entry. A COMPOUND assignment is a write too: a suspend spelled
# `reg32(MPU_CTRL) &= ~MPU_CTRL_ENABLE` moves the same register as a plain store, and reading
# only a bare `=` let exactly that shape pass. `==`, `!=`, `<=` and `>=` stay excluded: the
# operators below are the whole set, each is two characters where a comparison is one, and the
# trailing `[^=]` drops `==` itself.
WRITE_RE="(reg32|reg16|r32|r16)\\([^;]*(${REGS})[^;]*\\)[[:space:]]*([&|^+-]|<<|>>)?=[^=]"
# THE PMP ARM READS THE UNSTRIPPED LINE, and it has to: a `csrw` reaches the assembler inside
# a STRING, which the comment strip blanks along with every other literal, so the stripped
# scan sees an empty inline-asm statement and calls the file a non-writer. Two precise shapes
# rather than a bare `csrw pmp`, so prose naming the instruction is not a finding: the C
# statement that carries one, and a bare instruction opening a line of assembly.
PMP_WRITE_RE='__asm[^;]*csrw[[:space:]]+pmp(addr|cfg)|^[[:space:]]*csrw[[:space:]]+pmp(addr|cfg)'
RECORD_RE='g_mpu_held'
FLAG_RE='g_mpu_held_valid'
CLEAR_RE='g_mpu_held_valid[^;]*=[[:space:]]*false'
# A DECLARATION IS NOT A CLEAR. `static bool g_mpu_held_valid = false;` is the record's own
# definition and says nothing about any path putting it back: without this, a backend whose
# record nothing ever invalidates passes claim 2 on its initialiser alone.
DECL_RE='(^|[^A-Za-z0-9_])bool([^A-Za-z0-9_]|$)'
# A READ is any mention that is neither the flag's DEFINITION nor an assignment to it. Spelled
# as a subtraction rather than as a list of condition shapes, because `not g_mpu_held_valid`,
# `if (g_mpu_held_valid[core])` and `g_mpu_held_valid == false` are all reads and a gate keyed
# on one of them goes red when a backend is rewritten into another.
ASSIGN_RE='g_mpu_held_valid(\[[^]]*\])?[[:space:]]*=[^=]'
DEFN_RE='^[[:space:]]*(static[[:space:]]+)?bool[[:space:]]+g_mpu_held_valid'

# A WRITER THAT KEEPS NO RECORD, each with the reason it is allowed to. Both fields are read:
# a path with no reason is refused by the loader.
EXEMPT_PATHS='arch/riscv/rv32imac/arch_rv32imac.cc arch/riscv/chip/virt_rv64/startup.S'
exempt_reason() { # <path>
    case "$1" in
    arch/riscv/rv32imac/arch_rv32imac.cc)
        echo "PMP stays a TOTAL commit: the eight pmpaddr CSRs are individually addressable, but a per-entry skip spends a load and a taken branch where the write spends a load and a csrw, so it costs more than it saves (measured at M8.11)"
        ;;
    arch/riscv/chip/virt_rv64/startup.S)
        echo "rv64 is a TRANSLATING backend: kickos_arch_mpu_commit is empty and no descriptor set is ever programmed, so this boot PMP write has no record to fall out of step with"
        ;;
    *)
        echo ""
        ;;
    esac
}

# --- the scan ------------------------------------------------------------------------------
# writers <file-list> <workdir>, leaving in <workdir>: `writers` (paths holding a write),
# `records` (paths naming the record), `clears`, `reads`, `refused`, and `seen` (the count of
# register lines read at all, the positive control).
scan() {
    _list="$1"
    _w="$2"
    mkdir -p "$_w" || fail "mkdir failed under $_w"
    for _f in writers records clears reads refused seen; do
        : > "$_w/$_f"
    done

    while IFS= read -r f; do
        [ -f "$f" ] || fail "file in the corpus is missing from the worktree: $f"
        # Pre-filter only: almost nothing in the corpus names an MPU register.
        grep -qE "${REGS}|csrw[[:space:]]+pmp|${RECORD_RE}" "$f" || continue
        if awk -f "$STRIP" "$f" > "$_w/stripped" 2>> "$_w/refused"; then
            :
        else
            _rc=$?
            [ "$_rc" -eq 2 ] || fail "awk exited $_rc stripping $f"
            continue
        fi
        grep -cE "${REGS}|csrw[[:space:]]+pmp" "$_w/stripped" >> "$_w/seen"
        if grep -qE "$WRITE_RE" "$_w/stripped" || grep -qE "$PMP_WRITE_RE" "$f"; then
            printf '%s\n' "$f" >> "$_w/writers"
        fi
        grep -qE "$RECORD_RE" "$_w/stripped" && printf '%s\n' "$f" >> "$_w/records"
        grep -E "$CLEAR_RE" "$_w/stripped" | grep -qvE "$DECL_RE" \
            && printf '%s\n' "$f" >> "$_w/clears"
        grep -E "$FLAG_RE" "$_w/stripped" | grep -vE "$ASSIGN_RE" | grep -vE "$DEFN_RE" \
            | grep -q . && printf '%s\n' "$f" >> "$_w/reads"
    done < "$_list"
}

# --- self-test: prove the scan both ways before reading the tree ---------------------------
mkdir -p "$TMP/st"
cat > "$TMP/st/dirty.cc" <<'EOF'
// A resume path that reprograms a descriptor and tells the record nothing.
void resume(void)
{
    reg32(MPU_RNR) = 3;
    reg32(MPU_RBAR) = saved_base;
    reg32(MPU_RASR) = saved_attr;
}
EOF
cat > "$TMP/st/clean.cc" <<'EOF'
static struct arch_mpu_encoded g_mpu_held;
static bool g_mpu_held_valid = false;
void resume(void)
{
    g_mpu_held_valid = false;
    reg32(MPU_RNR) = 3;
    reg32(MPU_RBAR) = saved_base;
}
void commit(void)
{
    bool const total = not g_mpu_held_valid;
    (void)total;
}
EOF
cat > "$TMP/st/mention.cc" <<'EOF'
// MPU_RASR is the v7 attribute word and reg32(MPU_CTRL) = 0 must never come back here.
char const* s = "reg32(MPU_RBAR) = base";
constexpr uintptr_t MPU_RASR = 0xE000EDA0;
uint32_t rasr_of(uint32_t attr)
{
    if (reg32(MPU_CTRL) == 0) { return 0; }
    return attr | MPU_RASR_ENABLE;
}
EOF
cat > "$TMP/st/compound.cc" <<'EOF'
void suspend(void)
{
    reg32(MPU_CTRL) &= ~MPU_CTRL_ENABLE;
}
EOF
cat > "$TMP/st/cmp.cc" <<'EOF'
bool enabled(void)
{
    if (reg32(MPU_CTRL) >= 1u) { return true; }
    return reg32(MPU_CTRL) != 0u;
}
EOF
cat > "$TMP/st/pmp.cc" <<'EOF'
// A comment naming csrw pmpaddr0 and pmpcfg0 in prose, which is not a write.
void commit(void)
{
    __asm volatile("csrw pmpaddr3, %0" ::"r"(a) : "memory");
}
EOF
cat > "$TMP/st/pmp.S" <<'EOF'
kickos_boot:
    csrw    pmpcfg0, t0
EOF
cat > "$TMP/st/noclear.cc" <<'EOF'
static struct arch_mpu_encoded g_mpu_held;
static bool g_mpu_held_valid = false;
void commit(void)
{
    bool const total = not g_mpu_held_valid;
    if (total) { reg32(MPU_RBAR) = 0; }
    g_mpu_held_valid = true;
}
EOF
cat > "$TMP/st/noread.cc" <<'EOF'
static struct arch_mpu_encoded g_mpu_held;
static bool g_mpu_held_valid = false;
void resume(void)
{
    g_mpu_held_valid = false;
    reg32(MPU_RBAR) = 0;
}
void commit(void)
{
    g_mpu_held.rbar[0] = 1;
    g_mpu_held_valid = true;
}
EOF
for _t in dirty clean mention noclear noread compound cmp; do
    printf '%s\n' "$TMP/st/$_t.cc" > "$TMP/st/$_t.list"
    scan "$TMP/st/$_t.list" "$TMP/st/$_t.w"
done

for _t in pmp.cc pmp.S; do
    printf '%s\n' "$TMP/st/$_t" > "$TMP/st/$_t.list"
    scan "$TMP/st/$_t.list" "$TMP/st/$_t.w"
    [ -s "$TMP/st/$_t.w/writers" ] \
        || fail "the scan missed the planted PMP write in $_t; the strip blanks the string an
      inline csrw rides in, so this arm has to read the raw line"
done

[ -s "$TMP/st/dirty.w/writers" ] || fail "the scan missed a planted descriptor write; it would
      miss a real one and this gate would pass on any tree at all"
[ -s "$TMP/st/dirty.w/records" ] && fail "the scan found a record in a file that plants none"
[ -s "$TMP/st/clean.w/writers" ] || fail "the scan missed the write in the clean twin"
[ -s "$TMP/st/clean.w/records" ] || fail "the scan missed the record in the clean twin"
[ -s "$TMP/st/clean.w/clears" ] || fail "the scan missed the planted clear"
[ -s "$TMP/st/compound.w/writers" ] || fail "the scan missed a COMPOUND assignment to a
      descriptor register; a suspend spelled with &= would leave the record standing and this
      gate would report PASS over it"
[ -s "$TMP/st/cmp.w/writers" ] && fail "the scan read a COMPARISON against a descriptor
      register as a write; widening the operator set past assignment makes every read a finding"
[ -s "$TMP/st/mention.w/writers" ] && fail "the scan read a comment, a literal, an address
      definition or a COMPARISON as a descriptor write; every finding would be noise"
[ -s "$TMP/st/mention.w/seen" ] || fail "the strip ate the mention twin; the negative arm
      above proves nothing"
[ -s "$TMP/st/noread.w/reads" ] && fail "the scan read the record's own definition, or an
      assignment to the flag, as a READ of it: a record whose words are copied and never
      consulted would pass claim 2"
[ -s "$TMP/st/clean.w/reads" ] || fail "the scan missed the planted read"
[ -s "$TMP/st/noclear.w/clears" ] && fail "the scan read the record's own definition, or a
      seat of the flag, as a CLEAR of it: a record nothing ever invalidates would pass
      claim 2 on its initialiser alone"

# --- the corpus ----------------------------------------------------------------------------
corpus "$TMP/all" "tracked source file" \
    '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' '*.S'
N="$(wc -l < "$TMP/all" | tr -d ' ')"

scan "$TMP/all" "$TMP/w"

SEEN="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/w/seen")"
WRITERS="$(wc -l < "$TMP/w/writers" | tr -d ' ')"
echo "== checked $N tracked source file(s); $SEEN MPU register line(s) read, $WRITERS writer(s) =="

[ "$SEEN" -gt 0 ] \
    || fail "not one MPU register line was read across $N file(s); the scan read no code and
      every claim below is vacuous"
[ "$WRITERS" -gt 0 ] \
    || fail "no file in the tree writes an MPU descriptor, which cannot be true while the
      enforcing backends ship; the write shape has moved"

if [ -s "$TMP/w/refused" ]; then
    sort -u "$TMP/w/refused" >&2
    fail "the scan could not strip the file(s) above, so they were never read"
fi

rc=0

# claim 1: a writer keeps the record, or is exempt with a reason.
while IFS= read -r f; do
    grep -qxF "$f" "$TMP/w/records" && continue
    case " $EXEMPT_PATHS " in
    *" $f "*)
        [ -n "$(exempt_reason "$f")" ] \
            || bad "$f is exempt with no reason recorded; an unexplained waiver is how the
      next one gets added"
        continue
        ;;
    esac
    bad "$f writes an MPU descriptor and keeps no record of what the hardware holds. Either
      keep one (see arch/arm/common/arch_arm_mpu_pmsav7.cc), invalidate the backend's
      (g_mpu_held_valid = false) before the write, or add the path to EXEMPT in this gate
      with the reason its backend commits totally."
done < "$TMP/w/writers"

# claim 2: a record is both cleared and read.
while IFS= read -r f; do
    grep -qxF "$f" "$TMP/w/clears" \
        || bad "$f keeps a record nothing ever invalidates. A commit skipping against it can
      never be brought back into step once anything writes a descriptor behind it."
    grep -qxF "$f" "$TMP/w/reads" \
        || bad "$f keeps a record no commit reads, so the words are copied and the writes are
      not skipped: cost with no effect."
done < "$TMP/w/records"

# claim 3 rests on two predicates and each has to be able to REFUSE, or a waiver kept past its
# subject passes on a yes-to-everything answer. The control file is asserted tracked first, so
# the second twin cannot pass by naming a path that has simply been deleted.
grep -qxF 'arch/riscv/rv32imac/no_such_writer.cc' "$TMP/all" \
    && fail "a path that is not in the corpus reads as tracked, so an EXEMPT naming a deleted
      file would pass claim 3 and cover nothing"
grep -qxF 'kernel/time/time.cc' "$TMP/all" \
    || fail "the control file for the twin below is not in the corpus, so that twin proves
      nothing about a non-writer"
grep -qxF 'kernel/time/time.cc' "$TMP/w/writers" \
    && fail "a tracked source that programs no descriptor reads as a writer, so an EXEMPT kept
      past its subject's last write would pass claim 3"

# claim 3: an exemption still describes a writer.
for f in $EXEMPT_PATHS; do
    grep -qxF "$f" "$TMP/all" \
        || bad "EXEMPT names $f, which is not tracked. A waiver over a path that is not in the
      corpus covers nothing and hides the next one."
    grep -qxF "$f" "$TMP/w/writers" \
        || bad "EXEMPT names $f, which no longer writes an MPU descriptor. Drop the entry:
      a waiver kept past its subject is one a later edit rides on."
done

[ "$rc" -eq 0 ] || exit 1
echo "PASS: every MPU descriptor writer keeps a record, invalidates it and reads it"
