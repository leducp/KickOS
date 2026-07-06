# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Host (x86-64 Linux) toolchain for the KickOS "sim" target.
#
# The sim builds and links as an ordinary native ELF: the sim arch backend
# is the ONLY place host libc is used (ucontext/signals/mmap). Kernel, lib and
# userspace are still compiled freestanding (see cmake/kickos.cmake); this file
# only pins the host system + language baseline.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(KICKOS_ARCH   "sim"  CACHE STRING "KickOS arch backend selected by this toolchain")

# Native compilers; let CMake discover them unless the user overrides.
if(NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER cc)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER c++)
endif()
