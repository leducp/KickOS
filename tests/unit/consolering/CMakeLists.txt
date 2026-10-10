# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_unit_test(NAME byte_ring
  SOURCES byte_ring.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")

# user/src/console_ring.cc at both CRLF postures.
# HOST-ONLY: console_write.cc defines public kos_* names.
foreach(_crlf "raw;0" "crlf;1")
  list(GET _crlf 0 _crlf_name)
  list(GET _crlf 1 _crlf_value)
  kickos_add_unit_test(NAME console_write_${_crlf_name} TEST_PREFIX "${_crlf_name}."
    SOURCES console_write.cc "${PROJECT_SOURCE_DIR}/user/src/console_ring.cc"
    INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
             "${PROJECT_BINARY_DIR}/generated/include"
    POSTURE KICKOS_CONSOLE_CRLF=${_crlf_value})
endforeach()
