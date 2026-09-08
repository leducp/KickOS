# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PSP-BOUNDS arms, one image per guarded leg. pspguard_svc is the only image both ARM
# backends build, so it is what says the class is compiled here at all.

if(NOT TARGET pspguard_svc)
  return()
endif()

if(NOT (KICKOS_CHIP STREQUAL "mps2" OR KICKOS_BOARD STREQUAL "microbit"))
  return()
endif()

# The byte count the refusal must report, carried by the caller and not sniffed by the
# script: only this expectation separates "some leg refused the PSP" from "the leg that
# measured the FP block refused it". Mode 5 expects no refusal at all and takes the same
# argv shape with the count unused, so a call site cannot silently become the wrong arm.
set(_pg_gate "${PROJECT_SOURCE_DIR}/tests/integration/check_pspguard.sh")
# The SVC site's expected byte count is the whole SVC extent, read out of the header the
# guard on THIS arch reads, and the banner is the one that arch's fault reporter prints.
# Both macros must stay the plain integers tests/static/check_trap_redzone.sh scrapes: a
# header that stopped spelling them that way stops the configure instead of quietly pinning
# a stale expectation. The PendSV arms' 36 and 100 are KICKOS_ARMV7M_TRAP_FRAME and
# _FRAME_MAX, written out.
if(KICKOS_ARCH STREQUAL "armv6m")
  set(_pg_hdr
      "${PROJECT_SOURCE_DIR}/arch/arm/armv6m/include/kickos/arch/armv6m_trap_stack.h")
  set(_pg_pfx KICKOS_ARMV6M_TRAP)
  set(_pg_banner ARMV6M)
else()
  set(_pg_hdr
      "${PROJECT_SOURCE_DIR}/arch/arm/armv7m/include/kickos/arch/armv7m_trap_stack.h")
  set(_pg_pfx KICKOS_ARMV7M_TRAP)
  set(_pg_banner ARMV7M)
endif()
# NEST_SVC_DISPATCH exists on armv7m only, and only the KICKOS_KERNEL_STACKS 0 branch below
# reads it. armv6m cannot reach that branch: Kconfig gives it range 1 1 through
# ARCH_KERNEL_STACKS_MANDATORY, so scraping it there would demand a macro that arch has no
# reason to define.
set(_pg_macros NEST_SVC KERNEL_DEPTH_SVC)
if(NOT KICKOS_KERNEL_STACKS)
  list(APPEND _pg_macros NEST_SVC_DISPATCH)
endif()
foreach(_pg_macro ${_pg_macros})
  file(STRINGS "${_pg_hdr}" _pg_hit
       REGEX "^#define ${_pg_pfx}_${_pg_macro} +[0-9]+$")
  list(LENGTH _pg_hit _pg_n)
  if(NOT _pg_n EQUAL 1)
    message(FATAL_ERROR
      "pspguard: ${_pg_pfx}_${_pg_macro} is not one plain integer in ${_pg_hdr}, "
      "so the SVC expectation cannot be derived from the figure the guard enforces")
  endif()
  string(REGEX REPLACE "^.* " "" _pg_${_pg_macro} "${_pg_hit}")
endforeach()
# THE SAME LADDER THE HEADER RESOLVES, and it has to be here rather than read out of one
# macro because the gate scrapes plain integers and cannot evaluate the header's #if. At
# KICKOS_KERNEL_STACKS 1 the dispatch is on the kernel block and the site charges the window
# alone; at 0 it charges the window plus the dispatch.
if(KICKOS_KERNEL_STACKS)
  set(_pg_svc_need "${_pg_NEST_SVC}")
else()
  math(EXPR _pg_svc_need "${_pg_NEST_SVC_DISPATCH} + ${_pg_KERNEL_DEPTH_SVC}")
endif()

if(KICKOS_BOARD STREQUAL "microbit")
  # The armv6m pair, and they are opposite claims now: mode 2 is refused at the SVC class's
  # whole extent, mode 5 is ACCEPTED at the low edge of that same extent and answers with the
  # band verdict instead of a refusal. Both declare `contained`, the refusal costing this
  # backend the offending thread and not the system since it gained the seam.
  kickos_add_qemu_test(TARGET pspguard_svc
    SCRIPT "${_pg_gate}" ARGS 2 ${_pg_svc_need} SVCall "no room below" ${_pg_banner}
                         contained)
  kickos_add_qemu_test(TARGET pspguard_svcdepth
    SCRIPT "${_pg_gate}" ARGS 5 ${_pg_svc_need} SVCall "no room below" ${_pg_banner}
                         contained)
  # The block leg's room bound. THE BOARD THE HAZARD IS REAL ON: it carves blocks and has no
  # MPU, so a thread can aim its own PSP into kernel .bss and nothing refuses the entry. 32 is
  # KICKOS_ARMV6M_TRAP_FRAME, this arch's whole PendSV push, written out the way the MPS2 arms
  # write theirs. The refusal classifies as `under stack_lo`, the blocks sitting below every
  # arena stack, and main.cc refuses to run the arm if that stops being true.
  if(TARGET pspguard_block)
    kickos_add_qemu_test(TARGET pspguard_block
      SCRIPT "${_pg_gate}" ARGS 6 32 PendSV "under stack_lo" ${_pg_banner} contained)
  endif()
  # This board's arm set ends here; everything below is the MPS2 set.
  return()
endif()

kickos_add_qemu_test(TARGET pspguard
  SCRIPT "${_pg_gate}" ARGS 0 36 PendSV "no room below" ${_pg_banner} contained)
kickos_add_qemu_test(TARGET pspguard_svc
  SCRIPT "${_pg_gate}" ARGS 2 ${_pg_svc_need} SVCall "no room below" ${_pg_banner} contained)

kickos_add_qemu_test(TARGET pspguard_svcdepth
  SCRIPT "${_pg_gate}" ARGS 5 ${_pg_svc_need} SVCall "no room below" ${_pg_banner} contained)
# The block leg's room bound, on the MPS2 presets that carve blocks and do NOT enforce. Built
# only there, hence the TARGET guard: with enforcement the app is not compiled at all. 36 is
# KICKOS_ARMV7M_TRAP_FRAME, this arch's PendSV push, written out the way the arms above write
# theirs. The refusal classifies as `under stack_lo`, the blocks sitting below every arena
# stack, and main.cc refuses to run the arm if that stops being true.
if(TARGET pspguard_block)
  kickos_add_qemu_test(TARGET pspguard_block
    SCRIPT "${_pg_gate}" ARGS 6 36 PendSV "under stack_lo" ${_pg_banner} contained)
endif()
kickos_add_qemu_test(TARGET pspguard_above
  SCRIPT "${_pg_gate}" ARGS 3 36 PendSV "at or above stack_hi" ${_pg_banner} contained)
kickos_add_qemu_test(TARGET pspguard_below
  SCRIPT "${_pg_gate}" ARGS 4 36 PendSV "under stack_lo" ${_pg_banner} contained)
if(TARGET pspguard_fp)
  kickos_add_qemu_test(TARGET pspguard_fp
    SCRIPT "${_pg_gate}" ARGS 1 100 PendSV "no room below" ${_pg_banner} contained)

  # THE SWITCH CHAIN ACROSS A CONTAINED REFUSAL. Containment performs a physical swap without
  # going through either switcher, so it emits its own SWITCH record; kicktrace's structural
  # pass walks the chain requiring each `from` to equal the previous `to`, and a swap that
  # emitted nothing breaks the NEXT record rather than its own. telemetry_qemu_structural
  # covers the ordinary PendSV tail on tele_pingpong and never refuses a PSP, so only this
  # arm reaches the containment emit. Registered where telemetry is actually compiled.
  if(KICKOS_BOARD STREQUAL "qemu" AND KICKOS_TELEMETRY)
    kickos_add_qemu_test(NAME telemetry_qemu_contained TARGET pspguard_fp
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/telemetry/check_qemu.py"
      TIMEOUT 40 ARGS "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
  endif()
endif()
