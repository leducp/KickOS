#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The x86_64 PE32+ UEFI image link, run as ld's PE+ emulation with no compiler driver.
#
#   x86_64-link.sh [--one-pass] <ld> <readelf> <objdump> <objcopy>
#                  <link arguments...>
#
# Takes the compiler-driver spelling of a link: -Wl,a,b reaches ld as `a b`; -o, -L, -l and
# input files pass; any other argument, and a response file, is refused, since no driver is
# here to consume it. Each -l is resolved here to the archive path ld is then handed, so that
# tools/check-x86_64-no-got.sh and tools/check-x86_64-weak-undef.sh read exactly the files ld
# links. The inputs are first pre-linked into one ELF object with --gc-sections, which the PE
# emulation does not apply. An application image is then linked twice so that it carries a copy
# of its own relocation directory (tools/x86_64-krel.sh), and <image>.map beside it names every
# input as the two maps do together (tools/x86_64-map.awk). --one-pass is a single link, which
# writes no map. A failed link leaves neither file.
set -u

nl='
'
out=""

fail() {
    echo "FAIL: x86_64-link: $*" >&2
    if [ -n "$out" ]; then
        rm -f "$out" "$out.map"
    fi
    exit 1
}

cleanup() {
    if [ -n "$out" ]; then
        rm -f "$out.first" "$out.krel.bin" "$out.krel.o" "$out.pre.ld" "$out.pre.o" \
            "$out.premap" "$out.pemap"
    fi
}
trap cleanup EXIT

one_pass=0
if [ "${1:-}" = "--one-pass" ]; then
    one_pass=1
    shift
fi
[ $# -ge 4 ] || fail "usage: x86_64-link.sh [--one-pass] <ld> <readelf> <objdump> <objcopy>" \
    "<args...>"
LD="$1"; READELF="$2"; OBJDUMP="$3"; OBJCOPY="$4"
shift 4
HERE="$(cd "$(dirname "$0")" && pwd)"

# The map's LOAD lines must name every input by an absolute path.
absolute() {
    case "$1" in
        /*) printf '%s' "$1" ;;
        *) printf '%s/%s' "$(pwd)" "$1" ;;
    esac
}

# One token per word: A<argument>, or W<word> for each word of a -Wl, argument.
toks=""
for a in "$@"; do
    case "$a" in
        *"$nl"*) fail "refusing an argument that holds a newline" ;;
        @*) fail "refusing '$a': a response file is not read here" ;;
        -Wl,*)
            old_ifs="$IFS"
            IFS=','
            set -f
            for w in ${a#-Wl,}; do
                toks="${toks}W$w$nl"
            done
            set +f
            IFS="$old_ifs"
            ;;
        *) toks="${toks}A$a$nl" ;;
    esac
done

# ld reads every -L before it searches for any -l, wherever each one stands.
libdirs=""
pending=""
old_ifs="$IFS"
IFS="$nl"
set -f
for t in $toks; do
    v="${t#?}"
    if [ "$pending" = "L" ]; then
        libdirs="$libdirs$(absolute "$v")$nl"
        pending=""
        continue
    fi
    pending=""
    case "$v" in
        -L) pending="L" ;;
        -L*) libdirs="$libdirs$(absolute "${v#-L}")$nl" ;;
        -o|-l) pending="skip" ;;
    esac
done
[ "$pending" != "L" ] || fail "-L names no directory"

# resolve <spec>: the file -l<spec> names, the first in -L order.
resolve() {
    case "$1" in
        :*) want="${1#:}" ;;
        *) want="lib$1.a" ;;
    esac
    for d in $libdirs; do
        if [ -f "$d/$want" ]; then
            printf '%s' "$d/$want"
            return 0
        fi
    done
    return 1
}

# The pre-link (rargs) takes the inputs, their grouping, the symbols that root or rename their
# resolution, and --wrap, which must apply once; the PE link (pargs) takes every other argument
# and the pre-link's one object.
rargs=""
pargs=""
inputs=""
pe_script=""
pending=""
for t in $toks; do
    kind="${t%"${t#?}"}"
    v="${t#?}"
    case "$pending" in
        o)
            out="$(absolute "$v")"
            pending=""
            continue
            ;;
        L)
            pending=""
            continue
            ;;
        l)
            v="-l$v"
            pending=""
            ;;
        r)
            rargs="$rargs$v$nl"
            pending=""
            continue
            ;;
        b)
            rargs="$rargs$v$nl"
            pargs="$pargs$v$nl"
            pending=""
            continue
            ;;
        T)
            pe_script="$(absolute "$v")"
            pargs="$pargs$pe_script$nl"
            pending=""
            continue
            ;;
    esac
    case "$v" in
        -o) pending="o"; continue ;;
        -L) pending="L"; continue ;;
        -L*) continue ;;
        -l) pending="l"; continue ;;
        -l*)
            f="$(resolve "${v#-l}")" || fail "$v: no such archive under the -L directories"
            rargs="$rargs$f$nl"
            inputs="$inputs$f$nl"
            continue
            ;;
    esac
    if [ "$kind" = "W" ]; then
        case "$v" in
            --start-group|--end-group|-\(|-\)|--wrap=*) rargs="$rargs$v$nl" ;;
            --wrap) rargs="$rargs$v$nl"; pending="r" ;;
            -u|--undefined|--require-defined) rargs="$rargs$v$nl"; pargs="$pargs$v$nl"; pending="b" ;;
            -u?*|--undefined=*|--require-defined=*) rargs="$rargs$v$nl"; pargs="$pargs$v$nl" ;;
            -T) pargs="$pargs$v$nl"; pending="T" ;;
            *) pargs="$pargs$v$nl" ;;
        esac
        continue
    fi
    case "$v" in
        -*) fail "refusing '$v': not a linker argument (spell a linker flag -Wl,<flag>)" ;;
        *.o|*.obj|*.a)
            f="$(absolute "$v")"
            rargs="$rargs$f$nl"
            inputs="$inputs$f$nl"
            ;;
        *) pargs="$pargs$(absolute "$v")$nl" ;;
    esac
done
[ -z "$pending" ] || fail "-$pending names nothing"
[ -n "$out" ] || fail "no -o"
[ -n "$pe_script" ] || fail "no -T names the PE section script"
rm -f "$out" "$out.map"

# The pre-link's own script, from the PE script's kernel claims: each archive or object
# selector line becomes part of a .kickos.kernel.* section the PE script then places by name.
# The app's constructors and the unwind tables are kept whole, the collection dropping an
# orphan .eh_frame entirely.
prelink_script="$out.pre.ld"
awk '
    /^ *(KEEP \()?\*[^ ]*(\.a:|\.dir\/)\*\(/ {
        if ($0 ~ /\.init_array/) { init = init $0 "\n" }
        else if ($0 ~ /\(\.text /) { text = text $0 "\n" }
        else if ($0 ~ /\(\.data /) { data = data $0 "\n" }
        else if ($0 ~ /\(\.rodata /) { rodata = rodata $0 "\n" }
        else if ($0 ~ /\(\.bss /) { bss = bss $0 "\n" }
        else { print "unclassified kernel claim: " $0 > "/dev/stderr"; bad = 1 }
    }
    END {
        if (bad || text == "") { exit 1 }
        print "SECTIONS\n{"
        printf "  .kickos.kernel.text : {\n%s  }\n", text
        printf "  .kickos.kernel.data : {\n%s  }\n", data
        printf "  .kickos.kernel.init_array : {\n%s  }\n", init
        printf "  .kickos.kernel.rodata : {\n%s  }\n", rodata
        printf "  .kickos.kernel.bss : {\n%s  }\n", bss
        print "  .init_array : { KEEP (*(SORT(.init_array.*))) KEEP (*(.init_array)) }"
        print "  .fini_array : { KEEP (*(SORT(.fini_array.*))) KEEP (*(.fini_array)) }"
        print "  .eh_frame : { KEEP (*(.eh_frame .eh_frame.*)) }"
        print "}"
    }
' "$pe_script" > "$prelink_script" || fail "no kernel claim in $pe_script to pre-link by"

# shellcheck disable=SC2086
set -- $inputs
IFS="$old_ifs"
"$HERE/check-x86_64-no-got.sh" "$READELF" "$@" || fail "the global-offset-table guard refused"
"$HERE/check-x86_64-weak-undef.sh" "$READELF" "$@" || fail "the weak-undefined guard refused"

# The relocatable pre-link collects the garbage, which the PE emulation does not.
premap=""
if [ "$one_pass" -eq 0 ]; then
    premap="-Map$nl$out.premap$nl"
fi
IFS="$nl"
# shellcheck disable=SC2086
set -- $premap $rargs
IFS="$old_ifs"
# -d places a COMMON symbol by its input's claim, so a kernel one cannot
# reach the PE script's *(COMMON) in the app window.
LC_ALL=C "$LD" -m elf_x86_64 -r --force-group-allocation -d \
    --gc-sections -e efi_main -T "$prelink_script" \
    -o "$out.pre.o" "$@" || fail "ld refused the pre-link"
IFS="$nl"
set -f
# shellcheck disable=SC2086
set -- $pargs "$out.pre.o"
IFS="$old_ifs"
set +f

# --subsystem=10 makes the file an EFI APPLICATION, entered at efi_main on the Microsoft x64
# convention. --no-insert-timestamp keeps the image byte-identical across builds of one tree.
# An --image-base among the arguments overrides this one, which is how a rebased image links.
# -b names the INPUT format: binutils 2.42's ld under this emulation reads an ELF archive's
# symbol index and still extracts no member for an undefined symbol. The KickOS toolchain's 2.47
# extracts either way, so only CI's probe reports whether the flag is load-bearing.
# --discard-locals drops the assembler's temporary labels, which the pre-link keeps.
FIXED="-m i386pep --subsystem=10 --image-base=0x400000 -e efi_main --no-insert-timestamp"
FIXED="$FIXED -b elf64-x86-64 --discard-locals"

if [ "$one_pass" -eq 1 ]; then
    # shellcheck disable=SC2086
    LC_ALL=C "$LD" $FIXED -o "$out" "$@" || fail "ld refused the link"
    exit 0
fi

# The second link places the copy in .krel, after every section a fixup can sit in, so .reloc
# comes out the same; the check refuses an image where it did not.
# LC_ALL=C: the map is read by its English headings.
# shellcheck disable=SC2086
LC_ALL=C "$LD" $FIXED -o "$out.first" "$@" || fail "ld refused the first link"
"$HERE/x86_64-krel.sh" extract "$OBJDUMP" "$OBJCOPY" "$out.first" "$out.krel.bin" \
    || fail "no relocation directory to copy"
"$OBJCOPY" -I binary -O elf64-x86-64 -B i386:x86-64 \
    --rename-section .data=.krel,alloc,load,readonly,data,contents \
    "$out.krel.bin" "$out.krel.o" || fail "objcopy refused the relocation copy"
# shellcheck disable=SC2086
LC_ALL=C "$LD" $FIXED -Map "$out.pemap" -o "$out" "$@" "$out.krel.o" \
    || fail "ld refused the second link"
awk -v pre="$out.pre.o" -f "$HERE/x86_64-map.awk" "$out.premap" "$out.pemap" > "$out.map" \
    || fail "could not write the link map"
"$HERE/x86_64-krel.sh" check "$OBJDUMP" "$OBJCOPY" "$out.first" "$out" \
    || fail "the second link moved the relocation directory"
