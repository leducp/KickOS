# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

foreach(_case 1 2 3 4 5)
  kickos_add_app_target(panicgate${_case} main.cc)
  target_link_libraries(panicgate${_case} PRIVATE kickos)
  kickos_emit_image(panicgate${_case})
  target_compile_definitions(panicgate${_case} PRIVATE KICKOS_PANICGATE_CASE=${_case})
endforeach()
