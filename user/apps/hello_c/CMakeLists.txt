# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The plain-C twin of apps/hello: same demo, kos_* API instead of kos::. Built
# alongside hello so the two sit side by side as the dual-API reference.
kickos_add_application(hello_c
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
