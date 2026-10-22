# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# These standalone programs and Python checks do not require GTest.
add_subdirectory(telemetry)

if(KICKOS_BUILD_UNIT_TESTS)
  # kickos_add_kseam(<name> [POSTURE <MACRO=VALUE>...] [EXTRA <cc...>] [INCLUDES <dirs...>])
  #   The K-seam fixture (tests/unit/kfixture) over the kernel's stateful core, at a posture,
  #   with EXTRA translation units beside it. klock.cc is listed at every posture: below two
  #   kernel cores it compiles to nothing. A posture linking the real console.cc takes its
  #   kpanic from there, and one linking the real word.cc or syscall_mem.cc its mutex word
  #   access (word_seam.cc's definitions are weak).
  function(kickos_add_kseam name)
    cmake_parse_arguments(KS "" "" "POSTURE;EXTRA;INCLUDES" ${ARGN})
    set(_fx "${PROJECT_SOURCE_DIR}/tests/unit/kfixture")
    set(_k "${PROJECT_SOURCE_DIR}/kernel")
    set(_common "")
    if(NOT "KSEAM_REAL_CONSOLE=1" IN_LIST KS_POSTURE)
      list(APPEND _common "${PROJECT_SOURCE_DIR}/tests/unit/common/kpanic_seam.cc")
    endif()
    kickos_add_kernel_host_lib(${name}
      SOURCES "${_fx}/kfixture.cc" "${_fx}/karch_seam.cc" "${_fx}/word_seam.cc" ${_common}
        ${KS_EXTRA}
        "${_k}/sched/sched.cc" "${_k}/sched/policy_fifo_rr.cc" "${_k}/sync/sync.cc"
        "${_k}/sync/klock.cc" "${_k}/thread/park.cc" "${_k}/syscall/cap.cc"
        "${_k}/task/task.cc" "${_k}/irq/irq.cc" "${_k}/irq/irq_route.cc"
        "${_k}/notify/notify.cc" "${_k}/init/instance.cc" "${_k}/init/fault.cc"
        "${PROJECT_SOURCE_DIR}/arch/common/arch_irq_line_core_default.cc"
      INCLUDES "${_fx}" ${KS_INCLUDES}
      POSTURE ${KS_POSTURE})
  endfunction()

  set(_cs "${PROJECT_SOURCE_DIR}/tests/unit/consoleseam")
  set(_console_alone "${_cs}/console_alone.cc" "${_cs}/console_leaves.cc"
                     "${PROJECT_SOURCE_DIR}/lib/libc/fmt.cc")
  set(_console_seam "${_cs}/console_seam.cc" "${_cs}/ring_leaves.cc" "${_cs}/console_leaves.cc"
                    "${PROJECT_SOURCE_DIR}/lib/libc/fmt.cc")

  # Independent targets; consumers of shared fixture libraries follow their providers.
  foreach(_unit IN ITEMS
      ampconsole amphold ampreset ampshare ampwindow atomiccas benchdist c6apm capreply captable consolebaud consoleline consoleown
      consolepace consolering consoletxsmp ctxredirect dcacheplan
      deathspace doorbell drvbringup fastrefuse faultline frame fusedwake grantlookup grantnocache
      initwalk irqquiesce kstring
      k64fsysmpu ktime mapexec mapfence mapshoot mpuoverlap mpuskip mtimeconv placeadmit presync ramalign ramowner regionsync
      reschedowed rp2350gate rp2xxxbringup slotpool
      stdoutwrite taskbudget uartclass usersem vrange windowholder)
    add_subdirectory(${_unit})
  endforeach()

  # Define providers first so kickos_add_unit_test can read their configuration
  # macros and avoid conflicts with preset definitions.
  add_subdirectory(kfixture)
  foreach(_unit IN ITEMS
      cancelkind capbadge capprobe capsweep consolepub faultband faultrecord irqbindown libclock lockid parkresult priodonor rangecheck rootslot
      rrslice schedwake slayhook taskdeath taskpool v6mframe wildstack windowclose wordref)
    add_subdirectory(${_unit})
  endforeach()

  add_subdirectory(exitquiesce)
  add_subdirectory(reentseat)
  add_subdirectory(sysdispatch)
  foreach(_unit IN ITEMS consolehold irqlinecore parkdeath placement schedring semtimed mutextimed
      slaypeer tlscarve)
    add_subdirectory(${_unit})
  endforeach()
endif()
