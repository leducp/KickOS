# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(rootauth main.cc)
target_include_directories(rootauth PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
kickos_app_system(rootauth COMPOSITION system.yaml)
kickos_emit_image(rootauth)
