#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Run the double-precision compares the RX compiler emits, OFFLINE, and refuse a branch that
# goes the wrong way for any operand pair, a NaN, an infinity or a subnormal included.
#
# WHY. GNURX's DFPU compare branched on ORDERED for UNORDERED, so isnan, isinf, isfinite,
# fpclassify and the isless family read every finite double as a NaN under -mdfpu, and printf
# printed "nan" for 1.0, while every plain `<` and `==` stayed right. The toolchain's
# kickos-rx-dfpu-compare patch corrects it (docs/design-m10-toolchain.md section 1), and QEMU has
# no rxv3 machine, so without this gate only the board would see the patch go missing.
#
# HOW. A probe TU of `if (P(a, b)) k();` and `if (!P(a, b)) k();` for each predicate is compiled
# with the board's flags, and each function's straight-line code is stepped for every pair of a
# value set: dmov loads, dabs, the dcmp family, mvfdr and the Z branches. A function that calls k
# when P is false, or not when it is true, is a defect. The premise is the RXv3 software
# manual's (DCMPcm, MVFDR), and the one the board's plain compares bear out: `dcmpCC s, d` sets
# DCMR.RES when d CC s holds, and mvfdr copies RES into Z. Operands compare as IEEE 754 says,
# denormals unflushed: what is judged is the branch the compiler chose, not the DPSW.DDN
# posture a thread runs under. An instruction the stepper does not model refuses the run
# rather than being skipped, so a new code shape is a loud failure and not a pass.
#
# Planted controls run first: a wrong-way UNORDERED and UN* branch must be refused and the
# right-way ones passed, or no verdict below is trusted.
#
# usage: check_rx_dfpu_compare.py <cc> [<flag>...]
# exits 77 (skip) when the flags select no double-precision FPU.

import math
import struct
import subprocess
import sys

PREDICATES = [
    ("lt", "a < b", lambda a, b: a < b),
    ("le", "a <= b", lambda a, b: a <= b),
    ("gt", "a > b", lambda a, b: a > b),
    ("ge", "a >= b", lambda a, b: a >= b),
    ("eq", "a == b", lambda a, b: a == b),
    ("ne", "a != b", lambda a, b: a != b),
    ("unord", "__builtin_isunordered(a, b)", lambda a, b: math.isnan(a) or math.isnan(b)),
    ("isless", "__builtin_isless(a, b)", lambda a, b: a < b),
    ("islessequal", "__builtin_islessequal(a, b)", lambda a, b: a <= b),
    ("isgreater", "__builtin_isgreater(a, b)", lambda a, b: a > b),
    ("isgreaterequal", "__builtin_isgreaterequal(a, b)", lambda a, b: a >= b),
    ("islessgreater", "__builtin_islessgreater(a, b)", lambda a, b: a < b or a > b),
    ("isnan", "__builtin_isnan(a)", lambda a, b: math.isnan(a)),
    ("isinf", "__builtin_isinf(a)", lambda a, b: math.isinf(a)),
    ("isfinite", "__builtin_isfinite(a)", lambda a, b: math.isfinite(a)),
    ("isnormal", "__builtin_isnormal(a)",
     lambda a, b: math.isfinite(a) and abs(a) >= 2.2250738585072014e-308),
]

VALUES = [1.0, 2.0, -0.0, math.inf, -math.inf, math.nan, 5e-324, 1.7976931348623157e308,
          2.2250738585072014e-308]

BRANCHES = {"bz", "beq", "bnz", "bne", "bra", "bsr"}
OTHER_FLOW = {"bc", "bnc", "bgeu", "bltu", "bgtu", "bleu", "bpz", "bn", "bge", "blt", "bgt",
              "ble", "bo", "bno", "jmp", "jsr", "rtsd"}
IGNORED = {"dpushm", "dpopm", "nop", "mov", "pushm", "popm", "push", "pop"}
DCMP = {"dcmpun", "dcmpeq", "dcmplt", "dcmple"}


def bits(x):
    u = struct.unpack("<Q", struct.pack("<d", x))[0]
    return u & 0xFFFFFFFF, u >> 32


def value(lo, hi):
    return struct.unpack("<d", struct.pack("<Q", (hi << 32) | lo))[0]


def parse(asm):
    funcs = {}
    cur = None
    for raw in asm.splitlines():
        line = raw.split(";")[0].strip()
        if not line:
            continue
        if line.endswith(":"):
            name = line[:-1]
            if name.startswith("_p_") or name.startswith("_q_"):
                cur = funcs.setdefault(name[1:], [])
            elif cur is not None:
                cur.append(("label", name))
            continue
        if line.startswith(".") or cur is None:
            continue
        parts = line.split(None, 1)
        ops = [o.strip() for o in parts[1].split(",")] if len(parts) > 1 else []
        cur.append((parts[0], ops))
    return funcs


def dreg(op):
    if not op.startswith("dr") or not op[2:].isdigit():
        raise ValueError(f"not a DFPU register: {op}")
    return int(op[2:])


def step(body, a, b):
    """Run one probe function; True when it reached k."""
    gpr = {}
    gpr[1], gpr[2] = bits(a)
    gpr[3], gpr[4] = bits(b)
    dr = {}
    z = None
    dcmr = None
    labels = {ins[1]: i for i, ins in enumerate(body) if ins[0] == "label"}
    called = False
    pc = 0
    for _ in range(400):
        if pc >= len(body):
            raise ValueError("fell off the end of the function")
        mn, ops = body[pc]
        pc += 1
        if mn == "label":
            continue
        base = mn.split(".")[0].lower()
        if base == "dmov":
            src, dst = ops
            if dst.startswith("drl") or dst.startswith("drh"):
                k = int(dst[3:])
                lo, hi = dr.get(k, (0, 0))
                if src.startswith("#"):
                    word = int(src[1:], 0) & 0xFFFFFFFF
                elif src.startswith("r") and src[1:].isdigit():
                    word = gpr.get(int(src[1:]))
                    if word is None:
                        raise ValueError(f"{src} holds no argument word")
                else:
                    raise ValueError(f"unmodelled dmov source {src}")
                # .D into the upper half clears the lower one; .L keeps the other half.
                if dst.startswith("drl"):
                    lo = word
                elif mn.lower() == "dmov.d":
                    lo, hi = 0, word
                else:
                    hi = word
                dr[k] = (lo, hi)
            elif src.startswith("dr"):
                dr[dreg(dst)] = dr[dreg(src)]
            else:
                raise ValueError(f"unmodelled dmov {mn} {', '.join(ops)}")
        elif base == "dabs":
            lo, hi = dr[dreg(ops[0])]
            dr[dreg(ops[1])] = (lo, hi & 0x7FFFFFFF)
        elif base == "dneg":
            lo, hi = dr[dreg(ops[0])]
            dr[dreg(ops[1])] = (lo, hi ^ 0x80000000)
        elif base in DCMP:
            s = value(*dr[dreg(ops[0])])
            d = value(*dr[dreg(ops[1])])
            dcmr = {"dcmpun": math.isnan(s) or math.isnan(d), "dcmpeq": d == s,
                    "dcmplt": d < s, "dcmple": d <= s}[base]
        elif base == "mvfdr":
            if dcmr is None:
                raise ValueError("mvfdr before any dcmp")
            z = dcmr
        elif base in BRANCHES:
            target = ops[0]
            if base in ("bz", "beq", "bnz", "bne"):
                if z is None:
                    raise ValueError(f"{mn} on a Z no mvfdr set")
                if z != (base in ("bz", "beq")):
                    continue
            if target == "_k":
                if base == "bra":
                    return True
                called = True
                continue
            if target not in labels:
                raise ValueError(f"branch to {target}, which is not a local label")
            if base == "bsr":
                raise ValueError(f"bsr to a local label {target}")
            pc = labels[target]
        elif base == "rts":
            return called
        elif base in OTHER_FLOW:
            raise ValueError(f"unmodelled control flow {mn}")
        elif base in IGNORED:
            continue
        elif base.startswith("d"):
            raise ValueError(f"unmodelled DFPU instruction {mn}")
        else:
            raise ValueError(f"unmodelled instruction {mn}")
    raise ValueError("no return within 400 steps")


def judge(funcs, preds):
    """Return (defects, shapes); a defect is one wrong outcome, a shape a dcmp sequence seen."""
    defects = []
    shapes = set()
    for name, _, truth in preds:
        for form in ("p", "q"):
            fn = f"{form}_{name}"
            body = funcs.get(fn)
            if body is None:
                defects.append(f"{fn}: the compile emitted no such function")
                continue
            seq = [ins[0] for ins in body if ins[0] in DCMP]
            if not seq:
                defects.append(f"{fn}: no dcmp, so the compare never reached the DFPU")
                continue
            shapes.add(tuple(seq))
            wrong = []
            try:
                for a in VALUES:
                    for b in VALUES:
                        want = bool(truth(a, b)) if form == "p" else not truth(a, b)
                        if step(body, a, b) != want:
                            wrong.append(f"({a!r}, {b!r}) {'skips' if want else 'calls'} k")
            except (ValueError, KeyError) as e:
                defects.append(f"{fn}: {e}")
                continue
            if wrong:
                defects.append(f"{fn}: wrong for {len(wrong)} of {len(VALUES) ** 2} pairs, "
                               f"first {wrong[0]}")
    return defects, shapes


def probe_source(preds):
    lines = ["extern void k(void);"]
    for name, expr, _ in preds:
        lines.append(f"void p_{name}(double a, double b) {{ if ({expr}) k(); }}")
        lines.append(f"void q_{name}(double a, double b) {{ if (!({expr})) k(); }}")
    return "\n".join(lines) + "\n"


# Controls: hand-written code, two shapes per verdict. A wrong-way UNORDERED (k on an ordered
# pair) and a wrong-way UN* pre-test must be refused; the right-way ones must pass.
CTL_LOAD = "\tdmov.L\tr1, drl0\n\tdmov.L\tr2, drh0\n\tdmov.L\tr3, drl1\n\tdmov.L\tr4, drh1\n"
CTL_PREDS = [("unord", None, PREDICATES[6][2]), ("isgreater", None, PREDICATES[9][2])]


def ctl_asm(un_branch, ungt_branch):
    # p_unord: k on unordered. q_unord: k on ordered. p_isgreater: k when a > b, so the code
    # skips on UNLE(a, b). q_isgreater: k on UNLE(a, b).
    def unle(skip_label, call_on_skip):
        tail_ok = "\tbra\t_k\n" if not call_on_skip else "\trts\n"
        tail_skip = "\trts\n" if not call_on_skip else "\tbra\t_k\n"
        return (CTL_LOAD + f"\tdcmpun\tdr1, dr0\n\tmvfdr\n\t{ungt_branch}\t{skip_label}\n"
                f"\tdcmple\tdr1, dr0\n\tmvfdr\n\tbz\t{skip_label}\n" + tail_ok
                + f"{skip_label}:\n" + tail_skip)
    other = "bnz" if un_branch == "bz" else "bz"
    return ("_p_unord:\n" + CTL_LOAD + f"\tdcmpun\tdr1, dr0\n\tmvfdr\n\t{un_branch}\t.L1\n"
            "\trts\n.L1:\n\tbra\t_k\n"
            "_q_unord:\n" + CTL_LOAD + f"\tdcmpun\tdr1, dr0\n\tmvfdr\n\t{other}\t.L2\n"
            "\trts\n.L2:\n\tbra\t_k\n"
            "_p_isgreater:\n" + unle(".L3", False)
            + "_q_isgreater:\n" + unle(".L4", True))


def controls():
    good, _ = judge(parse(ctl_asm("bz", "bz")), CTL_PREDS)
    if good:
        sys.exit("FAIL: the stepper refuses right-way branches, so its verdict cannot be "
                 "trusted:\n  " + "\n  ".join(good))
    bad_un, _ = judge(parse(ctl_asm("bnz", "bz")), CTL_PREDS)
    if not any(d.startswith("p_unord: wrong") for d in bad_un):
        sys.exit("FAIL: the stepper passes a planted UNORDERED that branches on ordered")
    bad_ungt, _ = judge(parse(ctl_asm("bz", "bnz")), CTL_PREDS)
    if not any(d.startswith("p_isgreater: wrong") for d in bad_ungt):
        sys.exit("FAIL: the stepper passes a planted UN* pre-test that branches on ordered")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: check_rx_dfpu_compare.py <cc> [<flag>...]")
    cc, flags = sys.argv[1], sys.argv[2:]
    controls()
    macros = subprocess.run([cc, *flags, "-dM", "-E", "-x", "c", "-"], input="",
                            capture_output=True, text=True)
    if macros.returncode != 0:
        sys.exit(f"FAIL: {cc} could not preprocess with {' '.join(flags)}:\n{macros.stderr}")
    if "__RX_DFPU_INSNS__" not in macros.stdout:
        print(f"SKIP: {' '.join(flags) or 'no flags'} select no double-precision FPU")
        sys.exit(77)
    src = probe_source(PREDICATES)
    defects = []
    shapes = set()
    for opt in ("-O2", "-Os"):
        out = subprocess.run([cc, *flags, opt, "-S", "-x", "c", "-", "-o", "-"], input=src,
                             capture_output=True, text=True)
        if out.returncode != 0:
            sys.exit(f"FAIL: {cc} {opt} could not compile the probe:\n{out.stderr}")
        d, s = judge(parse(out.stdout), PREDICATES)
        defects += [f"{opt} {x}" for x in d]
        shapes |= s
    # Vacuity: the probe must have reached the one-test and the two-test shapes, the second
    # being where an unordered pre-test guards an ordered compare.
    if not any(len(s) == 1 and s[0] == "dcmpun" for s in shapes):
        defects.append("no function compiled to a lone dcmpun, so UNORDERED was never stepped")
    if not any(len(s) >= 2 and s[0] == "dcmpun" for s in shapes):
        defects.append("no function compiled to a dcmpun pre-test, so no UN* code was stepped")
    if defects:
        print(f"FAIL: {len(defects)} double-precision compare(s) from {cc} branch wrong:")
        for d in defects:
            print(f"  {d}")
        sys.exit(1)
    print(f"PASS: {len(PREDICATES) * 2} predicate forms at -O2 and -Os branch right for "
          f"{len(VALUES) ** 2} operand pairs each")


if __name__ == "__main__":
    main()
