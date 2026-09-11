# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `hello`. Most of them are not ABOUT hello: it is the smallest image that
# boots a chip, so a gate needing any linked image of this board takes this one. Each such
# gate says what it reads and out of what.

if(NOT TARGET hello)
  return()
endif()

set(_hello_qemu "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    hello_demo
    COMMAND "${CMAKE_COMMAND}" -E env
            "${PROJECT_SOURCE_DIR}/tests/integration/check_hello.py"
            "$<TARGET_FILE:hello>")
  set_tests_properties(hello_demo PROPERTIES TIMEOUT 15)
endif()

# Every board with an emulator boots this image through the same script and reports under the
# derived <board tag>_hello name. The one exclusion is a POSTURE and not a board: the
# enforcing rv32imac build registers no hello arm.
if(NOT (KICKOS_BOARD STREQUAL "qemu-riscv" AND KICKOS_HAVE_MPU))
  kickos_add_qemu_test(TARGET hello SCRIPT "${_hello_qemu}")
endif()

# The secondary-arrival gate, on the smallest image that boots the chip: every core comes up
# in arch_init, so the app itself is only what carries the release there. The expected count is
# the configured one, and the script boots the image a second time on a machine one core short.
# TIMEOUT covers two boots plus the arrival spin bound the refusal path has to reach.
#
# Both backends bind the short-machine arm to the count, by different mechanisms: arm64 has
# firmware that REFUSES a start, and rv64 has none, so there the missing hart never publishes
# arrival and the bounded wait names it.
if(KICKOS_NUM_CORES GREATER 1
   AND (KICKOS_BOARD STREQUAL "qemu-arm64" OR KICKOS_BOARD STREQUAL "qemu-riscv64"))
  kickos_add_qemu_test(NAME ${_tag}_smp_arrival TARGET hello
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_arrival.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" ${KICKOS_ARCH}
    TIMEOUT 240)
endif()

# The doorbell-and-lock gate, on the same image: the round runs in arch_init, so the app is only
# what carries it there. It reads QEMU's own GIC trace beside the console, which is why it needs
# no app of its own and why its verdict does not rest on a number the image printed.
#
# The rv64 arm has ONE channel rather than two and its header says so: the CLINT store that
# raises a doorbell carries no trace event, so no arm there counts raises. What the trap log
# does carry is the hart and the cause, which is what the per-peer and initiator arms read.
# It is keyed on the KERNEL-core count where arm64 is keyed on the machine's.
set(_hello_doorbell FALSE)
if(KICKOS_BOARD STREQUAL "qemu-arm64" AND KICKOS_NUM_CORES GREATER 1)
  set(_hello_doorbell TRUE)
elseif(KICKOS_BOARD STREQUAL "qemu-riscv64" AND KICKOS_KERNEL_CORES GREATER 1)
  set(_hello_doorbell TRUE)
endif()
if(_hello_doorbell)
  kickos_add_qemu_test(NAME ${_tag}_smp_doorbell TARGET hello
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_doorbell.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" ${KICKOS_ARCH}
    TIMEOUT 240)
  # Serial: the emulator arm reads whether a core took the doorbell INTERRUPT, and a core that
  # is already spinning in the lock's acquire loop answers by polling instead and acknowledges
  # nothing. Which path runs depends on host scheduling, the guest clock tracking host time, so
  # peers competing for CPU turn a correct image red.
  set_tests_properties(${_tag}_smp_doorbell PROPERTIES RUN_SERIAL TRUE)
endif()

# The TLB maintenance the map editor spends, read out of the linked image. Registered from BOTH
# arm64 postures because the shareability it requires differs between them and the gate is handed
# the core count; it runs no image, so it carries the host label.
if(KICKOS_ARCH STREQUAL "armv8a")
  add_test(NAME tlbi_shareability
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_tlbi_shareability.sh"
            "$<TARGET_FILE:hello>" "${KICKOS_KERNEL_CORES}" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(tlbi_shareability)
endif()

# Three orderings the arm64 entry and timer paths owe, read out of the linked image: the SPSel
# select ahead of the first stack write, and the ISB after each CNTP_CTL_EL0 disable ahead of the
# Device write it protects. Registered on the ARCH and not on a board or a core count: all three
# bodies are compiled on every armv8a posture, both GIC versions and one core included.
# It runs no image, so it carries the host label.
#
# Structural because no arm64 machine in the fleet can witness any of the three: QEMU enters at
# reset with PSTATE.SP already 1, and its timer model deasserts on the register write.
if(KICKOS_ARCH STREQUAL "armv8a")
  add_test(NAME arm64_entry_order
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_arm64_entry_order.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(arm64_entry_order)
endif()

# The doorbell service body's instruction barrier and its position, read out of the linked image.
# Keyed on the CORE count, not the kernel-core count: the service body is compiled whenever the
# image drives more than one core, so it exists under AMP, where one kernel schedules one core.
# It runs no image, so it carries the host label.
#
# armv8a ONLY, and rv64imac is absent by ruling rather than by oversight: the instruction-side
# barrier is FENCE.I there, and Zifencei is not in that board's ISA baseline, so the backend has
# no such instruction to assert. Its service body carries the TRANSLATION-side fence alone.
if(KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_NUM_CORES GREATER 1)
  add_test(NAME doorbell_isb
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_doorbell_isb.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(doorbell_isb)
endif()

# Where the route drain sits in every doorbell service body, read out of the SOURCE TREE. Keyed
# on nothing: the ordering is a source property, so every build checks all three bodies.
# It runs no image, so it carries the host label.
add_test(NAME route_service_order
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_route_service_order.sh"
          "${PROJECT_SOURCE_DIR}")
kickos_host_gate(route_service_order)

# The IrqLock bracket on the IRQ syscall arms that touch image-wide controller words, read out
# of the SOURCE TREE. Keyed on nothing: IrqLock folds to the local mask at one kernel core, so
# the shape a second core depends on is checked in every build.
# It runs no image, so it carries the host label.
add_test(NAME irq_syscall_locked
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_irq_syscall_locked.sh"
          "${PROJECT_SOURCE_DIR}")
kickos_host_gate(irq_syscall_locked)

# The sole decider of a logical line's delivery gating, read out of the SOURCE TREE. Keyed on
# nothing: the rule binds at one kernel core too.
# It runs no image, so it carries the host label.
add_test(NAME irq_line_op_sole
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_irq_line_op_sole.sh"
          "${PROJECT_SOURCE_DIR}")
kickos_host_gate(irq_line_op_sole)

# The per-core ATOMCTL seat and the read-back beside it, read out of the linked image.
# UNCONDITIONAL on the core count: ATOMCTL governs every S32C1I the image can execute, and a
# single-core LX6 build reaches the register on the same boot path.
# lx6 ONLY: Special Register 99 is Xtensa's.
# It runs no image, so it carries the host label.
if(KICKOS_ARCH STREQUAL "lx6")
  add_test(NAME lx6_atomctl
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_lx6_atomctl.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(lx6_atomctl)
endif()

# The interrupt posture the LX6 secondary park holds across its sleep decision, read out of the
# linked image. Keyed on the CORE count: the park is compiled only where the image drives more
# than one core.
# It runs no image, so it carries the host label.
if(KICKOS_ARCH STREQUAL "lx6" AND KICKOS_NUM_CORES GREATER 1)
  add_test(NAME lx6_park_mask
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_lx6_park_mask.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(lx6_park_mask)
endif()

# arch_ipi_fence's own barrier, read out of the linked image. It is reached through a plain call
# from amp::window_init and from the deferred-seat scan in the raise path, and no callgraph gate
# follows either, so nothing else in the tree would notice the body being emptied.
#
# KEYED ON WHERE A CALLER EXISTS, and not on the core count: the deferred-seat pairing lives in
# the GICv3 backend and in the AMP window, so a GICv2 SMP image links no caller and
# --gc-sections drops the symbol entirely. imx8mp-evk is the other half of the same key, being
# GICv3 at one core and no AMP node, so it links no caller either.
#
# RV64 OWES IT FOR THE AMP WINDOW ALONE, AND NOT FOR A HART COUNT. Its raise names a dense hart
# index rather than an affinity a peer must publish, so it defers no target and there is no
# store-then-load pairing for a fence to serve; the publication it does owe is ordered inside
# kickos_rv64_doorbell_send by a `fence rw, ow` of its own, the CLINT sitting in an I/O PMA. So
# a multi-hart rv64 image links no caller, and a clause keyed on the hart count demands a symbol
# the linker is entitled to drop. The clause lights up with the first rv64 AMP node instead.
# It runs no image, so it carries the host label.
set(_fence_full "")
set(_fence_refused "")
if(KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_ARM64_GIC_VERSION EQUAL 3
   AND (KICKOS_AMP_NODE OR KICKOS_NUM_CORES GREATER 1))
  set(_fence_full "ish")
  set(_fence_refused "ishld ishst")
elseif(KICKOS_ARCH STREQUAL "rv64imac" AND KICKOS_AMP_NODE)
  set(_fence_full "rw,rw")
  set(_fence_refused "r,r w,w rw,w r,rw")
endif()
if(NOT _fence_full STREQUAL "")
  add_test(NAME ipi_fence
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_ipi_fence.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}"
            "${_fence_full}" "${_fence_refused}")
  kickos_host_gate(ipi_fence)
endif()

# The store->load fence both sides of the rv64imac interrupt-controller handshake owe, read out
# of the linked image. UNCONDITIONAL on the core count, as the fence is: the three words the
# handshake turns on are image-wide rather than per hart, so a fence keyed on the count would
# encode an assumption about who may call that the backend does not enforce.
# It runs no image, so it carries the host label.
#
# rv64imac ONLY. The other backends reach an interrupt controller in hardware and take no
# software handshake between two words; where one exists it is a different shape and this
# reader's word names do not occur.
if(KICKOS_ARCH STREQUAL "rv64imac")
  add_test(NAME rv64_irq_fence
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_rv64_irq_fence.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(rv64_irq_fence)
endif()

# The one-cell-per-line invariant the lx6 interrupt-controller cells rest on, read out of the
# linked image, plus the atomic declaration in the source that each access needs. A mask is a
# store of 0 and an unmask a store of 1, whole values; an edit deriving a stored value from a
# loaded one reintroduces the cross-core lost update with no local symptom and no failing arm.
# UNCONDITIONAL on the core count, as the invariant is: the cells are image-wide rather than per
# core.
# lx6 ONLY: this reader's cell names occur in no other backend.
# It runs no image, so it carries the host label.
# Keyed on KICKOS_ENABLE_SELFTEST: arch_irq_inject, one of the four bodies read, is reached only
# from the inject syscall arm and the bench, so --gc-sections drops it from an image built
# without either, and this gate refuses an absence it cannot tell from a failure to read.
if(KICKOS_ARCH STREQUAL "lx6" AND KICKOS_ENABLE_SELFTEST)
  add_test(NAME lx6_irq_cells
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_lx6_irq_cells.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(lx6_irq_cells)
endif()

# The boundary between the doorbell's rendezvous half and its scheduling half, read out of the
# linked image. Keyed on the KERNEL-core count: the reschedule cell and the dispatch's scheduler
# entry exist only where one kernel schedules more than one core, and under AMP there is no
# scheduling half to separate.
# It runs no image, so it carries the host label.
if(KICKOS_KERNEL_CORES GREATER 1
   AND (KICKOS_ARCH STREQUAL "armv8a" OR KICKOS_ARCH STREQUAL "rv64imac"
        OR KICKOS_ARCH STREQUAL "lx6"))
  add_test(NAME doorbell_generic
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_doorbell_generic.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(doorbell_generic)
endif()

# Node 1's own vector table, read out of the linked image: every line but the doorbell parks,
# and the doorbell reaches the service body.
# It runs no image, so it carries the host label.
if(KICKOS_NUM_CORES GREATER 1 AND KICKOS_CHIP STREQUAL "rp2350")
  add_test(NAME rp_node_vectors
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_rp_node_vectors.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${KICKOS_CHIP}"
            "${PROJECT_SOURCE_DIR}")
  kickos_host_gate(rp_node_vectors)
endif()
