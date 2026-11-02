# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TRANSLATING AND ABOVE ONE CORE, both load-bearing: the space count it reads exists only where
# a space can be installed on a core at all, and a peer core is the whole hazard.
add_executable(taskleave main.cc)
target_include_directories(taskleave PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
add_executable(taskleave_peer peer.cc)
target_link_libraries(taskleave_peer PRIVATE kickos)
kickos_app_system(taskleave COMPOSITION system.yaml)
kickos_emit_image(taskleave)
