# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(budgetwit main.cc)
target_include_directories(budgetwit PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
add_executable(budgetwit_peer peer.cc)
target_link_libraries(budgetwit_peer PRIVATE kickos)
kickos_app_system(budgetwit COMPOSITION system.yaml)
kickos_emit_image(budgetwit)
