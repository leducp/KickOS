# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Failure-path gate for kickos::driver::bring_up. The choreography calls nothing but a
# handful of extern "C" kos_* syscalls, so a host program drives the real bodies over the
# recording fake in kos_seam.cc: no kernel, no scheduler, no image.
#
# user/src/driver_service.cc is COMPILED IN, never linked from kickos_user: that archive also
# carries the real syscall stubs, which would satisfy the kos_* names instead of the seam.
#
# HOST-ONLY, like the uartclass gate and for the same mechanism: kos_seam.cc defines public
# kos_* names, and a target image linking it would satisfy them from the executable and keep
# the real stubs' archive members out of the link (tests/static/check_class_backend.sh).
#
# driver_service.h uses designated initialisers in a constexpr aggregate and does not parse at
# gnu++17, so it needs the cxx_std_20 kickos_add_unit_test sets.
kickos_add_unit_test(NAME drv_bringup
  SOURCES bringup_unwind.cc kos_seam.cc
          "${PROJECT_SOURCE_DIR}/user/src/driver_service.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")

# A failing UART driver thread by posture: the thread bodies of uart_service.h and
# user/src/uart_service.cc over the same seam, a trap reaching the host as SIGILL.
kickos_add_unit_test(NAME drv_thread_trap
  SOURCES thread_trap.cc kos_seam.cc
          "${PROJECT_SOURCE_DIR}/user/src/driver_service.cc"
          "${PROJECT_SOURCE_DIR}/user/src/uart_service.cc"
          "${PROJECT_SOURCE_DIR}/user/src/console_ring.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")

# A zero-length send to a buffered UART console is a flush: the service and IRQ loops of
# uart_service.h on two host threads over a fake device. HOST-ONLY: uart_flush.cc defines public
# kos_* names.
kickos_add_unit_test(NAME drv_uart_flush
  SOURCES uart_flush.cc
          "${PROJECT_SOURCE_DIR}/user/src/uart_service.cc"
          "${PROJECT_SOURCE_DIR}/user/src/uart_service_dev.cc"
          "${PROJECT_SOURCE_DIR}/user/src/console_ring.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")

# The USIC SSC engine's and UART backend's routing of their events onto their line's index, over
# a host buffer for a window. HOST-ONLY: usic_route.cc defines public kos_* names.
kickos_add_unit_test(NAME drv_usic_route
  SOURCES usic_route.cc
          "${PROJECT_SOURCE_DIR}/system/driver/xmc4800/xmcssc/spi_usic.cc"
          "${PROJECT_SOURCE_DIR}/system/driver/xmc4800/uart_usic.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800" "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800/class"
           "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")
target_compile_options(drv_usic_route PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/usic_mmap.h")

# The driver witness's serve loop over its own two fakes: a reply to a caller whose timed call
# expired is not the driver's failure. HOST-ONLY: serve_reply.cc defines public kos_* names.
kickos_add_unit_test(NAME drv_witness_serve
  SOURCES serve_reply.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")

# The same bring-up built for more than one kernel core, in a directory of its own: its posture
# scrubs the build's core count from the directory it is declared in.
add_subdirectory(cores)

# The ESP UART family's flush over a host buffer for its window, once per chip. HOST-ONLY:
# uart_esp.cc defines the public kos_uart_* names.
foreach(_esp_chip "esp32c6;riscv" "esp32;xtensa")
  list(GET _esp_chip 0 _esp_name)
  list(GET _esp_chip 1 _esp_arch)
  kickos_add_unit_test(NAME drv_esp_uart_flush_${_esp_name} TEST_PREFIX "${_esp_name}."
    SOURCES esp_uart_flush.cc "${PROJECT_SOURCE_DIR}/system/driver/espuart/uart_esp.cc"
    INCLUDES "${PROJECT_SOURCE_DIR}/arch/${_esp_arch}/chip/${_esp_name}"
             "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include")
  target_compile_options(drv_esp_uart_flush_${_esp_name} PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/esp_mmap.h")
endforeach()

# The USB CDC class layer's bulk IN pump and the service's flush over a recording fake
# controller. HOST-ONLY: usb_zlp.cc defines public kos_* names.
kickos_add_unit_test(NAME drv_usb_zlp
  SOURCES usb_zlp.cc "${PROJECT_SOURCE_DIR}/user/src/console_ring.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_BINARY_DIR}/generated/include")
