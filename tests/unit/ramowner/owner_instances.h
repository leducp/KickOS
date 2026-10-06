// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A three-instance posture, which the sim build's own definitions do not give. Included first by
// both TUs of the gate, so the table and the test see one instance count.

#ifndef KICKOS_TESTS_UNIT_RAMOWNER_OWNER_INSTANCES_H
#define KICKOS_TESTS_UNIT_RAMOWNER_OWNER_INSTANCES_H

#undef KICKOS_MULTI_INSTANCE
#define KICKOS_MULTI_INSTANCE 1
#define KICKOS_MAX_INSTANCES 3

#endif
