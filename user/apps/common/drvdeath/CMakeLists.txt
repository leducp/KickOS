# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console reclaim on driver death. The published posture plus a driver that exits on cue is
# only automatable on the host (see tests/integration/check_sim_drvdeath.sh).
kickos_add_app_target(drvdeath main.cc)
target_link_libraries(drvdeath PRIVATE kickos)
kickos_emit_image(drvdeath)

# Case 3 of the gate: the console driver is TWO threads and the second one holds the
# register window. The same knob the service list reads (system/CMakeLists.txt); the app
# needs it because the deferred-reclaim assertion and the cancel that ends it live here.
if(KICKOS_SIMCON_WINDOW_THREAD)
  target_compile_definitions(drvdeath PRIVATE KICKOS_SIMCON_WINDOW_THREAD=1)
endif()
