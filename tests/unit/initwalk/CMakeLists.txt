# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The init's walk (system/init/compose) against a scripted kernel, over tables the tool emits from
# the golden compositions, each board's default composition and the walk's own fixtures, each
# table compiled under a name of its own. The walk's sources are COMPILED IN over the fakes of
# fake_kernel.cc, never linked from kickos_init_compose or kickos_user. lib/strerror.cc is left out:
# the composed init carries no prose errno table, so a call to kos_strerror fails this link.
#
# HOST-ONLY: fake_kernel.cc defines public kos_* names.

set(_systems golden_arm64 golden_x86 golden_xmc alarm chain chain_ends driver_restart init_priority wide
    default_esp32c6_wroom default_f411disco default_frdmk64f default_qemu_arm64 default_qemu_riscv64
    default_qemu_x86_64 default_xmc4800_relax)
set(_requests "systems=${CMAKE_CURRENT_BINARY_DIR}/systems.cc")
set(_outputs "${CMAKE_CURRENT_BINARY_DIR}/systems.cc")
foreach(_system IN LISTS _systems)
  set(_table "${CMAKE_CURRENT_BINARY_DIR}/table_${_system}.c")
  list(APPEND _requests "${_system}=${_table}")
  list(APPEND _outputs "${_table}")
  set_source_files_properties("${_table}" PROPERTIES COMPILE_DEFINITIONS
    "kickos_table=kickos_table_${_system};kickos_link_one_system_target=kickos_link_one_system_target_${_system}")
endforeach()
list(JOIN _requests "," _requests)
file(GLOB _tool CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/compose/kickos_compose/*.py")
file(GLOB_RECURSE _platform CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/platform/*.yaml")
file(GLOB _compositions CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/composition/systems/*.yaml"
     "${PROJECT_SOURCE_DIR}/boards/*/composition.yaml")
add_custom_command(
  OUTPUT ${_outputs}
  COMMAND "${CMAKE_COMMAND}" -DSOURCE=${PROJECT_SOURCE_DIR}
          -DSTATE=${CMAKE_CURRENT_BINARY_DIR}/compose "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/emit_systems.py"
          "-DFIXTURES=${_requests}" -P "${PROJECT_SOURCE_DIR}/tests/unit/grantlookup/emit_fixture.cmake"
  DEPENDS "${PROJECT_SOURCE_DIR}/tests/unit/grantlookup/emit_fixture.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/emit_systems.py"
          "${PROJECT_SOURCE_DIR}/tools/compose/tests/test_arms.py"
          "${PROJECT_SOURCE_DIR}/tools/compose/pyproject.toml" "${PROJECT_SOURCE_DIR}/tools/compose/uv.lock"
          ${_tool} ${_platform} ${_compositions}
  COMMENT "Emitting the init walk tests' systems"
  VERBATIM)

kickos_add_unit_test(NAME init_walk
  SOURCES walk_rules.cc walk_cost.cc fake_rules.cc driver_rules.cc harness.cc drivers.cc fake_kernel.cc ${_outputs}
          "${PROJECT_SOURCE_DIR}/system/init/compose/walk.cc"
          "${PROJECT_SOURCE_DIR}/system/init/compose/driver_path.cc"
          "${PROJECT_SOURCE_DIR}/system/init/compose/message.cc"
          "${PROJECT_SOURCE_DIR}/user/src/driver_service.cc"
          "${PROJECT_SOURCE_DIR}/user/src/init_status.cc"
          "${PROJECT_SOURCE_DIR}/user/src/task_trampoline.cc"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/system/init/compose"
           "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/lib/include"
           "${PROJECT_BINARY_DIR}/generated/include")
