#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The rules a linked image holds that no build rule can see, read out of the image itself. Each
# rule prints one line per finding, and each is run a second time on a planted copy of this
# image's own listing, which it must refuse, so a rule that has stopped reading is not a pass.
#
#   rv64_wx      every valid boot-table entry (.mmu_boot, .mmu_leaves) sets no bit at 54 or
#                above, and every leaf is R, not U and not W+X; kernel_l1's 512 entries are all
#                valid leaves, the first output a level-2 boundary and entry i outputting it plus
#                i * 2 MiB; every allocated section in the kernel window lies under kernel_l1
#                leaves that are X where it is executable and W where it is writable and not TLS
#   rv64_gp      kernel .text names gp only to seat it: `auipc gp,0x0` then `ld gp,N(gp)`, or
#                `li gp` or `mv gp`, and holds two such pairs at least. gp is the app's anchor
#                and U-mode writes it
#   rv32_trap    kickos_rv_mtvec holds KICKOS_MAX_IRQ slots, each a 4-byte `j` to a symbol's
#                start, at BASE + 4*i; every target
#                seats gp with the auipc/addi pair before its first transfer and names gp nowhere
#                above it; the image writes mtvec once; switch.S's object has no R_RISCV_RELAX
#   rp_node      g_node_isr_vector holds the park in every slot but the bell line's, which holds
#                the doorbell service; the park makes no call and branches only into itself
#   a53_pmcr     kickos_armv8a_percore_init writes PMCR_EL0 once, unconditionally, as read, D
#                (bit 3) cleared, E, C and LC (bits 0, 2, 6) set
#   arm64_entry  _start selects SP_ELx before its first write to SP; arch_timer_disarm and
#                kickos_armv8a_percore_init take an ISB between the CNTP_CTL_EL0 disable and
#                their next Device write or branch
#   c6_hp        Reset_Handler calls mtime_rate_init before it names an init_array bound
#   c6_lp        SystemCoreClock is a nonzero word of loaded data
#   ctor         every .init_array input the map places lies in __init_array_start/_end or
#                __kickos_app_init_array_start/_end, and none comes from a KickOS-owned object
#   cpu_id       at one core no image defines arch_cpu_id
#
# usage: check_image_rules.sh <rule> <objdump> <elf> [arg]...
#   The nm and readelf beside <objdump> read the symbols and sections.

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_image_rules.sh <rule> <objdump> <elf> [arg]..."
RULE="${1:?$_usage}"
OD="${2:?$_usage}"
ELF="${3:?$_usage}"
shift 3
NM="${OD%objdump}nm"
RE="${OD%objdump}readelf"
[ -f "$ELF" ] || fail "no image at $ELF"
for _t in "$OD" "$NM" "$RE"; do
    [ -x "$_t" ] || fail "no binutils tool at $_t"
done

scratch_dir
image_listing "$ELF" "$NM" "$OD" 100 --no-show-raw-insn
D="$TMP/dis"
N="$TMP/nm"
S="$TMP/sec"
W="$TMP/words"
X=""

AWKLIB='
function hex(s, n, i) {
    sub(/^0x/, "", s)
    for (i = 1; i <= length(s); i++) n = n * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
    return n + 0
}
function bit(n, k) { return int(n / 2 ^ k) % 2 }
function insn(l) {
    sub(/^[^:]*:[ \t]*/, "", l)
    sub(/[ \t]*(# |\/\/).*$/, "", l)
    gsub(/[ \t]+/, " ", l)
    sub(/ $/, "", l)
    return l
}
'

addr_of() { # <symbol>
    awk -v s="$1" '$NF == s { print $1; exit }' "$N"
}

# The ordinal of the first <need> past <open> against the first <close> past it, in <symbol>'s
# body, through the shared scope and window (tests/lib/objdump_window.awk).
cat > "$TMP/order.awk" <<'AWK'
win_n > win_open_n && need == 0 && win_text ~ needre { need = win_n }
END { if (need == 0 || need > win_close_n) print "#" need + 0 " against #" win_close_n }
AWK
order() { # <symbol> <open-ere> <need-ere> <close-ere>
    _r="$(scoped_body "$TMP/order.awk" "$D" "$1" -v win_open="$2" -v needre="$3" \
        -v win_close="$4")"
    [ -z "$_r" ] || echo "$1: /$3/ does not stand between /$2/ and /$4/: $_r"
}

rule_rv64_wx() {
    _l1="$(addr_of kickos_rv64_kernel_l1)"
    [ -n "$_l1" ] || { echo "no kickos_rv64_kernel_l1"; return; }
    awk -v l1="$_l1" "$AWKLIB"'
        FNR == NR {
            lo = hex(substr($2, 15, 2))
            i = (hex(substr($1, 9)) - hex(substr(l1, 9))) / 8
            mine = substr($1, 1, 8) == substr(l1, 1, 8) && i >= 0 && i < 512
            if (mine) {
                seen++
                out[i] = int(hex($2) / 1024) % 2 ^ 44 * 4096
                if (!bit(lo, 0)) print $1 ": kernel_l1[" i "] is invalid"
                else if (lo % 16 < 2) print $1 ": kernel_l1[" i "] points to a table, not a leaf"
            }
            if (!bit(lo, 0)) next
            if (substr($2, 1, 2) != "00" || hex(substr($2, 3, 1)) >= 4) print $1 ": sets a bit at 54 or above"
            if (lo % 16 < 2) next
            if (!bit(lo, 1)) print $1 ": a leaf without R is write-only"
            if (bit(lo, 4)) print $1 ": a leaf grants the unprivileged level"
            if (bit(lo, 2) && bit(lo, 3)) print $1 ": a leaf is writable and executable"
            if (mine) r[i] = lo
            next
        }
        $4 ~ /A/ && hex($3) > 0 && substr($2, 1, 8) == substr(l1, 1, 8) \
            && int(hex(substr($2, 9)) / 2 ^ 30) == int(hex(substr(l1, 9)) / 2 ^ 30) {
            off = hex(substr($2, 9)) % 2 ^ 30
            for (i = int(off / 2 ^ 21); i <= int((off + hex($3) - 1) / 2 ^ 21) && i < 512; i++) {
                if ($4 ~ /X/ && !bit(r[i], 3)) print $1 " is fetched from and kernel_l1[" i "] is not executable"
                if ($4 ~ /W/ && $4 !~ /T/ && !bit(r[i], 2)) print $1 " is stored into and kernel_l1[" i "] is read-only"
            }
        }
        END {
            if (seen != 512) print "kernel_l1 holds " seen + 0 " entries, not 512"
            if (out[0] % 2 ^ 30) printf "kernel_l1[0] outputs 0x%x, which no level-2 slot spans\n", out[0]
            for (i = 1; i < seen; i++)
                if (out[i] != out[0] + i * 2 ^ 21)
                    printf "kernel_l1[%d] outputs 0x%x, not 0x%x\n", i, out[i], out[0] + i * 2 ^ 21
        }' "$W" "$S"
}
ctl_rv64_wx() {
    awk 'n == 0 && $2 ~ /[13579bdf]$/ { $2 = "00000000000000df"; n = 1 } 1' "$W" > "$TMP/w.p"
    W="$TMP/w.p"
}

rule_rv64_gp() {
    awk "$AWKLIB"'
        /^Disassembly of section / { text = ($4 == ".text:"); next }
        !text || !/^[ \t]*[0-9a-f]+:/ { next }
        {
            t = insn($0); m = t; sub(/ .*/, "", m); o = substr(t, length(m) + 2)
            anchor = (m == "auipc" && o == "gp,0x0")
            ok = anchor || ((m == "li" || m == "mv") && o ~ /^gp,/) \
                || (m == "ld" && o ~ /^gp,-?[0-9]*\(gp\)$/ && prev)
            if (!ok && o ~ /(^|[^0-9A-Za-z_])gp([^0-9A-Za-z_]|$)/) print t
            if (m == "ld" && o ~ /^gp,-?[0-9]*\(gp\)$/ && prev) pairs++
            prev = anchor
            n++
        }
        END {
            if (n < 200) print "only " n + 0 " instruction(s) in .text"
            if (pairs < 2) print "only " pairs + 0 " paired gp anchor(s) in .text"
        }' "$D"
}
ctl_rv64_gp() {
    awk '{ print } /^Disassembly of section \.text:/ { print "    0:\tld\ta0,0(gp)" }' "$D" > "$TMP/d.p"
    D="$TMP/d.p"
}

rule_rv32_trap() {
    awk "$AWKLIB"'
        /^[0-9a-f]+ <.*>:$/ { b = $2; gsub(/[<>:]/, "", b); next }
        !/^[ \t]*[0-9a-f]+:/ { next }
        { t = insn($0); m = t; sub(/ .*/, "", m); sub(/^c\./, "", m); o = substr(t, length(m) + 2) }
        FNR == NR {
            if (m ~ /^csrr?[wsc]i?$/ && o ~ /(^|,)mtvec(,|$)/) w++
            if (b != "kickos_rv_mtvec") next
            a = $1; sub(/:$/, "", a)
            if (!ns++) base = hex(a)
            if (hex(a) != base + 4 * (ns - 1)) print "mtvec slot " ns - 1 " is not at BASE + 4*" ns - 1
            if (t !~ /^j [0-9a-f]+ <[^+>]+>$/) { print "mtvec slot is not a jump to a symbol: " t; next }
            g = t; sub(/.*</, "", g); sub(/>$/, "", g)
            if (!(g in tgt)) { tgt[g] = 1; nt++ }
            next
        }
        !(b in tgt) || (b in done) { next }
        m ~ /^(beqz?|bnez?|bltu?|bgeu?|blez|bgez|bltz|bgtz|bgtu?|bleu?|j|jal|jalr|jr|ret|tail|mret|ecall|ebreak)$/ {
            if (!seat[b]) print b " reaches " t " before it seats gp"
            done[b] = 1
            next
        }
        hi[b] {
            hi[b] = 0
            if (m ~ /^(addi|mv)$/ && o ~ /^gp,gp(,-?[0-9]+)?$/) { seat[b] = 1; next }
            print b ": auipc gp is not followed by its addi"
        }
        !seat[b] && m == "auipc" && o ~ /^gp,/ { hi[b] = 1; next }
        !seat[b] && o ~ /(^|[^0-9A-Za-z_])gp([^0-9A-Za-z_]|$)/ { print b " names gp above its seat: " t }
        END {
            if (w != 1) print w + 0 " mtvec write(s) in the image"
            if (!nt) print "no kickos_rv_mtvec slot"
            if (ns != slots) print "kickos_rv_mtvec holds " ns + 0 " slot(s) and KICKOS_MAX_IRQ is " slots
            for (g in tgt) if (!(g in done)) print g ": no control transfer read"
        }' slots="$SLOTS" "$D" "$D"
    awk '/file format/ { m = ($1 == "switch.S.obj:"); n += m }
        m && /R_RISCV_RELAX/ { print "switch.S.obj carries R_RISCV_RELAX at " $1 }
        END { if (n != 1) print n + 0 " switch.S.obj member(s) in the arch archive" }' "$X"
}
ctl_rv32_trap() {
    awk '{ print } $2 == "<trap_entry>:" { print "    0:\tlw\tt1,-1444(gp)" }' "$D" > "$TMP/d.p"
    D="$TMP/d.p"
}

rule_rp_node() {
    _bell="$(sed -n 's/^#define KICKOS_LAYOUT_LINE_SIO_IRQ_BELL \([0-9][0-9]*\).*/\1/p' "$X")"
    [ -n "$_bell" ] || { echo "no KICKOS_LAYOUT_LINE_SIO_IRQ_BELL in $X"; return; }
    awk -v p="$(addr_of kickos_rp2350_node_park)" -v s="$(addr_of kickos_rp2350_doorbell_service)" \
        -v bell="$_bell" "$AWKLIB"'
        {
            x = hex(NR - 1 == 16 + bell ? s : p)
            want = sprintf("%08x", x - x % 2 + 1)
            if (p == "" || s == "" || $2 != want) print "slot " NR - 1 " holds " $2 ", not " want
        }
        END { if (NR <= 16 + bell) print NR + 0 " slot(s), none for the bell line" }' "$W"
    awk "$AWKLIB"'
        /^[0-9a-f]+ <.*>:$/ { f = ($2 == "<kickos_rp2350_node_park>:"); n += f; next }
        !f || !/^[ \t]*[0-9a-f]+:/ { next }
        { t = insn($0); m = t; sub(/ .*/, "", m) }
        m ~ /^(bl|blx|bx)$/ || (m ~ /^b/ && t !~ /<kickos_rp2350_node_park[+>]/) { print "the park leaves: " t }
        END { if (!n) print "no kickos_rp2350_node_park body" }' "$D"
}
ctl_rp_node() {
    awk 'NR == 1 { $2 = "00000001" } 1' "$W" > "$TMP/w.p"
    W="$TMP/w.p"
}

rule_a53_pmcr() {
    awk "$AWKLIB"'
        /^[0-9a-f]+ <.*>:$/ { f = ($2 == "<kickos_armv8a_percore_init>:"); n += f; next }
        !f || !/^[ \t]*[0-9a-f]+:/ { next }
        { t = insn($0); gsub(/ x[0-9]+/, " x", t); body = body t ";" }
        t ~ /^(b\.|cbn?z |tbn?z )/ { print "a conditional branch: " t }
        t ~ /^msr pmcr_el0,/ { w++ }
        END {
            if (!n) print "no kickos_armv8a_percore_init body"
            if (w != 1) print w + 0 " PMCR_EL0 write(s)"
            if (index(body, "mrs x, pmcr_el0;and x, x, #0xfffffffffffffff7;mov x, #0x45;orr x, x, x;msr pmcr_el0, x;") == 0)
                print "PMCR_EL0 is not written as read, D cleared, E C LC set"
        }' "$D"
}
ctl_a53_pmcr() {
    sed 's/#0x45/#0x5/' "$D" > "$TMP/d.p"
    D="$TMP/d.p"
}

rule_arm64_entry() {
    order _start "" '^msr spsel,' '^(mov|add|sub|and|orr|eor|mvn) sp,'
    for _s in arch_timer_disarm kickos_armv8a_percore_init; do
        order "$_s" '^msr cntp_ctl_el0,' '^isb' '^(b|bl|blr|br|str|strb|strh|stp|stlr|stlrb|stlrh)( |$)'
    done
}
ctl_arm64_entry() {
    grep -v "${TAB}isb" "$D" > "$TMP/d.p" || true
    D="$TMP/d.p"
}

rule_c6_hp() {
    awk '
        /^[0-9a-f]+ <.*>:$/ { f = ($2 == "<Reset_Handler>:"); n += f; next }
        !f || !/^[ \t]*[0-9a-f]+:/ { next }
        { k++ }
        !c && /[ \t]jal[ \t][^<]*<[^>]*mtime_rate_init[^>]*>/ { c = k }
        !a && /<__init_array_(start|end)>/ { a = k }
        END {
            if (!n) print "no Reset_Handler body"
            else if (!c || !a || c > a) print "mtime_rate_init called at #" c + 0 ", the constructors reached at #" a + 0
        }' "$D"
}
ctl_c6_hp() {
    grep -v 'mtime_rate_init[^>]*>$' "$D" > "$TMP/d.p"
    D="$TMP/d.p"
}

rule_c6_lp() {
    awk '$NF == "SystemCoreClock" { t = $(NF - 1) }
        END { if (t !~ /^[DdGg]$/) print "SystemCoreClock is a [" t "] symbol, not loaded data" }' "$N"
    awk '$2 ~ /^0+$/ { print "SystemCoreClock loads as 0" } END { if (!NR) print "no SystemCoreClock word" }' "$W"
}
ctl_c6_lp() {
    sed 's/ [DdGg] SystemCoreClock$/ B SystemCoreClock/' "$N" > "$TMP/n.p"
    N="$TMP/n.p"
}

rule_ctor() {
    awk -v ps="$(addr_of __init_array_start)" -v pe="$(addr_of __init_array_end)" \
        -v as="$(addr_of __kickos_app_init_array_start)" -v ae="$(addr_of __kickos_app_init_array_end)" \
        "$AWKLIB"'
        BEGIN { if (ps == "" || pe == "" || as == "" || ae == "") print "a constructor window bound is missing" }
        /^Linker script and memory map/ { on = 1; next }
        !on { next }
        pend != "" { name = pend; pend = ""; a = $1; s = $2; o = $3 }
        pend == "" && /^ \.init_array/ {
            if (NF == 1) { pend = $1; next }
            name = $1; a = $2; s = $3; o = $4
        }
        name == "" || hex(s) == 0 { name = ""; next }
        {
            x = hex(a)
            e = x + hex(s)
            if (o ~ /libkickos_(kernel|arch_[a-z0-9]+|chip_[a-z0-9]+|lib)\.a|kickos_string\.dir\//)
                print o " is KickOS-owned and holds a constructor"
            if (!((x >= hex(ps) && e <= hex(pe)) || (x >= hex(as) && e <= hex(ae))))
                print o ": " name " at " a " is in neither constructor window"
            name = ""
        }' "$X"
}
ctl_ctor() {
    cp "$X" "$TMP/m.p"
    printf ' .init_array    0x%s        0x4 libkickos_kernel.a(planted.cc.obj)\n' \
        "$(addr_of __kickos_app_init_array_start)" >> "$TMP/m.p"
    X="$TMP/m.p"
}

rule_cpu_id() {
    awk '$NF ~ /^_*(arch_cpu_id|Z11arch_cpu_idv)$/ { print "a one-core image defines " $NF }' "$N"
}
ctl_cpu_id() {
    { cat "$N"; echo "00000000 00000004 T arch_cpu_id"; } > "$TMP/n.p"
    N="$TMP/n.p"
}

judge() { # <rule>
    "rule_$RULE" > "$TMP/found"
    if [ -s "$TMP/found" ]; then
        sed 's/^/    /' "$TMP/found" >&2
        fail "$ELF breaks rule $RULE (see the header of $0)"
    fi
    ( "ctl_$RULE"; "rule_$RULE" ) > "$TMP/planted"
    [ -s "$TMP/planted" ] || fail "rule $RULE reads its planted defect as clean, so its verdict
  on $ELF is no evidence"
    echo "PASS: $ELF holds rule $RULE, and its planted copy is refused:"
    sed 's/^/    /' "$TMP/planted"
}

case "$RULE" in
    rv64_wx)
        image_sections "$RE" "$ELF"
        image_words "$OD" "$ELF" 8 -j .mmu_boot -j .mmu_leaves > "$W" ;;
    rv32_trap)
        X="${1:?rv32_trap takes the arch archive}"
        _lim="${2:?rv32_trap takes the chip's chip_limits.h}"
        SLOTS="$(sed -n 's/^#define[[:space:]]\{1,\}KICKOS_MAX_IRQ[[:space:]]\{1,\}\([0-9]\{1,\}\).*$/\1/p' "$_lim")"
        [ -n "$SLOTS" ] || fail "no KICKOS_MAX_IRQ in $_lim"
        tool_out "$TMP/rel" "file format" "$OD" -r "$X"
        X="$TMP/rel" ;;
    rp_node)
        X="${1:?rp_node takes the generated chip_layout.h}"
        _a="$(addr_of g_node_isr_vector)"
        _z="$(awk '$4 == "g_node_isr_vector" { print $2 }' "$N")"
        [ -n "$_a" ] && [ -n "$_z" ] || fail "no sized g_node_isr_vector in $ELF"
        image_words "$OD" "$ELF" 4 --start-address="0x$_a" \
            --stop-address="$(printf '0x%x' $((0x$_a + 0x$_z)))" > "$W" ;;
    c6_lp)
        _a="$(addr_of SystemCoreClock)"
        : > "$W"
        if [ -n "$_a" ] && awk '$NF == "SystemCoreClock" && $(NF - 1) ~ /^[DdGg]$/ { f = 1 }
            END { exit !f }' "$N"; then
            image_words "$OD" "$ELF" 4 --start-address="0x$_a" \
                --stop-address="$(printf '0x%x' $((0x$_a + 4)))" > "$W"
        fi ;;
    ctor)
        X="$ELF.map"
        [ -r "$X" ] || fail "no link map at $X" ;;
    rv64_gp | a53_pmcr | arm64_entry | c6_hp | cpu_id) ;;
    *) fail "unknown rule '$RULE'. $_usage" ;;
esac
judge
