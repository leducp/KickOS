# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800 console DoS probe: a storm thread holding USIC0 channel 1 reroutes its receive interrupt
# onto the console's service-request node and storms it, while the task's entry heartbeats to show
# the console surviving. Three profiles from one source, one composition. Validated by an operator
# reading the heartbeat under load on silicon.
foreach(_t inprstorm inprstormmax inprstormfifo)
  add_executable(${_t} main.cc)
  kickos_app_system(${_t} COMPOSITION system.yaml SYSTEM inprstorm_system)
  kickos_emit_image(${_t})
  kickos_app_judge(${_t} tests/integration/check_inprstorm.sh)
  target_include_directories(${_t} PRIVATE "${PROJECT_SOURCE_DIR}/arch/arm/chip/xmc4800")
endforeach()
target_compile_definitions(inprstormmax PRIVATE INPRSTORM_MAX_RATE=1)
target_compile_definitions(inprstormfifo PRIVATE INPRSTORM_FIFO=1)
