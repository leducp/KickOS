#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NO KERNEL-TEXT INSTRUCTION NAMES AN APP-HALF SYMBOL, bar a named allowlist
# (docs/design-m6-mmu.md R2.2). The app's window is one level-2 slot of a PER-SPACE table, so
# an app-half address does not name one process: it names whichever process the core is on. A
# kernel store meant for process A therefore lands in process B, and nothing reports it.
#
# WHY THE LINK DOES NOT ALREADY REFUSE THIS, which is the finding this gate exists for. The
# linker script asserts the two halves are out of each other's medany reach, and that turns a
# PC-relative crossing into an auipc truncation. It says NOTHING about the absolute form: the
# app window is at 0x40000000, which is inside medlow's `lui`-reachable range, so the linker
# RELAXES a kernel reference to an app-half symbol into `lui`+`addi` and the link succeeds.
# Measured: a direct read and a direct write of `kickos_init_args` added inside a syscall both
# SUCCEED at run time, because sstatus.SUM is set for the life of kernel context and the
# running space maps the app's half U. No link error, no fault, no arm.
#
# THE CORPUS IS THE KERNEL-SIDE ARCHIVES' RELOCATIONS, NOT THE LINKED IMAGE'S DISASSEMBLY, and
# that is the opposite of the gp gate's answer for a reason worth stating. A relocation names
# the SYMBOL an instruction operand resolves to, and relaxation rewrites the encoding without
# ever changing that name, so the archives are exact here. A value scan over the image is not:
# `grant_hits_reserved` and `grant_region_admissible` materialise the Cortex-M bit-band
# constants 0x40000000 and 0x40100000 in generic kernel code, which are NUMERICALLY IDENTICAL
# to __kickos_app_rom_start and __kickos_app_sram_start on this board. Separating those from a
# real reference needs a pattern allowlist, which is the one thing this gate must not have.
#
# WHICH SYMBOLS ARE APP-HALF is a link-time fact and comes from the IMAGE, whose symbol table
# is the only place it exists: every GLOBAL or WEAK symbol whose value falls in
# [__kickos_app_rom_start, __kickos_app_sram_end]. Closed at the top on purpose: the
# one-past-the-end address reaches app bytes by subtraction, and `_kickos_heap_limit` is
# exactly that address.
#
# GLOBAL AND WEAK ONLY, AND A LOCAL ONE WOULD BE A FALSE POSITIVE. An anonymous-namespace
# symbol carries the same mangled name in a kernel TU and an app TU: `Sink::put` and
# `emit_uint` of kfmt.cc exist at BOTH an app-half and a kernel-half address in every image
# here. A local symbol resolves inside its own object and can never cross, so binding is the
# discriminator.
#
# INSTRUCTION OPERANDS ONLY. A relocated 64-bit word in kernel data is the SANCTIONED way to
# reach across, and one lives in a kernel text section: the gp anchor word `.text.privtrap`
# loads gp from. Data relocations are counted and named in the corpus line rather than
# refused, so a new one is a number that changed and not an invisible pass. The
# classification is by the DATA type list below, so a relocation type nobody has seen yet
# counts as an instruction and over-refuses, which is the safe direction.
#
# usage: check_riscv_kernel_apphalf.sh <readelf> <allowlist> <image>... `--` <archive>...
#   image    the linked ELF, read for the window bounds and the app-half symbol names
#   archive  the KERNEL-side static libraries, read for their .rela.text* relocations

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# readelf translates `File:` and `Relocation section` under a localised LANG, and both are
# parsed below. The cross readelf does not translate, but a host one in the same slot would.
export LC_ALL=C

if [ "$#" -lt 5 ]; then
    echo "usage: $0 <readelf> <allowlist> <image>... '--' <archive>..." >&2
    exit 2
fi

READELF="$1"; shift
ALLOWLIST="$1"; shift

command -v "$READELF" >/dev/null 2>&1 || fail "readelf not found: $READELF"
[ -f "$ALLOWLIST" ] || fail "allowlist not found: $ALLOWLIST"

IMAGES=""
while [ "$#" -gt 0 ]; do
    if [ "$1" = "--" ]; then
        shift
        break
    fi
    [ -f "$1" ] || fail "image not found: $1"
    IMAGES="$IMAGES $1"
    shift
done
ARCHIVES=""
while [ "$#" -gt 0 ]; do
    [ -f "$1" ] || fail "archive not found: $1"
    ARCHIVES="$ARCHIVES $1"
    shift
done

[ -n "$IMAGES" ] || fail "no image given, so the app-half symbol set would be empty and every
      reference would read as clean"
[ -n "$ARCHIVES" ] || fail "no archive given after the separator, so nothing is scanned and the gate would
      pass vacuously"

scratch_dir

# Floors, so a parse that stopped matching cannot read as clean. Each was chosen below the
# figure this board reports and above what a broken parse produces, which is zero.
MIN_APPHALF_SYMS=50
MIN_INSN_RELOCS=500
MIN_DATA_RELOCS=1

# --- The app-half symbol names, per image ---------------------------------------------------
#
# Values are compared as the fixed-width lowercase hex readelf prints, so no numeric
# conversion is needed and no awk extension is relied on. A width other than 16 means the
# output is not what this parse was written against, and it fails rather than mis-compares.
: > "$TMP/apphalf"
for IMG in $IMAGES; do
    tool_out "$TMP/syms" '^[[:space:]]*[0-9]+:' "$READELF" -sW "$IMG"

    awk -v img="$IMG" '
        NF >= 8 && $1 ~ /^[0-9]+:$/ {
            if (length($2) != 16) {
                printf "WIDTH %s\n", $2 > "/dev/stderr"
                bad = 1
                exit 1
            }
            if ($8 == "__kickos_app_rom_start") { lo = $2 }
            if ($8 == "__kickos_app_sram_end")  { hi = $2 }
            n++
        }
        END {
            if (bad) { exit 1 }
            if (lo == "" || hi == "") {
                printf "BOUNDS\n"
                exit 0
            }
            printf "OK %s %s %d\n", lo, hi, n
        }
    ' "$TMP/syms" > "$TMP/bounds" 2>"$TMP/bounds.err" \
        || fail "$IMG: readelf printed a symbol value that is not 16 hex digits, so the window
      comparison below is not the one this gate was written against: $(cat "$TMP/bounds.err")"

    read -r VERDICT LO HI NSYMS < "$TMP/bounds" || VERDICT=""
    [ "$VERDICT" = OK ] \
        || fail "$IMG: __kickos_app_rom_start or __kickos_app_sram_end is absent, so the app
      half has no extent and every reference into it would read as clean"

    awk -v lo="$LO" -v hi="$HI" '
        NF >= 8 && $1 ~ /^[0-9]+:$/ && ($5 == "GLOBAL" || $5 == "WEAK") {
            if ($2 >= lo && $2 <= hi) { print $8 }
        }
    ' "$TMP/syms" >> "$TMP/apphalf"

    echo "  $IMG: window $LO..$HI over $NSYMS symbol(s)"
done
sort -u "$TMP/apphalf" > "$TMP/apphalf.u"
N_APPHALF="$(wc -l < "$TMP/apphalf.u" | tr -d ' ')"
[ "$N_APPHALF" -ge "$MIN_APPHALF_SYMS" ] \
    || fail "$N_APPHALF app-half GLOBAL/WEAK symbol(s) across the images, floor
      $MIN_APPHALF_SYMS; the symbol parse is wrong, not the tree"

# --- The kernel-text relocations ------------------------------------------------------------
#
# One readelf per archive, so a member's name is reported with the file it came from. The
# `.rela.text` prefix is what restricts this to kernel TEXT: a relocation in .rela.data names
# the sanctioned word.
: > "$TMP/insn"
: > "$TMP/data"
for AR in $ARCHIVES; do
    tool_out "$TMP/rel" 'Relocation section' "$READELF" -rW "$AR"

    awk -v ar="$AR" '
        /^File:/ { member = $2; next }
        /^Relocation section/ {
            sec = $3
            gsub(/'"'"'/, "", sec)
            intext = (sec ~ /^\.rela\.text/)
            next
        }
        !intext { next }
        NF >= 5 && $3 ~ /^R_RISCV_/ {
            # RELAX and ALIGN are relaxation annotations and name no symbol.
            if ($3 == "R_RISCV_RELAX" || $3 == "R_RISCV_ALIGN") { next }
            where = member
            if (where == "") { where = ar }
            kind = "insn"
            if ($3 ~ /^R_RISCV_(NONE|32|64|RELATIVE|IRELATIVE|COPY|JUMP_SLOT|32_PCREL|ADD[0-9]+|SUB[0-9]+|SET[0-9]+|(SET|SUB)_ULEB128|TLS_DTP(MOD|REL)(32|64)|TLS_TPREL(32|64))$/) {
                kind = "data"
            }
            printf "%s\t%s\t%s\t%s\t%s\n", kind, $5, $3, sec, where
        }
    ' "$TMP/rel" > "$TMP/rel.parsed"

    awk -F"$TAB" '$1 == "insn"' "$TMP/rel.parsed" >> "$TMP/insn"
    awk -F"$TAB" '$1 == "data"' "$TMP/rel.parsed" >> "$TMP/data"
done

N_INSN="$(wc -l < "$TMP/insn" | tr -d ' ')"
N_DATA="$(wc -l < "$TMP/data" | tr -d ' ')"
[ "$N_INSN" -ge "$MIN_INSN_RELOCS" ] \
    || fail "$N_INSN instruction relocation(s) in the kernel archives' .rela.text sections,
      floor $MIN_INSN_RELOCS; the relocation parse is wrong, not the tree"
[ "$N_DATA" -ge "$MIN_DATA_RELOCS" ] \
    || fail "$N_DATA data relocation(s) in those same sections, floor $MIN_DATA_RELOCS; the
      gp anchor word is one, so a count below the floor means the type classification
      collapsed and every data word is now being read as an instruction"

# The data words that reach the app half, named rather than refused.
awk -F"$TAB" 'NR == FNR { app[$0] = 1; next } app[$2] { printf "%s (%s, %s)\n", $2, $3, $5 }' \
    "$TMP/apphalf.u" "$TMP/data" | sort -u > "$TMP/datahits"

# --- The verdict ----------------------------------------------------------------------------
sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$ALLOWLIST" | grep -v '^$' | sort -u > "$TMP/allowed"
require_nonempty "$TMP/allowed" "$ALLOWLIST holds no symbol, so every hit below would be
      unlisted and the gate would be a list of names nobody wrote"

awk -F"$TAB" 'NR == FNR { app[$0] = 1; next } app[$2] { print $2 "\t" $4 "\t" $5 }' \
    "$TMP/apphalf.u" "$TMP/insn" | sort -u > "$TMP/hits"
cut -f1 "$TMP/hits" | sort -u > "$TMP/hitnames"

comm -13 "$TMP/allowed" "$TMP/hitnames" > "$TMP/unlisted"
comm -23 "$TMP/allowed" "$TMP/hitnames" > "$TMP/stale"

N_HITNAMES="$(wc -l < "$TMP/hitnames" | tr -d ' ')"
echo "corpus: $N_APPHALF app-half GLOBAL/WEAK symbol(s), $N_INSN instruction and $N_DATA data"
echo "        relocation(s) in kernel .rela.text, $N_HITNAMES symbol(s) reached by kernel text"
if [ -s "$TMP/datahits" ]; then
    echo "        data word(s) into the app half, sanctioned:"
    sed 's/^/          /' "$TMP/datahits"
fi

RC=0
if [ -s "$TMP/unlisted" ]; then
    echo "FAIL: kernel text materialises an app-half address no allowlist line names." >&2
    echo "      An app-half address names whichever process is on the core, not the one this" >&2
    echo "      code meant. Reach it through a relocated word in kernel data instead, or add" >&2
    echo "      a line to $ALLOWLIST with the reason the value is never dereferenced." >&2
    while IFS= read -r SYM; do
        awk -F"$TAB" -v s="$SYM" '$1 == s { printf "      %s  from %s in %s\n", $1, $2, $3 }' \
            "$TMP/hits" >&2
    done < "$TMP/unlisted"
    RC=1
fi
if [ -s "$TMP/stale" ]; then
    echo "FAIL: $ALLOWLIST names symbol(s) no kernel-text instruction reaches any more." >&2
    echo "      A stale allowlist is how the list stops being an enumeration. Delete the" >&2
    echo "      line(s), or find out why the reference this gate used to see has gone." >&2
    sed 's/^/      /' "$TMP/stale" >&2
    RC=1
fi
[ "$RC" -eq 0 ] || exit 1

echo "PASS: every app-half address kernel text materialises is named and never dereferenced"
