<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

**M4.8.3 is MERGED (PR #21).** The task layer: a task is a set of threads that dies as one unit, the
address space stays on `Domain`, and the group gate is CREATORSHIP rather than possession. It also
carries the two things its own captures found -- `rxv3` fault isolation, and a published console no
longer swallowing the fault record. **It is merged, so anything found against it from here is a MISS
and gets filed as one, not folded back into the milestone.**

Behind it, **M4.8.2 (PR #20)**: the host unit-test layer, GoogleTest via Conan behind
`find_package(GTest QUIET CONFIG)` so vcpkg or a distro package satisfies it too, per-case ctest
entries under the `host` label, and the two substitution seams (U-seam at `extern "C" kos_*`, K-seam
at `arch_*` with a whole-`Kernel` reset). It is the tool that proved the `sched::wake()` dying-guard
repair that shipped with it.

Behind it on `master`: M4.8.1 (PR #19) -- one generic driver service replaced twelve
per-(class x chip) bring-ups, and `driver_bringup.h` is gone; M4.7.9 (PR #18), M4.7.8 (PR #17),
M4.7.7 (PR #16), M4.7.6 (PR #15), M4.7.5 (PR #14), M4.7.4 (PR #13), M4.7.3 (PR #12), M4.7.2 (PR #11),
M4.7.1 (PR #10), M4.6.1 (PR #9), M4.5.9 (PR #8), M4.5.8 + M4.5.7 (PR #7), M4.5.6 (PR #6).

**Three defects that predate the driver work were fixed alongside it**, and each is the kind that
only silicon or a sanitizer finds:
- **PendSV read `g_arch_next` and the deferred MPU stash unmasked** on both ARM ports. The pair is
  written together under the kernel lock, so a device IRQ between the two reads resumed a thread with
  another thread's regions programmed. It presented as a USB CDC hard fault on `picopi` about 75% of
  runs, and the fault frames were FOSSILS of an earlier SVC, which is why the dump read like an
  escalated `svc`.
- **Three selftest IRQ arms hardcoded NVIC line numbers with no per-chip free-line set.** Inert on
  QEMU, wired peripherals on RP2040. `KICKOS_IRQ_SOFT_ONLY_BASE` lets a chip declare the fact.
- **`thread_join` read its start time after the spawn**, so a higher-priority target could reach its
  sleep first and the measured interval no longer contained it.

**Two arms filed as unreliable were not flakes.** `rr_interleave` was a symptom of the PendSV race
(4 failures in 6 runs before, 0 in 6 after) and `thread_join` was 80% on one board and 0 across
twelve runs on four others. Both had been written down as marginal and left alone, and that is what
kept a genuine scheduler race hidden. **Measure a rate on both sides of a change before applying a
flake label.**

### What the captures do NOT witness

**The M4.7.6/M4.7.7, M4.7.8 and M4.8.1 silicon debts are PAID**, and those three capture sessions
are archived at `docs/archive/M4.7-M4.8.1_fleet_selftest_meas.md`: six boards, four ISAs, every
enforcement class the fleet has, and `picopi`'s first clean armv6m enforcement run. Go there for a
row. The lessons those passes taught that apply to the NEXT capture are below.

**M4.8.2 is witnessed on SIX boards at `b77a3ef4`**, which is every enforcement class the fleet can
currently reach: `picopi` is the only gap and it is not on any bus. A scheduler change is shipped
kernel code on every board, which is why the whole fleet ran rather than one representative. Logs
`.session/logs/m482-*.log`, and **all seven streams were piped through `tests/integration/check_tap_stream.sh`
by hand** rather than read off the printed counts. `f302nucleo` is the exception that proves the rule
and its permission sets are NOT declared anywhere in the tree: they were taken from
`CONTEXT.local.md`'s provisioning list, so for that board alone the check is only as good as that
list. `microbit` is the only board whose skip set the tree states.

| board | class | plan | result |
| --- | --- | --- | --- |
| `frdmk64f` | SYSMPU | `1..95` | 95 ok, enforce |
| `esp32c6-wroom` | PMP NAPOT | `1..95` | 95 ok, enforce |
| `xmc4800-relax` | PMSAv7 | `1..95` | 95 ok, enforce |
| `rx72m` | RX MPU, and the fleet's only board with no CI gate at all | `1..95` | 95 ok, enforce |
| `esp32-wroom` | LX6, no unit, and an IMMEDIATE-switch port | `1..91` | 91 ok |
| `f302nucleo` | ring-only | `1..51` + `1..40` | 51 + 40 ok, 3 + 7 skip, 0 + 4 partial |

**M4.8.2 AND M4.8.3 are together witnessed on SEVEN boards at `f8cc32bd`**, TAG `m483` and, for the
board that came back a day late, `m483c6`. That is EVERY enforcement class the fleet has:
`esp32c6-wroom` was on no bus for the first pass and was captured at the same tree afterwards, so PMP
NAPOT and `rv32imac` are owed nothing for either milestone. `picopi` closes the PMSAv6 hole the
`b77a3ef4` pass left open. All ELEVEN streams were piped through
`tests/integration/check_tap_stream.sh` by hand, against arm counts derived from
`user/apps/common/selftest/CMakeLists.txt` -- 99 enforcing, 95 no-MPU, split 51 + 44 -- and never
from a capture's own plan line. Every stream returned PASS.

| board | class | service list | plan | result |
| --- | --- | --- | --- | --- |
| `frdmk64f` | SYSMPU | `kickos_services_frdmk64f` (polled `k64uart` + DSPI) | `1..99` | 99 ok, enforce |
| `xmc4800-relax` | PMSAv7 | `kickos_services_xmc4800relax` | `1..99` | 99 ok, enforce |
| `picopi` | PMSAv6, the only armv6m enforcement unit | `kickos_services_none` | `1..99` | 99 ok, enforce, 0 skip |
| `rx72m` | RX MPU, no CI gate at all | `none`, then `kickos_services_rx72m_uartirq` | `1..99` twice | 99 ok both, enforce |
| `esp32-wroom` | LX6, no unit | `none`, then `kickos_services_esp32_uartirq` | `1..95` twice | 95 ok both |
| `esp32c6-wroom` | PMP NAPOT, the fleet's only `rv32imac` unit | `none`, then `kickos_services_esp32c6_uartirq` | `1..99` twice | 99 ok both, enforce, 0 skip, 0 partial |
| `f302nucleo` | ring-only | `kickos_services_none`; NO provider exists for this board | `1..51` + `1..44` | 51 + 44 ok, 3 + 7 skip, 0 + 4 partial |

**`picopi` reported ZERO skips**, so it runs `mem_self_grant` -- the arm M4.8.3's task pool cost
`microbit`, and so does `esp32c6-wroom`. The THREE `_uartirq` runs are what make the driver half of
9.5 non-vacuous on the three boards whose default list is `kickos_services_none`; on the default
preset a green run there says nothing about a driver, and all three were run BOTH ways for that
reason. On the C6 the driver run is legible in the stream itself: `[c6uart] device up (IRQ TX/RX)`
and a tap route reading `stdout endpoint -> console driver`, against `kernel debug console` on the
default list. Its 99 arm NAMES are identical, in order, to `frdmk64f`'s and `picopi`'s, which is what
says no arm was lost rather than merely that a count reconciled.

**THE DEATH PATH ITSELF NEEDED A SECOND APP: the selftest never faults by design** ("the deliberate
cross-domain MPU fault is a separate binary", `user/apps/common/selftest/main.cc`), so a selftest
capture exercises the narrowed dying guard through exit/join/kill traffic only and says nothing about
the window between a fault redirect and its stub. `faultsurvive` (`KICKOS_FS_MODE=0`) is the arm that
does, and it is a fault under teardown pressure because root is parked in `join()`. Clean on FOUR
boards and three arch classes at `f8cc32bd`: `frdmk64f` and `xmc4800-relax` (armv7m, `PC` + `CFSR=0x10000`),
`picopi` (armv6m, `PC` only, no CFSR to print) and `esp32c6-wroom` (`rv32imac`, `PC` + `mcause=0x2`,
the illegal instruction) each printed
`=== THREAD FAULT === thread 'faulter' killed, system continues` and then
`[fs] survivor ran after the fault`, in that order, with no panic dump. That ordering is the witness:
root's line cannot precede the kill. It is also where the 9.5 banner rename shows on silicon -- the
record says `thread`, not `task`. **The C6 run is the FIRST time `kickos_fault_kill_thread` has
executed on `rv32imac` silicon** (`arch/riscv/rv32imac/arch_rv32imac.cc`; `nm` confirms it plus
`arch_fault_is_user_thread`/`arch_fault_redirect_to_exit` in the flashed image), and it was taken
under `kickos_services_none` for the swallow reason SINCE FIXED below rather than on the default list.
A `m484` capture no longer needs that dodge.

**THE SWALLOWED FAULT RECORD IS FIXED, AND THE FIX IS A ROUTE RATHER THAN A RECLAIM (M4.8.3).**
The five record sites call `kprintf_fault`, which while the console is `USER_OWNED`
also hands the line to the published endpoint's ALREADY-PARKED receiver (`cap_console_deliver`), so
the DRIVER prints it and a healthy driver keeps its device. The ruling, the four rejected options and
the two things the fix still loses are `docs/design-m4.7.9-fault-isolation.md` section 9.5; the
transport is now part of `fault-record-is-printed-only-by-its-owner`.

**Witnessed at TAG `m484`, and MUTATION-PROVEN ON SILICON on two boards.** Reverting the five call
sites to plain `kprintf` and reflashing loses the record while both `[fs]` lines and the survival
stay: `frdmk64f` under its DEFAULT polled list (`m484mut-frdmk64f-faultsurvive.log`, banner
`58e7174e-dirty`, which is the mutant labelling itself) and `esp32c6-wroom` under
`kickos_services_esp32c6_uartirq` (`m484mut-esp32c6-wroom-faultsurvive.log`). The second one is the
witness that matters: that driver is IRQ-driven and buffered, and it is exactly the case a SCOPED
reclaim would have wedged, since every `arch_console_reclaim` body clears a TX interrupt enable.

**A CAPTURE'S CRLF TELLS YOU WHICH TRANSPORT CARRIED A LINE, BUT ONLY ON A POLLED DRIVER.** The
kernel chip path cooks `\n` to CRLF where the board asks for it, and the POLLED console drivers
(`k64uart`, `xmcuart`) do not, so on `frdmk64f` and `xmc4800-relax` the driver-routed record is
visibly UNCOOKED in the same stream as the CRLF-cooked kernel banner -- a per-line transport witness
inside one capture, and the reason those two captures need no second run to interpret. **It does NOT
generalise**: `uart_service.h` (the substrate every `_uartirq` driver shares) cooks CRLF itself
(`cook_crlf`), so on `esp32c6-wroom` and `rx72m` driver-routed output is cooked exactly like the
kernel's and the tell says nothing. Reading it as "the console was never published" is wrong, and it
was read that way once in this session before the mutation settled it; the app-side witness there is
the selftest's own `# tap route: stdout endpoint -> console driver` line, and the real witness is the
mutation.

**THE ESCALATION HALF IS NOW WITNESSED ON FOUR BOARDS, having been `rxv3`-only.** `faultsurvive_ovf`
and `faultsurvive_off` must reach the PANIC dump rather than the thread kill, and until `m484` only
`rx72m` had ever run them. All fourteen `m484` captures are `commit 58e7174e`, clean, with the two
deliberate mutants stamped `-dirty`. The rx72m pair also RETIRES the `-dirty` caveat on the only
prior escalation witnesses: both reproduced byte-for-byte at a clean tree (`MPDEA=0x121fc`,
`PC=0xffc0050d PSW=0x130001`).

| board | class | app | list | verdict |
| --- | --- | --- | --- | --- |
| `frdmk64f` | SYSMPU | `faultsurvive` | `kickos_services_frdmk64f` | record ON THE WIRE, uncooked, before the survivor line |
| `frdmk64f` | SYSMPU | `faultsurvive_ovf` | default | escalates, `=== HARD FAULT ===`, `CFSR=0x1400` + `SYSMPU ISOLATION FAULT port=3 addr=0x20018b30 W` |
| `frdmk64f` | SYSMPU | `faultsurvive_off` | default | escalates, `=== HARD FAULT ===`, `CFSR=0x10000`, no stacking-abort bit |
| `frdmk64f` | SYSMPU | `selftest` | default | `1..99`, 99 ok, enforce |
| `xmc4800-relax` | PMSAv7 | `faultsurvive` | `kickos_services_xmc4800relax` | record ON THE WIRE, uncooked, before the survivor line |
| `xmc4800-relax` | PMSAv7 | `faultsurvive_ovf` | default | escalates, `=== MPU FAULT ===`, `CFSR=0x92` (MSTKERR set), `MMFAR=0x20013f78` |
| `xmc4800-relax` | PMSAv7 | `faultsurvive_off` | default | escalates, `=== HARD FAULT ===`, `CFSR=0x10000`, no stacking-abort bit |
| `xmc4800-relax` | PMSAv7 | `selftest` | default | `1..99`, 99 ok, enforce |
| `esp32c6-wroom` | PMP NAPOT | `faultsurvive` | `kickos_services_esp32c6_uartirq` | record ON THE WIRE over an IRQ-driven driver, proven by mutation |
| `esp32c6-wroom` | PMP NAPOT | `faultsurvive_ovf` | default | escalates, `MPU FAULT: thread 'faulter' attempted write at 0x40833f00` |
| `esp32c6-wroom` | PMP NAPOT | `faultsurvive_off` | default | escalates, `=== RISC-V TRAP (illegal instruction) ===`, `mstatus=0x80` (MPP=U) |
| `esp32c6-wroom` | PMP NAPOT | `selftest` | default, then `_uartirq` | `1..99` twice, 99 ok, enforce |
| `rx72m` | RX MPU | `faultsurvive` | `kickos_services_rx72m_uartirq` | record ON THE WIRE over an IRQ-driven driver |
| `rx72m` | RX MPU | `faultsurvive_ovf` | default | escalates, `MPU FAULT: thread 'faulter' attempted write at 0x121fc` |
| `rx72m` | RX MPU | `faultsurvive_off` | default | escalates, `=== RX EXCEPTION (privileged instruction) ===`, `PSW=0x130001` (PM=1, user) |
| `rx72m` | RX MPU | `selftest` | default | `1..99`, 99 ok, enforce |

**`frdmk64f`'s overflow arm latches a DIFFERENT bit from every other armv7m board, and
`check_faultsurvive.sh` would misjudge it.** That gate's `armv7m:overflow` corroboration requires
`CFSR & 0x10` (MemManage MSTKERR), which is what `xmc4800-relax` produces (`CFSR=0x92`). The K64F's
SYSMPU is a BUS-level slave-port unit, not the ARM MemManage unit, so its stacking abort latches
BusFault `STKERR` (bit 12) plus `IMPRECISERR` -- `CFSR=0x1400`, with `MSTKERR` CLEAR -- and the real
evidence is the `SYSMPU ISOLATION FAULT` line naming the denied write. The claim being witnessed
holds on that board; the GATE's per-arch table does not cover it, and no ctest runner ever points it
at a SYSMPU board, so this is latent rather than red. Filed in `TODO.md`.

**`picopi` IS WITNESSED, at the squashed commit itself and not at a pre-squash tip** (TAG `m483pi`,
four captures, `commit bb8fae24` on every banner). It is the fleet's only armv6m enforcement unit and
it now carries all three fault modes, where every other board's escalation capture truncates before
its dump:

| arm | frame | outcome |
| --- | --- | --- |
| `selftest` | -- | `1..99`, 99 ok, 0 skip, 0 partial, enforce |
| `faultsurvive` | `PC=0x1000029c`, no CFSR to print on armv6m | `=== THREAD FAULT === thread 'faulter' killed`, THEN `[fs] survivor ran after the fault` |
| `faultsurvive_ovf` | `PC=0xffffffff xPSR=0x0` | escalates, `=== HARD FAULT ===` |
| `faultsurvive_off` | `PC=0x100002a0 R3=0x20008200` | escalates, `=== HARD FAULT ===` |

**The two escalation frames differ for a reason worth keeping.** `_ovf` recurses off the stack, so the
hardware stacking writes into the overflowed region and the frame it reports is GARBAGE -- a
`PC` of `0xffffffff` and a zero `xPSR` are the signature, not a capture defect. `_off` moves SP
outside the stack instead, faults on the first access, and its frame is intact. An arm asserting a
plausible PC on `_ovf` would be asserting something armv6m cannot deliver.

**What armv6m still cannot witness is the RECORD ROUTE, and that is a USB defect rather than a fault
one.** `picopi`'s only publishing service is `kickos_services_picopi_usbcdc`, and publishing blinds
UART0 by design (the kernel console is a different peripheral from the one the driver takes). Captured
that way the UART carries the banner and nothing after it, and no `ttyACM` appeared within a
0.2 s-resolution poll armed before the flash. That is CONSISTENT with the already-filed blocker --
under the production service list the device reaches `[rpusb] host configured the device` and then not
one byte reaches the ACM tty -- and it does NOT establish the stronger claim that enumeration never
happened: `faultsurvive` lives a few hundred ms, and `dmesg_restrict` is 1 on this box so the kernel
log was not available to discriminate. Log `.session/logs/m483picdc-picopi-faultsurvive.log`.

**`f302nucleo`'S FAULT REPORTER WAS NEVER BROKEN -- THE FLASH COMMAND WAS.** `st-flash
--connect-under-reset --reset write` leaves the core under halting debug with `DEMCR.VC_HARDERR`
armed, so the `udf` escalates to HardFault normally and the core then enters Debug state AT
`HardFault_Handler`'s first instruction instead of executing it. CPU stopped, so no LED, no dump, and
a board that looks locked up forever. Fixed in `tools/flash-stlink.sh`: `--reset` is dropped wherever
`--connect-under-reset` is used, because releasing NRST already starts the image.

**The old reading -- "exception entry itself fails, a bad vector fetch or LOCKUP during hardware
stacking" -- is FALSIFIED, and by two independent instruments.** On the live board: `DFSR=0x9`
(`VCATCH`), `DEMCR=0x01000501`, `HFSR=0x40000000` (`FORCED` with `VECTTBL` CLEAR, so not a vector
fetch), `DHCSR` `S_LOCKUP` CLEAR at a real instruction address rather than `0xFFFFFFFE`, `CFSR=0x10000`
(`UNDEFINSTR` alone, no `STKERR`, so not a stacking fault), and a stacked frame at `PSP` carrying the
`udf`'s own PC. The single-change proof is a gdb write of `DEMCR=0x01000000` on the SAME boot with no
reset and no reflash, after which the stalled boot finished its queued line and printed its fault
report. Control: the same image under `--connect-under-reset write` with NO `--reset` reports unaided.

**It was already visible in the archive and nobody read it.**
`.session/logs/m483-fs-f302nucleo-faultsurvive.log` (2026-08-12, `f8cc32bd`) holds TWO boots, and the
FIRST one prints `[fs] spawning the faulter`, `[fs] worker about to fault`, then `=== THREAD FAUL` --
the reporter running, truncated by the reset that produced the second boot. A `tail` of that file
shows only the second boot, which is how it stayed unread. **`bench-capture.sh` uses the safe order,
which is why the bench never saw the defect and only the recovery path did.**

The board still has NO MPU, so the MPU-fault arms remain genuinely out of reach there -- that part was
always a hardware fact. Fault isolation via `udf` needs none of it.

**`rxv3` NOW HAS FAULT ISOLATION AND IT IS WITNESSED ON SILICON. `lx6` still cannot have it.** The
`rte` the old decline named as never-executed has now executed, on `rx72m`, at TAG `m483rxg` -- and
the decline's reason was never wrong, it was about the TREE: there is still no RXv3 emulator and no
CI gate, so every rxv3 fault claim is unfalsifiable off that board. `lx6` is refused by
`KICKOS_HAVE_PRIV_RING` because `PS.UM` is 1 for kernel and thread alike, which is a HARDWARE fact
and not an unported feature -- it has no unprivileged thread to kill, so there is nothing for the
rule to discriminate and no backend code would change that. Do not record it as a gap of the same
kind as the Cortex-M0.

**FIVE arms on `rx72m` silicon, one tree, and the addresses are what separate them.** Every capture
is `commit f8cc32bd-dirty`, which is honest and load-bearing: `bench.sh` builds the working tree it
flashes, and the tree it flashed is the one this commit contains.

| app | fault | outcome |
| --- | --- | --- |
| `faultsurvive` (mode 0) | `mvtipl #0` in user mode, cause `0x50` | `=== THREAD FAULT === thread 'faulter' killed` at `PC=0xffc00505`, THEN `[fs] survivor ran after the fault` |
| `faultsurvive_off` (mode 2) | USP moved to `0x8200`, outside the stack | escalates, `=== RX EXCEPTION (privileged instruction) === PSW=0x130001` |
| `faultsurvive_ovf` (mode 1) | recursion off the stack, `MPDEA=0x121fc` | escalates, `MPU FAULT: thread 'faulter' attempted write` |
| `mpu_fault` | cross-domain write, `0x13200`, BELOW `domainA`'s stack | escalates, `MPU FAULT: thread 'domainA'` |
| `rxdrv` | ungranted `PORT8.PMR`, `0x8c068`, ABOVE the stack | `=== THREAD FAULT === thread 'rxdrv' killed`, `MPESTS=0x6 ADDR=0x8c068` |

**The ORDERING is the mode-0 witness, not the presence of the two lines**: root is parked in `join()`
so its line cannot precede the kill. `PC=0xffc00505` is exactly the `mvtipl #0` in `faulter`, which
also confirms the frame offset independently -- an instruction-cancelling exception saves the PC OF
the faulting instruction, so a wrong offset could not land on it.

**The mode-1 arm FAILED FIRST and that is the most useful thing in this pass.** At TAG `m483rxovf`
the overflow was KILLED, the stub ran privileged on the exhausted stack -- supervisor bypasses the RX
MPU, so nothing trapped the damage -- and it smashed its way to `=== RX EXCEPTION (trap) === PC=0x0
PSW=0x0`. Localized with `KICKOS_RX_MPU_TRACE` at TAG `m483rxovft`. Cause: RXv3 CANCELS the faulting
instruction and restores SP, so the USP reads as though the denied push never happened and no
SP-based test can see an overflow. Fixed by `kickos_fault_below_stack`, and the last two table rows
are the measured cost: `mpu_fault` used to die thread-scoped at the broken tree and now escalates,
because its target is below the faulting thread's stack. Both captures are kept.

**`f302nucleo`'s fault arm is INCONCLUSIVE and stays that board's own open defect.** Its capture
emitted `=== THREAD FAUL`, truncated, and the board RESTARTED inside the window without ever
reaching the survivor line. The first bytes do say the reporter was entered here, which is not what
the filed `fault`-app observation says -- but this is the board whose post-fault console output is
already known to be an unreliable instrument, so it is not evidence against that filing either.

**A `KICKOS_DIAG_TERSE` board's banner reads as damaged to the capture script.** `bench-capture.sh`
recovers a lone 8-hex token and labels it "banner damaged in transit", which is what `f302nucleo`'s
`c f8cc32bd` gets: the terse banner is `c`/`b`/`a`/`m`, not `commit`/`board`. The label is wrong on
that board and the capture is fine.

**THE `banner:` LINE `bench-capture.sh` PRINTS STRIPS A `-dirty` SUFFIX**, so its summary cannot tell
a clean tree from a dirty one and only the log can (`grep -a 'commit ' <log>`). The three `m483c6`
images say `f8cc32bd-dirty` because a concurrent branch held uncommitted RX and doc edits, one of them
in `faultsurvive/main.cc` itself. The C6 witness holds at `f8cc32bd` anyway, and the two checks that
say so are cheap: the generated `.config` is identical to a pristine worktree's, and the PREPROCESSED
`rv32imac` TU of `faultsurvive/main.cc` is byte-identical (`-E`, `#` lines dropped) because every RX
delta sits behind `#elif defined(__RX__)`. Comparing OBJECTS instead proves nothing here -- `-g`
embeds the build path, so all 71 differ -- and a normalised `objdump -d` is no better on this pair,
because a `-dirty` commit string is longer and shifts every `.rodata` address after it by 4.

**The witness survived the review fixes that followed it.** The primary evidence is the source diff:
those fixes are comment-only, which `git diff` shows outright. The artifact cross-check is worth
knowing for its traps. Every kernel object AND the `text` size changed, which looks alarming and is
not: `-g` embeds the build path in DWARF, and a worktree path is longer than the main checkout's;
the `text` delta is `tests/tap/tap.h`'s `__FILE__`, which is APP-side. Comparing normalised
`objdump -d` instead, with an untouched file as the control, every instruction stream is identical.
**Two limits on that method.** It disassembles no data, so a changed `.rodata` initialiser passes
silently. And it was run on `frdmk64f`, where `KICKOS_ASSERT` expands to a stringified condition; on
a `KICKOS_DIAG_TERSE` board (`f302nucleo` is the only one) the same macro emits `__LINE__` as an
immediate, so inserting a comment line above an assert legitimately moves a constant there and must
not be read as a code change. An object-size delta alone never means the code moved.

**`f302nucleo`'s second image needed the log SPLIT before the checker would accept it.** Both its
captures contain TWO plan lines, a board restart inside the capture window that
`.session/bench-capture.sh` flags itself, and p2's whole-file stream reconciles as "plan claims 40
but 44 were reported". Feeding the last run alone passes. p2's truncated fragment had reached 4 arms
with zero failures and p1's had reached none, which is what says restart rather than fault: check the
fragment before believing a reconciliation failure on this board. **It recurred at `f8cc32bd`**, where
BOTH fragments reached zero arms, so the restart belongs to the board rather than to one image or one
tree: expect two plan lines here and slice from the last one.

**A DRIVER IS ONLY IN THE IMAGE IF THE SERVICE LIST PUTS IT THERE.** `rx72m-st`, `esp32-wroom-st` and
`esp32c6-wroom-st` all default to `kickos_services_none`, so a green run on a default preset says
NOTHING about that board's driver. This was got wrong twice in one session. `bench.sh` takes
`SERVICE_LIST=` for exactly this, and the IRQ UART services live only in the `*_uartirq` providers and
are never a default. The polled default lists also claim no IRQ line at all, which is why a timing arm
can pass there and fail under `_uartirq` on the same board.

**`f302nucleo` is the one board whose capture is not self-validating**, and its skips are
provisioning (a 3-thread pool, a 7-slot cap table), not defects. `bench.sh` prints counts but does
not run `tests/integration/check_tap_stream.sh`, so both images were piped through it by hand against the arm
counts derived from `user/apps/common/selftest/CMakeLists.txt` rather than from their own plan
lines. Do the same for any future silicon capture on a board with no ctest gate: a plan that
reconciles with itself proves nothing about an arm that was deleted.

**`.session/bench-fleet.sh` is the instrument for a fleet pass now**, and it exists because a
caller must never pair a board with a probe serial by hand. `for b in "board sn"; do bench.sh $b`
is correct in bash and silently wrong in zsh, which does not word-split: the pair arrives as one
board name and `bench.sh` dies at its configure line before printing anything, so the board reads
as skipped rather than failed. The fleet script resolves serials itself from sysfs by `idProduct`,
reports an absent board as absent, and handles the two-image boards.

**A batch `ctest` across all suites is not a valid instrument for `sim` and `qemu`.** They have no
silicon clock, fail under the load of a back-to-back run, and pass standalone. CI runs one board
per job for that reason, and so must any local sweep. **That constraint is now MECHANICAL rather
than remembered**: every gate that executes no KickOS image carries `LABELS host`, so `ctest -L host`
is the batchable set and `ctest -LE host` is exactly the set that must run standalone. The label is
defined once in the root `CMakeLists.txt`. It is not a synonym for "runs on the build host":
`oot_export` runs the app it built and deliberately does not carry it.

**`EXPECT_SKIPS` and `EXPECT_PARTIALS` are PERMISSION SETS, not budgets.**
`tests/integration/check_tap_stream.sh` fails an UNLISTED skip but only NOTEs a listed arm that did not skip.
A LOSS of arena slack is therefore caught automatically and a GAIN is not: any change that moves
`microbit`'s `.bss` needs its skip set diffed by eye.

**A silicon selftest run is `--preset <board>-st`.** `KICKOS_ENABLE_SELFTEST` is ON only on the
`-st` variants and on the boards whose base variant is itself a run gate. Getting the variant
wrong costs arms and still reads as a clean pass. Flash, WAIT for the flash script's own `r;g` to
finish, arm exactly ONE reader by its `by-id` symlink, then reset separately;
`.session/bench.sh <board> [sn]` with `TAG=<milestone>` encodes the whole order and refuses
rather than producing a plausible-looking wrong log.

## What is next (locked order)

**M4.8.2 (PR #20) and M4.8.3 (PR #21) are both MERGED, and both silicon obligations are paid** -- the
fleet tables above, plus the `m483pi` armv6m set. NOTHING is landed-but-unmerged. The two milestone
records are `docs/design-m4.8.2-host-unit-tests.md` sections 8 and 9, and `docs/design-task-layer.md`.
What follows is what remains, in order.

**M4.8.4 IS THE TAIL OF THE THREE MERGED MILESTONES, AND IT RUNS IN PARALLEL WITH THE DRIVER ERA AND
A DOC AUDIT -- THREE TRACKS, SEPARATE WORKTREES.** The locked order below is a DEPENDENCY order, not a
schedule: nothing in M4.9.1 waits on the tail, so serialising them buys nothing. `TODO.md`'s
*M4.8.x triage* section sorts the 30 open items; M4.8.4 takes class A (six latent defects, headed by
the `sched::wake` guard whose premise a deferred switch defeats for every later wake in a chunk -- two
of the three clauses M4.8.2 shipped as its repair), class B (three accepted costs, headed by rxv3
escalating on any below-stack address where four other backends kill the thread alone), and class C
(the four instruments that let them through, one of which MISJUDGES rather than merely missing).
**The order INSIDE M4.8.4 is class C first**: a gate that misjudges cannot witness a fix to anything
else. This number was assigned in `roadmap.md` BEFORE the work, which is the whole difference from
parking bugs behind a fresh number -- and the work is the tail of merged milestones, so each item is a
MISS, already filed as one.

**What parallelism costs, and it is not nothing.** The bench is SERIAL -- one board, one reader, one
capture -- so a witness belongs to whichever tree was actually flashed, and `-dirty` in a banner is
the capture telling the truth. The `sched::wake` fix in class A is scheduler code every driver capture
exercises, so a driver witness taken while it is in flight dates to the tree that carried it. And all
three tracks write `STATE.md` and `TODO.md`: the tail owns the triage section and the blockers list,
the driver track appends its own, the doc audit touches everything and therefore goes LAST into any
file the other two are still editing.

1. **M4.8.3 -- MERGED (PR #21), kept here for the tail M4.8.4 closes**,
   `docs/design-task-layer.md`. A task is a set of threads;
   the address space attaches to Domain, not Task. `sizeof(Thread)` is unchanged across the WHOLE
   milestone -- 256 microbit, 264 picopi, 2480 sim, re-measured at the 9.5 tree -- and so is
   `microbit`'s `.bss`: `Task` was REPACKED for 9.4 rather than grown, so the skip set 9.3 cost that
   board did not grow again.
   - **9.3**: a `Task` owns the `Domain*`, one task per thread implicitly, `Thread::domain` became
     `Thread::task`.
   - **9.4**: `kos_task_create` makes an EMPTY group holding a domain built from its own grant, and
     `kos_thread_params::task` seats a member. The gate is CREATORSHIP, not possession: a task handle
     is a plain word (generation over a BIASED index, so the all-zero word means "no task" and every
     one of the 230 existing spawn sites keeps its meaning unedited), and the cap codec is untouched.
     **A DEV window is now the asking THREAD's region and never its task's domain**, so `domain_for`
     takes no MMIO argument, `dev_window_free` scans live THREADS and skips a `dying` one, and the
     window's Rule 7 admission moved to the spawn boundary where the grant is asked for.
   - **9.5**: a member's death ends its group, and `kos_task_kill` does it from a supervisor. The
     cancel is TOTAL over `WaitKind` and the DEATH POINT is the next syscall ENTRY -- which is what
     reaches a thread parked in `sem_wait` or `sleep`, neither of which has an error return to
     cooperate through. **0 still means the request was ACCEPTED, never that the thread is gone**: a
     thread that never re-enters the kernel is unreachable without preemption.
   **The drivers opt in through ONE call site**, `drv::bring_up`, so all twelve are converted by
   editing the generic service; `ThreadSet` is gone and the hand-rolled group kill with it. Two costs
   worth knowing: `rx72m/rxsci`'s relay thread now sees the ring block it declared no grant for (the
   block is the GROUP's region and the per-thread flag is read as the group's), and the sim's WINDOWED
   console posture deliberately keeps its window thread OUT of the driver's group, because a foreign
   holder of the registers is the only shape in which the deferred console reclaim is observable.
   **The banners now say `thread '%s'` where they said `task '%s'`**, because with a Task in the tree
   naming a thread and saying "task" is a second truth. Archived silicon captures keep the old
   string: editing those would falsify what a past image printed.
   **SILICON AT `f8cc32bd`**, TAG `m483` plus `m483c6`, in the same pass as M4.8.2's and recorded in
   the fleet table above: seven boards, the four task arms (`task_handles`, `task_member_refusals`,
   `task_creator_gate`, `task_group_kill`) ok on every one of them, and `rx72m`, `esp32-wroom` plus
   `esp32c6-wroom` run
   a second time under their `_uartirq` lists so the converted `drv::bring_up` is in the image rather
   than absent by default. The death point `tests/unit/taskdeath` cannot see is now witnessed on
   silicon on FOUR boards and three arch classes -- but by `faultsurvive`, not by the selftest, and
   only where `KICKOS_FAULT_ISOLATION` is 1. **`rx72m` (rxv3) joined that set in this milestone and is
   witnessed on silicon** (see the rxv3 section above); on `esp32-wroom` (lx6) the app is still not a
   target, a fault stays system-terminal, and that capture witnesses the group-death half alone. **`rv32imac` was the third arch 9.5 changed and it is now witnessed BOTH ways**, group kill
   and fault kill, on `esp32c6-wroom`.
2. **M4.9.1 -- USB CDC console**, continuing M4.6.2. The console now **enumerates and carries payload
   on an RP2040** (`picopi`, 5.4-5.8 KiB per run), where every earlier witness was RP2350. What it
   does not do is deliver its tail: `main` returns and the teardown drops about 2.7 KiB still queued
   in the PUBLISHED console's ring, because that path drains only the kernel transport.
3. **M4.9.2..N -- the fleet-wide witness pass**, and the per-chip `arch_console_reclaim` bodies.
   **Nothing in-tree can catch a wrong `arch_mpu_region_pow2()` literal in a backend**
   (`cmake/boot_arena.cmake` scrapes the same file the link resolves), so `rx72m` silicon is the only
   check on that class for the RX MPU.

Captures and records already stamped `M4.6.2` keep that name: a measurement is never renamed.

## Build posture

The fleet including the sim builds `MinSizeRel` (`-Os`, `-g` re-added). It shipped `-O0` until
M4.5.2, roughly 2x the footprint, so **every silicon witness taken before it needs re-running**
-- on the K64F `-Os` dropped a PIT clock-gate-race write that `-O0` had masked. `-Os` is a preset
default, invisible through `find_package(KickOS)`, and the floors in `docs/reference/porting.md`
assume it. The one commit to revert when bisecting a footprint or timing regression is
`build: optimise the fleet (MinSizeRel)`.

The build identity is **`0.4.7`**, in both `project()` and `KICKOS_VERSION` (root `CMakeLists.txt`).
The scheme is `0.<milestone>.<submilestone>` and **the bump belongs to the milestone**: it sat at
`0.4.5-2` across all of M4.6, so every M4.6 banner shipped a submilestone behind.

## Gates

**Do not carry a tally forward from this file.** One `panicgate` case or `ringpriv` registration
moves several at once, so the number here rots every milestone and has misled repeatedly. Re-derive:

    source .session/env.sh          # MANDATORY: all four cross families need it
    cmake --preset <tree> -B <dir>
    cmake --build <dir> -j8 && ctest --test-dir <dir>

**The posture is part of the preset**, so there is no flag to pass and no cached value to
reset: a board that can enforce does so on its base variant and carries a `<board>-flat`
preset for the ring-only one. `sim` has no flat variant at all -- it enforces through host
mprotect, so enforcement is the only posture it has.

**Swept 2026-08-04 at tree `e74933d`: zero failures, every tree, both postures, and all 26 presets
configure and build.** That sweep is the measurement; the shape worth remembering is that the
enforcing and ring-only postures differ by a handful of arms and that `microbit` is the only board
in the fleet with skips.

Two facts a re-derive will not tell you:

- **`oot_export_mcu` fails deterministically when `ctest` runs without `.session/env.sh` sourced** --
  it falls through to the picolibc C-only `/usr/bin` twin. 100% reproducible without the env, 100%
  absent with it. It is an environment artifact, not a code failure, and it re-configures in a
  subprocess so the toolchain vars must be exported at ctest time too.
- **`qemu-riscv` under enforcement is the only posture reporting zero partials** where every ARM
  enforcing posture reports one. Both are "all expected"; it is an encoded per-arch difference, not a
  defect, and it reads as a bug later if nobody says so.
- **`-L host` and `-LE host` do NOT sum to the sim total, and the partition is fine.** ctest pulls a
  required fixture into every filtered selection, so `kickos_build` appears in BOTH lists: 152 and 23
  against a total of 174, where the real split is 151 host, 22 image, 1 fixture. The `host` label is
  load-bearing now -- `kickos_decline_image_test` disables anything unlabelled when
  `KICKOS_BUILD_INTEGRATION_TESTS=OFF` -- so a new host gate that forgets it goes silently missing.

**The sim has no virtual time and its gates are not deterministic**: `arch_clock_now` reads
`CLOCK_MONOTONIC` and the tickless one-shot is a real `timer_create` delivering SIGALRM, so
preemption lands at an arbitrary host instruction and a sim gate can fail load-dependently.

`doc_names` catches a cross-reference that no longer resolves and has already caught two regressions,
one of them 33 references reverted by a rebase. Run it after any doc-heavy rebase; a clean
`git rebase` is not evidence. **It reads TRACKED MARKDOWN ONLY**, so the same citations in source
comments, CMake strings and workflow YAML rot silently -- and it validates a PATH and an IDENTIFIER,
never a LINE NUMBER, which is why this tree cites path + symbol and `design-capability-table.md` now
carries no `path:line` at all.

**The selftest arm count is an EXACT FLOOR per posture** in `user/apps/common/selftest/CMakeLists.txt`
(`_tap_arms`, plus the independent partition `_tap_arms_p1` + `_tap_arms_p2` for the two-image split,
with a totality FATAL if they disagree), so adding a case without moving both fails loudly rather
than passing quietly. **Adding a `static` to a shared test is not free**: it comes out of `microbit`'s
16 KiB arena, which is why a new case reports over its own endpoint rather than through file-scope
state. `-KOS_ENOMEM` cannot distinguish a full cap table from an empty pool, which is why
`-KOS_EMFILE` exists.

**`ringpriv` and `ringppb` are permanent CI, not bench captures**: `cmake --preset qemu` IS the
ring-only posture. `microbit` asserts the OPPOSITE outcome with one arm (`CONTROL.nPRIV == 0`) rather
than skipping, machine-checking the armv6m classification, and does not build `ringppb` -- on a
no-ring core the PPB read legitimately succeeds.

## Board matrix

**Every board runs an unprivileged root by construction**, so a row records which ENFORCEMENT
backend it can witness with: **enforcing** (a ring AND an MPU/PMP backend -- the only class that
can witness memory confinement), **ring-only** (a real ring, no MPU backend), **no-ring** (no
privilege axis, but the authority word is software and still bites, which is why `microbit` runs
`rootauth`).

- **enforcing** (14): `xmc4800-relax` (PMSAv7, the flagship and the only board carrying the
  class-2 write seam), `frdmk64f` (SYSMPU), `pizero2350` (PMSAv8), `f411disco` + `blackpill`
  (shared PMSAv7), `teensy41` (PMSAv7 + ERR011573), `picopi` (PMSAv6, the fleet's only armv6m
  enforcement proof), `rx72m` (RX MPU, no CI gate), `esp32c6-wroom` (PMP NAPOT), and the five run
  gates `qemu` / `qemu-m3` / `qemu-m7` / `qemu-m33` / `qemu-riscv`.
- **ring-only** (3): `f302nucleo` -- the only physically present no-MPU ARM board, and the ring
  arm's silicon witness; `bluepill-c8` (no unit, build-only); `due` (unit retired).
- **no-ring** (3): `microbit`, `esp32-wroom`, `sim`.

Per-board chips, cores and the fact that decides each class: `docs/reference/boards.md`.

## Open blockers

- **PAID 2026-08-12 at `f8cc32bd`: narrowing the dying guard puts MORE traffic through the preemptible
  window between a fault redirect and its stub.** The coupling runs the wrong way, so
  `fault-record-is-printed-only-by-its-owner` carried the weight and no host gate could discharge it.
  It wanted an enforcing board with a fault arm under teardown pressure, and it got four:
  `faultsurvive` mode 0 on `frdmk64f`, `xmc4800-relax`, `picopi` and `esp32c6-wroom`, root parked in
  `join()`, kill
  banner then survivor in that order, no panic. The fleet table above has the run. **What the pass
  ALSO found is that the record is invisible whenever a userspace console driver holds the UART**, so
  the invariant holds for the owner and the observer sees nothing -- proven both ways on one board.
  STILL OPEN and NOT discharged by any of this: the escalation half (`faultsurvive_ovf` /
  `faultsurvive_off`) has no silicon witness on ANY board, `rv32imac` included -- its mode-0 half is
  now witnessed there and its escalation half is not. Related and NOT
  independent:
  `endpoint_wait_timeout` already deflates a dying thread's priority from the timer, in the chunk
  gap, with no `dying` test, which lowers the very quantity the new guard compares against.

- **FIXED 2026-08-02: `ktime_rearm` re-derived the deadline it programmed.** It applied the
  min-delta floor itself against a fresh clock read, on every context switch, so a deadline inside
  the window was a DIFFERENT value each call -- exactly what every backend dedups on. The dedup
  never hit, the one-shot restarted before its compare, and such a deadline starved. The floor now
  lives where the deadline is BORN (`ktime_sleep_until`; `arm_slice` already had it) and
  `ktime_rearm` passes the absolute deadline through. Proven on all six boards (`m461n-*`) and by
  the `ktime_rearm` ctest, REGISTERED because it passes; mutation-proved by restoring the floor,
  which prints the drift directly (1017500, 1020000, 1022500 ... where 1015000 is wanted).
  **The two non-obvious boards are both green.** `esp32-wroom` needed a separate arch fix first --
  Xtensa `CCOMPARE0` is an equality match, so a compare landing behind the counter is MISSED rather
  than late and the next match is a 2^32-cycle wrap away, which presents as a hang at `sleep_order`
  (`b4e888d`). `rx72m` runs `rr_interleave` green with `rr order: ABABAB` under MPU enforcement.

- **FIXED 2026-08-02: the console reclaim was keyed on the endpoint's last RECEIVER.** `recv_holders`
  counts WAIT-bearing caps, so on a two-thread driver it hit zero when the SERVICE thread died --
  and the registers belong to the IRQ thread, which parks on a line cap and is not counted. The
  reclaim reprogrammed the UART under a live owner and silenced its source. It is now asked of the
  DEVICE: `console_on_driver_death` defers while any live domain holds `arch_console_reclaim_window()`.
  **"The driver is gone" is not expressible from kernel state, by design** -- `domain_for` skips the
  dedup loop for an MMIO grant and the dedup loop requires one region, so a driver's threads sit in
  different `Domain` objects: one grant, one domain, one thread. The isolation principle is what
  made the driver invisible to the kernel, so this defect was a consequence of it, not an oversight.
  The window comes from the ARCH rather than the publisher, because asking the publisher means
  trusting an unvalidated userspace address for the kernel's own registers -- and because storing
  the pair costs 8 bytes of `.bss`, which is enough to red `microbit_selftest` (measured).
  `KOS_SYS_THREAD_KILL` is what makes the deferred reclaim terminate: cooperative cancellation,
  gated on **spawn parenthood** rather than an authority bit or a capability, since parenthood
  grants the caller nothing it did not already have and cannot be delegated. Gated by
  `sim_driver_death` case 3, mutation-proved four ways. **No silicon**: the two real drivers are
  compile-verified only.

- **`kos_print` does not survive a published console**, and that is the whole of it -- NOT "app
  output is invisible". `printf` and `std::cout` DO reach a published driver via three
  publish-aware writers kept in step (`user/include/kickos/sys/emit.h`). What drops is
  `kos_print` / `kos_kconsole_write`, and RTT still carries it. **Silence from a `kos_print`-only app
  is not evidence of a dead driver**: the write is dropped before it reaches the ring, so a working
  driver and a dead one look identical on the wire -- pick a `printf` app (`gpioblink`) to probe one.
  The remaining exposure is a freestanding app using `kos_print`. What is INHERENT and not a bug: on
  a chip where the driver takes the UART the kernel was using, the kernel must let go first, so
  kernel-console writes in that span are dropped by construction -- one device, one owner.
- **M4.6.2's CDC console is witnessed only under the DIAG service list.** On `pizero2350` the host
  enumerates the device and binds `cdc_acm`, `lsusb -v` parses both interfaces, and bulk IN carried
  918 bytes byte-exactly with `drop=0 used=0` -- which validates the descriptor tables, the chapter
  9 request machine and the multi-packet EP0 data stage, the parts flagged highest-risk because no
  USB 2.0 or CDC/PSTN specification is in the local reference set. Under the PRODUCTION service
  list the device still reaches `[rpusb] host configured the device`, and then **not one byte
  reaches the ACM tty** across three attempts. Bulk OUT has never been exercised at all, `teensy41`
  is a marked seam rather than a half-built backend, and `Shared::configured` does not clear on
  unplug because no backend arms a disconnect or suspend source.
- **CLOSED -- `f302nucleo`'s silent fault report was the FLASH COMMAND, not the firmware**, root cause
  and evidence in *Where we are* above. The LED probe that read "dark forever, the reporter entry
  never ran" was correct and is EXPLAINED by it: the core was halted at the handler's first
  instruction, so nothing downstream of it could run. Two instrument lessons outlive the bug. The UART
  markers were CONFOUNDED -- every marker that fires runs with `CR1.TXEIE` clear, so every reading
  past that point was a false negative; use a raw `GPIOB->BSRR` LED store with cycle-counted dwells
  instead. And the `CFSR=0x00008200`/`BFAR=0xE000ED00` reading quoted as this defect's evidence for
  weeks belongs to `ringpriv`, not to `fault`. **A debugger left armed is a legitimate suspect before
  the silicon is**: three hardware hypotheses were carried for weeks and all three were dead.
- **No emulated gate can exercise a buffered-ring panic flush, and the sim cannot substitute** (its
  ring is provably empty at panic time; deleting `console_tx_flush_sync()` leaves the sim suite
  green). The drain is witnessed on `pizero2350` with a measured non-empty ring (`used_at_panic=419`
  of 511, 0 after the flush) and a negative control that strands all 419. Still no automated gate:
  the in-env hole is open, the behaviour is not in doubt.
- **`microbit` HAS NO ARENA SLACK AT ALL, and that is a structural fact, not a tight budget.**
  Measured 2026-08-11: `__kickos_ram_start` IS `_ebss`, both at `0x20001AE0`, so the arena begins
  exactly where `.bss` ends. The granule is 32 bytes, so ANY non-zero `.bss` addition, even four
  bytes, moves the arena base a full granule and costs one `kos_ram_alloc` grain. Proven both ways: a
  4-byte object and a single-slot pool move it identically. **So on this board the question is never
  "how many bytes" but "any bytes at all"**, and every future `.bss` growth will keep flipping an arm
  until something structural changes. M4.8.3's task pool took `mem_self_grant`'s grain, which is now
  a declared skip; the arm still runs on 13 other boards including `picopi`, the other armv6m part.
  **This is NOT a fleet property, and reading it as one would be the wrong lesson.** It is where that
  board's arena base comes from. On `bluepill-c8` the base is a fixed `0x20003400` against an `_ebss`
  of `0x200013E4`, so `.bss` growth lands in the gap and its 96 bytes of boot-arena slack are
  untouched. Check the two symbols before assuming either shape.
- **Two boards advertise thread slots their arena cannot back**, which is why
  `KICKOS_POOL_ARENA_ASSERT` stays opt-in. Headroom is PER-IMAGE, not per-preset -- each app's
  static footprint moves the arena base, so never quote one number per board.
- **`bluepill-c8-st` has 96 B of boot-arena slack** (measured at `6be8220`, up from zero because
  stage 0 handed back two reserved cap slots), so any static-RAM growth in a SHARED test still
  breaks its link -- and it has no ctest gate and no unit, so only a full-fleet build catches it.
- **A per-chip `arch_console_reclaim` body exists only on `mk64f`, `xmc4800` and `esp32`**, so
  elsewhere a driver death flips the state and the polled route works but the DEVICE is whatever
  the dead driver left. Per-chip bodies are fleet work; `roadmap.md`'s sub-milestone ledger says
  which number that is.
- **FOUR in-tree apps grant a DEV window a live board-service driver already holds**, which the
  one-holder-per-window check refuses. Silicon-only: no in-env gate covers any of them.
- **A missed `KICKOS_APP_AUTHORITY` declaration surfaces only at runtime.** The kernel cannot know
  what an app will call, so there is no configure-time equivalent of the root-MMIO `FATAL_ERROR`;
  the failure is a checked `-KOS_EPERM`. The nastiest shape is an app that ignores a failed
  `pinmux_set` and then drives an unmuxed pin.
- **`Debug` is not a supported configuration on the 64 KiB boards** (decided in M4.5.5, not a
  blocker). It is the whole class, and the overflow moves every milestone that grows the suite,
  so re-measure rather than quoting -- `docs/reference/porting.md` holds the current figures.

## History that must not be garbage-collected

**`c296feb` is reachable only from the local unpushed branch `m4.2-presquash`.** It holds
`git show c296feb:docs/design-m4-rx-irq-demux.md`, which `docs/design-m4.6-irq-driver.md` section 6
cites rather than reproduces for the RX routing-class taxonomy, the group-register table and the
level-versus-edge semantics. One `git branch -D m4.2-presquash` destroys an M4.6.1 prerequisite.

Captures and records across `TODO.md` and `docs/` stamp pre-squash tips (`c5d9b0d`, `270b6fa`,
`124b68c`, `989af16`, `16e4af0`, `788b1d8`) that folded into `dde73ca` and reach no branch. The
stamps stay as written.

The task-layer squash folded nine commits into one, so the tips stamped for it reach no branch except
a backup: `58e7174e` (the `m484` capture banners) is on `backup/presquash-m483-m484`, `f8cc32bd` (the
`m483` fleet pass) on `backup-preland-final`, `b77a3ef4` (M4.8.2's six boards) on
`backup-m4.8.2-presquash`. None of the three trees is reproduced by a current commit -- all three are
intermediate, and the squashed commit carries only the final tree. Their witnesses stand on the record
here, as with `dde73ca` above. A banner reading `58e7174e` names the tree that was flashed, not a
commit anyone can now check out; what makes that witness still good is that the two commits after it
touched docs plus one redundant cast, with the armv7m object byte-identical.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `roadmap.md` -- the milestone plan, and the sub-milestone ledger: the only place a number is
  ASSIGNED. This file carries the locked ORDER and cites those numbers.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
