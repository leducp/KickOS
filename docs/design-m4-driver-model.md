<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M4 driver packaging

> **Status: LANDED.** The code-synced contract is in
> [`reference/architecture.md`](reference/architecture.md), "Driver packaging: class versus
> service." Numbered rules below are retained for source and design references.

The **class** is the hardware transaction primitive. A **service** is a thread that owns
a class instance and serialises its methods over an endpoint. The wire protocol maps
1:1 to the class API; it does not define a separate driver API. A consumer can use the
class inline or pay for service arbitration. The class assumes no thread, endpoint or
exclusivity beyond the peripheral grant it received.

## 1. Resources are single-owner

A device window has one holder. Grants cannot overlap each other or the kernel's
reserved blocks. The grantable boundary follows what the protection unit and silicon
can isolate; independent channel windows may be assigned separately when they do not
overlap. A service owns the hardware grant; its clients receive endpoint capabilities.

## 2. Ownership follows the kernel's stake

- **Owns for life:** the kernel's timebase, interrupt controller and protection unit.
- **Neutralize then grant:** a boot hazard such as a watchdog is disabled early, after
  which the kernel keeps no live state in that instance and may grant it.
- **Stateful handover:** the console is quiesced, lent to a driver and reclaimable for
  panic or driver death. This needs an explicit ownership protocol.

## 3. Shared register logic is freestanding

Register code used by both the kernel and userspace is a class-driver leaf: POD state,
free functions, explicit init, no constructors, mutable statics, exceptions or STL.
Userspace wrappers may add RAII and richer C++ APIs. Sharing register *code* at link
time gives neither party a grant to the kernel-owned *instance*.

## 4. Pass the instance explicitly

A class operation takes a base address or POD descriptor, never an internal instance
index. Identical instances differ by base; different register layouts use different
functions. The owner supplies the descriptor for the instance it holds, while board
configuration stays outside the register logic.

## 5. Boot time and ownership are separate choices

A ROM-armed watchdog needs early kernel attention even if it is later grantable.
GPIO and SPI can wait for a userspace owner. "Boot critical" says when the first touch
happens; rule 2 says who retains the resource.

## 6. The shared leaf is one build dependency

The same freestanding leaf must link from the privileged kernel and from an
unprivileged driver. The service wrapper sits above it and supplies IPC, scheduling
and client arbitration. It does not duplicate the register engine.

## 7. The kernel refuses reserved blocks

`grant_region_admissible` rejects overlaps with the resources the kernel owns for
life, for privileged and unprivileged granters alike. Each enforcing chip supplies its
`arch_reserved_blocks` set; missing that definition fails the link. The full admission
rule, including aliases and RAM confinement, is in `reference/invariants.md` under
`grant-refuses-kernel-reserved-blocks`.

A watchdog is normally a class used by the thread whose liveness it proves. A
software-watchdog supervisor is the service exception: clients check in, and it kicks
the hardware only when all required clients have checked in.
