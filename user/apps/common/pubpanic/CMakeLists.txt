# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Terminal reporting on a published console: one source, two images (kos_panic, and an
# illegal instruction through the arch fault reporter). Case 2 traps via __builtin_trap and
# the published posture is only automatable on the host.
foreach(_case 1 2)
  kickos_add_app_target(pubpanic${_case} main.cc)
  target_link_libraries(pubpanic${_case} PRIVATE kickos)
  kickos_emit_image(pubpanic${_case})
  target_compile_definitions(pubpanic${_case} PRIVATE KICKOS_PUBPANIC_CASE=${_case})
endforeach()
