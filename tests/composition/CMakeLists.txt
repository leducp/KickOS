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
