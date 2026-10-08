# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `hello`. Most of them are not about hello: it is the smallest image that
# boots a chip, so a gate needing any linked image of this board takes this one. Each such
# gate says what it reads and out of what.

if(NOT TARGET hello)
  return()
endif()
kickos_app_judge(hello tests/integration/check_qemu_hello.sh)

set(_hello_qemu "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    hello_demo
    COMMAND "${CMAKE_COMMAND}" -E env
            "${PROJECT_SOURCE_DIR}/tests/integration/check_hello.py"
            "$<TARGET_FILE:hello>")
  add_test(
    NAME    hello_c_demo
    COMMAND "${CMAKE_COMMAND}" -E env
            "${PROJECT_SOURCE_DIR}/tests/integration/check_hello.py"
            "$<TARGET_FILE:hello_c>")
  # Above check_hello.py's own waits, 10 s for the exchanges and 5 s for the exit.
  set_tests_properties(hello_demo hello_c_demo PROPERTIES TIMEOUT 20)
endif()

# Every board with an emulator boots these images through the same script and reports under the
# derived <board tag>_hello and <board tag>_hello_c names.
kickos_add_qemu_test(TARGET hello SCRIPT "${_hello_qemu}")
kickos_add_qemu_test(TARGET hello_c SCRIPT "${_hello_qemu}")

# The secondary-arrival gate, on the smallest image that boots the chip: every core comes up
# in arch_init, so the app itself is only what carries the release there. The expected count is
# the configured one, and the script boots the image a second time on a machine one core short.
#
# Both backends bind the short-machine arm to the count, by different mechanisms: arm64 has
# firmware that refuses a start, and rv64 has none, so there the missing hart never publishes
# arrival and the bounded wait names it.
#
# Not x86_64, for this gate or the doorbell one below: q35 prints no `# smp:` banner, and QEMU's
# x86 model names no core in an interrupt event, so a second channel would have to read the
# execution log. The selftest holds both claims there, its `# smp sched` line counting the cores
# in the scheduler and its doorbell_xpoke arm running on every SMP image.
if(KICKOS_NUM_CORES GREATER 1 AND KICKOS_ARCH MATCHES "^(armv8a|rv64imac)$")
  kickos_add_qemu_test(NAME ${_tag}_smp_arrival TARGET hello
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_arrival.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" ${KICKOS_ARCH}
    BOOTS 2)
endif()

# The doorbell-and-lock gate, on the same image: the round runs in arch_init, so the app is only
# what carries it there. It reads QEMU's own GIC trace beside the console, which is why it needs
# no app of its own and why its verdict does not rest on a number the image printed.
#
# The rv64 arm has one channel rather than two and its header says so: the CLINT store that
# raises a doorbell carries no trace event, so no arm there counts raises. What the trap log
# does carry is the hart and the cause, which is what the per-peer and initiator arms read.
# It is keyed on the kernel-core count where arm64 is keyed on the machine's.
set(_hello_doorbell FALSE)
if(KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_NUM_CORES GREATER 1)
  set(_hello_doorbell TRUE)
elseif(KICKOS_ARCH STREQUAL "rv64imac" AND KICKOS_KERNEL_CORES GREATER 1)
  set(_hello_doorbell TRUE)
endif()
if(_hello_doorbell)
  kickos_add_qemu_test(NAME ${_tag}_smp_doorbell TARGET hello
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_smp_doorbell.sh"
    ARGS ${KICKOS_NUM_CORES} "pong 3" ${KICKOS_ARCH})
  # Serial: the emulator arm reads whether a core took the doorbell interrupt, and a core that
  # is already spinning in the lock's acquire loop answers by polling instead and acknowledges
  # nothing. Which path runs depends on host scheduling, the guest clock tracking host time, so
  # peers competing for CPU turn a correct image red.
  set_tests_properties(${_tag}_smp_doorbell PROPERTIES RUN_SERIAL TRUE)
endif()

# The TLB maintenance the map editor spends, read out of the linked image. Registered from both
# arm64 postures because the shareability it requires differs between them and the gate is handed
# the core count; it runs no image, so it carries the host label.
if(KICKOS_ARCH STREQUAL "armv8a")
  add_test(NAME tlbi_shareability
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_tlbi_shareability.sh"
            "$<TARGET_FILE:hello>" "${KICKOS_KERNEL_CORES}" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(tlbi_shareability)
endif()

# The doorbell service's instruction barrier, read out of the linked image. Keyed on the core
# count, not the kernel-core count: the service is compiled whenever the image drives more than one
# core, so it exists under AMP, where one kernel schedules one core.
# It runs no image, so it carries the host label.
#
# armv8a only, and rv64imac is absent by ruling rather than by oversight: the instruction-side
# barrier is FENCE.I there, and Zifencei is not in that board's ISA baseline, so the backend has
# no such instruction to assert. Its service body carries the translation-side fence alone.
if(KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_NUM_CORES GREATER 1)
  add_test(NAME doorbell_isb
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_doorbell_isb.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}")
  kickos_host_gate(doorbell_isb)
endif()

# Orders and words of the linked image no build rule can see (tests/static/check_image_rules.sh).
if(KICKOS_ARCH STREQUAL "armv8a")
  kickos_image_rule(arm64_entry hello)
elseif(KICKOS_ARCH STREQUAL "rv32imac")
  kickos_image_rule(rv32_trap hello "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
                    "${KICKOS_CHIP_LIMITS_H}")
endif()
if(KICKOS_NUM_CORES GREATER 1 AND KICKOS_CHIP STREQUAL "rp2350")
  kickos_image_rule(rp_node hello "${PROJECT_BINARY_DIR}/generated/chip/chip_layout.h")
endif()
if(KICKOS_CHIP STREQUAL "esp32c6" AND NOT (KICKOS_AMP_OWN_IMAGE AND KICKOS_AMP_NODE_ID EQUAL 1))
  kickos_image_rule(c6_hp hello)
endif()
if(KICKOS_NUM_CORES EQUAL 1)
  kickos_image_rule(cpu_id hello)
endif()

# The doorbell service's route drain, read out of the source tree. Keyed on nothing: the call is
# a source property, so every build checks it.
# It runs no image, so it carries the host label.
add_test(NAME route_service_order
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_route_service_order.sh"
          "${PROJECT_SOURCE_DIR}")
kickos_host_gate(route_service_order)

# The ESP UART's TX-empty acknowledgement, read out of the source tree. Keyed on nothing: the
# three bodies are source whichever board this build is for, and two of the three are compiled
# on chips this preset may not name.
# It runs no image, so it carries the host label.
#
# Structural because no board in the fleet can witness it: neither ESP part has an emulator, so
# nothing in a run raises the interrupt whose acknowledgement this is.
add_test(NAME esp_tx_latch_ack
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_esp_tx_latch_ack.sh"
          "${PROJECT_SOURCE_DIR}")
kickos_host_gate(esp_tx_latch_ack)

# The sole users of the delivery-gating seams, the software raise and the IPC fastpath, read out
# of the source tree. Keyed on nothing: the rule binds at one kernel core too.
# It runs no image, so it carries the host label.
add_test(NAME irq_line_op_sole
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_irq_line_op_sole.sh"
          "${PROJECT_SOURCE_DIR}")
kickos_host_gate(irq_line_op_sole)

# The per-core ATOMCTL seat with its read-back, and the interrupt posture the secondary park
# holds across its sleep decision, read out of the linked image. The seat on every LX6 image:
# ATOMCTL governs every S32C1I the image can execute, and a single-core build reaches the
# register on the same boot path. The park only where the image drives more than one core,
# the one place it is compiled.
# It runs no image, so it carries the host label.
if(KICKOS_ARCH STREQUAL "lx6")
  add_test(NAME lx6_park_mask
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_lx6_park_mask.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}" ${KICKOS_NUM_CORES})
  kickos_host_gate(lx6_park_mask)
endif()

# arch_ipi_fence's own barrier, read out of the linked image. It is reached through a plain call
# from amp::window_init and from the deferred-seat scan in the raise path, and no callgraph gate
# follows either, so nothing else in the tree would notice the body being emptied.
#
# Keyed on where a caller exists, and not on the core count: the deferred-seat pairing lives in
# the GICv3 backend and in the AMP window, so a GICv2 SMP image links no caller and
# --gc-sections drops the symbol entirely. imx8mp-evk is the other half of the same key, being
# GICv3 at one core and no AMP node, so it links no caller either.
#
# rv64 owes it for the AMP window alone, and not for a hart count. Its raise names a dense hart
# index rather than an affinity a peer must publish, so it defers no target and there is no
# store-then-load pairing for a fence to serve; the publication it does owe is ordered inside
# kickos_rv64_doorbell_send by a `fence rw, ow` of its own, the CLINT sitting in an I/O PMA. So
# a multi-hart rv64 image links no caller, and a clause keyed on the hart count demands a symbol
# the linker is entitled to drop. The RP2350 and ESP32-C6 owe it for the AMP window alone too.
# An M-profile DMB has one defined option, SY; a seq_cst fence emits the reserved `dmb ish`.
# It runs no image, so it carries the host label.
set(_fence_full "")
set(_fence_refused "")
if(KICKOS_ARCH STREQUAL "armv8a" AND KICKOS_ARM64_GIC_VERSION EQUAL 3
   AND (KICKOS_AMP_NODE OR KICKOS_NUM_CORES GREATER 1))
  set(_fence_full "ish")
  set(_fence_refused "ishld ishst")
elseif(KICKOS_ARCH MATCHES "^rv(32|64)imac$" AND KICKOS_AMP_NODE)
  set(_fence_full "rw,rw")
  set(_fence_refused "r,r w,w rw,w r,rw")
elseif(KICKOS_ARCH STREQUAL "armv7m" AND KICKOS_AMP_NODE)
  set(_fence_full "sy")
endif()
if(NOT _fence_full STREQUAL "")
  add_test(NAME ipi_fence
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_ipi_fence.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}"
            "${_fence_full}" "${_fence_refused}")
  kickos_host_gate(ipi_fence)
endif()

# The store->load fence both sides of the rv64imac interrupt-controller handshake owe, read out
# of the linked image. Registered at every core count, and told the kernel-core count because
# the handshake's shape follows it: image-wide words at one kernel core, a post to the line's
# hart above it.
# It runs no image, so it carries the host label.
#
# rv64imac only. The other backends reach an interrupt controller in hardware and take no
# software handshake between two words; where one exists it is a different shape and this
# reader's word names do not occur.
if(KICKOS_ARCH STREQUAL "rv64imac")
  add_test(NAME rv64_irq_fence
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_rv64_irq_fence.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${KICKOS_KERNEL_CORES}")
  kickos_host_gate(rv64_irq_fence)
endif()

# The send and the rendezvous never entering the scheduler, and lx6's dispatch consuming the
# reschedule ask, read out of the linked image. Keyed on the kernel-core count: the ask and the
# dispatch's scheduler entry exist only where one kernel schedules more than one core.
# It runs no image, so it carries the host label.
if(KICKOS_KERNEL_CORES GREATER 1
   AND (KICKOS_ARCH STREQUAL "armv8a" OR KICKOS_ARCH STREQUAL "rv64imac"
        OR KICKOS_ARCH STREQUAL "lx6"))
  add_test(NAME doorbell_generic
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_doorbell_generic.sh"
            "$<TARGET_FILE:hello>" "${CMAKE_NM}" "${CMAKE_OBJDUMP}" "${KICKOS_ARCH}")
  kickos_host_gate(doorbell_generic)
endif()
