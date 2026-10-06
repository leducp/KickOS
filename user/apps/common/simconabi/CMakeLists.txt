# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console endpoint's framed arm against the sim's published driver. The driver it
# exercises is system/driver/sim/simcon/simcon.cc, which is host-libc by construction (see
# tests/integration/check_sim_conabi.sh).
add_executable(simconabi main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
target_link_libraries(simconabi PRIVATE kickos)
kickos_emit_image(simconabi)
target_include_directories(simconabi PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
