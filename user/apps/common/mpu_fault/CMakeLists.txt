# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(mpu_fault main.cc)
target_link_libraries(mpu_fault PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(mpu_fault)
