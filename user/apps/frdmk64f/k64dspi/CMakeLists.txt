# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# K64F/DSPI0 consumer of the SPI class, built BOTH WAYS from one source: the client body is
# identical over the proxy and over the local engine, and only who hands the client which
# capability differs. KICKOS_SPI_LOCAL_ENGINE selects both the backend and the composition, and
# KICKOS_SPI_LOCAL is what the source reads. Judged from a silicon capture.
add_executable(k64dspi main.cc)
kickos_link_class_backends(k64dspi spi)
if(KICKOS_SPI_LOCAL_ENGINE)
  kickos_app_system(k64dspi COMPOSITION systems/local.yaml)
else()
  kickos_app_system(k64dspi COMPOSITION systems/proxy.yaml)
endif()
kickos_emit_image(k64dspi)
kickos_app_judge(k64dspi tests/integration/check_k64dspi.sh)

# What is on the wire, which the operator states. Default: the LAN9252 BYTE_TEST probe (EasyCAT
# shield, GPIO CS on D9/PTC4). ON: the SOUT->SIN loopback self-test (shield off, PTD2 to PTD3
# jumper in place). The judge reads this from the build's cache.
option(K64DSPI_LOOPBACK "Build k64dspi as the SOUT->SIN loopback self-test instead of the LAN9252 probe" OFF)
if(K64DSPI_LOOPBACK)
  target_compile_definitions(k64dspi PRIVATE K64DSPI_LOOPBACK=1)
endif()

# The slot witness on the local engine, both endings in every configure: k64dspi_death ends the
# holder with the bus open, k64dspi_close closes it first and probes from the holder's own task.
# Neither needs a fitting.
foreach(_arm death close)
  add_executable(k64dspi_${_arm} witness.cc)
  if(_arm STREQUAL "close")
    target_compile_definitions(k64dspi_${_arm} PRIVATE K64DSPI_CLOSE_ARM=1)
  else()
    target_compile_definitions(k64dspi_${_arm} PRIVATE K64DSPI_CLOSE_ARM=0)
  endif()
  target_link_libraries(k64dspi_${_arm} PRIVATE KickOS::kickos_spi_k64dspi)
  kickos_app_system(k64dspi_${_arm} COMPOSITION systems/witness.yaml)
  kickos_emit_image(k64dspi_${_arm})
  kickos_app_judge(k64dspi_${_arm} tests/integration/check_k64dspi_witness.sh ARGS ${_arm})
endforeach()
