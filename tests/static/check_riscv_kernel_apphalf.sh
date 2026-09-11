#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No kernel-text instruction names an APP-HALF symbol, bar a named allowlist. The app's window
# is one level-2 slot of a PER-SPACE table, so an app-half address does not name one process:
# it names whichever process the core is on. A kernel store meant for process A therefore
# lands in process B, and nothing reports it.
#
# The link does not already refuse this. The linker script's assert covers only the medany
# range; the app window is at 0x40000000, inside medlow's `lui`-reachable range, so the linker
# RELAXES a kernel reference to an app-half symbol into `lui`+`addi` and the link succeeds. At
# run time the read or write succeeds too: sstatus.SUM is set for the life of kernel context
# and the running space maps the app's half U.
#
# The corpus is the kernel-side archives' RELOCATIONS. A relocation names the SYMBOL an
# instruction operand resolves to, and relaxation rewrites the encoding without changing that
# name. A value scan over the image is not exact: `grant_hits_reserved` and
# `grant_region_admissible` materialise the Cortex-M bit-band constants 0x40000000 and
# 0x40100000 in generic kernel code, numerically identical to __kickos_app_rom_start and
# __kickos_app_sram_start on this board, and separating those needs a pattern allowlist, which
# is the one thing this gate must not have.
#
# Which symbols are app-half comes from the IMAGE symbol table: every GLOBAL or WEAK symbol
# whose value falls in [__kickos_app_rom_start, __kickos_app_sram_end]. Closed at the top on
# purpose: the one-past-the-end address reaches app bytes by subtraction, and
# `_kickos_heap_limit` is exactly that address.
#
# GLOBAL and WEAK only: an anonymous-namespace symbol carries the same mangled name in a kernel
# TU and an app TU (`Sink::put` and `emit_uint` of kfmt.cc exist at both an app-half and a
# kernel-half address in every image here), and a local symbol resolves inside its own object
# and can never cross, so binding is the discriminator.
#
# Instruction operands only. A relocated 64-bit word in kernel data is the SANCTIONED way to
# reach across, and one lives in a kernel text section: the gp anchor word `.text.privtrap`
# loads gp from. Data relocations are counted and named in the corpus line rather than refused.
# The classification is by the DATA type list below, so a relocation type nobody has seen yet
# counts as an instruction and over-refuses, which is the safe direction.
#
# THE SELF-TEST BELOW READS FORGED readelf OUTPUT: there is no compiler here to make a real
# object. So what the controls prove is the PARSER and the verdict, never the invocation: that
# the streams parsed are really this board's `-sW` and `-rW`, and that the archives named on
# the command line are the kernel-side ones, is outside them. The floors are the parameters
# the controls lower, a forged corpus being small by construction and a fleet-sized floor
# being what would catch every control.
#
# usage: check_riscv_kernel_apphalf.sh <readelf> <allowlist> <image>... `--` <archive>...
#   image    the linked ELF, read for the window bounds and the app-half symbol names
#   archive  the KERNEL-side static libraries, read for their .rela.text* relocations

set -eu
. "$(dirname "$0")/../lib/gate.sh"

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

SCAN_REAL="$(dirname "$0")/riscv_apphalf.awk"
[ -r "$SCAN_REAL" ] || fail "tests/static/riscv_apphalf.awk is unreadable, so neither the
      symbol table nor a relocation section can be read"

scratch_dir

# Every pattern the rule is made of, in one place, so a self-test arm can withdraw exactly one
# of them. A literal dot is BRACKETED and never backslash-escaped: awk processes escape
# sequences in a -v assignment, so `\.` would arrive as a plain `.` and match any character.
defaults() { # <apphalf-floor> <insn-floor> <data-floor>
    SCAN="$SCAN_REAL"
    LO_SYM=__kickos_app_rom_start
    HI_SYM=__kickos_app_sram_end
    VAL_WIDTH=16
    BIND_ERE='^(GLOBAL|WEAK)$'
    CLOSED=1
    SECT_ERE='^[.]rela[.]text'
    TYPE_ERE='^R_RISCV_'
    SKIP_ERE='^R_RISCV_(RELAX|ALIGN)$'
    DATA_ERE='^R_RISCV_(NONE|32|64|RELATIVE|IRELATIVE|COPY|JUMP_SLOT|32_PCREL|ADD[0-9]+|SUB[0-9]+|SET[0-9]+|(SET|SUB)_ULEB128|TLS_DTP(MOD|REL)(32|64)|TLS_TPREL(32|64))$'
    # Floors, so a parse that stopped matching cannot read as clean. Each fleet figure sits
    # below what this board reports and above what a broken parse produces, which is zero.
    MIN_APPHALF_SYMS="$1"
    MIN_INSN_RELOCS="$2"
    MIN_DATA_RELOCS="$3"
}

WLO=""
WHI=""
NSYMS=0
read_bounds() { # <syms-file> <label>; sets WLO WHI NSYMS
    tool_out "$TMP/bounds" '' awk -v PHASE=bounds -v LO="$LO_SYM" -v HI="$HI_SYM" \
        -v WIDTH="$VAL_WIDTH" -f "$SCAN" "$1"
    VERDICT=""
    B_LO=""
    B_HI=""
    B_N=""
    if ! read -r VERDICT B_LO B_HI B_N < "$TMP/bounds"; then
        VERDICT=""
    fi
    case "$VERDICT" in
        OK) ;;
        WIDTH)
            fail "$2: readelf printed the symbol value $B_LO, which is not $VAL_WIDTH hex
      digits wide, so the window comparison below is not the one this gate was written
      against" ;;
        *)
            fail "$2: $LO_SYM or $HI_SYM is absent, so the app
      half has no extent and every reference into it would read as clean" ;;
    esac
    WLO="$B_LO"
    WHI="$B_HI"
    NSYMS="$B_N"
}

read_names() { # <syms-file> <append-to>
    tool_out "$TMP/names" '' awk -v PHASE=names -v LO="$WLO" -v HI="$WHI" \
        -v BIND="$BIND_ERE" -v CLOSED="$CLOSED" -f "$SCAN" "$1"
    cat "$TMP/names" >> "$2"
}

# One readelf per archive, so a member's name is reported with the file it came from. The
# `.rela.text` prefix is what restricts this to kernel TEXT: a relocation in .rela.data names
# the sanctioned word.
read_relocs() { # <rel-file> <archive-label> <append-to>
    tool_out "$TMP/rel.parsed" '' awk -v PHASE=rel -v AR="$2" -v SECT="$SECT_ERE" \
        -v TYPE="$TYPE_ERE" -v SKIP="$SKIP_ERE" -v DATA="$DATA_ERE" -f "$SCAN" "$1"
    cat "$TMP/rel.parsed" >> "$3"
}

# Each manifest is TAB-separated `<label>\t<parsed readelf output>`, one line per image or
# archive. A path is never split on a delimiter it could hold.
pipeline() { # <image manifest> <archive manifest> <allowlist>
    : > "$TMP/summary"

    : > "$TMP/apphalf"
    while IFS="$TAB" read -r _lab _f; do
        read_bounds "$_f" "$_lab"
        read_names "$_f" "$TMP/apphalf"
        echo "  $_lab: window $WLO..$WHI over $NSYMS symbol(s)"
    done < "$1"
    sort -u "$TMP/apphalf" > "$TMP/apphalf.u"
    N_APPHALF="$(wc -l < "$TMP/apphalf.u" | tr -d ' ')"
    [ "$N_APPHALF" -ge "$MIN_APPHALF_SYMS" ] \
        || fail "$N_APPHALF app-half GLOBAL/WEAK symbol(s) across the images, floor
      $MIN_APPHALF_SYMS; the symbol parse is wrong, not the tree"

    : > "$TMP/records"
    while IFS="$TAB" read -r _lab _f; do
        read_relocs "$_f" "$_lab" "$TMP/records"
    done < "$2"
    tool_out "$TMP/insn" '' awk -F"$TAB" '$1 == "insn"' "$TMP/records"
    tool_out "$TMP/data" '' awk -F"$TAB" '$1 == "data"' "$TMP/records"

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
    tool_out "$TMP/datahits.raw" '' awk -F"$TAB" \
        'NR == FNR { app[$0] = 1; next } app[$2] { printf "%s (%s, %s)\n", $2, $3, $5 }' \
        "$TMP/apphalf.u" "$TMP/data"
    sort -u "$TMP/datahits.raw" > "$TMP/datahits"

    sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$3" | grep -v '^$' | sort -u > "$TMP/allowed"
    require_nonempty "$TMP/allowed" "$3 holds no symbol, so every hit below would be
      unlisted and the gate would be a list of names nobody wrote"

    tool_out "$TMP/hits.raw" '' awk -F"$TAB" \
        'NR == FNR { app[$0] = 1; next } app[$2] { print $2 "\t" $4 "\t" $5 }' \
        "$TMP/apphalf.u" "$TMP/insn"
    sort -u "$TMP/hits.raw" > "$TMP/hits"
    cut -f1 "$TMP/hits" | sort -u > "$TMP/hitnames"

    comm -13 "$TMP/allowed" "$TMP/hitnames" > "$TMP/unlisted"
    comm -23 "$TMP/allowed" "$TMP/hitnames" > "$TMP/stale"

    N_HITNAMES="$(wc -l < "$TMP/hitnames" | tr -d ' ')"
    printf '%s %s %s %s %s %s\n' "$N_APPHALF" "$N_INSN" "$N_DATA" "$N_HITNAMES" \
        "$(wc -l < "$TMP/unlisted" | tr -d ' ')" "$(wc -l < "$TMP/stale" | tr -d ' ')" \
        > "$TMP/summary"

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
        echo "      a line to $3 with the reason the value is never dereferenced." >&2
        while IFS= read -r SYM; do
            awk -F"$TAB" -v s="$SYM" '$1 == s { printf "      %s  from %s in %s\n", $1, $2, $3 }' \
                "$TMP/hits" >&2
        done < "$TMP/unlisted"
        RC=1
    fi
    if [ -s "$TMP/stale" ]; then
        echo "FAIL: $3 names symbol(s) no kernel-text instruction reaches any more." >&2
        echo "      A stale allowlist is how the list stops being an enumeration. Delete the" >&2
        echo "      line(s), or find out why the reference this gate used to see has gone." >&2
        sed 's/^/      /' "$TMP/stale" >&2
        RC=1
    fi
    [ "$RC" -eq 0 ] || return 1

    echo "PASS: every app-half address kernel text materialises is named and never dereferenced"
}

# --- self-test: one control per clause, each a minimal pair -------------------
ctldir="$TMP/ctl"
mkdir -p "$ctldir"

# readelf -sW prints a numbered row per symbol, the value in field 2 and the binding in field
# 5. Entry 0 is the undefined one and carries no name, so it has one field fewer and is not
# counted.
symtab() { # <outfile> <"<value> <binding> <name>">...
    _f="$1"
    shift
    printf "Symbol table '.symtab' contains %d entries:\n" $(($# + 1)) > "$_f"
    printf '   Num:    Value          Size Type    Bind   Vis      Ndx Name\n' >> "$_f"
    printf '     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND\n' >> "$_f"
    _i=0
    for _s in "$@"; do
        _i=$((_i + 1))
        _rest="${_s#* }"
        printf '%6d: %s    16 FUNC    %-6s DEFAULT    1 %s\n' \
            "$_i" "${_s%% *}" "${_rest%% *}" "${_rest#* }" >> "$_f"
    done
}

# readelf -rW prints a `File:` line per archive member, a `Relocation section` line per
# section, then one record per relocation with the symbol name in field 5. A relaxation
# annotation names no symbol, so field 5 of that row is the addend sign.
rel_member() { # <outfile> <member>
    printf '\nFile: %s\n' "$2" >> "$1"
}
rel_section() { # <outfile> <section>
    printf "\nRelocation section '%s' at offset 0x100 contains 1 entries:\n" "$2" >> "$1"
    printf "    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend\n" >> "$1"
}
rel_record() { # <outfile> <type> <symbol> <value>
    printf '0000000000000010  0000000300000013 %-22s %s %s + 0\n' "$2" "$4" "$3" >> "$1"
}
man_add() { # <manifest> <label> <file>
    printf '%s\t%s\n' "$2" "$3" >> "$1"
}

A_ROM=0000000040000000
A_END=0000000040100000
A_ROOT=0000000040001000
A_WEAK=0000000040002000
A_LOCAL=0000000040003000
A_KERN=0000000080000000
MEMBER='kernel.a(kmain.cc.obj)'

symtab "$ctldir/s_base" \
    "$A_ROM GLOBAL __kickos_app_rom_start" \
    "$A_END GLOBAL __kickos_app_sram_end" \
    "$A_ROOT GLOBAL kickos_root_entry" \
    "$A_WEAK WEAK kos_weak_app_thing" \
    "$A_LOCAL LOCAL kos_local_shadow" \
    "$A_END GLOBAL _kickos_heap_limit" \
    "$A_KERN GLOBAL kmain" \
    '000000003ffffff8 GLOBAL kos_just_below' \
    '0000000040100008 GLOBAL kos_just_above'
# The width clause: a 32-bit readelf in the same slot prints an 8-digit value column, and the
# window comparison this gate makes is only meaningful at one width.
symtab "$ctldir/s_narrow" \
    "40000000 GLOBAL __kickos_app_rom_start" \
    "40100000 GLOBAL __kickos_app_sram_end"
symtab "$ctldir/s_nohi" \
    "$A_ROM GLOBAL __kickos_app_rom_start" \
    "$A_ROOT GLOBAL kickos_root_entry"

: > "$ctldir/r_base"
rel_member  "$ctldir/r_base" "$MEMBER"
rel_section "$ctldir/r_base" .rela.text.kmain
rel_record  "$ctldir/r_base" R_RISCV_CALL_PLT   kickos_root_entry  "$A_ROOT"
rel_record  "$ctldir/r_base" R_RISCV_RELAX      ''                 0000000000000000
rel_record  "$ctldir/r_base" R_RISCV_HI20       kos_weak_app_thing "$A_WEAK"
rel_record  "$ctldir/r_base" R_RISCV_HI20       _kickos_heap_limit "$A_END"
rel_record  "$ctldir/r_base" R_RISCV_CALL_PLT   kmain              "$A_KERN"
rel_record  "$ctldir/r_base" R_RISCV_64         kickos_root_entry  "$A_ROOT"
# A type this gate has never seen counts as an instruction, which over-refuses.
rel_record  "$ctldir/r_base" R_RISCV_FUTURE_ONE kickos_root_entry  "$A_ROOT"
rel_member  "$ctldir/r_base" 'arch.a(arch_rv64imac.cc.obj)'
rel_section "$ctldir/r_base" .rela.text.arch
rel_record  "$ctldir/r_base" R_RISCV_HI20       kos_local_shadow   "$A_LOCAL"
rel_section "$ctldir/r_base" .rela.data.rel.ro
rel_record  "$ctldir/r_base" R_RISCV_64         kos_weak_app_thing "$A_WEAK"

cp "$ctldir/r_base" "$ctldir/r_unlisted"
rel_member  "$ctldir/r_unlisted" "$MEMBER"
rel_section "$ctldir/r_unlisted" .rela.text.kmain
rel_record  "$ctldir/r_unlisted" R_RISCV_HI20 __kickos_app_rom_start "$A_ROM"

# A localised readelf translates the two headers this parse keys on while leaving the TYPE
# column alone, so the records still look like records and nothing attributes them.
sed -e 's/^File:/Fichier :/' -e "s/^Relocation section/Section de reallocation/" \
    "$ctldir/r_base" > "$ctldir/r_french"

printf 'kickos_root_entry\nkos_weak_app_thing\n_kickos_heap_limit\n' > "$ctldir/a_base"
printf '%s\nkos_never_reached\n' "$(cat "$ctldir/a_base")" > "$ctldir/a_stale"
printf 'kickos_root_entry\n' > "$ctldir/a_root"
printf '# every symbol commented out\n\n' > "$ctldir/a_empty"

man_add "$ctldir/m_img"  hello    "$ctldir/s_base"
man_add "$ctldir/m_img2" hello    "$ctldir/s_base"
man_add "$ctldir/m_img2" cxxtest  "$ctldir/s_base"
man_add "$ctldir/m_narrow" hello  "$ctldir/s_narrow"
man_add "$ctldir/m_nohi"   hello  "$ctldir/s_nohi"
man_add "$ctldir/m_rel"      kernel.a "$ctldir/r_base"
man_add "$ctldir/m_unlisted" kernel.a "$ctldir/r_unlisted"
man_add "$ctldir/m_french"   kernel.a "$ctldir/r_french"

SCENARIOS=0
run_scenario() { # <label> <img-man> <ar-man> <allowlist> <rc> <tuple, empty to skip> <ere>
    if ( pipeline "$2" "$3" "$4" ) > "$ctldir/out" 2>&1; then
        _rc=0
    else
        _rc=$?
    fi
    if [ "$_rc" -ne "$5" ]; then
        sed 's/^/      /' "$ctldir/out" >&2
        fail "control '$1' exits $_rc, expected $5"
    fi
    if [ -n "$6" ]; then
        _got="$(cat "$TMP/summary")"
        [ "$_got" = "$6" ] || {
            sed 's/^/      /' "$ctldir/out" >&2
            fail "control '$1' reads corpus [$_got], expected [$6]. The counts are exact, so a
      control kept quiet by the wrong clause shows up as the wrong number rather than as a pass"
        }
    fi
    if [ -n "$7" ]; then
        grep -qE "$7" "$ctldir/out" || {
            sed 's/^/      /' "$ctldir/out" >&2
            fail "control '$1' reddened for the wrong reason, expected /$7/. A control another
      clause catches proves nothing about the clause it plants for"
        }
    fi
    SCENARIOS=$((SCENARIOS + 1))
}

# The control floors sit BELOW what the forged corpus produces, so no floor is what catches a
# clause violation; each floor is then raised on its own further down.
defaults 1 1 0
run_scenario clean "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 0 '5 6 1 3 0 0' ''

# Two images over the same window: the app-half sets are unioned, so the count must not double.
defaults 1 1 0
run_scenario two_images "$ctldir/m_img2" "$ctldir/m_rel" "$ctldir/a_base" 0 '5 6 1 3 0 0' ''

# The verdict the gate exists for, and the attribution beside it: a hit has to name the symbol,
# the section and the archive MEMBER, which is the part a localised readelf empties.
defaults 1 1 0
run_scenario unlisted "$ctldir/m_img" "$ctldir/m_unlisted" "$ctldir/a_base" 1 '5 7 1 4 1 0' \
    'no allowlist line names'
grep -qE '__kickos_app_rom_start  from [.]rela[.]text[.]kmain in kernel[.]a\(kmain[.]cc[.]obj\)' \
    "$ctldir/out" \
    || {
        sed 's/^/      /' "$ctldir/out" >&2
        fail "the unlisted hit does not name its section and archive member, so a red run
      cannot say which object to fix"
    }

defaults 1 1 0
run_scenario stale "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_stale" 1 '5 6 1 3 0 1' \
    'no kernel-text instruction reaches any more'

defaults 1 1 0
run_scenario width "$ctldir/m_narrow" "$ctldir/m_rel" "$ctldir/a_base" 1 '' \
    'which is not 16 hex'

defaults 1 1 0
run_scenario bounds "$ctldir/m_nohi" "$ctldir/m_rel" "$ctldir/a_base" 1 '' 'has no extent'

defaults 1 1 0
run_scenario localised "$ctldir/m_img" "$ctldir/m_french" "$ctldir/a_base" 1 '' \
    'the relocation parse is wrong'

defaults 1 1 0
run_scenario no_allowlist "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_empty" 1 '' \
    'holds no symbol'

# THE DEAD SCANNER, the one case a planted violation cannot reach: an awk that dies writes no
# record, and a gate that reads its own empty output reports the absence it was testing for.
# The corpus here is the CLEAN one, so only the tool death can redden it.
# Broken by an unbalanced parenthesis, which every awk refuses before reading a record. A
# call to a function that does not exist would be a RUNTIME error instead, so it would need
# the forged corpus to hold a record before it could fire at all.
sed 's|^PHASE == "bounds"|PHASE == ("bounds"|' "$SCAN_REAL" \
    > "$ctldir/broken.awk"
defaults 1 1 0
SCAN="$ctldir/broken.awk"
run_scenario dead_scanner "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '' \
    'exit [0-9]+ from: awk'

# --- the mutations the controls exist to survive ------------------------------
# Withdraw exactly one reading and the corpus counts must MOVE by an exact amount, so each
# near miss above is a near miss and not merely a clean record.
defaults 1 1 0
BIND_ERE='^(GLOBAL|WEAK|LOCAL)$'
run_scenario m_binding "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '6 6 1 4 1 0' \
    'no allowlist line names'

defaults 1 1 0
CLOSED=0
# Both __kickos_app_sram_end and _kickos_heap_limit sit AT the top, so opening the window
# drops two names and takes the allowlist line for the heap limit stale with them.
run_scenario m_open_top "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '3 6 1 2 0 1' \
    'no kernel-text instruction reaches any more'

defaults 1 1 0
SECT_ERE='^[.]rela[.]'
run_scenario m_any_section "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 0 '5 6 2 3 0 0' ''

defaults 1 1 0
SKIP_ERE='^R_RISCV_KOS_NOTHING$'
run_scenario m_no_skip "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 0 '5 7 1 3 0 0' ''

defaults 1 1 0
DATA_ERE='^R_RISCV_(NONE|32|RELATIVE)$'
run_scenario m_data_narrow "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 0 '5 7 0 3 0 0' ''

defaults 1 1 0
DATA_ERE='^R_RISCV_(64|FUTURE_ONE)$'
run_scenario m_data_wide "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 0 '5 5 2 3 0 0' ''

defaults 1 1 0
TYPE_ERE='^R_RISCV_KOS_NOTHING$'
run_scenario m_no_types "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '' \
    'the relocation parse is wrong'

defaults 1 1 0
LO_SYM=__kickos_app_rom_begin
run_scenario m_bound_name "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '' 'has no extent'

defaults 1 1 0
VAL_WIDTH=8
run_scenario m_width "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '' 'which is not 8 hex'

# Each floor on its own, raised one above what the clean corpus produces, so the message it
# fires with is the one a reader would act on.
defaults 6 1 0
run_scenario floor_syms "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '' \
    'the symbol parse is wrong, not the tree'
defaults 1 7 0
run_scenario floor_insn "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '' \
    'the relocation parse is wrong, not the tree'
defaults 1 1 2
run_scenario floor_data "$ctldir/m_img" "$ctldir/m_rel" "$ctldir/a_base" 1 '' \
    'the type classification'

[ "$SCENARIOS" -eq 21 ] || fail "$SCENARIOS of 21 whole-pipeline controls ran"

# --- each near miss read on its own -------------------------------------------
# A control silent inside the clean corpus could be silent for another clause's reason, so
# each is read again beside ONE allowlisted hit and nothing else: the hit count is exactly one
# and the near miss must not be the name in it.
near_miss() { # <label> <section> <type> <symbol> <value> <tuple>
    _r="$ctldir/r_$1"
    : > "$_r"
    rel_member  "$_r" "$MEMBER"
    rel_section "$_r" .rela.text.kmain
    rel_record  "$_r" R_RISCV_CALL_PLT kickos_root_entry "$A_ROOT"
    rel_section "$_r" "$2"
    rel_record  "$_r" "$3" "$4" "$5"
    : > "$ctldir/m_$1"
    man_add "$ctldir/m_$1" kernel.a "$_r"
    defaults 1 1 0
    run_scenario "near_$1" "$ctldir/m_img" "$ctldir/m_$1" "$ctldir/a_root" 0 "$6" ''
    [ "$(cat "$TMP/hitnames")" = kickos_root_entry ] \
        || fail "near miss '$1' changed which symbol kernel text is seen to reach:
      $(cat "$TMP/hitnames")"
}
near_miss local     .rela.text.arch     R_RISCV_HI20  kos_local_shadow   "$A_LOCAL" '5 2 0 1 0 0'
near_miss above     .rela.text.arch     R_RISCV_HI20  kos_just_above     0000000040100008 '5 2 0 1 0 0'
near_miss below     .rela.text.arch     R_RISCV_HI20  kos_just_below     000000003ffffff8 '5 2 0 1 0 0'
near_miss kernhalf  .rela.text.arch     R_RISCV_CALL_PLT kmain           "$A_KERN"  '5 2 0 1 0 0'
near_miss datasect  .rela.data.rel.ro   R_RISCV_64    kos_weak_app_thing "$A_WEAK"  '5 1 0 1 0 0'
near_miss relax     .rela.text.arch     R_RISCV_RELAX ''                 0000000000000000 '5 1 0 1 0 0'
near_miss dataword  .rela.text.arch     R_RISCV_64    kos_weak_app_thing "$A_WEAK"  '5 1 1 1 0 0'
[ "$SCENARIOS" -eq 28 ] || fail "$SCENARIOS of 28 controls ran"

# --- the images and the archives ----------------------------------------------
: > "$TMP/img.manifest"
_n=0
for IMG in $IMAGES; do
    _n=$((_n + 1))
    tool_out "$TMP/syms.$_n" '^[[:space:]]*[0-9]+:' "$READELF" -sW "$IMG"
    man_add "$TMP/img.manifest" "$IMG" "$TMP/syms.$_n"
done
: > "$TMP/ar.manifest"
_n=0
for AR in $ARCHIVES; do
    _n=$((_n + 1))
    tool_out "$TMP/rel.$_n" 'Relocation section' "$READELF" -rW "$AR"
    man_add "$TMP/ar.manifest" "$AR" "$TMP/rel.$_n"
done

defaults 50 500 1
pipeline "$TMP/img.manifest" "$TMP/ar.manifest" "$ALLOWLIST" || exit 1
