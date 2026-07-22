# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Run in script mode (cmake -P) on EVERY build to (re)generate a small TU defining
#   extern "C" char const kickos_build_stamp[]
# with the current build time + git short hash (+ '+' when the working tree is dirty).
#
# Why not __DATE__/__TIME__: those bake into whichever TU is compiled, so on an
# incremental build the banner shows the time that TU last compiled -- NOT the time the
# flashed image was linked. Regenerating this stamp each build (the target has no OUTPUT,
# so it is always out of date) makes the banner reflect the image actually on the chip.
# Requires -DOUT=<file.cc> -DSRC=<source dir>.

# Local time (easier to read than UTC) with the numeric offset appended, e.g.
# "2026-07-22 15:44:04 +0200".
string(TIMESTAMP _t "%Y-%m-%d %H:%M:%S")
string(TIMESTAMP _z "%z")
# Idiomatic build id: tag-relative if a tag exists, else the abbreviated commit hash
# (--always), with a "-dirty" suffix when the working tree has uncommitted changes.
set(_g "nogit")
execute_process(
    COMMAND git -C "${SRC}" describe --dirty --always
    OUTPUT_VARIABLE _d OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    RESULT_VARIABLE _rc)
if(_rc EQUAL 0 AND NOT _d STREQUAL "")
    set(_g "${_d}")
endif()

# Two symbols: the build time (its own banner line) and the commit (a dedicated line).
# Always rewrite -- a fresh timestamp each build is the whole point (image relinks, banner
# stays truthful about what is on the chip).
file(WRITE "${OUT}"
    "extern \"C\" char const kickos_build_time[] = \"${_t} ${_z}\";\n"
    "extern \"C\" char const kickos_build_commit[] = \"${_g}\";\n")
