# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# genconfig.py run as the build runs it, mostly over the xmc4800-relax base defconfig:
#
#   $KICKOS_KCONFIG_PY test_genconfig.py <cmake>
#
# kconfiglib does not fail on an out-of-range int: it warns and falls back on the symbol's
# DEFAULT, the fleet value rather than the one the defconfig asked for, so a refusal asserts the
# fleet value it names. Every KICKOS_* symbol must reach kickos_config.cmake: CMake reads a missing
# one as empty, and fed to a preprocessed linker script it collapses to '(() + 0 * ())'. The line
# each symbol is owed is derived here from kconfiglib's type and value, in a mapping independent
# of the generator's, so the two must disagree for that case to fail.

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

import kconfiglib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(os.path.dirname(HERE))
GEN = os.path.join(HERE, "genconfig.py")
FIX = os.path.join(SRC, "boards", "xmc4800-relax", "configs", "base", "defconfig")
AMP = os.path.join(SRC, "boards", "qemu-arm64", "configs", "amp", "defconfig")
# An own-image node of two, the one its seed and fingerprint are written for.
AMP_OWN = os.path.join(SRC, "boards", "qemu-arm64", "configs", "amp2", "defconfig")
CMAKE = None

sys.path.insert(0, HERE)
import genconfig  # noqa: E402

scratch = None
runs = 0


def setUpModule():
    global scratch
    scratch = tempfile.mkdtemp(prefix="kickos-genconfig-")


def tearDownModule():
    shutil.rmtree(scratch)


def gen(defconfig, *requests, out=None):
    global runs
    if out is None:
        runs += 1
        out = os.path.join(scratch, "gen%d" % runs)
    run = subprocess.run([sys.executable, GEN, SRC, defconfig, out] + list(requests),
                         capture_output=True, text=True)
    return run, out


def read(path):
    with open(path) as f:
        return f.read()


def fragment(out):
    return read(os.path.join(out, "kickos_config.cmake")).splitlines()


def oracle(kconf, config):
    """The fragment line each KICKOS_* symbol is owed, by name."""
    kconf.load_config(config)
    want = {}
    for sym in kconf.unique_defined_syms:
        if not sym.name.startswith("KICKOS_"):
            continue
        got = sym.str_value
        if sym.type == kconfiglib.STRING:
            for raw in ("\\", '"', "$", ";"):
                got = got.replace(raw, "\\" + raw)
            shown = '"' + got + '"'
        elif sym.type in (kconfiglib.INT, kconfiglib.HEX):
            # Unmet dependencies leave it empty, and CMake has no #ifndef fallback for that.
            shown = str(int(got, 0)) if got else "0"
        else:
            shown = "ON" if got == "y" else "OFF"
        want[sym.name] = "set(%s %s)" % (sym.name, shown)
    return want


def header_oracle(kconf, config):
    """The board_config.h define each KICKOS_* symbol is owed, by name: a bool as 0 or 1 even
    off its dependencies, a number as kconfiglib spells it and only where it resolved."""
    kconf.load_config(config)
    want = {}
    for sym in kconf.unique_defined_syms:
        if not sym.name.startswith("KICKOS_"):
            continue
        got = sym.str_value
        if sym.type == kconfiglib.BOOL:
            want[sym.name] = "#define %s %d" % (sym.name, got == "y")
        elif sym.type in (kconfiglib.INT, kconfiglib.HEX) and got:
            want[sym.name] = "#define %s %s" % (sym.name, got)
    return want


def header_defines(out):
    path = os.path.join(out, "include/kickos/board_config.h")
    return {l.split()[1]: l for l in read(path).splitlines()
            if l.startswith("#define KICKOS_") and l.split()[1] != "KICKOS_BOARD_CONFIG_H"}


class Genconfig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # A run that names no board measures the fleet defaults and agrees with itself.
        assert "CONFIG_BOARD_XMC4800_RELAX=y" in read(FIX).splitlines(), FIX
        run, cls.fix = gen(FIX)
        assert run.returncode == 0, run.stderr
        cls.cwd = os.getcwd()
        os.chdir(SRC)
        cls.kconf = kconfiglib.Kconfig("Kconfig", warn=False)

    @classmethod
    def tearDownClass(cls):
        os.chdir(cls.cwd)

    def accepted(self, defconfig, *requests):
        run, out = gen(defconfig, *requests)
        self.assertEqual(run.returncode, 0, run.stderr)
        return fragment(out)

    def refused(self, defconfig, request, *phrases):
        run, _ = gen(defconfig, request)
        self.assertNotEqual(run.returncode, 0, "%s was accepted" % request)
        self.assertIn("REFUSED", run.stderr)
        for phrase in phrases:
            self.assertIn(phrase, run.stderr)
        return run.stderr

    def test_artifacts_and_defaults(self):
        for artifact in (".config", "include/kickos/board_config.h", "kickos_config.cmake"):
            self.assertGreater(os.path.getsize(os.path.join(self.fix, artifact)), 0, artifact)
        # The armv7m stack floor is stated in no defconfig: the board reaches it through its chip.
        stanza = re.search(r"^config KICKOS_MIN_STACK_SIZE\s*\n(.*?)(?=^config |\Z)",
                           read("Kconfig"), re.M | re.S).group(1)
        floors = re.findall(r"^\s*default (\S+) if ARCH_ARMV7M\s*$", stanza, re.M)
        self.assertEqual(len(floors), 1, "KICKOS_MIN_STACK_SIZE has no one armv7m default")
        header = read(os.path.join(self.fix, "include/kickos/board_config.h")).splitlines()
        # The NVIC line count is a chip constant in the chip's own chip_limits.h.
        self.assertFalse([l for l in header if l.startswith("#define KICKOS_MAX_IRQ ")])
        for line in ("#define KICKOS_MAX_THREADS 8", "#define KICKOS_MIN_STACK_SIZE " + floors[0],
                     "#define KICKOS_BOARD_CONFIG_H"):
            self.assertIn(line, header)
        frag = fragment(self.fix)
        for line in ('set(KICKOS_BOARD "xmc4800-relax")', 'set(KICKOS_ARCH "armv7m")',
                     'set(KICKOS_ARCH_FAMILY "arm")', 'set(KICKOS_CHIP "xmc4800")',
                     'set(KICKOS_CONSOLE "both")', 'set(KICKOS_TELEMETRY "off")',
                     "set(KICKOS_MIN_STACK_SIZE %s)" % floors[0], "set(KICKOS_HAVE_MPU 1)"):
            self.assertIn(line, frag)
        sources = [l for l in frag if l.startswith("set(KICKOS_KCONFIG_SOURCES ")]
        self.assertEqual(len(sources), 1, "the fragment does not report what it read")
        read_back = sources[0][len('set(KICKOS_KCONFIG_SOURCES "'):-len('")')].split(";")
        for path in ("Kconfig", "boards/Kconfig", "arch/Kconfig"):
            self.assertIn(os.path.join(SRC, path), read_back)
        self.assertIn(FIX, read_back)

    def test_refusals(self):
        self.refused(FIX, "CONFIG_KICKOS_MAX_THREADS=999",
                     "outside the declared range [2, 64]: resolved to '16'")
        self.refused(FIX, "CONFIG_KICKOS_MAX_HANDLES=12", "no such symbol")
        # No symbol sets the interrupt-line count, so a request for one is refused outright.
        self.refused(FIX, "CONFIG_KICKOS_MAX_IRQ=64", "no such symbol")
        # The refusal carries the symbol's own help, read out of Kconfig.
        self.refused(FIX, "CONFIG_KICKOS_ISOLATED_CORES=0x1",
                     "unmet dependency: KICKOS_MULTICORE_SHARED",
                     "A bitmask of cores subtracted from the DEFAULT core set")
        self.refused(FIX, "KICKOS_MAX_THREADS=4", "not of the form CONFIG_<name>=<value>")

    def test_accepted_overrides(self):
        self.assertIn("set(KICKOS_HAVE_MPU 0)", self.accepted(FIX, "CONFIG_MEMORY_MODEL_FLAT=y"))
        self.accepted(FIX, "CONFIG_TELEMETRY_RTT=y")
        # A string knob round-trips too: one the translation omits, the fragment overwrites.
        self.assertIn('set(KICKOS_AMP_PORTS "0:2,1:0")',
                      self.accepted(AMP, 'CONFIG_KICKOS_AMP_PORTS="0:2,1:0"'))

    def test_string_reaches_cmake_inert(self):
        # A raw ';' splits a CMake list and a raw '$' expands even inside a quoted set(). No
        # request carries either past the partition's refusals, so the writer is asked directly.
        run, out = gen(AMP)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.kconf.load_config(os.path.join(out, ".config"))
        self.kconf.syms["KICKOS_AMP_PORTS"].set_value("0:2;two$LEAK_ME")
        genconfig.write_cmake_fragment(os.path.join(out, "kickos_config.cmake"), self.kconf, [], FIX)
        self.assertIn('set(KICKOS_AMP_PORTS "0:2\\;two\\$LEAK_ME")', fragment(out))
        verify = os.path.join(out, "verify.cmake")
        with open(verify, "w") as f:
            f.write('set(LEAK_ME "PWNED")\ninclude("%s/kickos_config.cmake")\n'
                    'list(LENGTH KICKOS_AMP_PORTS _len)\nlist(GET KICKOS_AMP_PORTS 0 _elem)\n'
                    'message("${_len}|${_elem}")\n'
                    % out)
        got = subprocess.run([CMAKE, "-P", verify], capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertEqual(got.stderr.strip(), "1|0:2;two$LEAK_ME")

    def test_cmake_escape(self):
        # A quote or backslash cannot reach the fragment from a defconfig unescaped by kconfiglib
        # itself, so the function is asked directly. '@' stays: the fragment is include()'d.
        for raw, want in (("plain", "plain"), ('a"b', 'a\\"b'), ("a;b", "a\\;b"),
                          ("a$b", "a\\$b"), ("a\\b", "a\\\\b"), ("a@b", "a@b")):
            self.assertEqual(genconfig.cmake_escape(raw), want)

    def test_stale_directory(self):
        gone = "CONFIG_KICKOS_GONE_SYMBOL=y"
        repair = "delete that .config to reload from"
        run, out = gen(FIX)
        self.assertEqual(run.returncode, 0, run.stderr)
        with open(os.path.join(out, ".config"), "a") as f:
            f.write(gone + "\n")
        run, _ = gen(FIX, out=out)
        self.assertNotEqual(run.returncode, 0, "a live .config naming no symbol was accepted")
        self.assertIn("REFUSED %s: no such symbol" % gone, run.stderr)
        self.assertIn(repair, run.stderr)
        # The same symbol from a defconfig is an edit to fix, not a stale build directory.
        dc = os.path.join(scratch, "gone-defconfig")
        with open(dc, "w") as f:
            f.write(read(FIX) + gone + "\n")
        run, _ = gen(dc)
        self.assertNotEqual(run.returncode, 0, "a defconfig naming no symbol was accepted")
        self.assertIn("REFUSED %s: no such symbol" % gone, run.stderr)
        self.assertNotIn(repair, run.stderr)

    def test_every_symbol_reaches_cmake(self):
        # Every variant: which symbols a board offers is what its own selects decide.
        defconfigs = sorted(glob.glob(os.path.join(SRC, "boards/*/configs/*/defconfig")))
        self.assertTrue(defconfigs)
        for dc in defconfigs:
            with self.subTest(defconfig=os.path.relpath(dc, SRC)):
                run, out = gen(dc)
                self.assertEqual(run.returncode, 0, run.stderr)
                header = os.path.join(out, "include/kickos/board_config.h")
                self.assertGreater(os.path.getsize(header), 0)
                want = oracle(self.kconf, os.path.join(out, ".config"))
                self.assertIn("KICKOS_BOARD", want)
                frag = set(fragment(out))
                self.assertEqual({n: l for n, l in want.items() if l not in frag}, {})
                owed = header_oracle(self.kconf, os.path.join(out, ".config"))
                self.assertIn("KICKOS_DEBUG", owed)
                self.assertEqual(header_defines(out), owed)

    def test_every_prompted_symbol_offered(self):
        # Each prompted non-choice symbol offered its own resolved value: a type derived wrongly
        # makes the request malformed and the read-back refuses it. An empty value is not offered.
        self.kconf.load_config(os.path.join(self.fix, ".config"))
        offers = []
        for sym in self.kconf.unique_defined_syms:
            if (not sym.name.startswith("KICKOS_") or sym.choice is not None
                    or not any(node.prompt for node in sym.nodes) or sym.str_value == ""):
                continue
            value = sym.str_value
            if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
                value = "ON" if value == "y" else "OFF"
            offers.append("offer:%s=%s" % (sym.name, value))
        self.assertIn("offer:KICKOS_MAX_THREADS=8", offers)
        self.assertGreater(len(offers), 10)
        self.accepted(FIX, *offers)

    def test_requests_by_name(self):
        # A promptless symbol is derived; the identity strings are, so no -D moves the board.
        self.refused(FIX, "assert:KICKOS_DOORBELL_CORES=1",
                     "REFUSED -DKICKOS_DOORBELL_CORES=1: no prompt")
        self.refused(FIX, "assert:KICKOS_CHIP=nosuchchip",
                     "REFUSED -DKICKOS_CHIP=nosuchchip: no prompt")
        self.refused(FIX, "assert:KICKOS_CORES_FOUR=ON",
                     "REFUSED -DKICKOS_CORES_FOUR=ON: a member of a choice")
        # The residual: a name Kconfig does not declare is dropped, KICKOS_BUILD_TESTS and its
        # kind sharing the namespace from CMake alone.
        frag = self.accepted(FIX, "assert:KICKOS_NOSUCHKNOB=1")
        self.assertFalse([l for l in frag if "KICKOS_NOSUCHKNOB" in l])

    def test_choice_value(self):
        # A -D of the console or telemetry by value resolves as the menu choosing its member does.
        for request, member in (("assert:KICKOS_CONSOLE=rtt", "CONFIG_CONSOLE_RTT=y"),
                                ("assert:KICKOS_CONSOLE=RTT", "CONFIG_CONSOLE_RTT=y"),
                                ("offer:KICKOS_CONSOLE=chip", "CONFIG_CONSOLE_CHIP=y"),
                                ("assert:KICKOS_TELEMETRY=rtt", "CONFIG_TELEMETRY_RTT=y"),
                                ("assert:KICKOS_TELEMETRY=off", "CONFIG_TELEMETRY_OFF=y")):
            run, by_value = gen(FIX, request)
            self.assertEqual(run.returncode, 0, run.stderr)
            run, by_member = gen(FIX, member)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(read(os.path.join(by_value, ".config")),
                             read(os.path.join(by_member, ".config")), request)
        self.assertIn('set(KICKOS_CONSOLE "rtt")', self.accepted(FIX, "offer:KICKOS_CONSOLE=Rtt"))
        self.refused(FIX, "assert:KICKOS_CONSOLE=nosuch",
                     "REFUSED -DKICKOS_CONSOLE=nosuch: not one of both, chip, none, rtt")
        self.refused(FIX, "offer:KICKOS_CONSOLE=nosuch", "not one of both, chip, none, rtt")
        run, _ = gen(FIX, "assert:KICKOS_CONSOLE=rtt", "CONFIG_CONSOLE_RTT=y")
        self.assertNotEqual(run.returncode, 0, "one member requested twice was accepted")
        self.assertIn("is requested twice", run.stderr)
        run, _ = gen(FIX, "assert:KICKOS_CONSOLE=chip", "assert:KICKOS_TELEMETRY=rtt")
        self.assertNotEqual(run.returncode, 0, "telemetry over RTT without an RTT console was accepted")
        self.assertIn("REFUSED CONFIG_TELEMETRY_RTT=y: unmet dependency", run.stderr)

    def test_shared_kernel_predicate(self):
        smp = os.path.join(SRC, "boards", "qemu-arm64", "configs", "smp", "defconfig")
        self.assertIn("set(KICKOS_MULTICORE_MODEL_SHARED 1)", self.accepted(smp))
        # A part meeting none of the six still takes the count under AMP.
        amp = os.path.join(SRC, "boards", "pizero2350", "configs", "amp", "defconfig")
        frag = self.accepted(amp)
        self.assertIn("set(KICKOS_NUM_CORES 2)", frag)
        self.assertIn("set(KICKOS_MULTICORE_MODEL_SHARED 0)", frag)
        self.refused(amp, "CONFIG_KICKOS_MULTICORE_SHARED=y", "unmet dependency", "ARCH_SMP_COHERENT",
                     "CHIP_SMP_TARGETING", "!ARCH_HAS_IPC_FASTPATH")
        # Two cores and no model on a part that cannot take the shared default.
        base = os.path.join(SRC, "boards", "pizero2350", "configs", "base", "defconfig")
        self.refused(base, "CONFIG_KICKOS_CORES_TWO=y", "its default KICKOS_MULTICORE_SHARED is unavailable",
                     "State one of KICKOS_MULTICORE_AMP")

    def test_smp_properties_selected_by_their_owner(self):
        # An arch stanza selects the ISA's three and a chip stanza the part's three, each all of
        # its three or none.
        owners = {"ARCH_SMP_": "ARCH_", "CHIP_SMP_": "CHIP_"}
        selected = {}
        for sym in self.kconf.unique_defined_syms:
            for target, _cond in sym.selects:
                for prop, owner in owners.items():
                    if target.name.startswith(prop):
                        self.assertTrue(sym.name.startswith(owner) and not sym.name.startswith(prop),
                                        "%s selects %s" % (sym.name, target.name))
                        selected.setdefault(sym.name, set()).add(target.name)
        self.assertIn("CHIP_ESP32", selected)
        self.assertIn("ARCH_LX6", selected)
        for name, props in selected.items():
            prefix = name.split("_")[0] + "_SMP_"
            every = {s.name for s in self.kconf.unique_defined_syms if s.name.startswith(prefix)}
            self.assertEqual(props, every, name)

    def test_amp_partition_facts(self):
        frag = fragment(self.fix)
        for line in ("set(KICKOS_AMP_SELF_NODE 0)", "set(KICKOS_AMP_PORT_COUNT 0)",
                     'set(KICKOS_AMP_PORT_NODE_INIT "")', 'set(KICKOS_AMP_PORT_PORT_INIT "")'):
            self.assertIn(line, frag)
        self.assertFalse([l for l in frag if "KICKOS_AMP_SELF_CORE" in l or "_FP_" in l])
        # The shared image seats for node 0, whatever node an entry names.
        run, out = gen(AMP)
        self.assertEqual(run.returncode, 0, run.stderr)
        for line in ("set(KICKOS_AMP_SELF_NODE 0)", "set(KICKOS_AMP_PORT_COUNT 3)",
                     "set(KICKOS_AMP_PORT_LOCAL 1)", 'set(KICKOS_AMP_PORT_NODE_INIT "0,1,1")',
                     'set(KICKOS_AMP_PORT_PORT_INIT "2,0,3")'):
            self.assertIn(line, fragment(out))
        self.assertFalse(os.path.exists(os.path.join(out, "amp-peer-seed.cmake")))
        frag = self.accepted(AMP_OWN, "assert:KICKOS_AMP_NODE_ID=1")
        for line in ("set(KICKOS_AMP_SELF_CORE 1)", "set(KICKOS_AMP_SELF_NODE 1)",
                     "set(KICKOS_AMP_PORT_COUNT 3)", "set(KICKOS_AMP_PORT_LOCAL 2)",
                     'set(KICKOS_AMP_PORT_NODE_INIT "0,1,1")', 'set(KICKOS_AMP_PORT_PORT_INIT "2,3,4")'):
            self.assertIn(line, frag)

    def test_amp_partition_refusals(self):
        for requests, phrase in (
                (["assert:KICKOS_AMP_SHARED_SIZE=0"], "KICKOS_AMP_SHARED_SIZE is unstated"),
                (["assert:KICKOS_AMP_PARTITION_BASE=0x0"], "KICKOS_AMP_PARTITION_BASE is unstated"),
                (["assert:KICKOS_AMP_TEXT_BASE=0x10000000"], "describe one aperture"),
                (["assert:KICKOS_AMP_TEXT_SHARE=0x100000"], "describe one aperture"),
                (["assert:KICKOS_AMP_NODE_ID=2"], "KICKOS_AMP_NODE_ID=2: the partition holds 2 node(s)"),
                (["assert:KICKOS_AMP_NODE_CORES=0"], "it names 1 core(s) for a partition of 2 node(s)"),
                (["assert:KICKOS_AMP_NODE_CORES=0,x"], "entry 'x' is not a core index"),
                (["assert:KICKOS_AMP_NODE_CORES=0,2"], "node 1 is on core 2 and the partition spans 2"),
                (["assert:KICKOS_AMP_NODE_CORES=0,0"], "two nodes are on core 0"),
                (["assert:KICKOS_AMP_NODE_CORES=1,0"], "node 0 is on core 1"),
                (["assert:KICKOS_AMP_PORTS="], "names no crossing"),
                (["assert:KICKOS_AMP_PORTS=0:2,x"], "entry 'x' is not a node:port pair"),
                (["assert:KICKOS_AMP_PORTS=0:2,2:3"], "names node 2 and the partition holds 2"),
                (["assert:KICKOS_AMP_PORTS=0:2,1:1"], "names port 1"),
                (["assert:KICKOS_AMP_PORTS=0:0,1:3"], "names port 0 for this image's own node"),
                (["assert:KICKOS_AMP_PORTS=0:2,1:32"], "31 is its last port"),
                (["assert:KICKOS_AMP_PORTS=0:2,0:02"], "node 0 port 2 is named twice"),
                (["assert:KICKOS_MAX_ENDPOINTS=3"], "names 3 crossing(s) and the board provisions 3")):
            with self.subTest(requests=requests):
                run, _ = gen(AMP_OWN, *requests)
                self.assertNotEqual(run.returncode, 0, "%s was accepted" % requests)
                self.assertIn("REFUSED", run.stderr)
                self.assertIn(phrase, run.stderr)
        # Port 0 is the echo of a node that runs no kernel, so a peer may serve it.
        self.accepted(AMP_OWN, "assert:KICKOS_AMP_PORTS=0:2,1:0")
        self.accepted(AMP_OWN, "assert:KICKOS_AMP_TEXT_BASE=0x10000000", "assert:KICKOS_AMP_TEXT_SHARE=0x100000")

    def test_amp_fingerprint_leaves_the_node_out(self):
        def fp(*requests):
            run, out = gen(AMP_OWN, *requests)
            self.assertEqual(run.returncode, 0, run.stderr)
            return [l for l in fragment(out) if l.startswith("set(KICKOS_AMP_CONFIG_FP_")]
        node0 = fp()
        self.assertEqual(len(node0), 2)
        self.assertEqual(node0, fp("assert:KICKOS_AMP_NODE_ID=1"))
        self.assertNotEqual(node0, fp("assert:KICKOS_MAX_THREADS=12"))

    def test_amp_seed_configures_a_peer_alike(self):
        run, out = gen(AMP_OWN, "assert:KICKOS_AMP_NODE_ID=1", "CONFIG_KICKOS_NEWLIB_NANO=n")
        self.assertEqual(run.returncode, 0, run.stderr)
        seed = read(os.path.join(out, "amp-peer-seed.cmake")).splitlines()
        self.assertEqual(seed[0], "# Written by node 1's configure. Read by tools/amp/build-partition.sh.")
        for line in ('set(KICKOS_AMP_PORTS "0:2,1:3,1:4" CACHE STRING "" FORCE)',
                     'set(KICKOS_CONSOLE "chip" CACHE STRING "" FORCE)',
                     'set(KICKOS_TELEMETRY "off" CACHE STRING "" FORCE)',
                     'set(KICKOS_AMP_USER_SHARE_UNCACHED ON CACHE BOOL "" FORCE)',
                     'set(KICKOS_MULTI_INSTANCE OFF CACHE BOOL "" FORCE)'):
            self.assertIn(line, seed)
        # Each node names these itself: build-partition.sh the board and the node, the build the
        # newlib flavour.
        for name in ("KICKOS_AMP_NODE_ID", "KICKOS_BOARD", "KICKOS_NEWLIB_NANO"):
            self.assertFalse([l for l in seed if l.startswith("set(%s " % name)], name)
        # Node 0 configured from the seed, as build-partition.sh configures a peer, resolves alike.
        offers = []
        for line in seed[1:]:
            name, value = re.fullmatch(r'set\((\w+) "?(.*?)"? CACHE \w+ "" FORCE\)', line).groups()
            offers.append("offer:%s=%s" % (name, value))
        run, peer = gen(AMP_OWN, "assert:KICKOS_AMP_NODE_ID=0", "CONFIG_KICKOS_NEWLIB_NANO=n", *offers)
        self.assertEqual(run.returncode, 0, run.stderr)
        run, node0 = gen(AMP_OWN, "assert:KICKOS_AMP_NODE_ID=0", "CONFIG_KICKOS_NEWLIB_NANO=n")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(read(os.path.join(peer, ".config")), read(os.path.join(node0, ".config")))

    def test_flag_both_ways(self):
        self.assertIn("set(KICKOS_TLS ON)", fragment(self.fix))
        self.assertEqual(header_defines(self.fix)["KICKOS_TLS"], "#define KICKOS_TLS 1")
        run, out = gen(FIX, "offer:KICKOS_TLS=OFF")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("set(KICKOS_TLS OFF)", fragment(out))
        self.assertEqual(header_defines(out)["KICKOS_TLS"], "#define KICKOS_TLS 0")
        self.refused(FIX, "offer:KICKOS_MAX_THREADS=999", "outside the declared range")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.stderr.write("usage: test_genconfig.py <cmake>\n")
        sys.exit(2)
    CMAKE = sys.argv[1]
    unittest.main(argv=sys.argv[:1])
