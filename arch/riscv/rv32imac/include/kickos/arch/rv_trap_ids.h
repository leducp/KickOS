/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Machine-interrupt demux IDs shared between the RV32IMAC trap entry (switch.S) and
 * any chip that must target them. Plain #defines so the assembler and C++ both use
 * ONE source of truth -- the doorbell ID had lived as a bare literal in switch.S and
 * a separate constant in chip_esp32c6.cc, with nothing tying them together (a silent
 * drift would route the C6 inject to a demux arm that just returns -> livelock).
 */

#ifndef KICKOS_ARCH_RV_TRAP_IDS_H
#define KICKOS_ARCH_RV_TRAP_IDS_H

/* External-inject doorbell: the CPU interrupt ID the ESP32-C6 interrupt matrix
 * routes injected lines onto. switch.S sends this ID to .Lext; chip_esp32c6 MUST map
 * the matrix FROM_CPU source to the same ID. Must avoid the CLINT-owned IDs the demux
 * already claims (3=msip, 7=mtip, 1=ssip) and be a valid C6 external ID (1-2,5-6,8-31). */
#define KICKOS_RV_INJECT_DOORBELL_CPU_INT 31

/* Real-device external interrupts: the CPU interrupt ID a hardware peripheral source is
 * routed onto (via the interrupt matrix), distinct from the software-inject doorbell.
 * switch.S sends this ID to .Lextdev; a chip maps its device source(s) to it. First user:
 * the ESP32-C6 UART0 TX-empty line driving the buffered console ring. Same valid-ID
 * constraint as the doorbell (avoid 3=msip, 7=mtip, 1=ssip). */
#define KICKOS_RV_DEV_CPU_INT 30

#endif /* KICKOS_ARCH_RV_TRAP_IDS_H */
