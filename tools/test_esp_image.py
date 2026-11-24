# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/esp-image.py over ELFs written here, with an nm that prints the symbols the test names:
# the flash offset it prints, the segment shapes it refuses before esptool runs, the digest it
# appends to a RAM-only-header image, and tools/flash-esptool.sh writing the esp32 image at that
# offset.

import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "esp-image.py")
FLASHER = os.path.join(HERE, "flash-esptool.sh")
PT_LOAD = 1
START = 0x400D0000
END = 0x400E0000


def elf(segments, payload=b""):
    """A 32-bit little-endian ELF of program headers (vaddr, filesz, memsz), or (vaddr, paddr,
    filesz, memsz), no sections, and `payload` after the headers, where every segment's file
    bytes start."""
    head = bytearray(52)
    head[0:4] = b"\x7fELF"
    head[4] = 1
    head[5] = 1
    struct.pack_into("<I", head, 28, 52)
    struct.pack_into("<HH", head, 42, 32, len(segments))
    at = 52 + 32 * len(segments)
    body = b""
    for row in segments:
        if len(row) == 3:
            row = (row[0], row[0], row[1], row[2])
        vaddr, paddr, filesz, memsz = row
        body += struct.pack("<IIIIIIII", PT_LOAD, at, vaddr, paddr, filesz, memsz, 0, 1)
    return bytes(head) + body + payload


class Fixture(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.nm = os.path.join(self.dir.name, "nm")
        self.elf = os.path.join(self.dir.name, "image")

    def symbols(self, text):
        with open(self.nm, "w") as f:
            f.write("#!/bin/sh\ncat <<'EOF2'\n%sEOF2\n" % text)
        os.chmod(self.nm, 0o755)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True)


class Offset(Fixture):
    def test_the_symbol_is_the_answer(self):
        self.symbols("00002000 A __kickos_esp32_flash_image_offset\n")
        result = self.run_tool("--offset", self.nm, self.elf)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "0x2000"), result.stderr)

    def test_an_image_without_it_is_refused(self):
        self.symbols("00002000 A __kickos_esp32_flash_text_load\n")
        result = self.run_tool("--offset", self.nm, self.elf)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("names no __kickos_esp32_flash_image_offset", result.stderr)

    def test_the_flasher_writes_at_it(self):
        self.symbols("00002000 A __kickos_esp32_flash_image_offset\n")
        bindir = os.path.join(self.dir.name, "bin")
        os.mkdir(bindir)
        os.symlink(self.nm, os.path.join(bindir, "xtensa-esp32-elf-nm"))
        stub = os.path.join(bindir, "esptool")
        with open(stub, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(stub, 0o755)
        for name in (self.elf, self.elf + ".app.bin"):
            open(name, "w").close()
        env = dict(os.environ, DRY_RUN="1", FLASH_IMAGE=self.elf, FLASH_PORT="",
                   PATH=bindir + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["bash", FLASHER, "esp32-wroom"], capture_output=True, text=True,
                                env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("write_flash 0x2000 ", result.stderr)


class Segments(Fixture):
    def refused(self, segments, text, window=""):
        self.symbols("%08x A __kickos_esp32_flash_text_start\n%08x A __kickos_esp32_flash_text_end\n"
                     "00070000 A __kickos_esp32_flash_text_load\n"
                     "00001000 A __kickos_esp32_flash_image_offset\n%s" % (START, END, window))
        with open(self.elf, "wb") as f:
            f.write(elf(segments))
        result = self.run_tool("esptool-is-not-reached", "esp32", self.nm, self.elf,
                               os.path.join(self.dir.name, "out"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(text, result.stderr)

    def test_a_segment_starting_below_and_reaching_in(self):
        self.refused([(START - 0x100, 0x200, 0x200)], "starts below the flash text's start")

    def test_a_segment_in_the_span_with_bytes_that_have_no_file_image(self):
        self.refused([(START, 0x100, 0x200)], "has 256 bytes with no file image")

    def test_a_segment_past_the_end(self):
        self.refused([(END - 0x100, 0x200, 0x200)], "runs past the flash text's end")


# The ESP32-C6 shape: the cache window spans 0x4200_0000 to 0x4300_0000 and an image's .data
# runs in SRAM and loads at a template its code span reserves.
C6_START = 0x42000000
C6_END = 0x42010000
C6_WINDOW = "42000000 A __kickos_esp32_flash_window_start\n43000000 A __kickos_esp32_flash_window_end\n"
C6_RAM = 0x40820000


class Window(Fixture):
    def symbols_c6(self):
        self.symbols("%08x A __kickos_esp32_flash_text_start\n%08x A __kickos_esp32_flash_text_end\n"
                     "00090000 A __kickos_esp32_flash_text_load\n"
                     "00000000 A __kickos_esp32_flash_image_offset\n%s" % (C6_START, C6_END, C6_WINDOW))

    def test_a_segment_in_the_window_outside_the_text_is_refused(self):
        self.symbols_c6()
        with open(self.elf, "wb") as f:
            f.write(elf([(C6_END + 0x100, 0x100, 0x100)]))
        result = self.run_tool("esptool-is-not-reached", "esp32c6", self.nm, self.elf,
                               os.path.join(self.dir.name, "out"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is in the flash window but outside its text", result.stderr)

    def test_a_segment_in_the_window_with_no_flash_text_is_refused(self):
        self.symbols(C6_WINDOW)
        with open(self.elf, "wb") as f:
            f.write(elf([(C6_RAM, 0x40, 0x40), (C6_START, 0x40, 0x40)]))
        result = self.run_tool("esptool-is-not-reached", "esp32c6", self.nm, self.elf,
                               os.path.join(self.dir.name, "out"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is in the flash window, and no flash text names it", result.stderr)

    def test_a_reservation_no_template_fills_is_refused(self):
        self.symbols_c6()
        with open(self.elf, "wb") as f:
            f.write(elf([(C6_START + 0x100, 0x0, 0x40), (C6_RAM, C6_START + 0x100, 0x20, 0x20)]))
        result = self.run_tool("esptool-is-not-reached", "esp32c6", self.nm, self.elf,
                               os.path.join(self.dir.name, "out"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has 32 bytes with no file image", result.stderr)

    def test_a_template_lands_at_its_load_address(self):
        self.symbols_c6()
        stub = os.path.join(self.dir.name, "esptool")
        with open(stub, "w") as f:
            f.write("#!/bin/sh\nwhile [ \"$1\" != --output ]; do shift; done\nprintf 'BOOT' > \"$2\"\n")
        os.chmod(stub, 0o755)
        payload = bytes(range(0x40))
        with open(self.elf, "wb") as f:
            f.write(elf([(C6_START + 0x100, 0x0, 0x40), (C6_RAM, C6_START + 0x100, 0x40, 0x40)],
                        payload))
        out = os.path.join(self.dir.name, "out")
        result = self.run_tool(stub, "esp32c6", self.nm, self.elf, out)
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(out, "rb") as f:
            image = f.read()
        self.assertEqual(image[:4], b"BOOT")
        self.assertEqual(image[0x90000 + 0x100:0x90000 + 0x140], payload)
        self.assertEqual(len(image), 0x90000 + C6_END - C6_START)


class Symbols(Fixture):
    """A merged AMP partition carries no symbols: --symbols names the ELF that holds them."""

    def test_a_partition_takes_its_flash_text_from_the_symbol_elf(self):
        node0 = os.path.join(self.dir.name, "node0")
        open(node0, "w").close()
        with open(self.nm, "w") as f:
            f.write("#!/bin/sh\ncase \"$1\" in\n*node0)\ncat <<'EOF2'\n"
                    "%08x A __kickos_esp32_flash_text_start\n%08x A __kickos_esp32_flash_text_end\n"
                    "00090000 A __kickos_esp32_flash_text_load\n"
                    "00000000 A __kickos_esp32_flash_image_offset\n%sEOF2\n;;\nesac\n"
                    % (C6_START, C6_END, C6_WINDOW))
        os.chmod(self.nm, 0o755)
        seen = os.path.join(self.dir.name, "seen.elf")
        stub = os.path.join(self.dir.name, "esptool")
        with open(stub, "w") as f:
            f.write("#!/bin/sh\nfor last; do :; done\ncp \"$last\" '%s'\n"
                    "while [ \"$1\" != --output ]; do shift; done\nprintf 'BOOT' > \"$2\"\n" % seen)
        os.chmod(stub, 0o755)
        payload = bytes(range(0x40))
        with open(self.elf, "wb") as f:
            f.write(elf([(C6_RAM, 0x40, 0x40), (C6_START, 0x40, 0x40)], payload))
        out = os.path.join(self.dir.name, "out")
        result = self.run_tool("--symbols", node0, stub, "esp32c6", self.nm, self.elf, out)
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(out, "rb") as f:
            image = f.read()
        self.assertEqual(image[:4], b"BOOT")
        self.assertEqual(image[0x90000:0x90040], payload)
        self.assertEqual(len(image), 0x90000 + C6_END - C6_START)
        with open(seen, "rb") as f:
            headers = f.read()
        types = [struct.unpack_from("<I", headers, 52 + 32 * i)[0] for i in range(2)]
        self.assertEqual(types, [PT_LOAD, 0])


# A stub esptool's output: an image header (magic, one segment) of 24 bytes, one empty segment
# and the checksum's pad, 48 bytes in all; byte 23 is hash_appended.
def boot(flagged=False):
    image = bytearray(48)
    image[0] = 0xE9
    image[1] = 1
    if flagged:
        image[23] = 1
    return bytes(image)


class Digest(Fixture):
    def stub(self, image):
        source = os.path.join(self.dir.name, "boot.bin")
        with open(source, "wb") as f:
            f.write(image)
        stub = os.path.join(self.dir.name, "esptool")
        with open(stub, "w") as f:
            f.write("#!/bin/sh\nwhile [ \"$1\" != --output ]; do shift; done\ncp '%s' \"$2\"\n" % source)
        os.chmod(stub, 0o755)
        return stub

    def plain(self, image, *options):
        self.symbols("")
        with open(self.elf, "wb") as f:
            f.write(elf([]))
        out = os.path.join(self.dir.name, "out")
        result = self.run_tool(self.stub(image), "esp32c6", self.nm, self.elf, out, *options)
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(out, "rb") as f:
            return f.read()

    def flash_text(self, load, image):
        self.symbols("%08x A __kickos_esp32_flash_text_start\n%08x A __kickos_esp32_flash_text_end\n"
                     "%08x A __kickos_esp32_flash_text_load\n"
                     "00000000 A __kickos_esp32_flash_image_offset\n%s" % (C6_START, C6_END, load, C6_WINDOW))
        with open(self.elf, "wb") as f:
            f.write(elf([(C6_START, 0x40, 0x40)], bytes(range(0x40))))
        out = os.path.join(self.dir.name, "out")
        return out, self.run_tool(self.stub(image), "esp32c6", self.nm, self.elf, out, "--ram-only-header")

    def assert_sealed(self, image, length):
        self.assertEqual(image[23], 1)
        self.assertEqual(image[length:length + 32], hashlib.sha256(image[:length]).digest())

    def test_the_plain_path_appends_the_digest_to_a_ram_only_header_image(self):
        image = self.plain(boot(), "--ram-only-header")
        self.assertEqual(len(image), 48 + 32)
        self.assert_sealed(image, 48)

    def test_the_flash_text_path_appends_the_digest_ahead_of_the_text(self):
        out, result = self.flash_text(0x1000, boot())
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(out, "rb") as f:
            image = f.read()
        self.assert_sealed(image, 48)
        self.assertEqual(image[48 + 32:0x1000], b"\xff" * (0x1000 - 48 - 32))
        self.assertEqual(image[0x1000:0x1040], bytes(range(0x40)))

    def test_the_digest_counts_against_the_text_load(self):
        out, result = self.flash_text(48 + 31, boot())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("past its text at 0x4F", result.stderr)
        out, result = self.flash_text(48 + 32, boot())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_an_image_already_flagged_is_left_as_is(self):
        self.assertEqual(self.plain(boot(flagged=True), "--ram-only-header"), boot(flagged=True))

    def test_an_image_with_a_header_of_its_own_is_left_as_is(self):
        self.assertEqual(self.plain(boot()), boot())


if __name__ == "__main__":
    unittest.main()
