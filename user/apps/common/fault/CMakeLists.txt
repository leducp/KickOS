# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(fault main.cc)
target_link_libraries(fault PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(fault)

# A divide by zero where no fault is isolated and the core traps it, so the dump is the only
# report. Where faults are isolated faultsurvive's divide arms judge it.
if(NOT KICKOS_FAULT_ISOLATION AND KICKOS_ARCH MATCHES "^(armv6m|lx6)$")
  foreach(_width 32 64)
    set(_name fault_div0)
    if(_width EQUAL 64)
      set(_name fault_ldiv0)
    endif()
    add_executable(${_name} main.cc)
    target_compile_definitions(${_name} PRIVATE KICKOS_FAULT_DIVIDE=${_width})
    target_link_libraries(${_name} PRIVATE KickOS::kickos KickOS::system_default)
    kickos_emit_image(${_name})
  endforeach()
endif()
