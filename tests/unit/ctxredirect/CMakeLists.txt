# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The block arm of arch_ctx_redirect_to_block, which every block-carving backend's slay rebuild
# runs and no image witnesses: a stub off the block still completes every death. The sim's
# arch_context carries no kernel block, so the test compiles the header over one that does.
kickos_add_unit_test(NAME ctx_redirect SOURCES ctx_redirect.cc KERNEL
  INCLUDES "${PROJECT_SOURCE_DIR}/arch/common")
