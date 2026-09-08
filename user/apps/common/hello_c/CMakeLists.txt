# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The plain-C twin of apps/hello: same demo, kos_* API instead of kos::. The source must stay
# .c: this is the fleet's only C translation unit.
kickos_add_app_target(hello_c main.c)
target_link_libraries(hello_c PRIVATE kickos)
kickos_emit_image(hello_c)
