# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The plain-C twin of apps/hello: same demo, kos_* API instead of kos::. SOURCES must stay
# .c: this is the fleet's only C translation unit.
kickos_add_application(hello_c
  SOURCES main.c
  BOARD   ${KICKOS_BOARD})
