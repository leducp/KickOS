# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_preset_key(<out> <board> <variant> <own-image> <node>)
#   The name a per-preset gate registers under. CMake names no preset, so the root CMakeLists
#   builds it from the configured facts and tests/static/preset_boards.cmake from the presets'.
function(kickos_preset_key out board variant own_image node)
  set(_key "${board}")
  if(NOT variant STREQUAL "base")
    string(APPEND _key "-${variant}")
  endif()
  # Own-image only: the shared-image posture carries a Kconfig default node index too.
  if(own_image)
    string(APPEND _key "-n${node}")
  endif()
  set(${out} "${_key}" PARENT_SCOPE)
endfunction()
