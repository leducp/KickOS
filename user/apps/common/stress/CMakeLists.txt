# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(stress main.cc)
target_link_libraries(stress PRIVATE kickos)
kickos_emit_image(stress)

# Endurance soak: repeat the conservation round forever (heartbeat per round, freeze
# on the first FAIL). OFF keeps the single-round CTest-gate behaviour byte-identical.
option(KICKOS_STRESS_FOREVER "stress: loop the conservation round forever (soak)" OFF)
if(KICKOS_STRESS_FOREVER)
  target_compile_definitions(stress PRIVATE KICKOS_STRESS_FOREVER)
endif()
