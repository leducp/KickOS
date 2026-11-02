# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A chip that holds one task runs the arms without the peer's.
add_executable(objbudget main.cc)
target_include_directories(objbudget PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
if(KICKOS_CHIP_ONE_TASK)
  target_link_libraries(objbudget PRIVATE KickOS::kickos KickOS::system_default)
else()
  add_executable(objbudget_peer peer.cc)
  target_link_libraries(objbudget_peer PRIVATE kickos)
  kickos_app_system(objbudget COMPOSITION system.yaml)
endif()
kickos_emit_image(objbudget)
