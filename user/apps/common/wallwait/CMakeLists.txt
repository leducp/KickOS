# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Booted under QEMU (tests/integration/gates/wallwait.cmake), its only witness.
add_executable(wallwait main.cc)
kickos_app_system(wallwait COMPOSITION system.yaml)
kickos_emit_image(wallwait)
