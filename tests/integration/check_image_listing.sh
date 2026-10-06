#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The image listing the bench reads (<build>/kickos-images.txt): four fields a row; a judge that
# is a gate script in the tree, an emulator, human or inapplicable mark, or `-`; a human or
# inapplicable mark that says why, the latter on an image no test the build runs names; and an
# image read as `emulator` exactly where a test the build's ctest runs names it, that test being
# neither a `host` gate nor disabled. Those two clauses read ctest's own test list, not the
# configure that wrote the listing.
# --self-test reads planted listings and a stub ctest.
#
#   check_image_listing.sh <listing> <kickos-source> <ctest> <cmake> <build> [<executable suffix>]
#   check_image_listing.sh --self-test

set -u
. "$(dirname "$0")/../lib/gate.sh"

# <kickos-source> <ctest> <cmake> <build> <suffix> <out>: `<image>|<test>` for each absolute path
# the command of a test ctest runs names, less the executable suffix.
run_images() {
    "$2" --test-dir "$4" --show-only=json-v1 -LE '^host$' > "$TMP/tests.json" || return 1
    "$3" "-DJSON=$TMP/tests.json" "-DOUT=$TMP/tests.tsv" "-DIMAGES=$6" "-DSUFFIX=$5" \
        -P "$1/tests/static/ctest_tests.cmake" > "$TMP/tests.err" 2>&1 || return 1
}

# <listing> <kickos-source> <run images>: prints the first defect and returns 1, or the row
# count and returns 0.
read_listing() {
    [ -s "$1" ] || { echo "no image listing at $1"; return 1; }
    _unbuilt="$(sed -n 's/^@none|//p' "$3" | head -n 1)"
    if [ -n "$_unbuilt" ]; then
        echo "ctest gives $_unbuilt no command, so the tree is not built"
        return 1
    fi
    _rows=0
    while IFS= read -r _row; do
        _rows=$((_rows + 1))
        _fields="$(printf '%s\n' "$_row" | awk -F '|' '{ print NF }')"
        if [ "$_fields" -ne 4 ]; then
            echo "row $_rows carries $_fields field(s), not 4: $_row"
            return 1
        fi
        _image="$(printf '%s\n' "$_row" | cut -d '|' -f 1)"
        _judge="$(printf '%s\n' "$_row" | cut -d '|' -f 3)"
        _args="$(printf '%s\n' "$_row" | cut -d '|' -f 4)"
        _run="$(awk -F '|' -v i="$_image" '$1 == i { print $2; exit }' "$3")"
        case "$_judge" in
            -) ;;
            emulator)
                if [ -z "$_run" ]; then
                    echo "$_image reads as emulator-judged and no test this build runs names it"
                    return 1
                fi
                ;;
            emulator-owed)
                if [ -n "$_run" ]; then
                    echo "$_image reads as emulator-owed and $_run runs it"
                    return 1
                fi
                ;;
            human | inapplicable)
                if [ -z "$_args" ]; then
                    echo "$_image is marked $_judge and says nothing of why"
                    return 1
                fi
                if [ "$_judge" = inapplicable ] && [ -n "$_run" ]; then
                    echo "$_image is marked inapplicable and $_run runs it"
                    return 1
                fi
                ;;
            *)
                if [ ! -f "$2/$_judge" ]; then
                    echo "$_image names a judge that is no gate script: $_judge"
                    return 1
                fi
                ;;
        esac
    done < "$1"
    echo "$_rows"
}

if [ "${1:-}" = --self-test ]; then
    scratch_dir
    src="$(cd "$(dirname "$0")/../.." && pwd)"
    cmake="$(command -v cmake)" || fail "no cmake on PATH to read the stub test list with"
    good='hello|kernel|tests/integration/check_qemu_hello.sh|
stackdepth0|kernel|emulator-owed|
blink|kernel|human|LED
rootfault|kernel|inapplicable|memory not enforced
fault|kernel|tests/integration/check_fault_dump.sh|THREAD FAULT
panicgate1|kernel|emulator|
rebootdemo|kernel|emulator|
sysdefault|kernel|-|'
    printf '%s\n' "$good" > "$TMP/good"
    # A stub ctest: panicgate1 and rebootdemo.efi are run by emulator gates; stackdepth0 is named
    # only by a host gate and by a disabled one. KOS_STUB_UNBUILT adds a test ctest gives no
    # command, as it does where the program is not built.
    mkdir -p "$TMP/build"
    cat > "$TMP/ctest" <<'STUB'
#!/bin/sh
unbuilt=''
if [ -n "${KOS_STUB_UNBUILT:-}" ]; then
    unbuilt='{"name": "b_hello", "properties": [{"name": "TIMEOUT", "value": 60}]},'
fi
cat <<JSON
{"tests": [ $unbuilt
 {"name": "b_panicgate1",
   "command": ["/usr/bin/cmake", "-E", "env", "/src/check.sh",
               "/b/user/apps/common/panicgate/panicgate1"],
   "properties": [{"name": "TIMEOUT", "value": 60}]},
 {"name": "b_rebootdemo",
   "command": ["/src/check.sh", "/b/user/apps/common/rebootdemo/rebootdemo.efi"]},
 {"name": "b_stackdepth0_map",
   "command": ["/src/read.sh", "/b/user/apps/common/stackdepth/stackdepth0"],
   "properties": [{"name": "LABELS", "value": ["host"]}]},
 {"name": "b_stackdepth",
   "command": ["/src/check.sh", "/b/user/apps/common/stackdepth/stackdepth0"],
   "properties": [{"name": "DISABLED", "value": true}]}
]}
JSON
STUB
    chmod +x "$TMP/ctest"
    run_images "$src" "$TMP/ctest" "$cmake" "$TMP/build" .efi "$TMP/runs" \
        || fail "the stub test list was not read: $(cat "$TMP/tests.err")"
    read_listing "$TMP/good" "$src" "$TMP/runs" > "$TMP/out" \
        || fail "a sound listing was refused: $(cat "$TMP/out")"
    # <name> <row> <refusal>: the sound listing with <row> added is refused with <refusal>.
    refused() {
        { printf '%s\n' "$good"; printf '%s\n' "$2"; } > "$TMP/$1"
        if read_listing "$TMP/$1" "$src" "$TMP/runs" > "$TMP/out"; then
            fail "a listing with $1 was accepted"
        fi
        grep -qF -- "$3" "$TMP/out" || fail "a listing with $1 was refused as: $(cat "$TMP/out")"
    }
    refused three-fields 'hello_c|kernel|tests/integration/check_qemu_hello.sh' 'not 4'
    refused human-silent 'blink2|kernel|human|' 'says nothing of why'
    refused void-silent 'mpu_fault|kernel|inapplicable|' 'says nothing of why'
    refused directory 'stress|kernel|tests/integration|' 'no gate script'
    refused missing-script 'stress|kernel|tests/integration/check_nothing.sh|' \
        'no gate script'
    refused unrun 'stackdepth1|kernel|emulator|' 'no test this build runs names it'
    refused host-only 'stackdepth0|kernel|emulator|' 'no test this build runs names it'
    refused owed-run 'panicgate1|kernel|emulator-owed|' 'b_panicgate1 runs it'
    refused owed-run-suffixed 'rebootdemo|kernel|emulator-owed|' 'b_rebootdemo runs it'
    refused void-run 'panicgate1|kernel|inapplicable|planted' 'b_panicgate1 runs it'
    KOS_STUB_UNBUILT=1 run_images "$src" "$TMP/ctest" "$cmake" "$TMP/build" .efi "$TMP/runs" \
        || fail "the unbuilt stub test list was not read: $(cat "$TMP/tests.err")"
    if read_listing "$TMP/good" "$src" "$TMP/runs" > "$TMP/out"; then
        fail "a test list with a commandless test was read"
    fi
    grep -qF 'gives b_hello no command' "$TMP/out" \
        || fail "a commandless test was refused as: $(cat "$TMP/out")"
    echo "PASS: a sound listing reads; a short row, a silent mark, a missing or directory judge,"
    echo "  an emulator row no running test names, an owed or inapplicable row a running test"
    echo "  names, with or without the executable suffix, and an unbuilt test list are refused"
    exit 0
fi

_usage="usage: check_image_listing.sh <listing> <kickos-source> <ctest> <cmake> <build> [<suffix>]"
listing="${1:?$_usage}"
src="${2:?$_usage}"
ctest="${3:?$_usage}"
cmake="${4:?$_usage}"
build="${5:?$_usage}"
scratch_dir
run_images "$src" "$ctest" "$cmake" "$build" "${6:-}" "$TMP/runs" \
    || fail "the tests of $build could not be listed: $(tail -n 3 "$TMP/tests.err" 2>/dev/null)"
if ! out="$(read_listing "$listing" "$src" "$TMP/runs")"; then
    fail "$out"
fi
echo "PASS: $out image row(s), each judged by a gate script, a mark or nothing"
exit 0
