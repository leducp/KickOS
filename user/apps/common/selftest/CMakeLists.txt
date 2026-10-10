# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A list: on a split board these have to reach EVERY image, or the parts get different
# suites.
set(_selftest_defs "")
# The only sources that see it: the kernel and every other app build the same either way.
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_defs KICKOS_ENABLE_SELFTEST=1)
endif()

# The two `return <int>;` literals cmake/boot_arena.cmake scraped out of the backend .cc this
# link resolves. Unset on the sim, which reaches no scrape. Both derefs MUST stay quoted:
# get_property leaves the variable UNSET for a property never set, and an unquoted unset name
# compares as its own literal, so the guard would pass and define both macros EMPTY, which
# still satisfies `#if defined`.
get_property(_mpu_min_region_cfg GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG)
get_property(_mpu_region_pow2_cfg GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG)
if(NOT "${_mpu_min_region_cfg}" STREQUAL "" AND NOT "${_mpu_region_pow2_cfg}" STREQUAL "")
  list(APPEND _selftest_defs
    KICKOS_MPU_MIN_REGION_CFG=${_mpu_min_region_cfg}
    KICKOS_MPU_REGION_POW2_CFG=${_mpu_region_pow2_cfg})
endif()

# A device the kernel never drives on each translating machine, which window_addr and window_get
# map at the address the kernel chooses: the PL031 RTC, the goldfish RTC and the HPET.
if(KICKOS_CHIP STREQUAL "virt_arm64")
  set(_selftest_spare_dev 0x09010000)
elseif(KICKOS_CHIP STREQUAL "virt_rv64")
  set(_selftest_spare_dev 0x00101000)
elseif(KICKOS_CHIP STREQUAL "q35")
  set(_selftest_spare_dev 0xFED00000)
endif()
if(DEFINED _selftest_spare_dev)
  list(APPEND _selftest_defs KICKOS_SELFTEST_SPARE_DEV=${_selftest_spare_dev})
endif()
# A block the kernel owns for life, a device window over which grant_reserved asks a spawn for.
if(DEFINED KICKOS_CHIP_RESERVED_BLOCK0)
  list(GET KICKOS_CHIP_RESERVED_BLOCK0 0 _selftest_reserved_base)
  list(GET KICKOS_CHIP_RESERVED_BLOCK0 1 _selftest_reserved_size)
  list(APPEND _selftest_defs KICKOS_SELFTEST_RESERVED_BASE=${_selftest_reserved_base}
                             KICKOS_SELFTEST_RESERVED_SIZE=${_selftest_reserved_size})
endif()
# The default stack size, below which the stack arms find a thread's guard.
list(APPEND _selftest_defs KICKOS_SELFTEST_USER_STACK_SIZE=${KICKOS_USER_STACK_SIZE})
# A bus master no driver of the selftest's composition holds: virtio-mmio slots 0 to 7.
if(KICKOS_CHIP STREQUAL "virt_arm64")
  list(APPEND _selftest_defs KICKOS_SELFTEST_BUS_MASTER_DEV=0x0A000000)
endif()

# main.cc cuts the registration list into contiguous REGIONS. An image carries a contiguous
# RUN of them and elides the rest, so the arms are partitioned between the images and still
# run in the order one image would run them in. main.cc refuses a build whose count differs
# from the one stated here, because a region no image carries runs nowhere and no plan check
# can see it.
set(_selftest_regions 10)

# The images this board ships, as the first region of each: image i carries its first region up
# to the next image's first less one, so the images are contiguous, in order, and cover every
# region once. Order across images does not matter, each one initialising the board and running
# its own arms alone.
#
# A per-board property, not a fleet one, and the binding budget is a different one on each of them:
# 64 KiB of FLASH on the two STM32 parts, the 128 KiB pow2 PMP code window (`_code_size` in
# arch/riscv/chip/esp32c6/esp32c6.ld) on an enforcing ESP32-C6, whose next step up would take 128
# KiB straight off the thread arena, the 240 KiB node slice on the ESP32-C6's own-image AMP
# partition, the arena that follows .bss on nrf51, and the 128 KiB IRAM every ESP32 instruction
# runs from (`IRAM` in arch/xtensa/chip/esp32/esp32.ld), which the suite overruns beside a console
# driver. The bench probes take the C6's code window past one more cut. None of these is a number
# the build can read back from the board, so the cut is stated here and nowhere else: the chip,
# KICKOS_HAVE_MPU, KICKOS_AMP_OWN_IMAGE and KICKOS_BENCH come from the board's own config, and
# every gate takes the image list and each image's arm count off the targets below rather than
# restating either.
set(_selftest_firsts 1)
if(KICKOS_CHIP STREQUAL "stm32f103" OR KICKOS_CHIP STREQUAL "stm32f302")
  set(_selftest_firsts 1 2 3 4 5 6 7 8 9 10)
elseif(KICKOS_CHIP STREQUAL "nrf51")
  set(_selftest_firsts 1 5 7 9 10)
elseif(KICKOS_CHIP STREQUAL "esp32c6" AND KICKOS_HAVE_MPU AND KICKOS_BENCH)
  set(_selftest_firsts 1 3 5 7 10)
elseif(KICKOS_CHIP STREQUAL "esp32c6" AND (KICKOS_HAVE_MPU OR KICKOS_AMP_OWN_IMAGE))
  set(_selftest_firsts 1 4 7 10)
elseif(KICKOS_CHIP STREQUAL "esp32")
  set(_selftest_firsts 1 7)
endif()
set(_selftest_bound "")
set(_selftest_prev 0)
foreach(_first IN LISTS _selftest_firsts)
  if(NOT _first GREATER _selftest_prev OR _first GREATER _selftest_regions)
    message(FATAL_ERROR
      "selftest: the images' first regions ${_selftest_firsts} must rise from 1 within the "
      "${_selftest_regions} region(s) main.cc cuts, or an image would carry no region at all, "
      "plan zero arms and pass.")
  endif()
  if(_selftest_prev GREATER 0)
    math(EXPR _last "${_first} - 1")
    list(APPEND _selftest_bound "${_selftest_prev}-${_last}")
  endif()
  set(_selftest_prev ${_first})
endforeach()
list(GET _selftest_firsts 0 _selftest_first)
if(NOT _selftest_first EQUAL 1)
  message(FATAL_ERROR "selftest: the first image must start at region 1, not ${_selftest_first}")
endif()
list(APPEND _selftest_bound "${_selftest_prev}-${_selftest_regions}")

# The suite runs under the kernel's console and under each console driver of this build's
# catalogue that consoles/<board>/ composes, each as its own run of images. An AMP node runs it
# under the kernel's console, as this node of a partition whose other nodes run ampecho, or as
# the shared image's one composition.
set(_selftest_consoles "")
set(_selftest_console_files "")
get_property(_selftest_catalogue GLOBAL PROPERTY KICKOS_DRIVER_CATALOGUE)
if(NOT KICKOS_AMP_NODE)
  file(GLOB _selftest_console_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/consoles/${KICKOS_BOARD}"
       CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/consoles/${KICKOS_BOARD}/*.yaml")
endif()
set(_selftest_amp_dir "systems/${KICKOS_BOARD}/${KICKOS_CONFIG_VARIANT}")
set(_selftest_partition "")
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  math(EXPR _selftest_last_node "${KICKOS_AMP_NODES} - 1")
  foreach(_node RANGE 0 ${_selftest_last_node})
    if(_node EQUAL KICKOS_AMP_NODE_ID)
      list(APPEND _selftest_partition "${_selftest_amp_dir}/node${_node}.yaml")
    else()
      list(APPEND _selftest_partition "${_selftest_amp_dir}/echo${_node}.yaml")
    endif()
  endforeach()
endif()
foreach(_file IN LISTS _selftest_console_files)
  string(REGEX REPLACE "\\.yaml$" "" _driver "${_file}")
  if(_driver IN_LIST _selftest_catalogue)
    list(APPEND _selftest_consoles ${_driver})
  endif()
endforeach()
# reply_abandoned_cap creates two endpoints of main's own beside each one main holds: every
# crossing the partition names and, under a console driver, the console's.
set(_selftest_main_endpoints ${KICKOS_AMP_PORT_COUNT})
if(_selftest_consoles)
  math(EXPR _selftest_main_endpoints "${_selftest_main_endpoints} + 1")
endif()
math(EXPR _selftest_endpoint_peak "${_selftest_main_endpoints} + 2")
if(KICKOS_TASK_ENDPOINT_BUDGET LESS _selftest_endpoint_peak)
  message(FATAL_ERROR
    "selftest: main holds up to ${_selftest_main_endpoints} endpoint(s) and reply_abandoned_cap "
    "creates 2 more, past KICKOS_TASK_ENDPOINT_BUDGET ${KICKOS_TASK_ENDPOINT_BUDGET}")
endif()
set(_selftest_images "")
set(_selftest_region_lo "")
set(_selftest_region_hi "")
set(_selftest_image_console "")
foreach(_console kernel ${_selftest_consoles})
  set(_console_img selftest)
  if(NOT _console STREQUAL "kernel")
    set(_console_img selftest_${_console})
  endif()
  set(_part 0)
  foreach(_run IN LISTS _selftest_bound)
    math(EXPR _part "${_part} + 1")
    string(REPLACE "-" ";" _run "${_run}")
    list(GET _run 0 _part_lo)
    list(GET _run 1 _part_hi)
    set(_part_img "${_console_img}_p${_part}")
    if(_part EQUAL 1)
      set(_part_img ${_console_img})
    endif()
    list(APPEND _selftest_images ${_part_img})
    list(APPEND _selftest_region_lo ${_part_lo})
    list(APPEND _selftest_region_hi ${_part_hi})
    list(APPEND _selftest_image_console ${_console})
  endforeach()
endforeach()
list(LENGTH _selftest_bound _selftest_parts)

list(APPEND _selftest_defs KICKOS_SELFTEST_REGIONS=${_selftest_regions})
# The one statement of which boards size their images (selftest.h reads it as KICKOS_SELFTEST_FIT).
set(_selftest_fit OFF)
if(NOT KICKOS_HAVE_ASPACE AND NOT "${_mpu_min_region_cfg}" STREQUAL ""
   AND NOT "${_mpu_region_pow2_cfg}" STREQUAL "")
  set(_selftest_fit ON)
  list(APPEND _selftest_defs KICKOS_SELFTEST_FIT=1)
else()
  list(APPEND _selftest_defs KICKOS_SELFTEST_FIT=0)
endif()
# Derived from the cut; never a per-board knob.
if(_selftest_parts GREATER 1)
  list(APPEND _selftest_defs KICKOS_TAP_TERSE=1)
endif()

set(_selftest_srcs main.cc selftest_common.cc
                   "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc")
# Four blocks of arms sit in files of their own. Each still carries the `#if` its block had in
# main.cc, so a file compiled where its guard is off is empty and costs only the header; the
# conditions here spare that parse and must therefore MATCH those guards. Getting one wrong
# cannot shrink a suite: the registration list names every arm, so a file this list omits while
# main.cc still registers its arms is an undefined reference at link.
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST)
  list(APPEND _selftest_srcs selftest_aspace.cc)
endif()
if(KICKOS_HAVE_ASPACE AND KICKOS_ENABLE_SELFTEST AND KICKOS_FAULT_ISOLATION
   AND KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND _selftest_srcs selftest_vector.cc)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_AMP_NODE)
  list(APPEND _selftest_srcs selftest_amp.cc)
endif()
if(KICKOS_ENABLE_SELFTEST AND KICKOS_KERNEL_CORES GREATER 1)
  list(APPEND _selftest_srcs selftest_smp.cc)
endif()

# No arm throws or reads a type at run time. A source property, so it follows the leaf's own
# options and holds on a leaf that compiles with exceptions and RTTI.
set_source_files_properties(${_selftest_srcs} PROPERTIES
  COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")

# On a region board each image is linked twice: once with every arm, for its size, and once with
# the arms that link's arena cannot back left out (arena_fit.py). The sources compile once.
set(_img_index 0)
foreach(_img IN LISTS _selftest_images)
  list(GET _selftest_region_lo ${_img_index} _img_lo)
  list(GET _selftest_region_hi ${_img_index} _img_hi)
  list(GET _selftest_image_console ${_img_index} _img_console)
  math(EXPR _img_index "${_img_index} + 1")
  set(_img_left_out "${CMAKE_CURRENT_BINARY_DIR}/${_img}_left_out.cc")
  if(_selftest_fit)
    set(_img_compile ${_img}_core)
    add_library(${_img_compile} OBJECT ${_selftest_srcs})
    add_executable(${_img} $<TARGET_OBJECTS:${_img_compile}> "${_img_left_out}")
  else()
    set(_img_compile ${_img})
    add_executable(${_img} ${_selftest_srcs} "${_img_left_out}")
  endif()
  if(_img_console STREQUAL "kernel" AND _selftest_partition)
    kickos_app_system(${_img} PARTITION ${_selftest_partition} SYSTEM selftest_system)
  elseif(_img_console STREQUAL "kernel" AND KICKOS_AMP_NODE)
    kickos_app_system(${_img} COMPOSITION ${_selftest_amp_dir}/node0.yaml SYSTEM selftest_system)
  elseif(_img_console STREQUAL "kernel")
    kickos_app_system(${_img} COMPOSITION system.yaml SYSTEM selftest_system)
  else()
    kickos_app_system(${_img} COMPOSITION consoles/${KICKOS_BOARD}/${_img_console}.yaml
                      SYSTEM selftest_${_img_console}_system)
    target_compile_definitions(${_img_compile} PRIVATE KICKOS_SELFTEST_CONSOLE_DRIVER=1)
  endif()
  kickos_emit_image(${_img})
  if(_selftest_fit)
    get_target_property(_img_system ${_img} KICKOS_APP_SYSTEM)
    target_link_libraries(${_img_compile} PRIVATE KickOS::kernel ${_img_system})
    set_target_properties(${_img} PROPERTIES KICKOS_SELFTEST_CORE ${_img_compile})
  endif()
  target_include_directories(${_img_compile} PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")
  target_compile_definitions(${_img_compile} PRIVATE ${_selftest_defs}
    KICKOS_SELFTEST_FIRST_REGION=${_img_lo} KICKOS_SELFTEST_LAST_REGION=${_img_hi})
  # Recorded on the target, so a gate picks an image's skip and partial sets by the regions
  # it carries and its console, instead of by a name written out a second time.
  set_target_properties(${_img} PROPERTIES
    KICKOS_SELFTEST_FIRST_REGION "${_img_lo}" KICKOS_SELFTEST_LAST_REGION "${_img_hi}"
    KICKOS_SELFTEST_CONSOLE "${_img_console}"
    KICKOS_IMAGE_JUDGE tests/integration/check_tap_stream.sh)
  # At the fleet Debug default this app's .text overflows the 64K-flash parts.
  if(NOT KICKOS_ARCH STREQUAL "sim")
    target_compile_options(${_img_compile} PRIVATE -Os)
  endif()
endforeach()
set_property(GLOBAL PROPERTY KICKOS_SELFTEST_IMAGES "${_selftest_images}")
set_property(GLOBAL PROPERTY KICKOS_SELFTEST_PARTS ${_selftest_parts})

# The linked image, for the host gates that read it beside its map.
foreach(_img IN LISTS _selftest_images)
  set_target_properties(${_img} PROPERTIES KICKOS_IMAGE_ELF "$<TARGET_FILE:${_img}>")
endforeach()
if(KICKOS_ARCH STREQUAL "x86_64")
  # OVMF places an image at its preferred base when it can, so every other image boots with
  # firmware's relocation pass idle. 1 TiB is past any q35 RAM this tree boots, so firmware
  # has to move this one, and the app window's words are then relocated twice.
  add_executable(selftest_rebased $<TARGET_OBJECTS:selftest>)
  set_target_properties(selftest_rebased PROPERTIES LINKER_LANGUAGE CXX
    ADDITIONAL_CLEAN_FILES "$<TARGET_FILE:selftest_rebased>.map")
  target_link_libraries(selftest_rebased PRIVATE $<TARGET_PROPERTY:selftest,LINK_LIBRARIES>)
  target_link_options(selftest_rebased PRIVATE $<TARGET_PROPERTY:selftest,LINK_OPTIONS>
                                               "LINKER:--image-base=0x10000000000")
  set_target_properties(selftest PROPERTIES
    KICKOS_REBASED_IMAGE_FILE "$<TARGET_FILE:selftest_rebased>")
endif()

# An arm whose body left main.cc has EXTERNAL linkage, so the compiler no longer reports it
# unused when its TAP_ADD line goes. Every such arm is declared in selftest.h and registered in
# main.cc, and this holds the two sets equal, which is the guard -Werror=unused-function gives
# the arms that stayed. It reads the files rather than a list, so neither side can be satisfied
# by a name written here.
#
# A commented-out TAP_ADD reads as a registration to a scan that has no notion of comments, and
# the two comment forms need different answers. A /* */ block can span lines, so it is stripped
# from the whole file. A // line comment cannot be stripped safely, a // inside a string literal
# being indistinguishable here, so the match is anchored after a newline and leading blank
# instead: a registration commented out with // has the // between the two and does not match.
# Drop that anchor and the scan reports a //-commented arm as registered, which is the state
# this check exists to prevent.
#
# The match runs on the text rather than on a CMake list of "lines": this file has macro bodies
# that continue a line with a trailing backslash, and turning a newline into a list-separating
# ";" makes that backslash read as an ESCAPED semicolon, silently merging the continued lines
# into one element and losing every match inside them.
file(READ main.cc _selftest_main_raw)
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _selftest_main_nocomment "${_selftest_main_raw}")
string(REGEX MATCHALL "\n[ \t]*TAP_ADD(_IRQ)?\\(\"[^\"]*\", *[A-Za-z_][A-Za-z0-9_]*[,)]" _selftest_added "${_selftest_main_nocomment}")
set(_selftest_registered "")
foreach(_row IN LISTS _selftest_added)
  if(_row MATCHES "TAP_ADD(_IRQ)?\\(\"[^\"]*\", *([A-Za-z_][A-Za-z0-9_]*)[,)]")
    list(APPEND _selftest_registered "${CMAKE_MATCH_2}")
  endif()
endforeach()

# A declaration can split `void` from its name across a newline, which a per-line scan cannot
# see: fold newlines to spaces before matching, across the whole header. A declaration's own
# trailing ";" can then double up with MATCHALL's list separator and leave an empty element
# behind, which is dropped rather than fed to the name extraction below.
file(READ selftest.h _selftest_h_raw)
string(REGEX REPLACE "\n" " " _selftest_h_flat "${_selftest_h_raw}")
string(REGEX MATCHALL "void[ \t]+t_[A-Za-z0-9_]*\\([ \t]*\\)[ \t]*;" _selftest_declared_rows "${_selftest_h_flat}")
list(REMOVE_ITEM _selftest_declared_rows "")
foreach(_row IN LISTS _selftest_declared_rows)
  string(REGEX MATCH "(t_[A-Za-z0-9_]*)\\(" _m "${_row}")
  if(NOT "${CMAKE_MATCH_1}" IN_LIST _selftest_registered)
    message(FATAL_ERROR
      "selftest.h declares the arm ${CMAKE_MATCH_1}, which no TAP_ADD line in main.cc "
      "registers. Its body is in a file of its own, so it has external linkage and the "
      "compiler cannot report it unused: the arm would be built into every image and run in "
      "none. Register it, or delete the declaration and the body together.")
  endif()
endforeach()
list(LENGTH _selftest_declared_rows _selftest_declared_n)

# Each region's arm NAMES, read off the same `#if KICKOS_SELFTEST_REGION(<n>)` ... `#undef TAP_ADD`
# bounds that make the split, so an image's permission sets can be cut to the arms it carries.
# Lexical: a name under a nested #if counts whether or not this posture compiles it.
set(_selftest_all_arm_names "")
foreach(_region RANGE 1 ${_selftest_regions})
  string(FIND "${_selftest_main_nocomment}" "#if KICKOS_SELFTEST_REGION(${_region})" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "selftest: main.cc has no `#if KICKOS_SELFTEST_REGION(${_region})`")
  endif()
  string(SUBSTRING "${_selftest_main_nocomment}" ${_at} -1 _tail)
  string(FIND "${_tail}" "#undef TAP_ADD" _end)
  if(_end EQUAL -1)
    message(FATAL_ERROR "selftest: main.cc's region ${_region} has no closing `#undef TAP_ADD`")
  endif()
  string(SUBSTRING "${_tail}" 0 ${_end} _body)
  string(REGEX MATCHALL "\n[ \t]*TAP_ADD(_IRQ|_PINNED)?\\(\"[^\"]*\"" _rows "${_body}")
  set(_selftest_arm_names_r${_region} "")
  foreach(_row IN LISTS _rows)
    string(REGEX REPLACE ".*\"([^\"]*)\"$" "\\1" _name "${_row}")
    list(APPEND _selftest_arm_names_r${_region} "${_name}")
  endforeach()
  if(NOT _selftest_arm_names_r${_region})
    message(FATAL_ERROR "selftest: no TAP_ADD line read out of main.cc's region ${_region}")
  endif()
  list(APPEND _selftest_all_arm_names ${_selftest_arm_names_r${_region}})
endforeach()
set_property(GLOBAL PROPERTY KICKOS_SELFTEST_ALL_ARM_NAMES "${_selftest_all_arm_names}")
message(STATUS "selftest: ${_selftest_declared_n} arm(s) declared in selftest.h, all registered")

# tap.cc plans from its runtime registry, so a dropped arm shrinks plan and cases together; only
# this count sees it.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_selftest_count "${PROJECT_SOURCE_DIR}/tests/static/selftest_demands.py"
    "${CMAKE_CURRENT_SOURCE_DIR}")
# Undefined unless a definition below names them: no header defines either. The per-image
# KICKOS_SELFTEST_CONSOLE_DRIVER is given to no count, so a guard reading it is refused.
foreach(_name KICKOS_ENABLE_SELFTEST KICKOS_SELFTEST_SPARE_DEV)
  list(APPEND _selftest_count --undefined ${_name})
endforeach()
if(KICKOS_ARCH STREQUAL "x86_64")
  list(APPEND _selftest_count --config __x86_64__)
else()
  list(APPEND _selftest_count --undefined __x86_64__)
endif()
get_directory_property(_selftest_dir_defs COMPILE_DEFINITIONS)
foreach(_def IN LISTS _selftest_dir_defs _selftest_defs)
  if(_def MATCHES "^[A-Za-z_][A-Za-z0-9_]*(=(0[xX][0-9a-fA-F]+|[1-9][0-9]*|0))?$")
    list(APPEND _selftest_count --config "${_def}")
  endif()
endforeach()
file(GLOB _selftest_count_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_selftest_count_srcs}
             "${PROJECT_SOURCE_DIR}/tests/static/selftest_demands.py")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" ${_selftest_count} --count
  OUTPUT_VARIABLE _selftest_arms
  ERROR_VARIABLE _selftest_arms_err
  RESULT_VARIABLE _selftest_arms_rc)
if(NOT _selftest_arms_rc EQUAL 0)
  message(FATAL_ERROR "selftest: the arms main.cc registers could not be counted: ${_selftest_arms_err}")
endif()
string(STRIP "${_selftest_arms}" _selftest_arms)
string(REPLACE "\n" ";" _selftest_arms "${_selftest_arms}")
list(LENGTH _selftest_arms _selftest_counted)
if(NOT _selftest_counted EQUAL _selftest_regions)
  message(FATAL_ERROR
    "selftest: main.cc registers arms in ${_selftest_counted} region(s), not ${_selftest_regions}")
endif()
set(_selftest_arms_total 0)
foreach(_region_arms IN LISTS _selftest_arms)
  math(EXPR _selftest_arms_total "${_selftest_arms_total} + ${_region_arms}")
endforeach()

# Each arm's workers, as it asks for them: a region board's image seats its widest ask's default
# stacks at start (st_ram_seat), and its link places its arena blocks past them. An ask wider than
# the thread pool is refused, so it seats no more than the pool's workers.
execute_process(
  COMMAND "${Python3_EXECUTABLE}" ${_selftest_count}
  OUTPUT_VARIABLE _selftest_demands
  ERROR_VARIABLE _selftest_demands_err
  RESULT_VARIABLE _selftest_demands_rc)
if(NOT _selftest_demands_rc EQUAL 0)
  message(FATAL_ERROR "selftest: the arms' asks could not be read: ${_selftest_demands_err}")
endif()
string(REGEX MATCHALL "[a-z0-9_]+ workers=[0-9]+" _selftest_workers_rows "${_selftest_demands}")
math(EXPR _selftest_worker_pool "${KICKOS_MAX_THREADS} - 1")

# tap.cc sizes its registry from KICKOS_TAP_MAX_TESTS. Per IMAGE and not per board, so it
# cannot go through _selftest_defs, which applies one list to every image. An arm the count
# misses overflows the registry and run_all() reports tap_registry_overflow.
#
# The SAME number is what a gate outside the image expects the run to plan, so each image
# records its own on itself and the gate reads it off the image it boots: a gate that picked
# the images by hand would be a second statement of which region is in which.
set(_img_index 0)
set(_selftest_plan "")
foreach(_img IN LISTS _selftest_images)
  list(GET _selftest_region_lo ${_img_index} _img_lo)
  list(GET _selftest_region_hi ${_img_index} _img_hi)
  math(EXPR _img_index "${_img_index} + 1")
  set(_img_arms 0)
  foreach(_region RANGE ${_img_lo} ${_img_hi})
    math(EXPR _at "${_region} - 1")
    list(GET _selftest_arms ${_at} _region_arms)
    math(EXPR _img_arms "${_img_arms} + ${_region_arms}")
  endforeach()
  if(_img_arms EQUAL 0)
    message(FATAL_ERROR
      "selftest: ${_img} registers no arm in this posture over region(s) ${_img_lo}-${_img_hi}, "
      "so it would plan zero arms and pass.")
  endif()
  set(_img_names "")
  foreach(_region RANGE ${_img_lo} ${_img_hi})
    list(APPEND _img_names ${_selftest_arm_names_r${_region}})
  endforeach()
  set(_img_seat 0)
  foreach(_row IN LISTS _selftest_workers_rows)
    string(REGEX REPLACE " workers=.*" "" _arm "${_row}")
    string(REGEX REPLACE ".* workers=" "" _workers "${_row}")
    if(_arm IN_LIST _img_names AND _workers GREATER _img_seat)
      set(_img_seat ${_workers})
    endif()
  endforeach()
  if(_img_seat GREATER _selftest_worker_pool)
    set(_img_seat ${_selftest_worker_pool})
  endif()
  set(_img_compile ${_img})
  if(_selftest_fit)
    set(_img_compile ${_img}_core)
  endif()
  target_compile_definitions(${_img_compile} PRIVATE KICKOS_TAP_MAX_TESTS=${_img_arms}
                                                     KICKOS_SELFTEST_SEAT_WORKERS=${_img_seat})
  # One bit per registration, the same size in the sizing link and the image.
  math(EXPR _img_left_out_bytes "(${_img_arms} + 7) / 8")
  set_target_properties(${_img} PROPERTIES KICKOS_SELFTEST_LEFT_OUT_BYTES ${_img_left_out_bytes})
  set(_img_left_out "${CMAKE_CURRENT_BINARY_DIR}/${_img}_left_out.cc")
  if(_selftest_fit)
    set(_img_left_out "${CMAKE_CURRENT_BINARY_DIR}/${_img}_sizing_left_out.cc")
    set(_img_seat 0)
  endif()
  # A sizing link, or an image no link sizes, leaves no arm out.
  file(CONFIGURE OUTPUT "${_img_left_out}" CONTENT
"// GENERATED by user/apps/common/selftest/CMakeLists.txt for ${_img}.
namespace selftest
{
    extern __attribute__((visibility(\"hidden\"))) unsigned char const st_left_out[${_img_left_out_bytes}] = {};
    extern __attribute__((visibility(\"hidden\"))) int const st_seat_workers = ${_img_seat};
}
")
  set_target_properties(${_img} PROPERTIES KICKOS_TAP_ARMS "${_img_arms}"
                                           KICKOS_SELFTEST_ARM_NAMES "${_img_names}")
  list(APPEND _selftest_plan "${_img} plans ${_img_arms} (region(s) ${_img_lo}-${_img_hi})")
endforeach()
# The sizing link of each image on a region board, and what its map leaves out.
if(_selftest_fit)
  kickos_ram_region_rule("${_mpu_min_region_cfg}" "${_mpu_region_pow2_cfg}" _selftest_stack_rule
                         "${KICKOS_USER_STACK_SIZE}")
  list(GET _selftest_stack_rule 0 _selftest_stack_size)
  list(GET _selftest_stack_rule 1 _selftest_stack_align)
  math(EXPR _selftest_stack_stride "(${_selftest_stack_size} + ${_selftest_stack_align} - 1)
       / ${_selftest_stack_align} * ${_selftest_stack_align}")
  # An emulator board runs every arm the posture carries: an image leaving one out does not build.
  set(_selftest_fit_refuses list)
  if(NOT "${KICKOS_QEMU_MACHINE}" STREQUAL "")
    set(_selftest_fit_refuses refuse)
  endif()
  set(_selftest_workers_file "${CMAKE_CURRENT_BINARY_DIR}/selftest_workers.txt")
  string(REPLACE ";" "\n" _selftest_workers_text "${_selftest_workers_rows}")
  string(REPLACE " workers=" " " _selftest_workers_text "${_selftest_workers_text}")
  file(CONFIGURE OUTPUT "${_selftest_workers_file}" CONTENT "${_selftest_workers_text}\n")
  foreach(_img IN LISTS _selftest_images)
    get_target_property(_img_system ${_img} KICKOS_APP_SYSTEM)
    set(_sizing ${_img}_sizing)
    add_executable(${_sizing} $<TARGET_OBJECTS:${_img}_core>
                   "${CMAKE_CURRENT_BINARY_DIR}/${_img}_sizing_left_out.cc")
    get_target_property(_img_left_out_bytes ${_img} KICKOS_SELFTEST_LEFT_OUT_BYTES)
    target_link_libraries(${_sizing} PRIVATE KickOS::kernel ${_img_system})
    target_link_options(${_sizing} PRIVATE
      "LINKER:-T,${CMAKE_CURRENT_SOURCE_DIR}/arena_records.ld")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_img}")
    add_custom_command(
      OUTPUT "${_out}_left_out.cc" "${_out}_left_out.txt" "${_out}_arena_ask.ld"
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/arena_fit.py"
              "$<TARGET_FILE:${_sizing}>" "${CMAKE_NM}" "${CMAKE_OBJCOPY}"
              "${_selftest_workers_file}" ${_selftest_worker_pool} ${_selftest_stack_align}
              ${_selftest_stack_stride} "prefix=${KICKOS_C_SYMBOL_PREFIX}" ${_img_left_out_bytes}
              ${_selftest_fit_refuses}
              "${_out}_left_out.cc" "${_out}_left_out.txt" "${_out}_arena_ask.ld" ${_img}
      DEPENDS ${_sizing} "${CMAKE_CURRENT_SOURCE_DIR}/arena_fit.py" "${_selftest_workers_file}"
      VERBATIM)
    target_link_options(${_img} PRIVATE
      "LINKER:-T,${CMAKE_CURRENT_SOURCE_DIR}/arena_records.ld" "LINKER:-T,${_out}_arena_ask.ld")
    set_property(TARGET ${_img} APPEND PROPERTY LINK_DEPENDS "${_out}_arena_ask.ld")
    set_target_properties(${_img} PROPERTIES KICKOS_SELFTEST_LEFT_OUT "${_out}_left_out.txt")
  endforeach()
endif()

if(_selftest_parts GREATER 1)
  string(REPLACE ";" ", " _selftest_plan "${_selftest_plan}")
  message(STATUS
    "selftest: split into ${_selftest_parts} images over ${_selftest_regions} regions: "
    "${_selftest_plan} (${_selftest_arms_total} together)")
endif()
