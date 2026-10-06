# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The terminate handler's report of an uncaught exception, riding `cxxterm`.

if(NOT TARGET cxxterm)
  return()
endif()
kickos_app_judge(cxxterm tests/integration/check_qemu_cxxterm.sh)

kickos_add_qemu_test(TARGET cxxterm
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_cxxterm.sh")

# The control of check_no_eh_runtime.sh: an image that throws links the runtime the read finds.
add_test(NAME ${_tag}_cxxterm_eh_control
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_no_eh_runtime.sh" --control
          "$<TARGET_FILE:cxxterm>.map")
kickos_host_gate(${_tag}_cxxterm_eh_control TIMEOUT 60)
