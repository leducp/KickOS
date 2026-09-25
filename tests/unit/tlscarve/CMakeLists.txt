# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# TLS carve gate for kernel/thread/tls.cc. Host-only of necessity: the four .tdata/.tbss
# bounds are linker symbols, so a target image only ever presents the ONE layout its own
# link produced, and neither a linker pad between the two sections nor an empty pair can be
# staged there. A host program fabricates the layout it wants in a data section and reads
# the block size that follows.
#
# ONE EXECUTABLE PER LAYOUT AND PER POSTURE, because the layout is fixed at link time and the
# posture is an #if inside the kernel source the shim compiles:
#
#   span   .tdata, a 16-byte pad, .tbss: the block must be sized by the SPAN, since the
#          compiler's thread_local offsets are relative to the layout the linker produced,
#          pad included, and the sum would carve short by that pad
#   empty  no thread_local anywhere, where .tbss lands four bytes BELOW .tdata and only the
#          two section sizes can decide emptiness
#
# Takes the kernel target's include path and definitions verbatim: the shim compiles a
# kernel source, so a divergent config here would gate a different translation unit than the
# one that ships.
foreach(_case span empty)
  kickos_add_unit_test(NAME tls_carve_${_case}
    SOURCES tlscarve_shim.cc tlscarve_${_case}.cc
    KERNEL
    LIBRARIES kickos_kseam)
endforeach()

# `seat` differs from the two above in ONE clause, TLSCARVE_FROM_SP 0: the arch that SEATS the
# thread pointer from the context. Same fabrication, same stride, same reserve, so what it
# reports is attributable to that clause.
kickos_add_unit_test(NAME tls_carve_seat
  SOURCES tlscarve_shim.cc tlscarve_seat.cc
  KERNEL
  LIBRARIES kickos_kseam
  DEFINITIONS TLSCARVE_FROM_SP=0)

# `idle` is a masking arch with a thread_local declared above one kernel core: every core's idle
# is refused by admission yet must still be created.
kickos_add_unit_test(NAME tls_carve_idle
  SOURCES tlscarve_shim.cc tlscarve_idle.cc
  LIBRARIES kickos_kseam_smp
  DEFINITIONS TLSCARVE_STRIDE=4096u)

# `reent` is the shared-kernel lx6 posture, the carve owed with no thread_local. Its mechanism is
# read off ARCH_LX6 in arch/Kconfig, so a select of ARCH_TLS_FROM_SP there fails this target.
file(READ "${PROJECT_SOURCE_DIR}/arch/Kconfig" _tlscarve_arch_kconfig)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/arch/Kconfig")
string(REGEX MATCH "\nconfig ARCH_LX6\n(\t[^\n]*\n)+" _tlscarve_lx6 "${_tlscarve_arch_kconfig}")
if(_tlscarve_lx6 STREQUAL "")
  message(FATAL_ERROR "tls_carve_reent: arch/Kconfig declares no `config ARCH_LX6` block")
endif()
set(_tlscarve_lx6_from_sp 0)
if(_tlscarve_lx6 MATCHES "select ARCH_TLS_FROM_SP\n")
  set(_tlscarve_lx6_from_sp 1)
endif()
kickos_add_unit_test(NAME tls_carve_reent
  SOURCES tlscarve_shim.cc tlscarve_reent.cc
  LIBRARIES kickos_kseam_smp
  DEFINITIONS TLSCARVE_REENT_IN_TCB=1 TLSCARVE_STRIDE=4096u
              TLSCARVE_FROM_SP=${_tlscarve_lx6_from_sp})
