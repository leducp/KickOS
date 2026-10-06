# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console endpoint's framed arm against the sim's packaged console driver, simcon, which its
# composition names as stdout (see tests/integration/check_sim_conabi.sh).
add_executable(simconabi main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
kickos_app_system(simconabi COMPOSITION system.yaml)
kickos_emit_image(simconabi)
target_include_directories(simconabi PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
