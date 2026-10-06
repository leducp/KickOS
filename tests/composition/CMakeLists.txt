# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The task sources of examples/composition, compiled as written against the user API and never
# linked: no system target is built here. sensor_cmos.cc reads ports with x86 instructions.
set(_sources app.cc health.cc sensor_rtc.cc sensor_spi.cc)
if(KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND _sources sensor_cmos.cc)
endif()
list(TRANSFORM _sources PREPEND "${PROJECT_SOURCE_DIR}/examples/composition/")
add_library(composition_example_sources OBJECT ${_sources})
target_link_libraries(composition_example_sources PRIVATE KickOS::kernel)
target_compile_options(composition_example_sources PRIVATE ${KICKOS_WARN_FLAGS})

# kickos_image_leaves answers the in-tree spellings: the kernel leaf by its alias, linked
# directly and through an INTERFACE library, and the old leaf.
add_library(composition_through_kernel INTERFACE)
target_link_libraries(composition_through_kernel INTERFACE KickOS::kernel)
add_library(composition_through_leaf INTERFACE)
target_link_libraries(composition_through_leaf INTERFACE kickos)
kickos_image_leaves(composition_example_sources _direct)
kickos_image_leaves(composition_through_kernel _through)
kickos_image_leaves(composition_through_leaf _leaf)
if(NOT _direct STREQUAL "kernel" OR NOT _through STREQUAL "kernel" OR NOT _leaf STREQUAL "kickos")
  message(FATAL_ERROR "kickos_image_leaves read '${_direct}', '${_through}' and '${_leaf}', not "
                      "'kernel', 'kernel' and 'kickos'")
endif()
