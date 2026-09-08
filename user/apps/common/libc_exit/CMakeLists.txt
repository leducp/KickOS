# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_app_target(libc_exit main.cc)
target_link_libraries(libc_exit PRIVATE kickos)
kickos_emit_image(libc_exit)
