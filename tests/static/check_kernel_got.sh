#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What an archive holding kernel text may not reference, where a translating backend splits
# the image in two. Two legs over the same archives.
#
# NO GOT REFERENCE (readelf -rW). A static link has ONE .got, a GOT slot is reached by adrp
# like anything else, and the halves are 2^40 apart: a .got with users in both cannot be placed
# at all. virt_arm64.ld gives it to the app, so the kernel side must want none. Under the small
# code model a WEAK EXTERN is what emits one, and whether one is emitted is a property of the
# toolchain rather than of the source. The code model is scoped per TU (kickos_split_image_tu,
# cmake/kickos.cmake) and this leg is the only thing that says the list is still complete: a
# reach across the halves fails the link loudly, while a GOT reference is silent until some
# later change forces the .got to be placed. The fix is that TU's source path added to the
# kickos_split_image_tu() call for the target that built the archive, never a relaxation here
# and never a de-typed declaration: a weak extern is the chip<->kernel contract for a window a
# chip may not carve.
#
# NO NAME OF THE APP'S RUNTIME (nm). memcpy, memset, strlen and the formatter are called from
# BOTH halves, and a global symbol has one value: kernel-side, and EL0's call is a high address
# it cannot reach; app-side, and the kernel calls text that carries privileged-execute-never.
# So the kernel links its own copies under the kickos/kruntime.h names. Most kernel-side
# references have no call in the .cc at all: the COMPILER emits them, from `ThreadAttr attr;`
# or `*d = Domain{}`, and kickos_privatise_runtime() rewrites them after `ar` through the maps
# cmake/kernel_runtime*.syms. A hit on a mapped name means the rewrite did not reach this
# archive; the fix is a kickos_privatise_runtime() call for the target that built it, or the
# missing name in the map, never a de-typed construction. The names refused are every map's
# left column, plus RUNTIME below.
#
# SCOPE: the archives that hold kernel text (kernel, arch, chip). kickos_lib is app-side and
# NOT scanned, and a region backend serves both privilege levels from one text mapping and has
# no split to protect.
#
# TWO PROPERTIES OF readelf's OUTPUT ARE LOAD-BEARING, and both are checked rather than
# assumed.
#
#   -W       without it readelf truncates the type column to 17 characters, and
#            R_AARCH64_LD64_GOT_LO12_NC comes out as R_AARCH64_LD64_GO, which does not hold
#            the substring this gate matches on: half the GOT relocation types would go
#            unseen. The wide column header is what says the flag took effect, so its
#            absence is refused. The match is a SUBSTRING for the same reason: the other
#            half, R_AARCH64_ADR_GOT_PAGE, truncates to R_AARCH64_ADR_GOT, so a pattern
#            anchored on a full relocation name matches nothing.
#
#   LC_ALL=C the host binutils is localised. Under a French locale readelf prints
#            `Fichier:` where `File:` is parsed, while leaving the TYPE column untranslated,
#            so the hits are still reported and only the part naming WHICH member is
#            silently empty. A hit no `File:` line attributes is therefore refused in its
#            own right.
#
# usage: check_kernel_got.sh <readelf> <nm> <archive>...

set -eu
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -lt 3 ]; then
    echo "usage: $0 <readelf> <nm> <archive>..." >&2
    exit 2
fi

READELF="$1"
NM="$2"
shift 2
command -v "$READELF" >/dev/null 2>&1 || fail "readelf not found: $READELF"
command -v "$NM" >/dev/null 2>&1 || fail "nm not found: $NM"

scratch_dir

GOT_ERE='_GOT'
WIDE_LIT="Symbol's Name"

# The app runtime's names, closed and independent of the maps, so a name dropped from a map is
# still refused while the rewrite of it stops. The ones no map carries no compiler emits or have
# no kernel twin: a hit is an explicit call to correct in the source, or a toolchain change worth
# a human reading. The RX ABI prefixes every C identifier with an underscore, so both spellings.
RUNTIME='memcpy memset memmove memcmp strlen strnlen bcmp bzero __memcpy_chk __memset_chk
kvsnprintf ksnprintf __clzdi2 __ctzdi2'

SYMS_DIR="$(dirname "$0")/../../cmake"
: > "$TMP/banned"
for _m in "$SYMS_DIR"/kernel_runtime*.syms; do
    [ -r "$_m" ] || fail "no runtime map under $SYMS_DIR, so the nm leg refuses nothing"
    awk '!/^#/ && NF == 2 { print $1 }' "$_m" >> "$TMP/banned"
done
grep -qx 'memset' "$TMP/banned" \
    || fail "cmake/kernel_runtime.syms maps no memset, so the map was misread and a name it
      adds beyond RUNTIME is not refused"
for _b in $RUNTIME; do
    printf '%s\n_%s\n' "$_b" "$_b" >> "$TMP/banned"
done
BANNED="$(sort -u "$TMP/banned" | tr '\n' ' ')"

# `File: <archive>(<member>)` is what attributes a record to its object. The record ends
# `<name> + <addend>`, so the name is three fields from the end.
scan_rel() { # <relocation-output> <archive-label> <hits-append>
    grep -qF "$WIDE_LIT" "$1" \
        || fail "the relocation dump for $2 carries no wide column header, so the type column
      is truncated to 17 characters and R_AARCH64_LD64_GOT_LO12_NC comes out as
      R_AARCH64_LD64_GO, which no longer holds the substring this gate matches. Pass -W, and
      keep LC_ALL=C so the header is not translated instead."
    awk -v arch="$2" -v got="$GOT_ERE" '
    /^File: / {
        member = $2
        sub(/^.*\(/, "", member)
        sub(/\)$/, "", member)
        next
    }
    $1 ~ /^[0-9a-f]+$/ && $3 ~ /^R_/ {
        total++
        if ($3 !~ got) { next }
        name = "?"
        if (NF >= 7) { name = $(NF - 2) }
        if (member == "") { printf "NOFILE %s %s %s\n", arch, $3, name }
        printf "GOT %s(%s) %s %s\n", arch, member, $3, name
    }
    END { printf "RELS %d\n", total + 0 }' "$1" >> "$3"
}

# nm -A prefixes each line with `<path>:<member>:`, and an UNDEFINED symbol has no address, so
# the record is exactly three fields: prefix, `U` (or `w`, which binds the same way), name. The
# member is the LAST colon-separated field, so a build path holding a colon does not shift it.
scan_refs() { # <symfile> <archive label> <hits-append>
    awk -v ban="$BANNED" -v arch="$2" '
    BEGIN {
        n = split(ban, b, " ")
        for (i = 1; i <= n; i++) { bad[b[i]] = 1 }
    }
    NF == 3 && ($2 == "U" || $2 == "w") {
        total++
        if (!($3 in bad)) { next }
        loc = $1
        sub(/:$/, "", loc)
        k = split(loc, p, ":")
        printf "RUNTIME %s(%s) %s\n", arch, p[k], $3
    }
    END { printf "REFS %d\n", total + 0 }' "$1" >> "$3"
}

# --- the planted controls, one per leg ----------------------------------------
# Forged tool output in the tools' own record shapes, so what they prove is the reader, never
# the invocation. Each holds one reference to report beside its near miss, and the answer is
# compared whole: a hit attributed to the wrong member or symbol is a wrong answer too.
{
    printf 'File: libkickos_kernel.a(aspace.cc.obj)\n\n'
    printf "Relocation section '.rela.text' at offset 0x100 contains 2 entries:\n"
    printf "    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend\n"
    printf '0000000000000010  0000006e0000011b R_AARCH64_LD64_GOT_LO12_NC 0000000000000000 kickos_chip_window + 0\n'
    printf '0000000000000014  0000006e00000113 R_AARCH64_ADR_PREL_PG_HI21 0000000000000000 kos_got_table + 0\n'
} > "$TMP/ctl.rel"
cat > "$TMP/ctl.sym" <<'EOF'
weird:path/libkickos_kernel.a:task.cc.obj:                 U memset
libkickos_kernel.a:kstring.cc.obj:0000000000000000 T memset
libkickos_kernel.a:tls.cc.obj:                 U kmemset
EOF
: > "$TMP/ctl.hits"
scan_rel "$TMP/ctl.rel" libkickos_kernel.a "$TMP/ctl.hits"
scan_refs "$TMP/ctl.sym" libkickos_kernel.a "$TMP/ctl.hits"
_want='GOT libkickos_kernel.a(aspace.cc.obj) R_AARCH64_LD64_GOT_LO12_NC kickos_chip_window
RELS 2
RUNTIME libkickos_kernel.a(task.cc.obj) memset
REFS 2'
[ "$(cat "$TMP/ctl.hits")" = "$_want" ] || {
    sed 's/^/      /' "$TMP/ctl.hits" >&2
    fail "the readers answered the above for the planted dumps rather than one GOT reference
      in aspace.cc.obj and one memset in task.cc.obj, so a real reference would go unreported
      or be blamed on the wrong object"
}

# --- the archives -------------------------------------------------------------
: > "$TMP/hits"
for A in "$@"; do
    [ -f "$A" ] || fail "archive not found: $A"
    # Positive control: every archive here carries relocated code, so a run that saw no
    # relocation record or no text symbol read nothing.
    tool_out "$TMP/rel" '^[0-9a-f]+[[:space:]]+[0-9a-f]+[[:space:]]+R_' "$READELF" -rW "$A"
    scan_rel "$TMP/rel" "$(basename "$A")" "$TMP/hits"
    tool_out "$TMP/sym" '[[:space:]][TtWw][[:space:]]' "$NM" -A "$A"
    scan_refs "$TMP/sym" "$(basename "$A")" "$TMP/hits"
done

rels="$(awk '/^RELS /{ t += $2 } END { print t + 0 }' "$TMP/hits")"
refs="$(awk '/^REFS /{ t += $2 } END { print t + 0 }' "$TMP/hits")"
[ "$rels" -gt 0 ] || fail "no relocation record in any of the $# archive(s) given: wrong
      readelf, wrong files, or a link model this gate cannot read (guard would pass vacuously)"
[ "$refs" -gt 0 ] || fail "no undefined symbol in any of the $# archive(s) given: wrong nm,
      wrong files, or a link model this gate cannot read (guard would pass vacuously)"

rc=0
if grep -q '^NOFILE ' "$TMP/hits"; then
    awk '/^NOFILE /{ printf "        %s emits %s against %s\n", $2, $3, $4 }' "$TMP/hits" >&2
    bad "a GOT reference was found and no File: line attributes it to a member, so the object
      that emits it cannot be named. Either LC_ALL is not C and readelf printed a translated
      header, or the input is not an archive."
fi
if grep -q '^GOT ' "$TMP/hits"; then
    awk '/^GOT /{ printf "        %s emits %s against %s\n", $2, $3, $4 }' "$TMP/hits" >&2
    bad "archive(s) holding kernel text emit GOT references. A static link has one .got and it
      belongs to the app's half, which kernel text may not reach. Add the object's source to
      the kickos_split_image_tu() call for the target that built this archive."
fi
if grep -q '^RUNTIME ' "$TMP/hits"; then
    awk '/^RUNTIME /{ printf "        %s names %s\n", $2, $3 }' "$TMP/hits" >&2
    bad "archive(s) holding kernel text name the APP's runtime. Kernel text runs from the
      privileged half and may not call app text, which carries privileged-execute-never.
      Either the target that built this archive has no kickos_privatise_runtime() call
      (cmake/kickos.cmake), or the name is missing from cmake/kernel_runtime.syms, or it is
      an explicit call in the source that should read the kickos/kruntime.h name instead."
fi
[ "$rc" -eq 0 ] || exit 1

echo "PASS: $# archive(s) holding kernel text, $rels relocation record(s) with no GOT reference,
  $refs undefined reference(s) with none to the app runtime"
