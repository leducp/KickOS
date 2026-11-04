# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console endpoint's framed arm against the sim's packaged console driver, simcon, which its
# composition names as stdout (see tests/integration/check_sim_conabi.sh). The arm is the program
# simconabi_main, and the image simconabi its system's.
add_executable(simconabi_main main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
target_link_libraries(simconabi_main PRIVATE kickos)
target_include_directories(simconabi_main PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
add_executable(simconabi)
kickos_app_system(simconabi COMPOSITION system.yaml)
kickos_emit_image(simconabi)
