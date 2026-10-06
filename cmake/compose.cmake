# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_compose(<system> <composition.yaml> [EXPORT_NAME <name>])
# kickos_compose(<system> PARTITION <node0.yaml> <node1.yaml>... [EXPORT_NAME <name>])
#   Turns a composition into a system target (docs/design-m10-target.md, section 5), or with
#   PARTITION the composition of this build's node, admitted with every node's (docs/
#   design-m10-fleet.md, section 9.1). At configure
#   it runs the host tool against KICKOS_MANIFEST, the package's manifest or the build's, and a
#   refusal fails the configure with the tool's `<file>:<line>: <rule>: <message>` lines. The
#   tool runs again only when the composition, the manifest, a description beside it or the tool
#   itself changed. It defines:
#     <system>_table  an object library of the emitted table, compiled as C;
#     <system>        an interface library linking KickOS::kernel, carrying ahead of the kernel's
#                     group the table's objects, KickOS::init's, KickOS::init_drivers' where
#                     the composition names a packaged driver and KickOS::init_no_drivers'
#                     where it names none, KickOS::main's where the
#                     composition names kickos_main, each named packaged driver's archive and
#                     the libraries its CLIENT declares for the tasks using it, the
#                     link-time asserts script as a link input, and KICKOS_USER_HEAP_SIZE, a
#                     link symbol and a compile definition, from the composition's `heap`; on node 0 of a partition, or on a chip stating a
#                     partition gate, the gate assignment's object too.
#   EXPORT_NAME installs both under KickOS::<name> and KickOS::<name>_table.
#
#   Requires uv on PATH, which runs the tool under a Python of 3.12 or newer with the ruamel.yaml
#   its uv.lock pins, in one environment per build tree.

# The host tool: tools/compose in the source tree, the compose folder an installed package
# carries beside this file.
if(KICKOS_IN_TREE)
  get_filename_component(_kickos_compose_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/compose" ABSOLUTE)
else()
  set(_kickos_compose_tool "${CMAKE_CURRENT_LIST_DIR}/compose")
endif()
set_property(GLOBAL PROPERTY KICKOS_COMPOSE_TOOL "${_kickos_compose_tool}")

function(kickos_compose system)
  cmake_parse_arguments(KC "" "EXPORT_NAME" "PARTITION" ${ARGN})
  set(_named ${KC_UNPARSED_ARGUMENTS})
  if(KC_PARTITION)
    if(_named)
      message(FATAL_ERROR "kickos_compose(${system}): unexpected arguments ${_named} beside PARTITION")
    endif()
    set(_named ${KC_PARTITION})
  else()
    list(LENGTH _named _count)
    if(NOT _count EQUAL 1)
      message(FATAL_ERROR "kickos_compose(${system}): takes one composition, or PARTITION and the node "
        "compositions in node order, not '${_named}'")
    endif()
  endif()
  get_property(_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
  if(NOT "C" IN_LIST _languages)
    message(FATAL_ERROR "kickos_compose(${system}): the project does not enable C, which the "
      "emitted table is compiled as. Name C in project(... LANGUAGES ...).")
  endif()
  get_property(_tool GLOBAL PROPERTY KICKOS_COMPOSE_TOOL)
  set(_compositions "")
  foreach(_named_one IN LISTS _named)
    get_filename_component(_one "${_named_one}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_one}")
      message(FATAL_ERROR "kickos_compose(${system}): no composition at ${_one}")
    endif()
    list(APPEND _compositions "${_one}")
  endforeach()
  list(GET _compositions 0 _composition)
  set(_what "${_composition}")
  set(_source_args "${_composition}")
  if(KC_PARTITION)
    string(REPLACE ";" " " _what "the partition ${_compositions}")
    set(_source_args --partition ${_compositions})
  endif()
  if(NOT KICKOS_MANIFEST OR NOT EXISTS "${KICKOS_MANIFEST}")
    message(FATAL_ERROR "kickos_compose(${system}): KICKOS_MANIFEST names no manifest "
      "('${KICKOS_MANIFEST}'). find_package(KickOS) sets it to the package's; the kernel build "
      "sets it once the manifest is written.")
  endif()
  if(NOT EXISTS "${_tool}/uv.lock")
    message(FATAL_ERROR "kickos_compose(${system}): no host tool at ${_tool}")
  endif()

  get_filename_component(_manifest_dir "${KICKOS_MANIFEST}" DIRECTORY)
  file(GLOB_RECURSE _descriptions CONFIGURE_DEPENDS "${_manifest_dir}/platform/*.yaml")
  file(GLOB_RECURSE _tool_sources CONFIGURE_DEPENDS "${_tool}/kickos_compose/*.py")
  set(_inputs ${_compositions} "${KICKOS_MANIFEST}" ${_descriptions} ${_tool_sources}
              "${_tool}/pyproject.toml" "${_tool}/uv.lock")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  set(_hashes "")
  foreach(_input IN LISTS _inputs)
    file(SHA256 "${_input}" _input_hash)
    string(APPEND _hashes "${_input} ${_input_hash}\n")
  endforeach()
  string(SHA256 _hash "${_hashes}")

  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/kickos_compose/${system}")
  set(_outputs table.c asserts.ld system.cmake)
  set(_recorded "")
  if(EXISTS "${_dir}/inputs.sha256")
    file(READ "${_dir}/inputs.sha256" _recorded)
  endif()
  set(_stale FALSE)
  if(NOT _recorded STREQUAL _hash)
    set(_stale TRUE)
  endif()
  foreach(_file IN LISTS _outputs)
    if(NOT EXISTS "${_dir}/${_file}")
      set(_stale TRUE)
    endif()
  endforeach()

  if(_stale)
    find_program(KICKOS_UV uv)
    if(NOT KICKOS_UV)
      message(FATAL_ERROR "kickos_compose(${system}): uv not found on PATH; the host tool runs "
        "under uv (https://docs.astral.sh/uv/)")
    endif()
    set(_fresh "${_dir}/emit")
    file(REMOVE_RECURSE "${_fresh}")
    file(REMOVE "${_dir}/inputs.sha256")
    file(MAKE_DIRECTORY "${_fresh}/tmp")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env
              "UV_PROJECT_ENVIRONMENT=${CMAKE_BINARY_DIR}/kickos_compose/venv"
              UV_PYTHON_DOWNLOADS=never "PYTHONPATH=${_tool}" PYTHONDONTWRITEBYTECODE=1
              "TMPDIR=${_fresh}/tmp"
              "${KICKOS_UV}" run --project "${_tool}" --locked --quiet
              python -m kickos_compose emit ${_source_args} --manifest "${KICKOS_MANIFEST}"
              -o "${_fresh}/table.c" --asserts "${_fresh}/asserts.ld"
              --fragment "${_fresh}/system.cmake" --gate "${_fresh}/gate.c"
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err
      TIMEOUT 600)
    # Indented, so CMake prints each of the tool's lines as it wrote it.
    string(STRIP "${_out}\n${_err}" _said)
    string(REPLACE "\n" "\n  " _said "${_said}")
    # The tool exits 3 when it refused the composition, as kickos_compose/__main__.py's REFUSED
    # states; any other failure is the tool not running.
    if(_rc STREQUAL "3")
      message(FATAL_ERROR "kickos_compose(${system}): the host tool refused ${_what} "
        "against ${KICKOS_MANIFEST}:\n  ${_said}")
    elseif(NOT _rc STREQUAL "0")
      message(FATAL_ERROR "kickos_compose(${system}): could not run the host tool (uv and Python >= 3.12 "
        "required, with the ruamel.yaml its uv.lock pins) on ${_what} (${_rc}):\n  ${_said}")
    endif()
    if(NOT _said STREQUAL "")
      message(STATUS "kickos_compose(${system}): the host tool says:\n  ${_said}")
    endif()
    foreach(_file IN LISTS _outputs)
      file(COPY_FILE "${_fresh}/${_file}" "${_dir}/${_file}" ONLY_IF_DIFFERENT)
    endforeach()
    if(EXISTS "${_fresh}/gate.c")
      file(COPY_FILE "${_fresh}/gate.c" "${_dir}/gate.c" ONLY_IF_DIFFERENT)
    else()
      file(REMOVE "${_dir}/gate.c")
    endif()
    file(WRITE "${_dir}/inputs.sha256" "${_hash}")
    message(STATUS "kickos_compose(${system}): emitted ${_what}")
  else()
    message(STATUS "kickos_compose(${system}): ${_what} and its inputs unchanged")
  endif()
  include("${_dir}/system.cmake")
  if(NOT KICKOS_COMPOSE_HEAP MATCHES "^[0-9]+$")
    message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states no heap, which is the "
      "`heap:` of ${_what}")
  endif()

  set(_table_sources "${_dir}/table.c")
  if(KICKOS_COMPOSE_GATE)
    if(NOT EXISTS "${_dir}/gate.c")
      message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states a gate assignment and "
        "${_dir}/gate.c holds none")
    endif()
    list(APPEND _table_sources "${_dir}/gate.c")
  endif()
  add_library(${system}_table OBJECT ${_table_sources})
  # The include directories and definitions an app TU sees.
  target_link_libraries(${system}_table PRIVATE KickOS::kernel)
  add_library(${system} INTERFACE)
  set_target_properties(${system} PROPERTIES KICKOS_SYSTEM_STDOUT "${KICKOS_COMPOSE_STDOUT}")

  set(_table_name ${system}_table)
  set(_asserts "${_dir}/asserts.ld")
  if(KC_EXPORT_NAME)
    set_target_properties(${system}_table PROPERTIES EXPORT_NAME ${KC_EXPORT_NAME}_table)
    set_target_properties(${system} PROPERTIES EXPORT_NAME ${KC_EXPORT_NAME})
    kickos_export_targets(${system}_table ${system})
    kickos_export_name(${system}_table _table_name)
    install(FILES "${_asserts}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos/${KC_EXPORT_NAME}")
    set(_asserts_link
      "$<BUILD_INTERFACE:${_asserts}>"
      "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KC_EXPORT_NAME}/asserts.ld>")
  else()
    set(_asserts_link "${_asserts}")
  endif()

  set(_objects ${_table_name} KickOS::init)
  if(KICKOS_COMPOSE_DRIVERS)
    list(APPEND _objects KickOS::init_drivers)
  else()
    list(APPEND _objects KickOS::init_no_drivers)
  endif()
  if(KICKOS_COMPOSE_MAIN)
    list(APPEND _objects KickOS::main)
  endif()
  set(_archives "")
  foreach(_driver IN LISTS KICKOS_COMPOSE_DRIVERS)
    list(APPEND _archives KickOS::kickos_${_driver})
  endforeach()
  foreach(_client IN LISTS KICKOS_COMPOSE_CLIENTS)
    list(APPEND _archives KickOS::${_client})
  endforeach()
  set(_links "")
  foreach(_o IN LISTS _objects)
    list(APPEND _links "$<TARGET_OBJECTS:${_o}>")
  endforeach()
  # Objects and archives first, so the kernel's group, after them, resolves what they reference.
  target_link_libraries(${system} INTERFACE ${_links} ${_archives} ${_asserts_link} KickOS::kernel)
  kickos_heap_defsym(_heap "${KICKOS_COMPOSE_HEAP}")
  target_link_options(${system} INTERFACE "LINKER:${_heap}")
  target_compile_definitions(${system} INTERFACE KICKOS_USER_HEAP_SIZE=${KICKOS_COMPOSE_HEAP})
  set_property(TARGET ${system} APPEND PROPERTY INTERFACE_LINK_DEPENDS ${_asserts_link})
endfunction()
