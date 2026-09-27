#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Check incoming calls to the scheduler and timer's caller-held entry points.

The compiler's callgraph supplies the active calls and their source positions. A
call is discharged by an IrqLock live at that position; otherwise the obligation
travels to every caller. An unknown edge or an unlisted entry root is a failure.
"""

import collections
import functools
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

import trap_redzone as cg

HERE = Path(__file__).resolve().parent

TARGETS = (
    "kickos::sched::add", "kickos::sched::reseat",
    "kickos::sched::reschedule", "kickos::sched::block_current",
    "kickos::sched::detach_current", "kickos::sched::wake",
    "kickos::sched::wake_no_resched", "kickos::sched::resched_after_wake",
    "kickos::sched::place_ready", "kickos::sched::switch_prepare",
    "kickos::sched_effective_level",
    "kickos::sched::set_prio", "kickos::sched::tick_rr",
    "kickos::ktime_deadline_arm", "kickos::ktime_deadline_cancel",
    "kickos::ktime_rearm", "kickos::ktime_disarm",
)
MARKED_HEADERS = ("kernel/include/kickos/sched.h", "kernel/include/kickos/time.h")
# A declaration's marker is the comment block directly above it. A declaration on the
# next line shares that block, as wake_no_resched and resched_after_wake do.
MARKER = re.compile(
    r"\bcaller holds (?:the (?:kernel's )?exclusion|irqlock)\b"
    r"|\brequire the caller to hold the exclusion\b", re.IGNORECASE)
DECLARED = re.compile(r"(?<![\w:])([A-Za-z_]\w*)\s*\(")
LOCK = re.compile(r"\b(?:kickos::)?IrqLock\s+([A-Za-z_]\w*)\s*(?:;|\{\s*\}|\(\s*\)\s*;)")
TOKEN = re.compile(r"[{}]|\b(?:kickos::)?IrqLock\s+[A-Za-z_]\w*\s*(?:;|\{\s*\}|\(\s*\)\s*;)")
COMMENT = re.compile(r"//[^\n]*|/\*[\s\S]*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'")
FUNCTION = re.compile(
    r"(?<!\w)(~?[A-Za-z_]\w*)\s*\([^;{}()]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{",
    re.MULTILINE)
CONTROL = {"if", "for", "while", "switch", "catch"}
COMPILE = {}
ACTIVE = {}


def marked_entries(root):
    """Qualified names of the namespace-scope functions the headers mark caller-held."""
    marked = set()
    for header in MARKED_HEADERS:
        scopes = []
        pending = []
        shared = None
        statement = ""
        for line in Path(root, header).read_text().splitlines():
            stripped = line.strip()
            if stripped.startswith("//"):
                if shared is not None:
                    pending, shared = [], None
                pending.append(stripped[2:])
                continue
            code = line.split("//", 1)[0]
            if not code.strip() or code.lstrip().startswith("#"):
                pending, shared = [], None
                continue
            block = pending if shared is None else shared
            at_namespace = all(scope is not None for scope in scopes)
            name = DECLARED.search(code)
            if at_namespace and not statement.strip() and name:
                if MARKER.search(" ".join(" ".join(block).split())):
                    marked.add("::".join(scopes + [name.group(1)]))
                shared = block
            else:
                shared = None
            pending = []
            for char in code:
                if char in ";{}":
                    if char == "{":
                        space = re.fullmatch(r"\s*namespace\s+([A-Za-z_]\w*)\s*", statement)
                        scopes.append(space.group(1) if space else None)
                    elif char == "}":
                        if not scopes:
                            raise ValueError(f"{header}: unmatched close brace")
                        scopes.pop()
                    statement = ""
                else:
                    statement += char
            if statement.strip():
                statement += " "
        if scopes:
            raise ValueError(f"{header}: unclosed scope")
    return marked


def check_targets(root):
    marked = marked_entries(root)
    unlisted = sorted(marked - set(TARGETS))
    unmarked = sorted(set(TARGETS) - marked)
    if unlisted:
        raise ValueError("caller-held marker with no TARGETS entry: " + ", ".join(unlisted))
    if unmarked:
        raise ValueError("TARGETS entry with no caller-held marker: " + ", ".join(unmarked))


@functools.lru_cache(maxsize=None)
def source(path):
    raw = Path(path).read_text()
    # Keep every position and newline so the compiler's file:line:column still points
    # at the same byte. Inactive preprocessor arms and quoted braces cannot
    # change the scope containing a call or a lock declaration.
    if path in COMPILE:
        active = active_lines(path)
        raw_lines = raw.splitlines(keepends=True)
        raw = "".join(line if n in active else re.sub(r"[^\n]", " ", line)
                      for n, line in enumerate(raw_lines, 1))
    clean = COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), raw)
    starts = [0]
    starts += [i + 1 for i, c in enumerate(clean) if c == "\n"]
    scopes = [0]
    next_scope = 1
    locks = []
    events = []
    for match in TOKEN.finditer(clean):
        tok = match.group()
        if tok == "{":
            scopes.append(next_scope)
            next_scope += 1
        elif tok == "}":
            if len(scopes) == 1:
                raise ValueError(f"{path}: unmatched close brace")
            scopes.pop()
        else:
            name = LOCK.fullmatch(tok)
            if name is None:
                raise ValueError(f"{path}: unrecognized lock declaration {tok}")
            locks.append((match.end(), tuple(scopes), name.group(1)))
        events.append((match.start(), tuple(scopes)))
    return clean, raw, starts, events, locks


def offset(location):
    parts = location.rsplit(":", 2)
    if len(parts) != 3 or not parts[1].isdigit() or not parts[2].isdigit():
        raise ValueError(f"call has no usable source location: {location}")
    path, line, column = parts[0], int(parts[1]), int(parts[2])
    clean, raw, starts, events, locks = source(path)
    if line > len(starts):
        raise ValueError(f"{location}: past end of source")
    at = starts[line - 1] + column - 1
    if at >= len(clean):
        raise ValueError(f"{location}: past end of source")
    return path, at


def locked_at(path, at):
    clean, raw, starts, events, locks = source(path)
    # The compiler reports the expression's column. The last brace event before it
    # names every lexical block still alive at that point.
    import bisect
    i = bisect.bisect_right(events, (at, (1 << 60,))) - 1
    scopes = events[i][1] if i >= 0 else (0,)
    active = active_lines(path)
    for began, held_scopes, name in reversed(locks):
        if began > at:
            continue
        if bisect.bisect_right(starts, began - 1) not in active:
            continue
        if held_scopes[-1] not in scopes:
            continue
        # Explicit end drops the bracket while the C++ object remains in scope.
        if re.search(r"\b" + re.escape(name) + r"\s*\.\s*end\s*\(", raw[began:at]):
            continue
        return True
    return False


def active_lines(path):
    if path in ACTIVE:
        return ACTIVE[path]
    if path not in COMPILE:
        raise ValueError(f"{path}: no compile command to check the active lock declaration")
    entry = COMPILE[path]
    argv = entry.get("arguments") or shlex.split(entry["command"])
    command = []
    skip = False
    for arg in argv:
        if skip:
            skip = False
        elif arg == "-o":
            skip = True
        elif arg != "-c":
            command.append(arg)
    command += ["-E", "-fdirectives-only"]
    result = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f"{path}: preprocessing failed: {result.stderr[:300]}")
    current = None
    line_no = 0
    active = set()
    for line in result.stdout.splitlines():
        marker = re.match(r'^#\s+(\d+)\s+"([^"]+)"', line)
        if marker:
            line_no = int(marker.group(1))
            current = os.path.realpath(marker.group(2))
        else:
            if current == path:
                active.add(line_no)
            line_no += 1
    if not active:
        raise ValueError(f"{path}: preprocessor produced no active source lines")
    ACTIVE[path] = active
    return active


@functools.lru_cache(maxsize=None)
def functions(path):
    clean = source(path)[0]
    pairs = {}
    stack = []
    for i, c in enumerate(clean):
        if c == "{":
            stack.append(i)
        elif c == "}" and stack:
            pairs[stack.pop()] = i
    spans = []
    for match in FUNCTION.finditer(clean):
        name = match.group(1)
        opening = match.end() - 1
        if name not in CONTROL and opening in pairs:
            spans.append((opening, pairs[opening], name))
    return spans


def function_at(path, at):
    matches = [entry for entry in functions(path) if entry[0] <= at <= entry[1]]
    if not matches:
        # GCC locates an implicit destructor edge at the destructor's signature.
        matches = [entry for entry in functions(path)
                   if at <= entry[0] <= at + 512]
        if matches:
            return min(matches, key=lambda entry: entry[0])
    if not matches:
        raise ValueError(f"{path}: no function encloses byte {at}")
    return min(matches, key=lambda entry: entry[1] - entry[0])


def node_definitions(build_dir):
    result = {}
    for ci in cg.corpus(build_dir):
        for line in Path(ci).read_text().splitlines():
            match = cg.NODE_RE.match(line)
            if match is None or cg.FRAME_RE.search(match.group(2)) is None:
                continue
            place = re.search(r"\\n(/[^\\]+):(\d+):(\d+)", match.group(2))
            if place:
                result[cg.node_key(match.group(1))] = (place.group(1), int(place.group(2)))
    return result


def guarded(graph, definitions, caller, location):
    path, at = offset(location)
    if locked_at(path, at):
        return True
    helper = function_at(path, at)[2]
    if caller not in definitions:
        raise ValueError(f"{caller}: inlined call at {location} has no defining source")
    caller_path, caller_line = definitions[caller]
    caller_starts = source(caller_path)[2]
    caller_at = caller_starts[caller_line - 1]
    caller_name = function_at(caller_path, caller_at)[2]
    if helper == caller_name:
        return False
    spans = [s for s in functions(caller_path)
             if s[2] == caller_name and s[0] >= caller_at]
    if not spans:
        raise ValueError(f"{caller}: cannot locate body for inlined call at {location}")
    body = min(spans, key=lambda s: s[0])
    clean = source(caller_path)[0]
    def sites(span, name):
        if name.startswith("~"):
            pattern = r"\b" + re.escape(name[1:]) + r"\s+[A-Za-z_]\w*"
        else:
            pattern = r"\b" + re.escape(name) + r"\s*\("
        return [span[0] + m.start() for m in re.finditer(pattern, clean[span[0]:span[1]])]

    calls = sites(body, helper)
    if not calls and caller_path == path:
        # GCC can inline more than one helper and report only the innermost
        # location. Find the helper call in its source body, then the call of
        # that helper in this caller; refuse if the chain is not visible.
        for intermediate in functions(path):
            if intermediate[2] != caller_name and sites(intermediate, helper):
                calls.extend(sites(body, intermediate[2]))
    if not calls:
        raise ValueError(f"{caller}: inlined {helper} at {location} has no visible callsite")
    return all(locked_at(caller_path, site) for site in calls)


def direct_locations(graph, build_dir):
    result = collections.defaultdict(set)
    for ci in cg.corpus(build_dir):
        if ci in graph.unlinked:
            continue
        # The graph has already removed default seam units and unlinked nodes.
        for line in Path(ci).read_text().splitlines():
            match = cg.EDGE_RE.match(line)
            if match is None:
                continue
            src, dst, loc = (cg.node_key(match.group(1)),
                             cg.node_key(match.group(2)), match.group(3))
            if dst != cg.INDIRECT and dst in graph.edges.get(src, ()):
                result[src, dst].add(loc)
    return result


def check(build_dir, arch, preset, cores):
    source.cache_clear()
    functions.cache_clear()
    COMPILE.clear()
    ACTIVE.clear()
    check_targets(HERE.parents[1])
    for entry in json.loads(Path(build_dir, "compile_commands.json").read_text()):
        path = os.path.realpath(entry["file"])
        COMPILE.setdefault(path, entry)
    graph = cg.Graph(build_dir)
    raw = cg.read_bindings(str(HERE / "trap_redzone_indirect.txt"), arch, preset, cores)
    for number, line in enumerate((HERE / "caller_held_indirect.txt").read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        words, sep, reason = line.partition(" reason:")
        fields = words.split()
        if len(fields) != 4 or fields[0] != "ignore" or not sep or not reason.strip():
            raise ValueError(f"caller_held_indirect.txt:{number}: invalid ignore record")
        _, named_arch, named_preset, site = fields
        if named_arch == arch and named_preset in ("*", preset):
            if site in raw:
                raise ValueError(f"caller_held_indirect.txt:{number}: {site} is already bound")
            raw[site] = ([], f"caller_held_indirect.txt:{number}")
    bindings = cg.resolve_bindings(graph, raw)
    unbound = graph.all_sites() - set(bindings)
    if unbound:
        raise ValueError("unknown indirect edge: " + graph.where(sorted(unbound)[0]))
    graph.bind_indirect(bindings)
    locations = direct_locations(graph, build_dir)
    definitions = node_definitions(build_dir)
    incoming = collections.defaultdict(list)
    for src, targets in graph.edges.items():
        for dst in targets:
            if dst.startswith(cg.SITE_PREFIX):
                site = dst[len(cg.SITE_PREFIX):]
                _src, loc = graph.site_at[site]
                if loc is not None and not os.path.isabs(loc.rsplit(":", 2)[0]):
                    if src not in definitions:
                        raise ValueError(f"{site}: no defining source for indirect call")
                    loc = definitions[src][0] + ":" + loc.rsplit(":", 2)[1] + ":" + loc.rsplit(":", 2)[2]
                incoming[dst].append((src, loc))
            elif src.startswith(cg.SITE_PREFIX):
                incoming[dst].append((src, None))
            else:
                for loc in locations[src, dst]:
                    incoming[dst].append((src, loc))
                if not locations[src, dst]:
                    raise ValueError(f"{src} -> {dst}: direct edge lacks source location")

    checked = 0
    kernel_commands = Path(build_dir, "compile_commands.json").read_text()
    fastpath = 'KICKOS_ARCH_HAS_IPC_FASTPATH=1' in kernel_commands
    masked_roots = {"kickos_ipc_fastpath"} if fastpath else set()
    for root in masked_roots:
        if root not in graph.universe():
            raise ValueError(f"declared masked-context root {root} is absent")
        if incoming[root]:
            raise ValueError(f"masked-context root {root} acquired a C caller")

    def walk(node, trail):
        nonlocal checked
        if node in trail:
            raise ValueError("unguarded cycle: " + " -> ".join(trail + (node,)))
        # These four assembly trap entries enter the fastpath with interrupts
        # masked. Above one core the fastpath does not exist. A new C caller is
        # refused above, because it has no assembly mask guarantee.
        if node in masked_roots:
            return
        if node.startswith(cg.SITE_PREFIX) and node in graph.unbound:
            raise ValueError("unknown indirect edge: " + " -> ".join(trail + (node,)))
        calls = incoming[node]
        if not calls:
            raise ValueError("unguarded entry root: " + " -> ".join(trail + (node,)))
        for caller, loc in calls:
            checked += 1
            if caller.startswith(cg.SITE_PREFIX):
                if caller in graph.unbound:
                    raise ValueError("unknown indirect edge: " + caller)
                walk(caller, trail + (node,))
            elif loc is not None and guarded(graph, definitions, caller, loc):
                continue
            else:
                walk(caller, trail + (node,))

    present = 0
    expected = set(TARGETS) - {"kickos::sched::switch_prepare"}
    if cores == 1:
        expected -= {"kickos::sched::reseat", "kickos::sched::place_ready",
                     "kickos::sched_effective_level"}
    if fastpath:
        expected.add("kickos::sched::switch_prepare")
    for target in TARGETS:
        matches = [key for key, pretty in graph.pretty.items() if pretty == target]
        if len(matches) > 1:
            raise ValueError(f"{target}: ambiguous graph nodes: {matches}")
        if not matches:
            if target in expected:
                raise ValueError(f"{target}: declared caller-held entry missing from graph")
            continue
        if target not in expected:
            raise ValueError(f"{target}: entry present outside its declared posture")
        present += 1
        walk(matches[0], ())
    if present != len(expected):
        raise ValueError(f"{present} entries in graph, expected {len(expected)}")
    print(f"caller_held: {preset}: {present} entries, {checked} incoming calls checked")


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: check_caller_held.py <build-dir> <arch> <preset> <cores>")
    build, arch, preset, cores = sys.argv[1:]
    try:
        check(build, arch, preset, int(cores))
    except (cg.Bad, OSError, ValueError) as exc:
        sys.exit(f"FAIL: {exc}")


if __name__ == "__main__":
    main()
