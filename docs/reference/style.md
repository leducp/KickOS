<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# House style

The rules a change must follow. **There is no formatter.** `.clang-format` was removed: the tree
never converged on it (144 of 289 C++ files diverged), so it described no state the code held, and
a config that disagrees with the code misleads a reader into "fixing" conforming files. Layout here
is written by hand and reviewed by eye until a checker exists; `../design-style-enforcement.md`
proposes one, and a shared tool would serve `../../../KickCAT` and `../../../kickmsg` too.

Rules marked **gated** are checked by something in `tests/static/`. The rest are not: they hold
because they are written here and read in review.

## Layout

| | |
| --- | --- |
| indent | 4 spaces, never a tab |
| braces | Allman: every opening brace on its own line |
| one-liners | none. Every `if`, `else`, `for`, `while` and `case` body is braced, including a single statement |
| line length | no hard limit; wrap by hand where it reads better |
| namespaces | indented, and spelled `namespace a::b`, not nested blocks |
| namespace close | a bare `}`. No `// namespace x` comment: it says nothing and goes stale |
| `case` labels | indented one level inside the `switch` |
| access specifiers | outdented one level |
| pointers | left-aligned: `char* p` |
| qualifiers | east const (`char const*`), west volatile (`volatile T x`) |
| ctor init lists | leading comma |
| includes | not sorted; grouped by hand |

## Language

- **No ternary `?:`.** Use `if`/`else`, an early return, or a variable set in a branch. This holds
  for plural selection too: set a `char const*` in an `if`.
- **Spelled logical operators**: `and`, `or`, `not`. `!=` stays, and a `#if` directive keeps `&&`.
  The rule holds in a header that must also compile as C, which puts `#include <iso646.h>` in its
  include block, **unconditionally**: the three are C++ keywords but only macros from that header
  in C, and it is `#ifndef __cplusplus` inside, so an `#ifdef __cplusplus` around the include
  would guard nothing. A freestanding C implementation must provide it, so no backend lacks it.
  There is therefore no reason to split a condition into nested ifs to keep a header C-valid.
- **`while (true)`**, never `for (;;)`. **gated**
- **Traditional include guards**, never `#pragma once`. The macro derives from the project prefix
  plus the file path. **gated**
- **Fixed-width C99 types**: `uint8_t`, `int32_t`, `size_t`. Avoid `long`, `short` and bare
  `unsigned` except where a foreign ABI dictates them, and say so at the declaration when it does.
- **A header holds what must be a header**: templates, `constexpr`, and declarations. A
  non-template function body goes in a `.cc` -- `user/src/` for the user substrate -- so the
  tree carries one definition rather than a copy per including TU for the linker to fold.
- **A C-facing header compiles as C11.** **gated** Guarding `extern "C"` with `#ifdef
  __cplusplus` is what declares a header C-facing, and `tests/static/check_c_headers.sh`
  compiles every such header, plus every header one of them includes, as a standalone
  `-std=c11` TU with the board's own C compiler. So `static_cast`, `nullptr`, `alignas`,
  `static_assert`, a `bool` without `<stdbool.h>`, and the spelled `and`/`or`/`not` of the rule
  above are all errors there: write both spellings under the guard, as
  `<kickos/sys/byte_ring.h>` does for `std::atomic<uint32_t>` and `_Atomic uint32_t` and
  `<kickos/sys/uart.h>` does for `static_assert` and `_Static_assert`, or split the condition.
  A header with no C consumer says so by leaving the guard off, as `<kickos/kernel.h>` and
  `arch/include/kickos/arch/arch.h` do: `extern "C"` alone is a C syntax error, so the gate
  never selects it.
- **`volatile` is not a concurrency tool, and the tree no longer uses it as one.** A field
  one thread or an ISR writes and another reads is a `kickos::Atomic<T, Order>` from
  `kickos/sys/atomic.h`, which carries the ordering in the TYPE: declare
  `Atomic<uint32_t, Order::RELAXED> head;`, then read it as plain `head` and write it as
  `head = v`. The ordering parameter has no default, so every declaration names it.
  Relaxed load and store compile to the same single instruction as `volatile` on all five
  backends, so this costs nothing where it applies.
  - **Why a wrapper and not a bare `std::atomic`.** There, a bare `load()`, a bare
    `store()`, `x = v` and an implicit conversion all mean seq_cst, which emits a fence, so
    correctness depends on spelling `std::memory_order_relaxed` at **every** access and one
    omission is silent. The wrapper has no spelling for seq_cst, and no way to override the
    declared order at a call site.
  - **No `fetch_add` or any other read-modify-write.** **gated** An atomic RMW is a libcall on
    armv6m and rxv3, and a freestanding link has no libatomic. The wrapper exposes no RMW
    surface at all, so `x++`, `x += 1`, `fetch_add` and `compare_exchange` do not compile.
    Every such field here has a single writer, so `x = x + 1` under the lock that was
    already there is what replaces a `++`. A site with two real writers needs the lock
    fixed, not an RMW. The gate catches the named spellings (`.fetch_add(`, `.exchange(`,
    `.compare_exchange_*(`, the C11 generics, the `__atomic_` and `__sync_` builtins)
    outright; it catches the operator forms (`++ -- += -= &= |= ^=`) by harvesting which
    identifiers were declared atomic, so read the header of
    `tests/static/check_atomic_rmw.sh` for the shapes that harvest cannot reach.
  - **No `static_assert(is_always_lock_free)`.** It is 0 on armv6m and rxv3 even where the
    load and store are inline plain instructions, because RMW is not. The wrapper bounds
    the width with `sizeof(T) <= 4` instead, there being no standard trait for "a plain
    load and store are single instructions".
  - **The accessors are `always_inline`, and that is load-bearing.** At `-Os` GCC otherwise
    emits an out-of-line copy and turns every access into a call.
  - **`std::atomic` stays where a pure C main linking libkickos must name the struct**:
    `kos_byte_ring` and `kos_uart_stats` spell `KOS_ATOMIC_U32`. The wrapper is a C++ class
    template and cannot serve C; one spelling for both languages needs C++23. A call site
    there never spells a load, a store or an order either: an increment goes through
    `kos_counter_increment` (`byte_ring.h`), valid in both languages, and correct only
    because each of those counters has a single writer.
  - `volatile` stays for the three things it *is* the tool for: MMIO, an object the
    compiler must not elide or hoist, and a **64-bit** cross-thread field, because a
    relaxed 64-bit atomic load is a `__atomic_load_8` libcall on every backend including
    armv7m. Say which of the three at the declaration.
- **Check a return** that can fail. A discarded status is how a correct refusal becomes a silent
  hang; `(void)` it only where the value carries nothing, and say why.

## Corpus

- **ASCII only**, in every tracked file. `--` not an em dash, `->` not an arrow, straight quotes,
  "section" spelled out. **gated**
- **SPDX header** within the first five lines, with the copyright line beside it. **gated**
- No trailing whitespace, no CRLF, a final newline.
- `set -u` in a gate script.

## Comments

A comment earns its place by warning of something a reader would otherwise undo: a hidden
constraint, a subtle invariant, a specific workaround, behaviour that would surprise. It does not
restate the code, explain a naming or wrapper choice, or recount how the code came to be that way.

- **No narration.** No dates, no "measured on", no "this used to", no war stories. Git holds that.
- **No ` -- ` in a code comment.** It is a docs separator; in code, use a comma, a colon or a
  second sentence.
- Prefer one line. Length is not the metric, but a paragraph should be carrying a constraint that
  needs one.

The same rule governs commit messages: a subject plus a bullet changelist saying WHAT landed. Never
stats, test results, or how a defect was found.

## Docs

`../README.md` holds the documentation conventions: which tier a page belongs to, the code-synced
contract for this one, and *prose is regenerable, a measurement is not*.
