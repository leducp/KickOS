# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The K-seam fixture: the kernel's stateful core compiled for the HOST, with karch_seam.cc
# answering the arch boundary. AT THE PRESET'S OWN POSTURE, which is what makes it the
# fixture the ordinary gates share; the postured variants are their own libraries.
#
# Nothing in the tree tests __STDC_HOSTED__ or __EXCEPTIONS today, so the preprocessed
# kernel TUs are identical to the shipping build's; an #if on one of those in a kernel
# header would silently diverge the gate from the image.
kickos_add_kseam(kickos_kseam)
