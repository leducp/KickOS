#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the enforced (KICKOS_HAVE_MPU) boot-ctor split. The chip linker
# scripts route ONLY the closed KickOS-owned archive set (libkickos_kernel.a /
# libkickos_arch_<arch>.a / libkickos_chip_<chip>.a / libkickos_lib.a) into
# .init_array (Reset_Handler runs those before kmain) and send every OTHER
# ctor (app / libstdc++ / libsupc++ / newlib / KickCAT) into .kickos_app_init_array,
# which root_entry runs LATER, kernel-live. That set is duplicated across every chip
# linker script in the tree, so a future kernel-side archive whose ctor is NOT added to
# it falls into .kickos_app_init_array and runs too late, leaving kmain to use an
# unconstructed kernel object.
#
# The check works on POINTERS, not symbol addresses: each _GLOBAL__sub_I/_D ctor is
# a function in .text.startup, and the two init_array sections hold POINTERS (thumb bit
# set) to those functions. The pointer words inside each window are what get resolved back
# to the archive their ctor came from.
#
# ZERO CTORS ANYWHERE IS A HEALTHY RESULT: the kernel's static objects are constinit, so
# the closed archive set defines no _GLOBAL__sub_I and both windows are legitimately
# empty. Every count below may be zero, and each assertion engages by itself the day a
# ctor reappears. A missing TOOL RESULT is fatal on the spot: an archive that is not on
# disk, an nm/objcopy that failed, an absent window symbol, or a section whose byte count
# contradicts its window symbols. Those are what catch a renamed archive or section.
#
# THE SELF-TEST DRIVES THE WHOLE JUDGE THROUGH STUB nm/objcopy, so what it does NOT prove is
# the real tools' output SHAPE: a future binutils changing the nm or od column layout leaves
# every control passing, with only the tool_out landmarks and the byte reconciliation left in
# the way.
#
# usage: check_kernel_ctor_placement.sh <elf> <nm> <objcopy> <kernel.a> <arch.a> <chip.a> <lib.a>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

ELF="$1"
NM="$2"
OBJCOPY="$3"
shift 3
# remaining args ($@) are the four kernel-owned archives

scratch_dir

# Bit 0 of a Thumb function pointer, which an init_array word carries and a symbol address
# does not. It is a MASK held in a variable rather than written into even_hex(), so the
# self-test can withdraw it and show that its controls were near misses.
PTR_KEEP=$((~1))

# Every address below is compared as a STRING, so every producer of one goes through
# here: an nm address, an init_array pointer word and a masked ctor address must come out
# in the same format or an assertion compares two spellings and passes vacuously.
even_hex() { # <hex digits>
  printf '%x\n' $((0x$1 & PTR_KEEP))
}

# The symbol nm reports at an address, in even_hex form. nm zero-pads its address column,
# which even_hex does not, so the padding is stripped here rather than at each caller.
name_at() { # <even-hex address>
  awk -v t="$1" '{ a = tolower($1); sub(/^0+/, "", a); if (a == t) { print $3 } }' \
    "$_t/nm_elf" | head -1
}

# The pointer words a ctor array actually holds, thumb bit cleared, sorted unique.
# The byte count is reconciled against the entry count the window symbols imply, because
# objcopy exits 0 and writes ZERO bytes for a section that does not exist: a linker script
# that renamed the OUTPUT section while keeping the symbols would leave every assertion
# below reading an empty file with the entry counts above still looking healthy.
ctor_targets() { # <section> <entries> <outfile>
  "$_objcopy" -O binary --only-section="$1" "$_elf" "$_t/sect.bin" \
    || fail "objcopy could not extract $1"
  NREAD=$(( $(wc -c < "$_t/sect.bin") / 4 ))
  [ "$NREAD" -eq "$2" ] \
    || fail "$1: the window symbols span $2 entr(y/ies) but objcopy read $NREAD: output section renamed or NOBITS"
  od -An -tx4 "$_t/sect.bin" | tr ' ' '\n' | grep -E '^[0-9a-fA-F]{8}$' | while read -r W; do
    even_hex "$W"
  done | sort -u > "$3"
}

judge() { # <elf> <nm> <objcopy> <archive>...
  _elf="$1"
  _nm="$2"
  _objcopy="$3"
  shift 3

  [ -f "$_elf" ] || fail "ELF not found: $_elf"
  # A fresh subdirectory per call, so no file a previous control left behind can be read as
  # this one's corpus.
  _t="$TMP/judge"
  rm -rf "$_t"
  mkdir -p "$_t"

  # Status-checked once, since a pipeline hands back awk's status and every
  # absence-assertion below would read a dead tool as a clean image.
  tool_out "$_t/nm_elf" '^[0-9a-fA-F]+ [A-Za-z] ' "$_nm" "$_elf"

  # --- app-ctor window bounds (the section symbols the split defines) -----------
  START="$(awk '$3=="__kickos_app_init_array_start"{print $1}' "$_t/nm_elf")"
  END="$(  awk '$3=="__kickos_app_init_array_end"  {print $1}' "$_t/nm_elf")"
  if [ -z "$START" ] || [ -z "$END" ]; then
    fail "ELF has no __kickos_app_init_array_{start,end}: not an enforced ELF with the ctor split (wrong target?)"
  fi
  SDEC=$((0x$START))
  EDEC=$((0x$END))
  [ "$EDEC" -ge "$SDEC" ] || fail "app window end (0x$END) is below start (0x$START); corrupt ELF"

  # 4 bytes per pointer: registered on armv7m only (see the CMakeLists guard).
  APP_ENTRIES=$(((EDEC - SDEC) / 4))
  echo "== app-ctor window [0x$START, 0x$END) : $APP_ENTRIES entr(y/ies) =="

  # --- kernel-owned ctor NAMES (from the four closed-set archives) --------------
  : > "$_t/kctors.txt"
  for A in "$@"; do
    [ -f "$A" ] || fail "kernel archive not found: $A"
    # Per-archive, so a tool failure on one archive cannot hide behind the aggregate count.
    tool_out "$_t/karch" '^[0-9a-fA-F]* *[A-Za-z] ' "$_nm" --defined-only "$A"
    awk '$3 ~ /^_GLOBAL__sub_[ID]/ {print $3}' "$_t/karch" >> "$_t/kctors.txt"
  done
  sort -u "$_t/kctors.txt" -o "$_t/kctors.txt"
  KCOUNT=$(wc -l < "$_t/kctors.txt" | tr -d ' ')
  echo "== $KCOUNT kernel-owned global-ctor name(s) across the closed archive set =="

  # --- kernel-owned ctor addresses as they landed in the final ELF --------------
  # (only those still present after --gc-sections matter; mask the thumb bit).
  awk '$3 ~ /^_GLOBAL__sub_[ID]/ {print $1, $3}' "$_t/nm_elf" > "$_t/elf_ctors.txt"
  echo "== $(wc -l < "$_t/elf_ctors.txt" | tr -d ' ') surviving global ctor(s) in the image =="
  while read -r ADDR NAME; do
    printf '%s %s\n' "$(even_hex "$ADDR")" "$NAME"
  done < "$_t/elf_ctors.txt" > "$_t/ctor_even.txt"
  while read -r EVEN NAME; do
    if grep -qxF "$NAME" "$_t/kctors.txt"; then
      printf '%s %s\n' "$EVEN" "$NAME"
    fi
  done < "$_t/ctor_even.txt" > "$_t/kernel_ctor_addrs.txt"

  # ===========================================================================
  # Assertion 1 (PRIVILEGE BOUNDARY): nothing FOREIGN in the privileged window.
  # ===========================================================================
  # The linker partitions by input ARCHIVE, which an app cannot forge: naming a section
  # .init_array.00099 still misses every archive selector. A fifth selector, or a regression
  # to a monolithic `KEEP(*(.init_array))`, is what pulls an app ctor privileged.
  PSTART="$(awk '$3=="__init_array_start"{print $1}' "$_t/nm_elf")"
  PEND="$(  awk '$3=="__init_array_end"  {print $1}' "$_t/nm_elf")"
  if [ -z "$PSTART" ] || [ -z "$PEND" ]; then
    fail "ELF has no __init_array_{start,end}: cannot verify the privileged ctor window"
  fi
  PSDEC=$((0x$PSTART))
  PEDEC=$((0x$PEND))
  [ "$PEDEC" -ge "$PSDEC" ] || fail "privileged window end (0x$PEND) is below start (0x$PSTART)"

  PRIV_ENTRIES=$(((PEDEC - PSDEC) / 4))
  echo "== privileged-ctor window [0x$PSTART, 0x$PEND) : $PRIV_ENTRIES entr(y/ies) =="

  # An empty window is skipped rather than extracted: objcopy writes zero bytes both for an
  # empty section and for a missing one, so extracting it proves nothing and would only trip
  # the decoded-nothing guard below.
  : > "$_t/priv_targets.txt"
  if [ "$PRIV_ENTRIES" -gt 0 ]; then
    ctor_targets .init_array "$PRIV_ENTRIES" "$_t/priv_targets.txt"
    require_nonempty "$_t/priv_targets.txt" \
      ".init_array decoded to zero pointer words although it spans $PRIV_ENTRIES entr(y/ies)"
  fi

  FOREIGN=""
  while read -r TGT; do
    if ! awk -v t="$TGT" '$1==t{found=1} END{exit !found}' "$_t/kernel_ctor_addrs.txt"; then
      # POSIX awk only: strtonum/and/compl are gawk, and a green run never executes this line.
      NAME="$(awk -v t="$TGT" '$1==t{print $2}' "$_t/ctor_even.txt" | head -1)"
      [ -n "$NAME" ] || NAME="$(name_at "$TGT")"
      [ -n "$NAME" ] || NAME="<unresolved>"
      FOREIGN="$FOREIGN
  $NAME (0x$TGT)"
    fi
  done < "$_t/priv_targets.txt"

  if [ -n "$FOREIGN" ]; then
    echo "FAIL: non-kernel ctor(s) landed in the PRIVILEGED .init_array:" >&2
    echo "      that window runs from Reset_Handler with full privilege, before kmain," >&2
    echo "      so these entries are inside the TCB. Check that the chip linker script" >&2
    echo "      still partitions .init_array by ARCHIVE and has not regressed to a" >&2
    echo "      monolithic KEEP(*(.init_array)).$FOREIGN" >&2
    exit 1
  fi
  echo "PASS: privileged ctor window holds only closed-set kernel ctors"

  # ===========================================================================
  # Assertion 2 (ORDERING): no kernel ctor in the late app window.
  # ===========================================================================
  # The app window is EMPTY on every board wired to this gate, --gc-sections dropping every
  # app ctor and the kernel's statics being constinit; assertion 3 is what keeps that from
  # passing vacuously.
  : > "$_t/app_targets.txt"
  if [ "$APP_ENTRIES" -gt 0 ]; then
    ctor_targets .kickos_app_init_array "$APP_ENTRIES" "$_t/app_targets.txt"
    require_nonempty "$_t/app_targets.txt" \
      ".kickos_app_init_array decoded to zero pointer words although it spans $APP_ENTRIES entr(y/ies)"
  fi

  LEAK=""
  while read -r ADDR NAME; do
    if grep -qxF "$ADDR" "$_t/app_targets.txt"; then
      LEAK="$LEAK
  $NAME (0x$ADDR)"
    fi
  done < "$_t/kernel_ctor_addrs.txt"

  if [ -n "$LEAK" ]; then
    echo "FAIL: kernel-owned global ctor(s) landed in .kickos_app_init_array:" >&2
    echo "      these run in root_entry AFTER kmain, so kmain uses an unconstructed object." >&2
    echo "      Add the offending archive to the .init_array closed set in EVERY chip linker script.$LEAK" >&2
    exit 1
  fi

  echo "PASS: no kernel-owned ctor is in the app-ctor window ($APP_ENTRIES entr(y/ies))"

  # ===========================================================================
  # Assertion 3 (REACHABILITY): every surviving ctor is in one of the two windows.
  # ===========================================================================
  # --gc-sections keeps a _GLOBAL__sub_I only because a KEEP'd array entry roots it, so one
  # surviving in NEITHER array will never run.
  ORPHAN=""
  while read -r ADDR NAME; do
    EVEN="$(even_hex "$ADDR")"
    if grep -qxF "$EVEN" "$_t/priv_targets.txt"; then
      continue
    fi
    if grep -qxF "$EVEN" "$_t/app_targets.txt"; then
      continue
    fi
    ORPHAN="$ORPHAN
  $NAME (0x$ADDR)"
  done < "$_t/elf_ctors.txt"

  if [ -n "$ORPHAN" ]; then
    echo "FAIL: global ctor(s) survive in the image but are in NEITHER init array:" >&2
    echo "      nothing will ever run them. A third .init_array-like bucket in the chip" >&2
    echo "      linker script, or an entry dropped while its code was kept.$ORPHAN" >&2
    exit 1
  fi

  echo "PASS: all $(wc -l < "$_t/elf_ctors.txt" | tr -d ' ') surviving ctor(s) are reachable from one of the two windows"
}

# --- self-test: one control per clause, each a minimal pair -------------------
# The stubs answer for the real tools. `nm <file>` prints <file>.nm, and objcopy writes
# <elf><section>.bin, which is objcopy's own behaviour for a section that does not exist:
# exit 0 and zero bytes. A section name starts with a dot, so `.init_array` beside an ELF
# called `elf` is `elf.init_array.bin`.
CTL="$TMP/ctl"
mkdir -p "$CTL/bin"
cat > "$CTL/bin/nm" <<'STUB'
#!/bin/sh
f=""
for a in "$@"; do
    case "$a" in
        -*) ;;
        *) f="$a" ;;
    esac
done
[ -f "$f.nm" ] || exit 0
cat "$f.nm"
STUB
cat > "$CTL/bin/objcopy" <<'STUB'
#!/bin/sh
sect=""
w1=""
w2=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --only-section=*) sect="${1#--only-section=}" ;;
        -O) shift ;;
        -*) ;;
        *) if [ -z "$w1" ]; then w1="$1"; else w2="$1"; fi ;;
    esac
    shift
done
if [ -f "$w1$sect.bin" ]; then
    cat "$w1$sect.bin" > "$w2"
else
    : > "$w2"
fi
STUB
printf '#!/bin/sh\nexit 4\n' > "$CTL/bin/dead"
chmod +x "$CTL/bin/nm" "$CTL/bin/objcopy" "$CTL/bin/dead"

# Four raw bytes, little endian, which is what od -An -tx4 reads back as one word. Every
# address planted below has a nonzero byte in every position, so no NUL is written and no
# shell's printf has to be trusted with one.
le32() { # <8 hex digits>
    _oct="$(printf '\\%03o\\%03o\\%03o\\%03o' \
        "$(( (0x$1) % 256 ))" "$(( ((0x$1) / 256) % 256 ))" \
        "$(( ((0x$1) / 65536) % 256 ))" "$(( ((0x$1) / 16777216) % 256 ))")"
    printf "$_oct"
}

# The clean image: two kernel ctors in the privileged window, one app ctor in the app window,
# every pointer carrying the thumb bit and every symbol address even.
K1=08111234
K2=08111334
APP=08111434
world() { # <dir>
    _cw="$1"
    rm -rf "$_cw"
    mkdir -p "$_cw"
    : > "$_cw/elf"
    {
        printf '08110000 T Reset_Handler\n'
        printf '%s t _GLOBAL__sub_I_kernel_thing\n' "$K1"
        printf '%s t _GLOBAL__sub_I_arch_thing\n' "$K2"
        printf '%s t _GLOBAL__sub_I_app_thing\n' "$APP"
        printf '20001100 D __init_array_start\n'
        printf '20001108 D __init_array_end\n'
        printf '20001200 D __kickos_app_init_array_start\n'
        printf '20001204 D __kickos_app_init_array_end\n'
    } > "$_cw/elf.nm"
    for _a in k arch chip lib; do
        : > "$_cw/$_a.a"
        printf '\n%s.a(one.obj):\n00000000 T kos_%s_text\n         U kos_extern\n' \
            "$_a" "$_a" > "$_cw/$_a.a.nm"
    done
    printf '%s t _GLOBAL__sub_I_kernel_thing\n' "$K1" >> "$_cw/k.a.nm"
    printf '%s t _GLOBAL__sub_I_arch_thing\n' "$K2" >> "$_cw/arch.a.nm"
    { le32 08111235; le32 08111335; } > "$_cw/elf.init_array.bin"
    le32 08111435 > "$_cw/elf.kickos_app_init_array.bin"
}

CW="$TMP/world"
call() { # runs the judge over the planted world; output in $CTL/out
    ( judge "$CW/elf" "$CTL/bin/nm" "$CTL/bin/objcopy" \
        "$CW/k.a" "$CW/arch.a" "$CW/chip.a" "$CW/lib.a" ) > "$CTL/out" 2>&1
}
FIRED=0
ctl() { # <label> <expect-ere>
    if call; then
        cat "$CTL/out"
        fail "positive control '$1' passed, so the assertion it plants for fires on nothing"
    fi
    grep -qE "$2" "$CTL/out" || {
        cat "$CTL/out" >&2
        fail "positive control '$1' reddened for the wrong reason, expected /$2/. A control
      another assertion catches proves nothing about the one it plants for"
    }
    FIRED=$((FIRED + 1))
}
QUIET=0
neg() { # <label>
    if call; then
        QUIET=$((QUIET + 1))
    else
        cat "$CTL/out" >&2
        fail "negative control '$1' reports; the gate would redden a correctly split image"
    fi
}

# The clean image, and the image with NO ctor at all, which the header calls a healthy result.
world "$CW"; neg clean
world "$CW"
{
    printf '08110000 T Reset_Handler\n'
    printf '20001100 D __init_array_start\n'
    printf '20001100 D __init_array_end\n'
    printf '20001200 D __kickos_app_init_array_start\n'
    printf '20001200 D __kickos_app_init_array_end\n'
} > "$CW/elf.nm"
rm -f "$CW/elf.init_array.bin" "$CW/elf.kickos_app_init_array.bin"
neg no_ctor_at_all
# A ctor name an archive defines that --gc-sections dropped from the image: named in the
# closed set, absent from every window, and not a finding.
world "$CW"; printf '00000010 t _GLOBAL__sub_I_dropped\n' >> "$CW/k.a.nm"
neg gc_dropped_ctor
[ "$QUIET" -eq 3 ] || fail "$QUIET of 3 negative controls ran silent"

# Assertion 1: a foreign pointer in the privileged window. The privileged window is widened to
# hold it, so no surviving ctor becomes unreachable and assertion 3 stays silent.
world "$CW"
edit_nm() { # <sed-script>
    sed "$1" "$CW/elf.nm" > "$CW/elf.nm.new"
    mv "$CW/elf.nm.new" "$CW/elf.nm"
}
edit_nm 's/^20001108 D __init_array_end$/2000110c D __init_array_end/'
{ le32 08111235; le32 08111335; le32 08111435; } > "$CW/elf.init_array.bin"
ctl foreign_in_privileged 'non-kernel ctor\(s\) landed in the PRIVILEGED'

# Assertion 2: a kernel ctor in the late app window.
world "$CW"
edit_nm 's/^20001204 D __kickos_app_init_array_end$/20001208 D __kickos_app_init_array_end/'
{ le32 08111235; le32 08111435; } > "$CW/elf.kickos_app_init_array.bin"
ctl kernel_ctor_in_app_window 'kernel-owned global ctor\(s\) landed in .kickos_app_init_array'

# Assertion 3: a surviving ctor no window points at.
world "$CW"; printf '08111534 t _GLOBAL__sub_I_orphan\n' >> "$CW/elf.nm"
ctl orphan_ctor 'survive in the image but are in NEITHER init array'

# The window symbols, both windows, both directions.
world "$CW"; edit_nm '/__kickos_app_init_array_start/d'
ctl app_window_absent 'has no __kickos_app_init_array_\{start,end\}'
world "$CW"; edit_nm '/__init_array_start/d'
ctl priv_window_absent 'has no __init_array_\{start,end\}'
world "$CW"; edit_nm 's/^20001204 D __kickos_app_init_array_end$/200011f0 D __kickos_app_init_array_end/'
ctl app_window_inverted 'app window end .* is below start'
world "$CW"; edit_nm 's/^20001108 D __init_array_end$/200010f0 D __init_array_end/'
ctl priv_window_inverted 'privileged window end .* is below start'

# The byte reconciliation: a renamed OUTPUT section leaves objcopy exiting 0 with no bytes.
world "$CW"; rm -f "$CW/elf.init_array.bin"
ctl section_renamed 'the window symbols span 2 entr\(y/ies\) but objcopy read 0'
world "$CW"; le32 08111235 > "$CW/elf.init_array.bin"
ctl section_short 'the window symbols span 2 entr\(y/ies\) but objcopy read 1'

# The tools themselves. A dead nm, an nm that prints nothing a symbol table can be read out
# of, a dead objcopy, and a missing input, each named rather than read as a clean image.
world "$CW"
if ( judge "$CW/elf" "$CTL/bin/dead" "$CTL/bin/objcopy" "$CW/k.a" "$CW/arch.a" "$CW/chip.a" "$CW/lib.a" ) > "$CTL/out" 2>&1; then
    fail "an nm that exits nonzero read the whole image and said nothing"
fi
grep -q '^FAIL: exit 4 from: ' "$CTL/out" || {
    cat "$CTL/out" >&2
    fail "a dead nm reddened the gate without naming the tool failure"
}
FIRED=$((FIRED + 1))

world "$CW"; : > "$CW/elf.nm"
ctl nm_says_nothing 'nothing matching /\^\[0-9a-fA-F\]\+ \[A-Za-z\] / came out of'

world "$CW"
if ( judge "$CW/elf" "$CTL/bin/nm" "$CTL/bin/dead" "$CW/k.a" "$CW/arch.a" "$CW/chip.a" "$CW/lib.a" ) > "$CTL/out" 2>&1; then
    fail "an objcopy that exits nonzero read the whole image and said nothing"
fi
grep -q 'objcopy could not extract .init_array' "$CTL/out" || {
    cat "$CTL/out" >&2
    fail "a dead objcopy reddened the gate without naming the extraction"
}
FIRED=$((FIRED + 1))

world "$CW"; rm -f "$CW/chip.a"
ctl archive_absent 'kernel archive not found'
world "$CW"; rm -f "$CW/elf"
ctl elf_absent 'ELF not found'
[ "$FIRED" -eq 14 ] || fail "$FIRED of 14 positive controls reddened"

# The mask withdrawn: an init_array word carries the thumb bit and a symbol address does not,
# so with bit 0 kept in the comparison NOTHING matches and the clean image reports every
# privileged entry as foreign.
world "$CW"
if ( PTR_KEEP=-1
     judge "$CW/elf" "$CTL/bin/nm" "$CTL/bin/objcopy" \
        "$CW/k.a" "$CW/arch.a" "$CW/chip.a" "$CW/lib.a" ) > "$CTL/out" 2>&1; then
    fail "with the thumb bit kept in every comparison the clean image still passed, so the
      masking is not what makes the pointer and the symbol meet"
fi
grep -qE 'non-kernel ctor\(s\) landed in the PRIVILEGED' "$CTL/out" || {
    cat "$CTL/out" >&2
    fail "withdrawing the thumb-bit mask reddened something other than assertion 1"
}

# --- the image ----------------------------------------------------------------
judge "$ELF" "$NM" "$OBJCOPY" "$@"
