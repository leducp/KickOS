<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Stack safety research

> **Status: EXPLORATORY; M9.0 DECIDED.** The source survey, M8.9 audit and M9.0 experiments
> are in [the research record](archive/M8-M9_stack_safety_research.md). The current stack
> and switch contracts are in [invariants](reference/invariants.md) and code.

Keep per-thread kernel continuations. The M9.0 comparison did not justify replacing them
with shared per-core kernel stacks; the latter changes blocking, unwinding and stack lifetime
across the scheduler. Static stack-budget checks and runtime boundary tests remain the
incremental safety measures. The archived record separates measured stack costs from
emulator-only observations and lists the remaining audit findings.
