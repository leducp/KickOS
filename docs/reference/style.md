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

**These are the C and C++ rules.** A tracked file in another language follows that language's own
conventions rather than these, so the ternary ban below reads on the C family and the gate that
holds it reads the C family only. What does not vary by language is the **Comments** discipline
further down: it binds every tracked file.

- **No ternary `?:`.** Use `if`/`else`, an early return, or a variable set in a branch. This holds
  for plural selection too: set a `char const*` in an `if`. **gated**
  `tests/static/check_ternary.sh` reads the residue of `tests/lib/strip_comments.awk` and reports
  a `?` in it, which is what catches a ternary split over several lines and what leaves `::`, a
  label and a bitfield unreachable rather than merely unlisted. Its header names what a scan of
  source text cannot reach.
- **Spelled logical operators**: `and`, `or`, `not`. `!=` stays, and a `#if` directive keeps `&&`.
  The rule holds in a header that must also compile as C, which puts `#include <iso646.h>` in its
  include block, **unconditionally**: the three are C++ keywords but only macros from that header
  in C, and it is `#ifndef __cplusplus` inside, so an `#ifdef __cplusplus` around the include
  would guard nothing. A freestanding C implementation must provide it, so no backend lacks it.
  There is therefore no reason to split a condition into nested ifs to keep a header C-valid.
- **`while (true)`**, never `for (;;)`. **gated** by `tests/static/check_ternary.sh`, which
  reads the same comment-stripped residue the ternary rule above is read from.
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
  - **No `fetch_add` or any other read-modify-write.** **gated** The wrapper exposes no RMW
    surface at all, so `x++`, `x += 1`, `fetch_add` and `compare_exchange` do not compile.
    Every field here has a single writer, and a single writer needs neither a lock nor an
    RMW: `x = x + 1` is the same work and a plain word on every backend. **A site with two
    real writers is a lock problem**, and the lock is the mechanism.
    The reason this surface is closed is NOT that an RMW is impossible. A `_4` RMW is a
    libcall on armv6m and rxv3 that a freestanding link cannot resolve, but that only rules
    out taking one from the toolchain -- a lock-bracketed RMW is implementable everywhere.
    It stays closed because the cheapest CORRECT mechanism differs per backend, so an RMW
    belongs behind a per-arch seam like the MPU backends, and no such seam exists yet.
    `../design-m7-smp.md` carries the measured costs and the one correctness rule such a
    seam would have to enforce, namely that `IrqLock`-bracketing is wrong on a dual-core part.
    The gate catches the named spellings outright (`.fetch_add(`, `.exchange(`,
    `.compare_exchange_*(`, the C11 generics, and the `__atomic_` / `__sync_` builtins that
    are READ-MODIFY-WRITE, each listed by name); it catches the operator forms
    (`++ -- += -= &= |= ^=`) by harvesting which identifiers were declared atomic, so read
    the header of `tests/static/check_atomic_rmw.sh` for the shapes that harvest cannot
    reach. **The rule is read-modify-write, never the `__atomic_` PREFIX**:
    `__atomic_load_n` and `__atomic_store_n` are plain accesses and are used in-tree (see
    below). Widening the alternation to a wildcard fails the gate's own self-test, which
    carries a plain-access line in its NEGATIVE corpus for exactly that reason.
  - **No `static_assert(is_always_lock_free)`.** It is 0 on armv6m and rxv3 even where the
    load and store are inline plain instructions, because RMW is not. The wrapper bounds
    the width with `sizeof(T) <= 4` instead, there being no standard trait for "a plain
    load and store are single instructions".
  - **The accessors are `always_inline`, and that is load-bearing.** At `-Os` GCC otherwise
    emits an out-of-line copy and turns every access into a call.
  - **There is ONE atomic mechanism and `<atomic>` appears in exactly one file**, this
    header. A struct a pure C main must name cannot use a C++ class template, so it carries
    `kos_counter_t` (`<kickos/sys/uart.h>`), a ONE-MEMBER STRUCT wrapping a `uint32_t`, and
    every access goes through `kos_counter_increment` / `kos_counter_load`, which are valid
    in both languages. Sound because each such counter has a SINGLE WRITER.
    **The type is what enforces the discipline, not the convention.** A one-member struct has
    no `++`, no `+=`, no bare read and no bare write from an integer: every one of those is a
    COMPILE error in C11 and C++20 alike, so the rule cannot be violated by a call site that
    simply did not know it. Aggregate initialisation still works. This is the same
    make-it-unrepresentable move as nesting `kos_recv_info` inside `kos_recv_timed_opts` so a
    plain recv has no timeout field to reach.
    **The helper BODIES are the only place a mechanism is spelled**, and they use
    `__atomic_load_n` / `__atomic_store_n` at `__ATOMIC_RELAXED`, which are the same single
    instruction as a plain access at 4 bytes aligned on every backend but leave a reader
    racing the writer DEFINED rather than UB. That is the point of the seam: the mechanism
    can be switched in two function bodies without touching a call site.
    Two costs worth knowing before anyone "simplifies" it back. On Xtensa, GCC's default
    `-mserialize-volatile` emits a `MEMW` before every atomic access whatever the order, and
    `-mno-serialize-volatile` is NOT a free fix because it would also drop `MEMW` before
    `volatile` MMIO. And an atomic access is an OPTIMISATION BARRIER, so it can flip an
    inlining decision: a minimal probe TU shows byte-identical codegen while a real driver
    grows.
    `kos_byte_ring` has no C consumer, so it uses the wrapper and its header leaves the
    `extern "C"` guard off.
  - `volatile` stays for the three things it *is* the tool for: MMIO, an object the
    compiler must not elide or hoist, and a **64-bit** cross-thread field, because a
    relaxed 64-bit atomic load is a `__atomic_load_8` libcall on every backend including
    armv7m. Say which of the three at the declaration. **not gated**
    **SAY IT ONCE PER FILE where every such word in a translation unit invokes the same
    exception or exceptions**, at the top rather than on each declaration: the x86_64
    bring-up probes carry sixty-three between them and sixty-three line comments would say
    less than one paragraph does. The point of the clause is that a reader can tell an
    INVOKED exception from an unexamined one, and a file-level statement does that as well
    as a per-line one. It has to name which exception and be true of every word below it, so
    a TU whose words split across exceptions says so, as `probe4_x86_64.cc` does.
    **AN ISR WRITING A FIELD IS THE RULE, NOT AN EXCEPTION TO IT.** That case is the first
    sentence above and its answer is `Atomic`. Where such a field stays `volatile` it is
    because one of the three applies on its own merits, most often because the wrapper
    refuses the width or because the observing loop would hoist. A two-writer word is neither:
    `Atomic` exposes no read-modify-write, so a contended cell is a lock problem or a
    hardware primitive being measured, and it is outside this mechanism rather than exempt
    from it (`user/apps/esp32-wroom/lx6smp`).
    **NOTHING CHECKS EITHER UNIT, and the reason is the corpus rather than the rule.** Most
    tracked files carrying the keyword spell it inside an MMIO accessor body or on a spin
    bound the compiler must not elide, and almost none of them says which exception applies.
    An instrument would have to tell a declaration from a cast and from a parameter, and
    every file it then read would need the sentence written before it could be green: that is
    a source sweep, not a gate. So this clause holds by review, and an unannotated
    `volatile` reaches the tree unremarked whether it is spelled at the declaration or at the
    top of the file.
- **Check a return** that can fail. A discarded status is how a correct refusal becomes a silent
  hang; `(void)` it only where the value carries nothing, and say why.

## Corpus

- **ASCII only**, in every tracked file. A comma or a single `-` for an em dash, `->` not an arrow,
  straight quotes, "section" spelled out. **gated** `tests/static/check_ascii.sh`
- **SPDX header** within the first five lines, with the copyright line beside it. **gated**
- No trailing whitespace, no CRLF, a final newline, no space immediately before a tab in a
  line's indent, no blank line at end of file. **gated** by `tests/static/check_ascii.sh`,
  which walks every tracked file once for the byte rule above and these five line classes.
- `set -u` in a gate script.

## Comments

A comment earns its place by warning of something a reader would otherwise undo: a hidden
constraint, a subtle invariant, a specific workaround, behaviour that would surprise. It does not
restate the code, explain a naming or wrapper choice, or recount how the code came to be that way.

**This section binds every tracked language**, unlike the Layout and Language rules above, which are
the C family's. A Python tool or a shell gate writes its comments under exactly these rules.

- **No narration.** No dates, no "measured on", no "this used to", no war stories. Git holds that.
- **No ` -- ` in software**, in a comment, in a string literal a user reads, or in a commit
  message. It is essay punctuation; use a comma, a colon, a single ` - ` or a second sentence.
  A flag (`--help`), an end-of-options separator (`git ls-files --`), the decrement operator and
  a banner run (`# ---- box ----`) are not punctuation and are untouched. **gated**
- Prefer one line. Length is not the metric, but a paragraph should be carrying a constraint that
  needs one.

The same rule governs commit messages: a subject plus a bullet changelist saying WHAT landed. Never
stats, test results, or how a defect was found.

## Docs

`../README.md` holds the documentation conventions: which tier a page belongs to, the code-synced
contract for this one, and *prose is regenerable, a measurement is not*.
