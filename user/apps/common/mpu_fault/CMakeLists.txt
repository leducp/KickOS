# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(mpu_fault main.cc)
target_link_libraries(mpu_fault PRIVATE kickos)
kickos_emit_image(mpu_fault)
