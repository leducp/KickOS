# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The blocks that remap or rewrite the flash task code runs from, per chip, each followed by the
# errno a spawn naming a 0x1000 device window at it answers. tests/unit/grantaperture holds each
# answer against the chip's own tables.
set(KICKOS_CODE_MAP_esp32c6 0x60002000 EINVAL 0x60003000 EINVAL 0x600C8000 EINVAL)
set(KICKOS_CODE_MAP_rp2350 0x400C8000 EINVAL 0x400D0000 EINVAL)
set(KICKOS_CODE_MAP_imxrt1062 0x402A8000 EINVAL 0x403EC000 EINVAL)
set(KICKOS_CODE_MAP_mk64f 0x4001F000 EINVAL 0x40020000 EINVAL)
set(KICKOS_CODE_MAP_rx72m 0x007FE000 EINVAL)

# kickos_code_map(<chip> <blocks-out> <answers-out>): the chip's blocks and answers as two
# comma-separated C initialiser lists, both empty when the chip names none.
function(kickos_code_map chip blocks_out answers_out)
  set(_blocks "")
  set(_answers "")
  set(_pairs ${KICKOS_CODE_MAP_${chip}})
  list(LENGTH _pairs _n)
  set(_i 0)
  while(_i LESS _n)
    math(EXPR _j "${_i} + 1")
    list(GET _pairs ${_i} _block)
    list(GET _pairs ${_j} _answer)
    list(APPEND _blocks "${_block}u")
    list(APPEND _answers "-KOS_${_answer}")
    math(EXPR _i "${_i} + 2")
  endwhile()
  list(JOIN _blocks "," _blocks)
  list(JOIN _answers "," _answers)
  set(${blocks_out} "${_blocks}" PARENT_SCOPE)
  set(${answers_out} "${_answers}" PARENT_SCOPE)
endfunction()
