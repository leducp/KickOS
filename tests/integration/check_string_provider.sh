#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every mem*/str* function an image links has one provider, the one its rule names, read from the
# link map: the inputs its LOAD lines name and the archive members it lists taken.
#   - no two archives of one link define the same mem*/str* name, since the link order would
#     then pick the copy;
#   - no KickOS archive defines one;
#   - kickos: the kickos_string object is in the link, and no member taken from an archive
#     defines a name it defines;
#   - libc: no KickOS object defines one either, so each comes from the libc.
#
#   check_string_provider.sh <nm> kickos|libc --images <list>
#   check_string_provider.sh --self-test <cc> <ar> <nm>
#
# A relative LOAD path is resolved against the list's directory, the build tree each link ran in.
# The map's headings are matched in English, as check_no_libnosys.sh's are.

set -u
. "$(dirname "$0")/../lib/gate.sh"

NAME_ERE='^_?(mem|str)[A-Za-z0-9_]*$'

# A path normalised by text, `dir/./x` and `dir/sub/../x` as the toolchain's LOAD lines spell
# them, for the awk programs below.
NORM_AWK='
    function norm(p,    n, i, k, part, out, s) {
        n = split(p, part, "/"); k = 0
        for (i = 1; i <= n; i++) {
            if (part[i] == "." || (part[i] == "" && i > 1)) { continue }
            if (part[i] == ".." && k > 0 && out[k] != "..") { k--; continue }
            out[++k] = part[i]
        }
        s = out[1]
        for (i = 2; i <= k; i++) { s = s "/" out[i] }
        return s
    }'

# <map> <base> <inputs> <taken>: the inputs the map LOADs, absolute, one per line, and each
# archive member it lists taken as `<archive> <member>`. Status 2 when either was not read.
map_read() {
    awk -v base="$2" "$NORM_AWK"'
        /^LOAD / {
            p = substr($0, 6)
            if (p !~ /\.(a|o|obj)$/) { next }
            if (p !~ /^\//) { p = base "/" p }
            print norm(p)
        }' "$1" | sort -u > "$3"
    awk -v base="$2" "$NORM_AWK"'
        /^Archive member included/ { on = 1; next }
        on && /^(Discarded input sections|Memory Configuration|Allocating common symbols)/ { exit }
        on && /^[^ \t]/ && match($0, /^[^(]*\.a\([^)]*\)/) {
            s = substr($0, 1, RLENGTH)
            i = index(s, ".a(")
            a = substr(s, 1, i + 1)
            m = substr(s, i + 3, length(s) - i - 3)
            if (a !~ /^\//) { a = base "/" a }
            print norm(a) "\t" m
        }' "$1" | sort -u > "$4"
    [ -s "$3" ] || return 2
    grep -q "libkickos_kernel\.a$TAB" "$4" || return 2
    return 0
}

# <map> <base> <rule>: prints each finding, status 1 when there is one. A tool that could not read
# an input fails the run. Each input is read once per run, into $TMP/alldefs.
judge_map() {
    map_read "$1" "$2" "$TMP/inputs" "$TMP/taken" \
        || fail "$1 lists no LOAD input or no kernel archive member taken, so it was not read"
    # x86_64's link removes the relocation object it built the image's directory from.
    grep -vxF -- "${1%.map}.krel.o" "$TMP/inputs" > "$TMP/inputs.on" || true
    grep -vxFf "$TMP/read" "$TMP/inputs.on" > "$TMP/inputs.new" || true
    while IFS= read -r _p; do
        [ -f "$_p" ] || fail "$1 loads $_p, which is not on disk"
        "$NM" -A "$_p" > "$TMP/nm.out" 2> "$TMP/nm.err" \
            || fail "nm could not read $_p: $(head -n 1 "$TMP/nm.err")"
        awk -v f="$_p" -v re="$NAME_ERE" '
            NF >= 3 && $(NF - 1) ~ /^[A-TV-Z]$/ && $NF ~ re {
                n = split($1, part, ":")
                member = "-"
                if (n >= 3) { member = part[n - 1] }
                print f "\t" member "\t" $NF
            }' "$TMP/nm.out" >> "$TMP/alldefs"
        printf '%s\n' "$_p" >> "$TMP/read"
    done < "$TMP/inputs.new"
    awk -F "$TAB" 'FNR == NR { on[$0] = 1; next } $1 in on' "$TMP/inputs.on" "$TMP/alldefs" \
        | sort -u > "$TMP/defs"
    [ -s "$TMP/defs" ] || fail "$1: no input defines any mem*/str* name, so nm read nothing"

    awk -F "$TAB" -v rule="$3" '
        FNR == NR { taken[$1 "\t" $2] = 1; next }
        {
            file = $1; member = $2; name = $3
            ar = (file ~ /\.a$/)
            base = file; sub(/.*\//, "", base)
            if (ar) {
                if (!(name in arch1)) { arch1[name] = file }
                else if (arch1[name] != file && !((name, file) in seen2)) {
                    seen2[name, file] = 1
                    print "  " name ": offered by two archives, " arch1[name] " and " file
                }
                if (base ~ /^libkickos_/) {
                    print "  " name ": offered by the KickOS archive " file "(" member ")"
                }
                if ((file "\t" member) in taken) { from[name] = file "(" member ")" }
            } else if (file ~ /\/kickos_string(\.dir)?\//) {
                own[name] = file
                if (rule == "libc") {
                    print "  " name ": defined by the KickOS object " file ", where the libc provides it"
                }
            }
        }
        END {
            n = 0
            for (k in own) { n++ }
            if (rule == "kickos" && n == 0) {
                print "  no kickos_string object is in the link, so a libc copy answers the kernel"
            }
            for (k in own) {
                if (k in from) { print "  " k ": taken from " from[k] " beside the kickos_string object" }
            }
        }' "$TMP/taken" "$TMP/defs" > "$TMP/finding"
    if [ -s "$TMP/finding" ]; then
        cat "$TMP/finding"
        return 1
    fi
    return 0
}

scratch_dir
: > "$TMP/read"
: > "$TMP/alldefs"

if [ "${1:-}" = --self-test ]; then
    [ "$#" -eq 4 ] || fail "usage: check_string_provider.sh --self-test <cc> <ar> <nm>"
    CC="$2"; AR="$3"; NM="$4"
    W="$TMP/w"
    mkdir -p "$W/kernel" "$W/libc" "$W/CMakeFiles/kickos_string.dir/libc"
    # <dir> <archive> <member>:<function>...: an archive whose members each define one function.
    archive() {
        _ar_dir="$1"; _ar_name="$2"; shift 2
        _ar_objs=""
        for _ar_spec in "$@"; do
            _ar_m="${_ar_spec%%:*}"
            printf 'int %s(void) { return 0; }\n' "${_ar_spec#*:}" > "$TMP/src.c"
            "$CC" -fno-builtin -c -o "$_ar_dir/$_ar_m" "$TMP/src.c" || fail "$CC could not compile a control"
            _ar_objs="$_ar_objs $_ar_m"
        done
        # shellcheck disable=SC2086
        (cd "$_ar_dir" && "$AR" rc "$_ar_name" $_ar_objs) || fail "$AR could not archive a control"
    }
    archive "$W/kernel" libkickos_kernel.a kmain.o:kmain
    archive "$W/kernel" libkickos_lib.a string.cc.obj:memset fmt.cc.obj:kvsnprintf
    archive "$W/kernel" libkickos_clean.a fmt.cc.obj:kvsnprintf
    archive "$W/libc" libc.a libc_a-memset.o:memset libc_a-strcmp.o:strcmp
    printf 'int memset(void) { return 0; }\n' > "$TMP/src.c"
    "$CC" -fno-builtin -c -o "$W/CMakeFiles/kickos_string.dir/libc/string.cc.obj" "$TMP/src.c" \
        || fail "$CC could not compile a control"
    # <name> <list of LOAD lines> <list of taken members>: a planted map and its one-line image list.
    plant() {
        {
            echo 'Archive member included to satisfy reference by file (symbol)'
            echo
            echo 'kernel/libkickos_kernel.a(kmain.o)'
            echo '                              main.o (kmain)'
            for _pl_t in $3; do
                echo "$_pl_t"
                echo '                              main.o (memset)'
            done
            echo
            echo 'Discarded input sections'
            echo
            for _pl_l in $2; do
                echo "LOAD $_pl_l"
            done
        } > "$W/$1.map"
        echo "$W/$1.map" > "$W/$1.list"
    }
    # <name> <rule> <expected text> <what>: the planted map is refused with that text.
    refuses() {
        if sh "$0" "$NM" "$2" --images "$W/$1.list" > "$TMP/run.out" 2>&1; then
            fail "the $2 rule passed $4"
        fi
        grep -qF -- "$3" "$TMP/run.out" \
            || fail "the $2 rule refused $4 without '$3': $(cat "$TMP/run.out")"
    }
    passes() {
        sh "$0" "$NM" "$2" --images "$W/$1.list" > "$TMP/run.out" 2>&1 \
            || fail "the $2 rule refused $3: $(cat "$TMP/run.out")"
    }
    KOBJ=CMakeFiles/kickos_string.dir/libc/string.cc.obj
    plant twin "kernel/libkickos_kernel.a kernel/libkickos_lib.a libc/libc.a" \
        "kernel/libkickos_lib.a(string.cc.obj)"
    refuses twin libc "memset: offered by two archives" "an image whose memset two archives offer"
    refuses twin kickos "memset: offered by two archives" "an image whose memset two archives offer"
    refuses twin libc "memset: offered by the KickOS archive" "a KickOS archive offering memset"
    plant libc "kernel/libkickos_kernel.a kernel/libkickos_clean.a libc/libc.a" \
        "libc/libc.a(libc_a-memset.o)"
    passes libc libc "an image taking memset from the libc alone"
    refuses libc kickos "no kickos_string object is in the link" \
        "an image whose kernel takes the libc's memset"
    plant own "$KOBJ kernel/libkickos_kernel.a kernel/libkickos_clean.a libc/libc.a" ""
    passes own kickos "an image taking memset from the kickos_string object"
    refuses own libc "defined by the KickOS object" "a KickOS object defining memset"
    plant both "$KOBJ kernel/libkickos_kernel.a libc/./libc.a" "libc/libc.a(libc_a-memset.o)"
    refuses both kickos "taken from $W/libc/libc.a(libc_a-memset.o) beside" \
        "an image taking a libc memset beside the object"
    plant unread "kernel/libkickos_kernel.a libc/libc.a" ""
    sed '/libkickos_kernel\.a(kmain\.o)/d' "$W/unread.map" > "$W/unread.tmp" \
        && mv "$W/unread.tmp" "$W/unread.map"
    refuses unread libc "was not read" "a map whose member list holds no kernel member"
    plant absent "kernel/libkickos_kernel.a libc/missing.a" ""
    refuses absent libc "not on disk" "a map loading an input that is not on disk"
    : > "$W/empty.list"
    if sh "$0" "$NM" libc --images "$W/empty.list" > "$TMP/run.out" 2>&1; then
        fail "an empty image list passed"
    fi
    echo "PASS: two archives offering one name, a KickOS archive offering one, a missing object"
    echo "  under the kickos rule, an object under the libc rule and a libc member beside the"
    echo "  object are each refused; an unread map, a missing input and an empty list fail"
    exit 0
fi

[ "$#" -eq 4 ] && [ "$3" = --images ] \
    || fail "usage: check_string_provider.sh <nm> kickos|libc --images <list>"
NM="$1"
RULE="$2"
case "$RULE" in
    kickos|libc) ;;
    *) fail "rule '$RULE' is neither kickos nor libc" ;;
esac
[ -f "$4" ] || fail "no image list at $4"
BASE="$(cd "$(dirname "$4")" && pwd)"
grep -v '^$' "$4" > "$TMP/maps"
[ -s "$TMP/maps" ] || fail "$4 names no image"
COUNT=0
BAD=0
while IFS= read -r MAP; do
    [ -f "$MAP" ] || fail "no link map at $MAP"
    COUNT=$((COUNT + 1))
    if ! judge_map "$MAP" "$BASE" "$RULE" > "$TMP/out"; then
        echo "$(basename "$MAP"):" >&2
        cat "$TMP/out" >&2
        BAD=$((BAD + 1))
    fi
done < "$TMP/maps"
if [ "$BAD" -ne 0 ]; then
    fail "$BAD of $COUNT image(s) take a mem*/str* function from other than the $RULE provider"
fi
echo "PASS: each of $COUNT image(s) takes every mem*/str* function from the $RULE provider alone"
