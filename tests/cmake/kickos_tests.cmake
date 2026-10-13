# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The in-tree test machinery: the build fixture, the add_test wrapper, the image-test decline
# pass, and the helpers the test directories register through. Included once, under
# KICKOS_BUILD_TESTS, before any directory registers a test.

enable_testing()

if(KICKOS_BUILD_UNIT_TESTS)
  include(GoogleTest)
endif()

# Must not contain a `;`: FAIL_REGULAR_EXPRESSION is a CMake list of regexes.
file(STRINGS "${PROJECT_SOURCE_DIR}/tests/lib/panic.ere" KICKOS_PANIC_REGEX LIMIT_COUNT 1)
if(KICKOS_PANIC_REGEX STREQUAL "")
  message(FATAL_ERROR "tests/lib/panic.ere is empty; the sim panic gates would register "
                      "an empty regex and no fault would ever fail a test")
endif()

# Hands every test's TIMEOUT to it as KOS_CTEST_TIMEOUT_S, which tests/lib/gate.sh charges each
# boot's bound against, at the end of the directory once every set_tests_properties has run.
# The add_test wrapper arms it; a directory registering only through _add_test arms it itself.
function(kickos_hand_timeouts)
  get_property(_names DIRECTORY PROPERTY TESTS)
  foreach(_name IN LISTS _names)
    get_test_property("${_name}" TIMEOUT _timeout)
    if(_timeout)
      set_property(TEST "${_name}" APPEND PROPERTY ENVIRONMENT "KOS_CTEST_TIMEOUT_S=${_timeout}")
    endif()
  endforeach()
endfunction()
macro(kickos_arm_timeouts)
  get_property(_kickos_timeouts_armed DIRECTORY PROPERTY kickos_timeouts_armed)
  if(NOT _kickos_timeouts_armed)
    set_property(DIRECTORY PROPERTY kickos_timeouts_armed ON)
    cmake_language(DEFER CALL kickos_hand_timeouts)
  endif()
endmacro()
kickos_arm_timeouts()

# The preset name, for the gates that rebuild this preset in a scratch tree.
set(_fs_own_image OFF)
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  set(_fs_own_image ON)
endif()
kickos_preset_key(_fs_preset_key "${KICKOS_BOARD}" "${KICKOS_CONFIG_VARIANT}" "${_fs_own_image}"
                  "${KICKOS_AMP_NODE_ID}")

# No test runs a stale binary: the add_test wrapper below makes every test require this fixture.
add_test(NAME kickos_build COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}")
set_tests_properties(kickos_build PROPERTIES FIXTURES_SETUP kickos_build TIMEOUT 900)

# A macro, so the test lands in the caller's directory. kickos_build is registered before it, or
# it would require itself.
macro(add_test)
  _add_test(${ARGV})
  kickos_arm_timeouts()
  if("${ARGV0}" STREQUAL "NAME")
    set(_kickos_test_name "${ARGV1}")
  else()
    set(_kickos_test_name "${ARGV0}")
  endif()
  set_property(TEST "${_kickos_test_name}" APPEND PROPERTY FIXTURES_REQUIRED kickos_build)
  # <image>|<test> for each image the command names, which the image listing reads.
  string(REGEX MATCHALL "\\$<TARGET_FILE:[^>]+>" _kickos_test_images "${ARGV}")
  foreach(_kickos_test_image IN LISTS _kickos_test_images)
    string(REGEX REPLACE "^\\$<TARGET_FILE:(.+)>$" "\\1" _kickos_test_image
           "${_kickos_test_image}")
    set_property(DIRECTORY APPEND PROPERTY kickos_image_tests
                 "${_kickos_test_image}|${_kickos_test_name}")
  endforeach()
  if(NOT KICKOS_BUILD_INTEGRATION_TESTS)
    # cmake_language(DEFER CALL) expands its arguments when the call RUNS, so a per-test
    # defer would see the LAST name registered in the directory and decline only that one.
    set_property(DIRECTORY APPEND PROPERTY kickos_declinable "${_kickos_test_name}")
    get_property(_kickos_armed DIRECTORY PROPERTY kickos_decline_armed)
    if(NOT _kickos_armed)
      set_property(DIRECTORY PROPERTY kickos_decline_armed ON)
      # The end of the directory is the earliest point the `host` label is readable.
      cmake_language(DEFER CALL kickos_decline_image_tests)
    endif()
  endif()
endmacro()

function(kickos_decline_image_tests)
  get_property(_names DIRECTORY PROPERTY kickos_declinable)
  foreach(_name IN LISTS _names)
    get_test_property("${_name}" LABELS _labels)
    if(NOT "host" IN_LIST _labels)
      set_tests_properties("${_name}" PROPERTIES DISABLED TRUE)
    endif()
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_qemu_test([NAME <n>] TARGET <app> SCRIPT <sh>
#                      [MACHINE <m>] [BOOTS <n>] [WORK <s>] [ARGS <arg...>])
#   Register a QEMU boot gate: run SCRIPT against the app's ELF and treat exit 77 as SKIP (a
#   missing qemu-system is a skip, not a failure).
#
#   TOTAL over the fleet: a board with no emulator registers nothing, so a call site owes no
#   board predicate. A call site still owes any condition about the CLAIM rather than the
#   emulator: an arch, an MPU posture, a core count, or the existence of the target it rides.
#
#   NAME defaults to <board tag>_<target>, the board name with `-` turned into `_`; state it
#   where the ctest name is not that (one target carrying several gates, or a gate named for its
#   claim rather than its image).
#   QEMU_MACHINE is always passed: check_fault_dump.sh reads an UNSET QEMU_MACHINE as "this
#   is the sim, run natively". MACHINE overrides the board default. ARGS are extra script
#   arguments after the ELF. BOOTS and WORK size the TIMEOUT, see kickos_boot_timeout.
# ---------------------------------------------------------------------------
function(kickos_add_qemu_test)
  cmake_parse_arguments(QT "" "NAME;TARGET;SCRIPT;MACHINE;BOOTS;WORK" "ARGS" ${ARGN})
  if(NOT QT_TARGET OR NOT QT_SCRIPT)
    message(FATAL_ERROR "kickos_add_qemu_test: TARGET and SCRIPT are required")
  endif()
  if(NOT QT_NAME)
    string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")
    set(QT_NAME "${_tag}_${QT_TARGET}")
  endif()
  kickos_qemu_machine("${KICKOS_BOARD}" _env _machine)
  if(_machine STREQUAL "")
    return()
  endif()
  if(QT_MACHINE)
    set(_machine "${QT_MACHINE}")
  endif()
  list(APPEND _env QEMU_MACHINE=${_machine})
  add_test(NAME "${QT_NAME}"
    COMMAND "${CMAKE_COMMAND}" -E env ${_env}
            "${QT_SCRIPT}" "$<TARGET_FILE:${QT_TARGET}>" ${QT_ARGS})
  set_tests_properties("${QT_NAME}" PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout("${QT_NAME}" "${QT_SCRIPT}" BOOTS ${QT_BOOTS} WORK ${QT_WORK})
endfunction()

# ---------------------------------------------------------------------------
# kickos_boot_timeout(<test> <script> [BOOTS <n>] [WORK <s>])
#   Register the TIMEOUT of a test booting the configured board's image, under its emulator or
#   natively on the sim, as the charge tests/lib/gate.sh's boot_bound puts on its boots, plus one
#   second, so that charge can refuse only a test whose BOOTS is miscounted. Each boot is bounded
#   at the QEMU_TIMEOUT (on the sim, SIM_TIMEOUT) this configure sees, else at <script>'s own
#   default, else at run_image's, and carries gate.sh's firmware allowance under uefi-pe and its
#   stop. A ctest run under another bound needs a reconfigure.
#
#   BOOTS (default 1) counts the boots gate.sh charges, those of a script <script> runs
#   included. WORK is what the test spends beside them, such as building the image it boots.
#
#   An emulated test holds one ctest slot per emulated core.
# ---------------------------------------------------------------------------
function(kickos_boot_timeout test script)
  cmake_parse_arguments(QB "" "BOOTS;WORK" "" ${ARGN})
  if(NOT DEFINED QB_BOOTS)
    set(QB_BOOTS 1)
  endif()
  if(NOT DEFINED QB_WORK)
    set(QB_WORK 0)
  endif()
  set(_lib "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../lib/gate.sh")
  set(_var QEMU_TIMEOUT)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_var SIM_TIMEOUT)
  endif()
  set(_default_re "(^: |boot_bound )\"\\$\\{${_var}:[-=][0-9]+\\}\"")
  set(_bound "$ENV{${_var}}")
  if(_bound STREQUAL "")
    file(STRINGS "${script}" _bound REGEX "${_default_re}" LIMIT_COUNT 1)
  endif()
  if(_bound STREQUAL "")
    file(STRINGS "${_lib}" _bound REGEX "${_default_re}" LIMIT_COUNT 1)
  endif()
  string(REGEX REPLACE "^.*${_var}:[-=]([0-9]+)}\"$" "\\1" _bound "${_bound}")
  file(STRINGS "${_lib}" _firmware REGEX "^KOS_UEFI_FIRMWARE_S=[0-9]+$")
  file(STRINGS "${_lib}" _ticks REGEX "^KOS_STOP_TICKS=[0-9]+$")
  string(REPLACE "KOS_UEFI_FIRMWARE_S=" "" _firmware "${_firmware}")
  string(REPLACE "KOS_STOP_TICKS=" "" _ticks "${_ticks}")
  foreach(_n IN ITEMS _bound _firmware _ticks)
    if(NOT "${${_n}}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "kickos_boot_timeout(${test}): ${_n} reads '${${_n}}', not whole "
        "seconds, from ${_var}, ${script} or ${_lib}")
    endif()
  endforeach()
  kickos_qemu_machine("${KICKOS_BOARD}" _env _machine _cpus)
  if(NOT "KICKOS_BOOT=uefi-pe" IN_LIST _env)
    set(_firmware 0)
  endif()
  math(EXPR _timeout "${QB_WORK} + ${QB_BOOTS} * (${_bound} + ${_firmware} + ${_ticks} / 5) + 1")
  set_tests_properties("${test}" PROPERTIES TIMEOUT ${_timeout})
  if(NOT _machine STREQUAL "")
    set_tests_properties("${test}" PROPERTIES PROCESSORS ${_cpus})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# kickos_host_gate(<name>... [TIMEOUT <s>])
#   The tail a gate that runs on the BUILD HOST owes: the `host` label the decline pass reads,
#   and a timeout, defaulting to 120s. A gate without the label is taken for a runner of an
#   image and declined wherever the integration tests are off.
#
#   It states the TAIL and not the add_test: an argument holding an escaped `;` survives a
#   literal add_test and is SPLIT by any re-expansion through cmake_parse_arguments, shifting
#   every positional after it.
#
# kickos_tree_gate(<name>... [TIMEOUT <s>])
#   The same tail for a gate that reads only the source tree, labelled `tree` as well.
# ---------------------------------------------------------------------------
function(kickos_host_gate)
  cmake_parse_arguments(HG "" "TIMEOUT" "" ${ARGN})
  if(NOT HG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_host_gate: at least one registered test name is required")
  endif()
  if(NOT HG_TIMEOUT)
    set(HG_TIMEOUT 120)
  endif()
  set_tests_properties(${HG_UNPARSED_ARGUMENTS} PROPERTIES TIMEOUT ${HG_TIMEOUT} LABELS host)
endfunction()

function(kickos_tree_gate)
  kickos_host_gate(${ARGN})
  cmake_parse_arguments(TG "" "TIMEOUT" "" ${ARGN})
  set_property(TEST ${TG_UNPARSED_ARGUMENTS} APPEND PROPERTY LABELS tree)
endfunction()

# kickos_image_rule(<rule> <target> [<arg>...])
#   Host gate image_<rule>: tests/static/check_image_rules.sh's <rule> over <target>'s image.
function(kickos_image_rule rule target)
  add_test(NAME image_${rule}_${target}
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_image_rules.sh" ${rule} "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:${target}>" ${ARGN})
  kickos_host_gate(image_${rule}_${target})
endfunction()

# ---------------------------------------------------------------------------
# The host gate seam: real kernel translation units compiled for the build host at a
# POSTURE the running preset does not carry.
#
# A posture is a set of KICKOS_* macros whose value decides which arms of a kernel source
# exist at all. Overriding one is not a matter of appending a -D: the value reaches every
# in-tree TU through the root's add_compile_definitions, and TWO -D of one macro with
# different values is a redefinition -Werror refuses. It arrives by two routes that both have
# to be cut, and cutting one silently leaves the preset's value in force:
#   the DIRECTORY property a test directory inherits from the root, and
#   kickos_kernel's own COMPILE_DEFINITIONS, which the gate reads to compile the same
#   translation unit that ships.
# kickos_posture_scrub does the cutting; nothing else in the tree may spell the filter.
#
# kickos_posture_scrub(<out> <MACRO=VALUE>...)
#   Drops exactly the macros named here from the CALLER'S directory property (a function
#   shares its caller's directory scope), and returns in <out> the definition list a host
#   target compiling kernel sources wants: kickos_kernel's own definitions with those same
#   macros filtered out, then the given values.
function(kickos_posture_scrub out)
  set(_names "")
  foreach(_d IN LISTS ARGN)
    if(NOT _d MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=")
      message(FATAL_ERROR "kickos_posture_scrub: '${_d}' is not <MACRO>=<VALUE>")
    endif()
    list(APPEND _names "${CMAKE_MATCH_1}")
  endforeach()
  set(_kernel_defs "$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>")
  if(_names)
    list(REMOVE_DUPLICATES _names)
    string(REPLACE ";" "|" _alt "${_names}")
    set(_re "^(${_alt})=")
    get_directory_property(_dirdefs COMPILE_DEFINITIONS)
    list(FILTER _dirdefs EXCLUDE REGEX "${_re}")
    set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_dirdefs}")
    set(_kernel_defs "$<FILTER:${_kernel_defs},EXCLUDE,${_re}>")
  endif()
  set(${out} "${_kernel_defs}" ${ARGN} PARENT_SCOPE)
endfunction()

# kickos_add_kernel_host_lib(<name> SOURCES <cc...> [INCLUDES <dirs...>]
#                            [POSTURE <MACRO=VALUE>...])
#   Kernel sources compiled for the host at a posture, as an OBJECT library SEVERAL gates
#   link. It carries its include path, definitions and C++ standard PUBLIC, so a gate that
#   links it inherits the whole posture and its own TU is compiled the same way.
#
#   The kernel's COMPILE_OPTIONS are not inherited: -ffreestanding, -fno-common and
#   -fno-use-cxa-atexit describe an image with no host libc, and a gate is a hosted program
#   that needs stdio, setjmp and atexit. The three LANGUAGE flags below change what a
#   translation unit MEANS, so they are carried by hand.
#
#   The posture is recorded on the target: kickos_add_unit_test scrubs a linking gate's
#   directory for it, so a consumer states nothing.
function(kickos_add_kernel_host_lib name)
  cmake_parse_arguments(KHL "" "" "SOURCES;INCLUDES;POSTURE" ${ARGN})
  if(NOT KHL_SOURCES)
    message(FATAL_ERROR "kickos_add_kernel_host_lib(${name}): SOURCES is required")
  endif()
  kickos_posture_scrub(_khl_defs ${KHL_POSTURE})
  add_library(${name} OBJECT ${KHL_SOURCES})
  target_compile_features(${name} PUBLIC ${KICKOS_CXX_STANDARD})
  target_include_directories(${name} PUBLIC
    $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES> ${KHL_INCLUDES})
  target_compile_definitions(${name} PUBLIC ${_khl_defs})
  target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS}
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
  set_property(TARGET ${name} PROPERTY KICKOS_POSTURE "${KHL_POSTURE}")
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_unit_test(NAME <target> SOURCES <cc...> [INCLUDES <dirs...>]
#                     [DEFINITIONS <defs...>] [LIBRARIES <libs...>]
#                     [KERNEL] [POSTURE <MACRO=VALUE>...] [TEST_PREFIX <p>])
#   One host unit-test executable on GoogleTest, with PER-CASE ctest entries.
#
#   KERNEL adds kickos_kernel's include path and its definitions VERBATIM, for a gate that
#   compiles a kernel translation unit or includes a kernel header: a divergent config would
#   gate a different translation unit than the one that ships. POSTURE is the same with the
#   named macros replaced, and implies KERNEL.
#
#   TEST_PREFIX is for a gate built SEVERAL times from one source: every binary defines the
#   same gtest suite and case names, and without a prefix they would register under one
#   ctest name and only one geometry would be checked.
#
#   gtest_discover_tests writes its add_test calls at BUILD time, past the add_test wrapper:
#   the `host` label and FIXTURES_REQUIRED have to be passed through PROPERTIES here.
function(kickos_add_unit_test)
  cmake_parse_arguments(UT "KERNEL" "NAME;TEST_PREFIX"
    "SOURCES;INCLUDES;DEFINITIONS;LIBRARIES;POSTURE" ${ARGN})
  if(NOT UT_NAME OR NOT UT_SOURCES)
    message(FATAL_ERROR "kickos_add_unit_test: NAME and SOURCES are required")
  endif()
  # A linked posture library carries its macros PUBLIC, so this directory's inherited copy
  # of them collides exactly as an own POSTURE would.
  set(_posture ${UT_POSTURE})
  foreach(_lib IN LISTS UT_LIBRARIES)
    if(TARGET ${_lib})
      get_target_property(_lib_posture ${_lib} KICKOS_POSTURE)
      if(_lib_posture)
        list(APPEND _posture ${_lib_posture})
      endif()
    endif()
  endforeach()
  if(_posture OR UT_KERNEL)
    kickos_posture_scrub(_ut_kernel_defs ${_posture})
  endif()
  add_executable(${UT_NAME} ${UT_SOURCES})
  target_link_libraries(${UT_NAME} PRIVATE ${UT_LIBRARIES} GTest::gtest_main)
  target_compile_features(${UT_NAME} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_options(${UT_NAME} PRIVATE ${KICKOS_WARN_FLAGS})
  if(UT_KERNEL OR UT_POSTURE)
    target_include_directories(${UT_NAME} PRIVATE
      $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>)
  endif()
  if(UT_INCLUDES)
    target_include_directories(${UT_NAME} PRIVATE ${UT_INCLUDES})
  endif()
  if(UT_KERNEL OR UT_POSTURE)
    target_compile_definitions(${UT_NAME} PRIVATE ${_ut_kernel_defs})
  endif()
  if(UT_DEFINITIONS)
    target_compile_definitions(${UT_NAME} PRIVATE ${UT_DEFINITIONS})
  endif()
  gtest_discover_tests(${UT_NAME}
    TEST_PREFIX "${UT_TEST_PREFIX}"
    PROPERTIES TIMEOUT 30 LABELS host FIXTURES_REQUIRED kickos_build
    DISCOVERY_TIMEOUT 60)
endfunction()
