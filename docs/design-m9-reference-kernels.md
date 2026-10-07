<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M9 reference-kernel survey: design use

> **Status: EXPLORATORY.** The nineteen source-by-source rows and their dated
> citations are in archived `M9_reference_kernel_survey.md`.
> This summary commits KickOS to no borrowed locking scheme.

The survey compared lock domains and acquisition order, remote wake, migration,
lock handling through a switch, kernel stacks and each project's published evidence.
It did not rank kernels. A mechanism on one port is not a project-wide guarantee;
a single-core implementation gives no SMP precedent; and a source reading does not
supply a KickOS latency bound or a measured lock-versus-message crossover.

The relevant decision rule for KickOS is to measure the workload and hardware that
would use a borrowed idea, then prove its lifetime and ordering requirements in this
kernel. In particular, the rows do not establish a safe reclamation period, a bounded
cross-core wait, or when a deferred remote wake beats a direct one. Licences and
checkout revisions accompany each archived row because both code reuse and factual
citation depend on the exact source.
