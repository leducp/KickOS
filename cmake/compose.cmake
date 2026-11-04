# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_compose(<system> <composition.yaml> [EXPORT_NAME <name>] [DEFAULT <target>] [LOCAL_DRIVERS])
# kickos_compose(<system> PARTITION <node0.yaml> <node1.yaml>... [EXPORT_NAME <name>])
#   Turns a composition into a system target (docs/design-m10-target.md, section 5), or with
#   PARTITION the composition of this build's node, admitted with every node's (docs/
#   design-m10-fleet.md, section 9.1). At configure
#   it runs the host tool against KICKOS_MANIFEST, the package's manifest or the build's, and a
#   refusal fails the configure with the tool's `<file>:<line>: <rule>: <message>` lines. The
#   tool runs again only when the composition, the manifest, a description or default composition
#   beside it, or the tool itself changed. It defines:
#     <system>_table  an object library of the emitted table, compiled as C;
#     <system>_init_table  where the init is an image of its own (KICKOS_INIT_IMAGE), the
#                     init image's copy of it, which names no entry and declares no task;
#     <system>_task<k>  there too, for each declaration k, the copy and the image header of
#                     that task's own image, which name its entry alone and declare it alone;
#     <system>        an interface library linking KickOS::kernel, carrying ahead of the kernel's
#                     group the table's objects, or its init copy's and its task copies' where
#                     the init is an image of its own, KickOS::init's,
#                     KickOS::init_drivers' where the composition names a packaged driver and
#                     KickOS::init_no_drivers' where it names none, each named packaged
#                     driver's archive and the libraries its CLIENT declares for the tasks
#                     using it, the
#                     link-time asserts script as a link input, and KICKOS_USER_HEAP_SIZE, a
#                     link symbol and a compile definition, from the composition's `heap`, and
#                     KICKOS_IMAGE_DECL_COUNT, the number of declarations its table emits; on
#                     node 0 of a partition, or on a chip stating a
#                     partition gate, the gate assignment too; and in a kernel link group, the
#                     archive of what the kernel reads of the system.
#   EXPORT_NAME installs them under KickOS::<name>, KickOS::<name>_table and
#   KickOS::<name>_init_table.
#
#   Each `program:` the composition names is an executable target, resolved once the directory
#   ends, so it may be declared after the system, and compiled with its main as main. `program:
#   main` is the executable linking <system>, or with DEFAULT <target>, which the system is then
#   linked into and which kickos_emit_image(<system>) emits. A program other than the executable
#   linking <system> is linked through kickos_program_link, and each image of it replays that
#   link (tools/image-link.py).
#
#   Requires uv on PATH, which runs the tool under a Python of 3.12 or newer with the ruamel.yaml
#   its uv.lock pins, in one environment per build tree.

# The host tool: tools/compose in the source tree, the compose folder an installed package
# carries beside this file. The image header source a declared task's image compiles, beside
# it in the package.
if(KICKOS_IN_TREE)
  get_filename_component(_kickos_compose_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/compose" ABSOLUTE)
  get_filename_component(_kickos_image_header "${CMAKE_CURRENT_LIST_DIR}/../user/src/image_header.c"
                         ABSOLUTE)
else()
  set(_kickos_compose_tool "${CMAKE_CURRENT_LIST_DIR}/compose")
  set(_kickos_image_header "${CMAKE_CURRENT_LIST_DIR}/image_header.c")
endif()
set_property(GLOBAL PROPERTY KICKOS_COMPOSE_TOOL "${_kickos_compose_tool}")
set_property(GLOBAL PROPERTY KICKOS_COMPOSE_IMAGE_HEADER "${_kickos_image_header}")

# kickos_compose_python(<out> <tool> <venv> <tmpdir> [<pythonpath>...])
#   Sets <out> to the command that runs Python under <tool>'s uv.lock in the environment <venv>,
#   with <tool> and each <pythonpath> on PYTHONPATH; `-m kickos_compose ...` or a script follows.
#   Script-safe, so a cmake -P run includes this file for it.
function(kickos_compose_python out tool venv tmpdir)
  find_program(KICKOS_UV uv)
  if(NOT KICKOS_UV)
    message(FATAL_ERROR "KickOS: uv not found on PATH; tools/compose runs under uv (https://docs.astral.sh/uv/)")
  endif()
  string(JOIN ":" _path "${tool}" ${ARGN})
  set(${out} "${CMAKE_COMMAND}" -E env "UV_PROJECT_ENVIRONMENT=${venv}" UV_PYTHON_DOWNLOADS=never
             "PYTHONPATH=${_path}" PYTHONDONTWRITEBYTECODE=1 "TMPDIR=${tmpdir}"
             "${KICKOS_UV}" run --project "${tool}" --locked --quiet python PARENT_SCOPE)
endfunction()

# kickos_compose_inputs(<out> <tool>)
#   Sets <out> to the files a run of <tool> depends on: its modules, pyproject.toml and uv.lock.
function(kickos_compose_inputs out tool)
  file(GLOB _sources CONFIGURE_DEPENDS "${tool}/kickos_compose/*.py")
  set(${out} ${_sources} "${tool}/pyproject.toml" "${tool}/uv.lock" PARENT_SCOPE)
endfunction()

function(kickos_compose system)
  cmake_parse_arguments(KC "LOCAL_DRIVERS" "EXPORT_NAME;DEFAULT" "PARTITION" ${ARGN})
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
  if(NOT "CXX" IN_LIST _languages)
    message(FATAL_ERROR "kickos_compose(${system}): the project does not enable CXX, which the "
      "system's kernel pools are compiled as. Name CXX in project(... LANGUAGES ...).")
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
  set(_manifest "${KICKOS_MANIFEST}")
  if(KC_LOCAL_DRIVERS)
    get_filename_component(_local_dir "${KICKOS_MANIFEST}" DIRECTORY)
    set(_manifest "${_local_dir}/manifest_local.yaml")
  endif()
  if(NOT _manifest OR NOT EXISTS "${_manifest}")
    message(FATAL_ERROR "kickos_compose(${system}): KICKOS_MANIFEST names no manifest "
      "('${_manifest}'). find_package(KickOS) sets it to the package's; the kernel build "
      "sets it once the manifest is written.")
  endif()
  if(NOT EXISTS "${_tool}/uv.lock")
    message(FATAL_ERROR "kickos_compose(${system}): no host tool at ${_tool}")
  endif()

  get_filename_component(_manifest_dir "${_manifest}" DIRECTORY)
  file(GLOB_RECURSE _descriptions CONFIGURE_DEPENDS "${_manifest_dir}/platform/*.yaml"
                                                   "${_manifest_dir}/boards/*.yaml")
  kickos_compose_inputs(_tool_inputs "${_tool}")
  set(_inputs ${_compositions} "${_manifest}" ${_descriptions} ${_tool_inputs})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
  set(_hashes "${KC_EXPORT_NAME}\n")
  foreach(_input IN LISTS _inputs)
    file(SHA256 "${_input}" _input_hash)
    string(APPEND _hashes "${_input} ${_input_hash}\n")
  endforeach()
  string(SHA256 _hash "${_hashes}")

  # An exported system's files cite its composition by a name no machine path is in.
  set(_name_args "")
  if(KC_EXPORT_NAME)
    file(RELATIVE_PATH _display "${_manifest_dir}" "${_composition}")
    if(_display MATCHES "^\\.\\./")
      get_filename_component(_display "${_composition}" NAME)
    endif()
    set(_name_args --name "${_display}")
  endif()

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
    set(_fresh "${_dir}/emit")
    kickos_compose_python(_python "${_tool}" "${CMAKE_BINARY_DIR}/kickos_compose/venv" "${_fresh}/tmp")
    file(REMOVE_RECURSE "${_fresh}")
    file(REMOVE "${_dir}/inputs.sha256")
    file(MAKE_DIRECTORY "${_fresh}/tmp")
    execute_process(
      COMMAND ${_python} -m kickos_compose emit ${_source_args} --manifest "${_manifest}"
              -o "${_fresh}/table.c" --asserts "${_fresh}/asserts.ld"
              --fragment "${_fresh}/system.cmake" --gate "${_fresh}/gate.c" ${_name_args}
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err
      TIMEOUT 600)
    # Indented, so CMake prints each of the tool's lines as it wrote it.
    string(STRIP "${_out}\n${_err}" _said)
    string(REPLACE "\n" "\n  " _said "${_said}")
    # The tool exits 3 when it refused the composition, as kickos_compose/__main__.py's REFUSED
    # states; any other failure is the tool not running.
    if(_rc STREQUAL "3")
      message(FATAL_ERROR "kickos_compose(${system}): the host tool refused ${_what} "
        "against ${_manifest}:\n  ${_said}")
    elseif(NOT _rc STREQUAL "0")
      message(FATAL_ERROR "kickos_compose(${system}): could not run the host tool (uv and Python >= 3.12 "
        "required, with the ruamel.yaml its uv.lock pins, which a first run fetches from PyPI or finds in "
        "uv's cache) on ${_what} (${_rc}):\n  ${_said}")
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
  if(NOT KICKOS_COMPOSE_DECLS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states no declaration count, "
      "which is the number of tasks of ${_what}")
  endif()
  # Each declaration running a program: `<k>|<task>|<program>`, `main` the DEFAULT target's.
  set(_programs "")
  foreach(_row IN LISTS KICKOS_COMPOSE_PROGRAMS)
    if(NOT _row MATCHES "^([0-9]+)\\|([^|]+)\\|([^|]+)$")
      message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states a program row '${_row}' "
        "that is not <declaration>|<task>|<program>")
    endif()
    set(_program "${CMAKE_MATCH_3}")
    if(_program STREQUAL "main" AND KC_DEFAULT)
      set(_program "${KC_DEFAULT}")
    endif()
    list(APPEND _programs "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}|${_program}")
  endforeach()
  if(NOT KICKOS_COMPOSE_HEAP MATCHES "^[0-9]+$")
    message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states no heap, which is the "
      "`heap:` of ${_what}")
  endif()

  add_library(${system}_table OBJECT "${_dir}/table.c")
  # The include directories and definitions an app TU sees; a program's main is main.
  target_link_libraries(${system}_table PRIVATE KickOS::kernel)
  # Where the init is an image of its own, its own copy of the table, which names no entry, and
  # each declared task's image its own copy and header.
  set(_task_tables "")
  if(KICKOS_INIT_IMAGE)
    add_library(${system}_init_table OBJECT "${_dir}/table.c")
    target_link_libraries(${system}_init_table PRIVATE KickOS::kernel)
    target_compile_definitions(${system}_init_table PRIVATE KOS_TABLE_INIT_IMAGE)
    get_property(_image_header GLOBAL PROPERTY KICKOS_COMPOSE_IMAGE_HEADER)
    set(_ks "")
    if(KICKOS_COMPOSE_DECLS GREATER 0)
      math(EXPR _last "${KICKOS_COMPOSE_DECLS} - 1")
      foreach(_k RANGE 0 ${_last})
        list(APPEND _ks ${_k})
      endforeach()
    endif()
    foreach(_k IN LISTS _ks)
      add_library(${system}_task${_k} OBJECT "${_dir}/table.c" "${_image_header}")
      target_link_libraries(${system}_task${_k} PRIVATE KickOS::kernel)
      target_compile_definitions(${system}_task${_k} PRIVATE KOS_TABLE_IMAGE_TASK=${_k}
                                                             KICKOS_IMAGE_TASK=${_k})
      # A program's image starts at its main and names its program; another task's names the
      # executable's.
      foreach(_row IN LISTS _programs)
        string(REPLACE "|" ";" _row "${_row}")
        list(GET _row 0 _pk)
        list(GET _row 2 _program)
        if(_pk EQUAL _k)
          target_compile_definitions(${system}_task${_k} PRIVATE KICKOS_IMAGE_TASK_PROGRAM=1)
        endif()
        if(_pk EQUAL _k AND NOT _program STREQUAL "main")
          target_compile_definitions(${system}_task${_k} PRIVATE
                                     "KICKOS_IMAGE_PROGRAM_NAME=\"${_program}\"")
        endif()
      endforeach()
      list(APPEND _task_tables ${system}_task${_k})
    endforeach()
  endif()
  # What of the system the kernel reads, the console's kind, the gate assignment and the storage
  # of its pools, is an archive in a kernel link group, which the chip member asking for it is in.
  if(NOT KICKOS_COMPOSE_USB_DEVICE_CONSOLE MATCHES "^[01]$")
    message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states no USB device console "
      "fact, which is the console kind of ${_what}")
  endif()
  kickos_write_if_changed("${_dir}/kernel.c"
    "// GENERATED by cmake/compose.cmake from ${_dir}/system.cmake.\n\nchar const kickos_usb_device_console = ${KICKOS_COMPOSE_USB_DEVICE_CONSOLE};\n")
  # The kernel's pools at the sizes the host tool emitted, in the order KICKOS_SYSTEM_POOLS takes
  # them (<kickos/pool_storage.h>), then the init's budgets in <kickos/sys/budget.h> order.
  if("${KICKOS_COMPOSE_POOLS}" STREQUAL "")
    message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states no size for the "
      "kernel's pools, which its manifest ${_manifest} gives")
  endif()
  if("${KICKOS_COMPOSE_INIT_BUDGETS}" STREQUAL "")
    message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states no budgets for the "
      "init, which the pools TU's descriptor carries")
  endif()
  string(REPLACE ";" ", " _pool_args "${KICKOS_COMPOSE_POOLS};${KICKOS_COMPOSE_INIT_BUDGETS}")
  kickos_write_if_changed("${_dir}/pools.cc"
    "// GENERATED by cmake/compose.cmake from ${_dir}/system.cmake.\n\n#include <kickos/pool_storage.h>\n\nKICKOS_SYSTEM_POOLS(${_pool_args})\n")
  set(_kernel_sources "${_dir}/kernel.c" "${_dir}/pools.cc")
  if(KICKOS_COMPOSE_GATE)
    if(NOT EXISTS "${_dir}/gate.c")
      message(FATAL_ERROR "kickos_compose(${system}): ${_dir}/system.cmake states a gate assignment and "
        "${_dir}/gate.c holds none")
    endif()
    list(APPEND _kernel_sources "${_dir}/gate.c")
  endif()
  add_library(${system}_kernel STATIC ${_kernel_sources})
  target_link_libraries(${system}_kernel PRIVATE KickOS::kernel)
  # The pools' storage is constant-initialised, which needs C++20's constexpr constructors.
  target_compile_features(${system}_kernel PRIVATE cxx_std_20)
  # A KickOS archive by name, which the link-map gates admit.
  set_target_properties(${system}_kernel PROPERTIES OUTPUT_NAME kickos_${system}_kernel)
  set(_kernel_name ${system}_kernel)
  if(KC_EXPORT_NAME)
    set_target_properties(${system}_kernel PROPERTIES EXPORT_NAME ${KC_EXPORT_NAME}_kernel)
    kickos_export_targets(${system}_kernel)
    kickos_export_name(${system}_kernel _kernel_name)
  endif()
  set(_gate_links "$<LINK_GROUP:KICKOS_KERNEL,${_kernel_name}>")
  add_library(${system} INTERFACE)
  set_target_properties(${system} PROPERTIES KICKOS_SYSTEM_STDOUT "${KICKOS_COMPOSE_STDOUT}")

  set(_table_name ${system}_table)
  set(_init_table_name ${system}_init_table)
  set(_asserts "${_dir}/asserts.ld")
  if(KC_EXPORT_NAME)
    set_target_properties(${system}_table PROPERTIES EXPORT_NAME ${KC_EXPORT_NAME}_table)
    set_target_properties(${system} PROPERTIES EXPORT_NAME ${KC_EXPORT_NAME})
    kickos_export_targets(${system}_table ${system})
    kickos_export_name(${system}_table _table_name)
    if(KICKOS_INIT_IMAGE)
      set_target_properties(${system}_init_table PROPERTIES
        EXPORT_NAME ${KC_EXPORT_NAME}_init_table)
      kickos_export_targets(${system}_init_table)
      kickos_export_name(${system}_init_table _init_table_name)
      set(_exported "")
      foreach(_t IN LISTS _task_tables)
        string(REPLACE "${system}_" "${KC_EXPORT_NAME}_" _export "${_t}")
        set_target_properties(${_t} PROPERTIES EXPORT_NAME ${_export})
        kickos_export_targets(${_t})
        kickos_export_name(${_t} _name)
        list(APPEND _exported ${_name})
      endforeach()
      set(_task_tables ${_exported})
    endif()
    install(FILES "${_asserts}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos/${KC_EXPORT_NAME}")
    set(_asserts_link
      "$<BUILD_INTERFACE:${_asserts}>"
      "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KC_EXPORT_NAME}/asserts.ld>")
  else()
    set(_asserts_link "${_asserts}")
  endif()

  set(_objects KickOS::init)
  if(KICKOS_INIT_IMAGE)
    list(APPEND _objects ${_init_table_name} ${_task_tables})
  else()
    list(APPEND _objects ${_table_name})
  endif()
  if(KICKOS_COMPOSE_DRIVERS)
    list(APPEND _objects KickOS::init_drivers)
  else()
    list(APPEND _objects KickOS::init_no_drivers)
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
  target_link_libraries(${system} INTERFACE ${_links} ${_archives} ${_asserts_link} KickOS::kernel
                                            ${_gate_links})
  kickos_heap_defsym(_heap "${KICKOS_COMPOSE_HEAP}")
  target_link_options(${system} INTERFACE "LINKER:${_heap}")
  # Where no launcher takes the kernel's groups as one (kickos_image_link_features), the system's
  # group comes ahead of the kernel's, whose archives alone name the pools' storage: the link asks
  # for it by name.
  if(NOT KICKOS_IMAGE_SPLIT AND NOT KICKOS_IMAGE_EMBED)
    target_link_options(${system} INTERFACE
      "LINKER:--require-defined=${KICKOS_C_SYMBOL_PREFIX}kickos_kernel_pools")
  endif()
  target_compile_definitions(${system} INTERFACE KICKOS_USER_HEAP_SIZE=${KICKOS_COMPOSE_HEAP}
                                                  KICKOS_IMAGE_DECL_COUNT=${KICKOS_COMPOSE_DECLS})
  set_property(TARGET ${system} APPEND PROPERTY INTERFACE_LINK_DEPENDS ${_asserts_link})

  # The programs the system runs. `main` no DEFAULT binds is the executable linking the system,
  # compiled as a program; every other is resolved once the directory ends.
  set_target_properties(${system} PROPERTIES KICKOS_SYSTEM_PROGRAMS "${_programs}"
                                             KICKOS_SYSTEM_DEFAULT "${KC_DEFAULT}"
                                             KICKOS_SYSTEM_COMPOSITION "${_what}")
  foreach(_row IN LISTS _programs)
    string(REPLACE "|" ";" _row "${_row}")
    list(GET _row 0 _k)
    list(GET _row 1 _task)
    list(GET _row 2 _program)
    if(_program STREQUAL "main")
      set(_rec "-")
    else()
      set(_rec "$<IF:$<STREQUAL:$<TARGET_PROPERTY:NAME>,${_program}>,-,$<TARGET_FILE:${_program}>>")
      set_property(TARGET ${system} APPEND PROPERTY INTERFACE_LINK_DEPENDS
        "$<$<NOT:$<STREQUAL:$<TARGET_PROPERTY:NAME>,${_program}>>:$<TARGET_FILE:${_program}>>")
    endif()
    # Read by tools/image-link.py, and refused by the linker itself, so a link no launcher
    # reads fails.
    if(KICKOS_INIT_IMAGE)
      target_link_options(${system} INTERFACE
        "-Wl,--kickos-image-task-program=${_k}:${_task}:${_rec}")
    endif()
  endforeach()
  # A program's image takes the system's archives after its program's own inputs.
  if(KICKOS_INIT_IMAGE AND _programs)
    foreach(_a IN LISTS _archives)
      target_link_options(${system} INTERFACE "-Wl,--kickos-image-system-archive=$<TARGET_FILE:${_a}>")
    endforeach()
  endif()
  if(_programs OR KC_DEFAULT)
    # A deferred call reads its arguments when it runs, so they are spelled out here.
    cmake_language(EVAL CODE
      "cmake_language(DEFER DIRECTORY [[${CMAKE_CURRENT_SOURCE_DIR}]] CALL _kickos_compose_bind [[${system}]])")
  endif()
endfunction()

# _kickos_compose_bind(<system>)
#   At the end of the directory composing <system>: links <system> into its DEFAULT target, and
#   resolves each program it runs to an executable target compiled as a program. A program the
#   executable linking <system> is runs in that executable's own link; any other is linked on
#   its own through tools/image-link.py --program, whose command each image of it replays.
function(_kickos_compose_bind system)
  get_target_property(_programs ${system} KICKOS_SYSTEM_PROGRAMS)
  get_target_property(_default ${system} KICKOS_SYSTEM_DEFAULT)
  get_target_property(_what ${system} KICKOS_SYSTEM_COMPOSITION)
  if(_default)
    if(NOT TARGET ${_default})
      message(FATAL_ERROR "kickos_compose(${system}): DEFAULT ${_default} is no target of this project")
    endif()
    target_link_libraries(${_default} PRIVATE ${system})
    set_target_properties(${_default} PROPERTIES KICKOS_APP_SYSTEM ${system})
  endif()
  foreach(_row IN LISTS _programs)
    string(REPLACE "|" ";" _row "${_row}")
    list(GET _row 1 _task)
    list(GET _row 2 _program)
    if(_program STREQUAL "main")
      continue()
    endif()
    if(NOT TARGET ${_program})
      message(FATAL_ERROR "kickos_compose(${system}): task `${_task}` of ${_what} runs program "
        "`${_program}`, which is no target of this project")
    endif()
    get_target_property(_type ${_program} TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
      message(FATAL_ERROR "kickos_compose(${system}): task `${_task}` of ${_what} runs program "
        "`${_program}`, a ${_type}; a program is an executable, add_executable(${_program} ...)")
    endif()
    get_target_property(_links ${_program} LINK_LIBRARIES)
    if(_links AND system IN_LIST _links)
      continue()
    endif()
    if(NOT KICKOS_INIT_IMAGE)
      message(FATAL_ERROR "kickos_compose(${system}): task `${_task}` of ${_what} runs program "
        "`${_program}`, and this board links one image, so the only program a system runs is the "
        "executable linking it")
    endif()
    kickos_program_link(${_program})
  endforeach()
endfunction()
