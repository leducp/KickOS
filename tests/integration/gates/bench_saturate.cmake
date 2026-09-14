# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The phase table's saturation rule, witnessed on every emulator-capable board that builds the
# bench image. The target exists only under KICKOS_BENCH (user/apps/common/CMakeLists.txt), and
# kickos_add_qemu_test registers nothing for a board with no emulator, so the silicon bench
# boards declare themselves out without a name list here.
if(NOT TARGET bench)
  return()
endif()

# Longer than the default: the phase table prints after the whole call/reply sweep.
kickos_add_qemu_test(NAME ${_tag}_bench_saturate TARGET bench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_bench_saturate.sh"
  TIMEOUT 300)

# Serial above one kernel core, for the reason the gates beside it are: the sweep is a quarter
# of a million round trips on four emulated cores and the poll bound is wall clock, so a peer
# competing for host CPU turns a correct image red.
if(KICKOS_KERNEL_CORES GREATER 1)
  if(TEST ${_tag}_bench_saturate)
    set_tests_properties(${_tag}_bench_saturate PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()
