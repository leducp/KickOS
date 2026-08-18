# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Admission gate for the S7 non-cacheable memory type, over the real kernel/grant/grant.cc.
# The include path and definitions come from the kernel target VERBATIM: grant.cc must
# compile exactly as the shipping build compiles it. That build sets KICKOS_HAVE_MPU=1;
# the enforcement-off posture is a header inline and not this gate's subject.
kickos_add_unit_test(NAME grant_nocache
  SOURCES nocache_admission.cc
          "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)
