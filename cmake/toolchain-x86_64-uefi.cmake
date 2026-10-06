# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Toolchain for the KickOS x86_64 target: the KickOS toolchain's x86_64-elf family
# (docs/design-m10-toolchain.md). The image firmware loads is a PE32+ UEFI application, which
# that family's GNU ld writes with its PE+ emulation (i386pep) from the ELF objects.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

include("${CMAKE_CURRENT_LIST_DIR}/toolchain-common.cmake")

set(KICKOS_TOOLCHAIN_DEFAULT_BOARD "qemu-x86_64")
set(KICKOS_BOARD "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}" CACHE STRING "Target board: qemu-x86_64")

kickos_toolchain_board_descriptor("x86_64")
kickos_toolchain_cpu_baseline("x86_64" "x86")
kickos_toolchain_export_baseline("${_kos_cpu}")

# CMake keeps these flags from the first configure. Upgrade existing q35 build trees too, to
# x86-64-v3, to the app posture's vectors and hosted C library, and off the visibility header
# the compiler's binds-local patch replaced.
foreach(_kos_lang C CXX ASM)
  if(CMAKE_${_kos_lang}_FLAGS MATCHES "(^| )-march=x86-64( |$)")
    string(REGEX REPLACE "(^| )-march=x86-64( |$)" "\\1-march=x86-64-v3\\2"
      _kos_flags "${CMAKE_${_kos_lang}_FLAGS}")
    set(CMAKE_${_kos_lang}_FLAGS "${_kos_flags}" CACHE STRING "Compiler flags" FORCE)
  endif()
  foreach(_kos_gone " -mno-sse -mno-mmx -mno-80387" " -ffreestanding")
    if(CMAKE_${_kos_lang}_FLAGS MATCHES "${_kos_gone}")
      string(REPLACE "${_kos_gone}" "" _kos_flags "${CMAKE_${_kos_lang}_FLAGS}")
      set(CMAKE_${_kos_lang}_FLAGS "${_kos_flags}" CACHE STRING "Compiler flags" FORCE)
    endif()
  endforeach()
  # Matched by pattern: the install step reads a path under this directory as a file to ship.
  string(REGEX REPLACE " -include [^ ]*/toolchain-x86_64-hidden[.]h" ""
    _kos_flags "${CMAKE_${_kos_lang}_FLAGS}")
  if(NOT "${_kos_flags}" STREQUAL "${CMAKE_${_kos_lang}_FLAGS}")
    set(CMAKE_${_kos_lang}_FLAGS "${_kos_flags}" CACHE STRING "Compiler flags" FORCE)
  endif()
endforeach()

set(KICKOS_ARCH_FAMILY "x86" CACHE STRING "KickOS ISA family (arm|rx|xtensa|riscv|arm64|x86)")

kickos_toolchain_package(x86_64-elf)
find_program(CMAKE_OBJDUMP x86_64-elf-objdump HINTS "${_kos_tc_bin}" NO_DEFAULT_PATH REQUIRED)
find_program(CMAKE_NM      x86_64-elf-nm      HINTS "${_kos_tc_bin}" NO_DEFAULT_PATH REQUIRED)
find_program(CMAKE_READELF x86_64-elf-readelf HINTS "${_kos_tc_bin}" NO_DEFAULT_PATH REQUIRED)

# The image is linked by ld directly, through the link rule below: the compiler driver cannot
# select the PE+ emulation.
find_program(KICKOS_X86_64_LD x86_64-elf-ld HINTS "${_kos_tc_bin}" NO_DEFAULT_PATH REQUIRED)

# Refused here: an ld without the PE+ emulation reports an unrecognised -m at the link step
# and nothing there says the toolchain was the problem.
execute_process(COMMAND "${KICKOS_X86_64_LD}" -V
                OUTPUT_VARIABLE _kos_ld_emulations
                ERROR_VARIABLE  _kos_ld_emulations
                RESULT_VARIABLE _kos_ld_rc)
if(NOT _kos_ld_rc EQUAL 0 OR NOT "${_kos_ld_emulations}" MATCHES "i386pep")
  message(FATAL_ERROR
    "KickOS x86_64 toolchain: ${KICKOS_X86_64_LD} lists no i386pep emulation, so it cannot "
    "write the PE32+ image UEFI loads. The KickOS toolchain's x86_64-elf binutils are "
    "configured with --enable-targets=x86_64-pep, which gives ld that emulation.")
endif()

# -fpie is load-bearing and not a hardening flag: it keeps every reference from CODE
# PC-relative, so no instruction carries an absolute address. Built -fno-pic the same
# sources emit R_X86_64_32/32S all through the text.
#
# -mno-red-zone: privileged x86_64 text takes interrupts on its own stack.
#
# The KickOS x86_64-elf GCC binds every symbol but a weak one locally under -fpie, declarations
# included (docs/design-m10-toolchain.md section 5.5), so no reference reaches a global offset
# table the PE32+ image cannot hold, and tools/check-x86_64-no-got.sh refuses a survivor before
# every link.
#
# These are the app's posture, x87, SSE and AVX included: every core enables that state for its
# threads (arch/x86/x86_64/entry_x86_64.cc) and both switch paths save it (switch.S). The kernel
# half adds KICKOS_X86_64_KERNEL_FLAGS (cmake/x86_64_boot.cmake), and so never touches the state
# it saves for the threads. Hosted, as every other target's apps are, for the toolchain's newlib
# and libstdc++: KickOS's own libraries take -ffreestanding from kickos_apply_freestanding.
string(JOIN " " _kos_common ${_kos_cpu}
       -fno-stack-protector -fno-stack-clash-protection
       -mno-red-zone -fpie -mcmodel=small -fno-ident
       -fno-asynchronous-unwind-tables -fno-unwind-tables
       -ffunction-sections -fdata-sections)
kickos_toolchain_flags_init("${_kos_common}")
# The kernel half's posture: no x87, MMX, SSE or AVX register in the generated code, and a
# floating-point type in a signature a compile error.
set(KICKOS_X86_64_KERNEL_FLAGS -mgeneral-regs-only)
string(APPEND CMAKE_CXX_FLAGS_INIT " -fno-exceptions -fno-rtti")

kickos_toolchain_bare_metal_rules()

# Both languages' executable link rule is tools/x86_64-link.sh, which an installed package
# carries beside this file, and their suffix .efi; only the per-language suffix survives
# CMakeGenericSystem. The rule leaves out <FLAGS>: compile flags, which ld would refuse.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/x86_64-link.sh")
  set(_kos_link "${CMAKE_CURRENT_LIST_DIR}/x86_64-link.sh")
else()
  get_filename_component(_kos_link "${CMAKE_CURRENT_LIST_DIR}/../tools/x86_64-link.sh" ABSOLUTE)
endif()
# ld runs without a compiler driver, so the toolchain's own library directories are the rule's,
# found here under the board's flags. A link names those libraries by -l, which keeps this
# machine's paths out of an exported link interface. A try-compile builds a static library and
# links nothing, so it skips the probes; the cache keys them on the compiler and its flags.
get_property(_kos_in_try_compile GLOBAL PROPERTY IN_TRY_COMPILE)
set(_kos_libdirs_key "${CMAKE_C_COMPILER} ${_kos_common}")
if(NOT _kos_in_try_compile AND NOT "${KICKOS_X86_64_LIBDIRS_KEY}" STREQUAL "${_kos_libdirs_key}")
  separate_arguments(_kos_flags NATIVE_COMMAND "${_kos_common}")
  set(_kos_dirs "")
  foreach(_kos_a c m gcc stdc++ supc++)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" ${_kos_flags} "-print-file-name=lib${_kos_a}.a"
                    OUTPUT_VARIABLE _kos_path OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _kos_rc)
    if(NOT _kos_rc EQUAL 0 OR NOT IS_ABSOLUTE "${_kos_path}" OR NOT EXISTS "${_kos_path}")
      message(FATAL_ERROR "KickOS x86_64 toolchain: ${CMAKE_C_COMPILER} -print-file-name found "
        "no lib${_kos_a}.a ('${_kos_path}'). The x86_64-elf package installs it.")
    endif()
    get_filename_component(_kos_dir "${_kos_path}" DIRECTORY)
    file(REAL_PATH "${_kos_dir}" _kos_dir)
    list(APPEND _kos_dirs "${_kos_dir}")
  endforeach()
  list(REMOVE_DUPLICATES _kos_dirs)
  set(_kos_libdirs "")
  foreach(_kos_dir IN LISTS _kos_dirs)
    string(APPEND _kos_libdirs " \"-L${_kos_dir}\"")
  endforeach()
  set(KICKOS_X86_64_LIBDIRS "${_kos_libdirs}" CACHE INTERNAL
      "The -L arguments of the x86_64 link rule")
  set(KICKOS_X86_64_LIBDIRS_KEY "${_kos_libdirs_key}" CACHE INTERNAL
      "The compiler and flags KICKOS_X86_64_LIBDIRS was probed under")
endif()
foreach(_kos_lang C CXX)
  set(CMAKE_EXECUTABLE_SUFFIX_${_kos_lang} ".efi")
  set(CMAKE_${_kos_lang}_LINK_EXECUTABLE
      "\"${_kos_link}\" \"${KICKOS_X86_64_LD}\" \"${CMAKE_READELF}\" \"${CMAKE_OBJDUMP}\" \"${CMAKE_OBJCOPY}\"${KICKOS_X86_64_LIBDIRS} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
endforeach()
