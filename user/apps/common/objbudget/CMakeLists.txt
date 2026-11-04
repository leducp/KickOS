# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(objbudget main.cc)
target_include_directories(objbudget PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
add_executable(objbudget_peer peer.cc)
target_link_libraries(objbudget_peer PRIVATE kickos)
kickos_app_system(objbudget COMPOSITION system.yaml)
kickos_emit_image(objbudget)
