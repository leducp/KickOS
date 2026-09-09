# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The arena-ownership table, over the REAL kernel/mem/ramown.cc. The include path and
# definitions come from the kernel target VERBATIM: the table must be sized by the
# KICKOS_RAM_OWNER_SLOTS the shipping build carries, or the exhaustion arm would witness a
# ceiling no board has. That build sets KICKOS_HAVE_MPU=1; the unenforced posture is a
# header inline and not this gate's subject.
kickos_add_unit_test(NAME ram_owner
  SOURCES owner_table.cc
          "${PROJECT_SOURCE_DIR}/kernel/mem/ramown.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
