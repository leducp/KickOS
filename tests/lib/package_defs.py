#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The reader behind gate.sh's package_defs(): the -D set a consumer's OWN translation
# unit(s) actually compile with, PER TU, refusing rather than unioning when the corpus
# disagrees. An entry carries either "command" (one shell-escaped string) or "arguments"
# (an argv list already split); this project's generator (Ninja) emits "command", so it is
# un-escaped with shlex and not a whitespace split, because a -D value can itself carry a
# quoted space or comma that a naive split would cut in the wrong place.
#
# usage: package_defs.py <compile_commands.json>
# stdout: one "-DNAME[=VALUE]" per line, the ONE set every TU in the corpus carries.
# Disagreement, or a corpus with no TU or no -D at all, is a fatal error on stderr naming
# the translation units and the defines they differ on.

import json
import shlex
import sys


def tu_argv(entry):
    if "arguments" in entry:
        return [str(a) for a in entry["arguments"]]
    if "command" in entry:
        return shlex.split(entry["command"])
    return None


def defines_of(argv):
    defs = []
    i = 0
    while i < len(argv):
        tok = argv[i]
        if tok == "-D":
            i += 1
            if i < len(argv):
                defs.append("-D" + argv[i])
        elif tok.startswith("-D"):
            defs.append(tok)
        i += 1
    return defs


def main(argv):
    if len(argv) != 2:
        print("usage: package_defs.py <compile_commands.json>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        with open(path) as f:
            corpus = json.load(f)
    except (OSError, ValueError) as exc:
        print(f"{path}: cannot be read as JSON: {exc}", file=sys.stderr)
        return 1
    if not isinstance(corpus, list) or not corpus:
        print(f"{path} names no translation unit at all", file=sys.stderr)
        return 1

    per_tu = {}
    order = []
    for entry in corpus:
        name = entry.get("file") or entry.get("output") or "<unnamed translation unit>"
        argv_tu = tu_argv(entry)
        if argv_tu is None:
            print(f"{name}: entry carries neither \"command\" nor \"arguments\"",
                  file=sys.stderr)
            return 1
        if name not in per_tu:
            per_tu[name] = set()
            order.append(name)
        per_tu[name].update(defines_of(argv_tu))

    base_name = order[0]
    reference = per_tu[base_name]
    if not reference:
        print(f"{base_name} carries no -D argument at all", file=sys.stderr)
        return 1

    disagreeing = [name for name in order[1:] if per_tu[name] != reference]
    if disagreeing:
        print("package_defs: the corpus's translation units do not agree on their -D set:",
              file=sys.stderr)
        for name in disagreeing:
            diff = per_tu[name].symmetric_difference(reference)
            print(f"  {base_name} vs {name}: disagree on {sorted(diff)}", file=sys.stderr)
        return 1

    for d in sorted(reference):
        print(d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
