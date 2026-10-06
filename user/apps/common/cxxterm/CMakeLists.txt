# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(cxxterm main.cc)
target_link_libraries(cxxterm PRIVATE kickos_cxx)
kickos_emit_image(cxxterm)
# The map check_no_eh_runtime.sh --control reads; x86_64's link rule writes <image>.map itself.
if(NOT KICKOS_ARCH STREQUAL "x86_64")
  target_link_options(cxxterm PRIVATE "-Wl,-Map=$<TARGET_FILE:cxxterm>.map")
  set_property(TARGET cxxterm APPEND PROPERTY ADDITIONAL_CLEAN_FILES "$<TARGET_FILE:cxxterm>.map")
endif()
