# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A task whose image is image 1, restarted once, each start reading its statics as the image
# holds them.
add_executable(restartwit main.cc)
add_executable(restartwit_task task.cc)
target_link_libraries(restartwit_task PRIVATE kickos)
kickos_app_system(restartwit COMPOSITION system.yaml)
kickos_emit_image(restartwit)
