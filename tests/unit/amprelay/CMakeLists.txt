# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# system/service/amprelay against a scripted kernel: its main and its writes come in under
# prefixed names, so the gate's own main and the host libc's write stay apart from them.
# A fortified host glibc marks write warn_unused_result, and the rename carries that onto the relay.
set(_relay "${PROJECT_SOURCE_DIR}/system/service/amprelay/amprelay.c")
set_source_files_properties("${_relay}" DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_DEFINITIONS "main=amprelay_main;write=amprelay_write"
             COMPILE_OPTIONS "-U_FORTIFY_SOURCE")
kickos_add_unit_test(NAME amprelay
  SOURCES relay.cc "${_relay}"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/lib/include"
           "${PROJECT_BINARY_DIR}/generated/include")
