# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console reclaim on driver death, against the packaged simcon its composition names as stdout.
# The death knobs are simcon's (system/driver/sim/simcon); the app carries the window-thread one.
add_executable(drvdeath main.cc)
kickos_app_system(drvdeath COMPOSITION system.yaml)
kickos_emit_image(drvdeath)
if(KICKOS_SIMCON_WINDOW_THREAD)
  target_compile_definitions(drvdeath PRIVATE KICKOS_SIMCON_WINDOW_THREAD=1)
endif()
