# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(cxxterm main.cc)
target_link_libraries(cxxterm PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(cxxterm)

# The same throw in a worker of a task of its own.
add_executable(cxxterm_worker worker.cc)
target_include_directories(cxxterm_worker PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
add_executable(cxxterm_worker_peer peer.cc)
target_link_libraries(cxxterm_worker_peer PRIVATE kickos)
kickos_app_system(cxxterm_worker COMPOSITION system.yaml)
kickos_emit_image(cxxterm_worker)
