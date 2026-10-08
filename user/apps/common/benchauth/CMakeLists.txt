# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(benchauth main.cc)
kickos_app_system(benchauth COMPOSITION system.yaml)
target_include_directories(benchauth PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
kickos_emit_image(benchauth)
