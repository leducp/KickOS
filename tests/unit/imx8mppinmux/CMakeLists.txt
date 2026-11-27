# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The i.MX 8M Plus pinmux plan (arch/arm64/chip/imx8mp/pinmux.h): which registers one call writes,
# and what it refuses. imx8mp_mmap.h is forced ahead of every include: a host build generates no
# imx8mp chip header.
kickos_add_unit_test(NAME imx8mppinmux
  SOURCES pinmux.cc
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/system/include")
target_compile_options(imx8mppinmux PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/imx8mp_mmap.h")
