# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The CAPABILITY ABI family, for tests/static/check_cap_sigdiff.sh. This file is handed to
# awk as a SECOND program after tests/static/aspace_seam.awk:
#
#   awk -f tests/static/aspace_seam.awk -f tests/static/cap_seam_family.awk <stripped>
#
# awk runs BEGIN blocks in the order the programs are named, so the assignment below replaces
# the aspace family's PREFIX and nothing else. The extraction RULES stay the aspace
# extractor's, byte for byte, for the reason entry_seam_family.awk gives: a copy with one line
# changed lets the verdicts drift apart about what a signature IS.
#
# MEMBERSHIP. The capability layer as a USER PROGRAM sees it: the handle type, the rights
# mirror, the grant record a spawn carries, the authority word, the well-known index plane,
# and the syscalls that close and narrow. M6.5 makes mapping a capability operation over frame
# and page-table objects, and F3's expected result is that the cap layer absorbs them with NO
# NEW ADDRESSING CONCEPT anywhere in it. This family is what that claim is checked against: an
# address, a page number or a frame number arriving in any record below IS the finding.
#
# THE FRAME AND PAGE-TABLE PREFIXES MATCH NOTHING TODAY, on purpose. They are here so the
# members M6.5 adds arrive as a DIFF rather than as silence, and their group floor is 0
# because a family alternative with no member yet is not a broken extraction.
#
# WHAT THIS FAMILY CANNOT SEE, stated here because a reader must not take its PASS for more
# than it is:
#   - The KERNEL-SIDE surface. kernel/include/kickos/cap.h is C++ (a namespace, an
#     `enum class`, and constants that are `static constexpr` rather than `#define`) and the
#     aspace extractor reads a C header. It yields twelve records of ANY name, so a corpus
#     built over it would compare two nearly-empty sets and report clean. CapType, CapRights,
#     CapEntry and the resolve chokepoint are therefore OUT of this verdict. What guards their
#     bit budget is the static_assert set in that header (KCAP_TYPE_BITS, KCAP_RIGHTS_BITS and
#     the reply sequence packed beside them), not this instrument.
#   - A member M6.5 names outside the prefixes below. Adding the prefix is part of landing it,
#     and the group table in check_cap_sigdiff.sh is what refuses a member admitted here and
#     classified nowhere.

BEGIN {
    PREFIX = "^(kos_cap_t|KOS_CAP_NONE" \
             "|kos_cap_rights|KOS_CAP_WAIT|KOS_CAP_SIGNAL|KOS_CAP_TRANSFER" \
             "|kos_cap_grant|KOS_SPAWN_DELEGATED" \
             "|kos_cap_authority|KOS_AUTH_" \
             "|kos_cap_index|KOS_CAP_STDOUT|KOS_CAP_CLOCK|KOS_CAP_FIRST_DYNAMIC" \
             "|KOS_CAP_AUTHORITY" \
             "|KOS_SYS_HANDLE_CLOSE|KOS_SYS_CAP_NARROW" \
             "|kos_frame|KOS_FRAME|kos_ptab|KOS_PTAB" \
             "|KOS_SYS_FRAME|KOS_SYS_PTAB|KOS_SYS_MAP|KOS_SYS_UNMAP)"
}
