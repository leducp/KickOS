# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# XMC4800/USIC0-CH1 SSC consumer of the SPI class, built BOTH WAYS from one source: the client body
# is identical over the proxy and over the local engine, and only who hands the client which
# capability differs. KICKOS_SPI_LOCAL_ENGINE selects both the backend and the composition, and
# KICKOS_SPI_LOCAL is what the source reads. Judged from a silicon capture.
add_executable(xmcssc main.cc)
kickos_link_class_backends(xmcssc spi)
if(KICKOS_SPI_LOCAL_ENGINE)
  kickos_app_system(xmcssc COMPOSITION systems/local.yaml)
else()
  kickos_app_system(xmcssc COMPOSITION systems/proxy.yaml)
endif()
kickos_emit_image(xmcssc)
kickos_app_judge(xmcssc tests/integration/check_xmcssc.sh)
