#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The reader behind check_chip_divisor_rate.sh. Reads one chip PORT and answers, for every
# site that commits a console baud divisor, whether the value stored reaches a rate the port
# re-derives at runtime.
#
# IT IS A BACKWARD SLICE OVER THE STORE, NOT A PATTERN MATCH ON THE VALUE. The plausible
# wrong fixes for this defect all leave the store reading a constant. A literal lifted into a
# named constant is still one number, and a rate variable read and then ignored never reaches
# the store, so a reader that asked whether the file MENTIONS a rate would accept either. What makes a value live here is a bare
# re-assignment, an address handed to a call, or a call/register read: a constexpr is a
# constant however it is spelled, and a constant defined identically in two headers is still
# one constant.
#
# AND A CONDITION IS NOT A DERIVATION UNLESS IT SELECTS. Falling back to any enclosing
# condition accepts the third wrong fix, one number written inside `if (rate != 0)`, which
# borrows the guard's liveness while writing exactly what it wrote before. A guard counts here
# only where its chain stores the same sink under two of its branches and stores two different
# things, which is the shape of a port reading its clock mux back.
#
# usage: chip_divisor_rate.py <port-dir> <file>...   (paths relative to the repo root)
#        chip_divisor_rate.py --sinks                 (the sink register set, one per line)
# one record per site on stdout: LITERAL|DERIVED <file>:<line> <sink> <root>
# a port with no runtime rate, or none of these sites, prints one SKIP.

import re
import sys

# The console divisor registers, the whole set the arch_console_reclaim audit in
# docs/reference/invariants.md lists. A token is a sink when its last :: component equals one
# of these or ends with _<name>, which reaches UART0_BDH and LPUART6_BAUD without reaching
# CONSOLE_BAUD, a baud RATE that names no register. C4 carries the K64 1/32 fine-adjust, which
# is part of the divisor and not of the frame.
SINK_REGS = ("BDH", "BDL", "C4", "BRR", "BRGR", "IBRD", "FBRD", "BAUD", "CLKDIV", "FDR",
             "BRG", "divisor_lo", "divisor_hi")

# The helpers that commit a divisor without naming a register at the call site. The wrapper
# asserts each still has a definition in the tree, so a rename reddens rather than quietly
# shrinking the sink set.
SINK_HELPERS = ("set_baud", "usart_brr", "baud_select", "baud_sbr", "baud_divider")

# Accessor names that wrap the register token. Never the value.
MMIO = ("r8", "r16", "r32", "reg8", "reg16", "reg32", "put", "read32", "write32",
        "static_cast", "reinterpret_cast", "sizeof")

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*")
COND = re.compile(r"\b(if|switch|while)\s*\((.*)\)\s*\{?\s*$")
ELSE_KW = re.compile(r"(?:^|[};\s])else\b")
CASE_LABEL = re.compile(r"^\s*(?:case\b|default\s*:)")
CALL = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
# A parameter list has TYPE NAME pairs; an argument list has expressions. This is what tells
# a helper's DEFINITION from a call to it.
PARAM_PAIR = re.compile(r"(?:^|,)\s*(?:const\s+)?[A-Za-z_][\w:]*(?:\s*const)?\s*[*&]?\s+"
                        r"[*&]?\s*[A-Za-z_]\w*\s*(?:,|$)")
DECL = re.compile(r"^\s*(?:static|inline|constexpr|extern|const|volatile)\b")
FREQ = re.compile(r"\b\d{6,}\b|_HZ\b|_CLOCK\b|[Cc]lock|_hz\b")


def strip_prose(text):
    """Blank comments and string literals, preserving the line count."""
    out = []
    inblock = False
    for line in text.split("\n"):
        buf = []
        i = 0
        instr = None
        while i < len(line):
            two = line[i:i + 2]
            if inblock:
                if two == "*/":
                    inblock = False
                    i += 2
                    continue
                i += 1
                continue
            if instr is not None:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == instr:
                    instr = None
                i += 1
                continue
            if two == "/*":
                inblock = True
                i += 2
                continue
            if two == "//":
                break
            if line[i] in "\"'":
                instr = line[i]
                i += 1
                continue
            buf.append(line[i])
            i += 1
        out.append("".join(buf))
    return out


def last_component(tok):
    return tok.split("::")[-1]


def sink_reg(tok):
    name = last_component(tok)
    for reg in SINK_REGS:
        if name == reg or name.endswith("_" + reg):
            return True
    return False


def idents(expr):
    return [m.group(0) for m in IDENT.finditer(expr)]


def args_at(line, open_paren):
    depth = 0
    for i in range(open_paren, len(line)):
        if line[i] == "(":
            depth += 1
        elif line[i] == ")":
            depth -= 1
            if depth == 0:
                return line[open_paren + 1:i]
    return line[open_paren + 1:]


def inside_parens(line, pos):
    depth = 0
    for i in range(pos):
        if line[i] == "(":
            depth += 1
        elif line[i] == ")":
            depth -= 1
    return depth > 0


class Port:
    def __init__(self, files):
        self.lines = {}
        self.mutated = {}    # bare name -> [rhs of every re-assignment]
        self.declared = {}   # bare name -> [rhs of every declaration]
        self.addr_taken = set()
        self.defines = {}    # helper name -> set of files defining it
        self.bodies = {}     # file -> [(first line, last line)] of each helper's own body
        for path in files:
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    self.lines[path] = strip_prose(fh.read())
            except OSError:
                self.lines[path] = []
        self._harvest()

    def _harvest(self):
        # A DECLARATION carries a type before the name; a MUTATION does not. The difference
        # is the whole predicate: a constexpr repeated in two headers is not a moved value.
        decl = re.compile(r"(?:^|[;{}(,])\s*(?:(?:static|inline|constexpr|extern|const|"
                          r"volatile|thread_local)\s+)*[A-Za-z_][\w:]*(?:\s*<[^<>]*>)?"
                          r"\s*[*&]?\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*=\s*([^=;][^;]*);")
        mut = re.compile(r"(?:^|[;{}]|\)\s*)\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*"
                         r"(?:\.[A-Za-z_]\w*)?\s*=\s*([^=;][^;]*);")
        for path, lines in self.lines.items():
            for line in lines:
                hit = False
                for m in decl.finditer(line):
                    self.declared.setdefault(m.group(1), []).append(m.group(2))
                    hit = True
                if not hit:
                    for m in mut.finditer(line):
                        self.mutated.setdefault(m.group(1), []).append(m.group(2))
                for m in re.finditer(r"&\s*([A-Za-z_]\w*)", line):
                    self.addr_taken.add(m.group(1))
        self._span_helper_bodies()

    def _span_helper_bodies(self):
        """A call or a store INSIDE a divisor helper is that helper's own arithmetic, which
        the unit tests cover; only a PORT's call to it commits a divisor."""
        for path, lines in self.lines.items():
            n = 0
            while n < len(lines):
                line = lines[n]
                hit = None
                for m in CALL.finditer(line):
                    if m.group(1) in SINK_HELPERS and self._is_definition(line, m):
                        hit = m.group(1)
                        break
                if hit is None:
                    n += 1
                    continue
                self.defines.setdefault(hit, set()).add(path)
                depth = 0
                opened = False
                end = n
                for k in range(n, len(lines)):
                    depth += lines[k].count("{") - lines[k].count("}")
                    if "{" in lines[k]:
                        opened = True
                    end = k
                    if opened and depth <= 0:
                        break
                if opened:
                    self.bodies.setdefault(path, []).append((n + 1, end + 1))
                    n = end + 1
                else:
                    n += 1

    @staticmethod
    def _is_definition(line, match):
        args = args_at(line, match.end() - 1)
        return PARAM_PAIR.search(args) is not None

    def moves_a_rate(self):
        """A port with a degrade path RE-ASSIGNS a rate away from its declared value."""
        for name, rhs in self.mutated.items():
            if FREQ.search(name):
                return name
            for r in rhs:
                if FREQ.search(r):
                    return name
        return None

    def is_live(self, name, seen=None):
        if seen is None:
            seen = set()
        bare = last_component(name)
        if bare in seen:
            return False
        seen.add(bare)
        if bare in self.mutated:
            return True
        if bare in self.addr_taken:
            return True
        for r in self.declared.get(bare, []):
            for m in CALL.finditer(r):
                if m.group(1) not in MMIO:
                    return True
            if re.search(r"\b(?:r8|r16|r32|reg8|reg16|reg32)\s*\(", r):
                return True
            for tok in idents(r):
                if last_component(tok) == bare:
                    continue
                if self.is_live(tok, seen):
                    return True
        return False

    def live_root(self, expr):
        for m in CALL.finditer(expr):
            if m.group(1) not in MMIO:
                return m.group(1) + "()"
        for tok in idents(expr):
            if last_component(tok) in MMIO:
                continue
            if self.is_live(tok):
                return tok
        return None

    def sites(self):
        found = []
        for path in sorted(self.lines):
            found.extend(self._sites_in_file(path))
        return found

    @staticmethod
    def _leading_closes(stripped):
        n = 0
        for ch in stripped:
            if ch == "}":
                n += 1
            elif ch.isspace():
                continue
            else:
                break
        return n

    def _sites_in_file(self, path):
        """A guard's reach is its BLOCK, and an `else` continues the chain its `if` opened.
        Binding a frame at the depth of the body rather than at the depth of the `if` is what
        stops the condition from surviving the closing brace and lending its liveness to the
        next store in the function."""
        depth = 0
        bound = []      # [(body depth, frame)], innermost last
        pending = []    # frames whose body has not opened yet
        closed = None   # the frame whose branch just ended, for an `else` to continue
        conds = {}      # chain -> the conditions that select between its branches
        nchain = 0
        raw = []
        stores = {}     # (chain, sink) -> branch -> set of values stored there
        for n, line in enumerate(self.lines[path], 1):
            stripped = line.strip()
            lead = Port._leading_closes(stripped)
            after_lead = depth - lead
            while bound and bound[-1][0] > after_lead:
                closed = bound.pop()[1]
            # A case label opens the next branch of the switch it sits in, so a switch over a
            # clock source selects exactly as an if/else chain does.
            if CASE_LABEL.match(line) is not None and bound:
                if bound[-1][1]["kind"] == "switch":
                    bound[-1][1]["branch"] += 1
            # BEFORE the condition on this line is read: a helper called inside an `if` is a
            # store this file makes, and the condition it is spelled in cannot be its own
            # guard.
            sink = self._sink_of(path, line)
            if sink is not None and self._in_helper_body(path, n):
                sink = None
            if sink is not None:
                active = [(f["chain"], f["branch"]) for _, f in bound]
                active += [(f["chain"], f["branch"]) for f in pending]
                value = " ".join(sink[1].split())
                for chain, branch in active:
                    by_branch = stores.setdefault((chain, sink[0]), {})
                    by_branch.setdefault(branch, set()).add(value)
                raw.append((n, sink[0], sink[1], active))
            cond = COND.search(line)
            iselse = ELSE_KW.search(line) is not None
            opened = False
            if cond is not None:
                if iselse and closed is not None:
                    frame = {"chain": closed["chain"], "branch": closed["branch"] + 1,
                             "kind": cond.group(1)}
                    conds[frame["chain"]].append(cond.group(2))
                else:
                    nchain += 1
                    frame = {"chain": nchain, "branch": 0, "kind": cond.group(1)}
                    conds[nchain] = [cond.group(2)]
                pending.append(frame)
                closed = None
                opened = True
            elif iselse and closed is not None:
                pending.append({"chain": closed["chain"], "branch": closed["branch"] + 1,
                                "kind": "else"})
                closed = None
                opened = True
            depth += line.count("{") - line.count("}")
            if pending:
                if depth > after_lead:
                    for f in pending:
                        bound.append((depth, f))
                    pending = []
                elif not opened and stripped != "":
                    pending = []
            if not opened and stripped.lstrip("} \t") != "":
                closed = None

        found = []
        for n, sink, expr, active in raw:
            guards = []
            for chain, _branch in reversed(active):
                if Port._selects(stores, chain, sink):
                    guards.append(" ".join(conds[chain]))
            found.append((path, n, sink, expr, guards))
        return found

    @staticmethod
    def _selects(stores, chain, sink):
        """A GUARD ONLY COUNTS WHEN IT SELECTS. A condition that merely encloses one store
        lends that store its own liveness and changes nothing about the number written, which
        is how `if (rate != 0) { REG = 0x16C; }` reads as derived. What makes a guarded
        constant right is the port choosing BETWEEN constants on the rate it landed on, so the
        chain must store this sink under two of its branches and store two different things.
        Two spellings of one number defeat this, and no reader of source text can tell them
        apart."""
        by_branch = stores.get((chain, sink), {})
        if len(by_branch) < 2:
            return False
        values = set()
        for vals in by_branch.values():
            values |= vals
        return len(values) >= 2

    def _in_helper_body(self, path, n):
        for first, last in self.bodies.get(path, []):
            if first <= n <= last:
                return True
        return False

    def _sink_of(self, path, line):
        if "static_assert" in line or "_Static_assert" in line:
            return None
        for m in CALL.finditer(line):
            name = m.group(1)
            if name not in SINK_HELPERS:
                continue
            if self._is_definition(line, m):
                return None
            return name, args_at(line, m.end() - 1)
        if DECL.match(line):
            return None
        for m in IDENT.finditer(line):
            tok = m.group(0)
            if not sink_reg(tok):
                continue
            # The register must be an ACCESSOR ARGUMENT. Naming it left of an `=` is the
            # header stating its address, which stores no divisor.
            if not inside_parens(line, m.start()):
                continue
            rhs = Port._stored_value(line, m)
            if rhs is None:
                continue
            return tok, rhs
        return None

    @staticmethod
    def _stored_value(line, m):
        """Only what is STORED. Slicing the whole line would let the enclosing function's
        own name, or a neighbouring statement, stand in for the divisor's provenance."""
        tail = line[m.end():]
        close = tail.find(")")
        if close >= 0:
            after = tail[close + 1:]
            eq = re.search(r"(?<![=!<>])=(?!=)", after)
            if eq is not None:
                end = after.find(";")
                if end < 0:
                    end = len(after)
                return after[eq.end():end]
        # No assignment after it. Either the register is one argument among several and the
        # value is its siblings, or it is the whole argument, which is a READ of the
        # divisor, and reading one commits nothing.
        open_paren = line.rfind("(", 0, m.start())
        if open_paren < 0:
            return None
        args = args_at(line, open_paren)
        if "," not in args:
            return None
        return args.replace(m.group(0), " ")


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--sinks":
        for reg in SINK_REGS:
            print(reg)
        return 0
    if len(sys.argv) < 3:
        sys.stderr.write("usage: chip_divisor_rate.py <port-dir> <file>...\n")
        return 2
    port_dir = sys.argv[1]
    port = Port(sys.argv[2:])
    if not any(port.lines.values()):
        sys.stderr.write("chip_divisor_rate.py: %s: read no source\n" % port_dir)
        return 2
    rate = port.moves_a_rate()
    if rate is None:
        print("SKIP %s no-runtime-rate" % port_dir)
        return 0
    sites = port.sites()
    if not sites:
        print("SKIP %s no-divisor-site rate=%s" % (port_dir, rate))
        return 0
    for path, n, sink, expr, guards in sites:
        root = port.live_root(expr)
        why = "value"
        if root is None:
            for g in guards:
                root = port.live_root(g)
                if root is not None:
                    why = "guard"
                    break
        if root is None:
            print("LITERAL %s:%d %s -" % (path, n, sink))
        else:
            print("DERIVED %s:%d %s %s=%s" % (path, n, sink, why, root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
