# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What tools/image-link.py --embed states to the kernel's link from the programs it embeds
# (kernel_exports): a copy or an alias name takes image 0's value, whatever the programs after it
# state, a least or most name the bound over every program, and a check name one assert per
# program that states it.

import importlib.util
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', '..', 'tools', 'image-link.py')
EMBED = os.path.join(HERE, '..', '..', 'cmake', 'embed_names.txt')

spec = importlib.util.spec_from_file_location('image_link', TOOL)
image_link = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image_link)

# The init's layout and a task's, every bound of one apart from the other's.
INIT = {
    'kickos_image_header': 0x40000100,
    '__kickos_app_rom_start': 0x40000000,
    '__kickos_app_rom_end': 0x40008000,
    '__kickos_app_sram_start': 0x40200000,
    '__kickos_app_sram_end': 0x40200400,
    '__kickos_appbss_start': 0x402001a0,
    '__kickos_appbss_end': 0x402003f0,
    '_kickos_heap_start': 0x402003f0,
    '_kickos_heap_limit': 0x40200400,
    '__kickos_tls_segment_start': 0x40108010,
    '__kickos_tls_segment_end': 0x40108010,
    '__kickos_tdata_start': 0x40108010,
    '__kickos_tdata_end': 0x40108010,
    '__kickos_tbss_start': 0x40108010,
    '__kickos_tbss_end': 0x40108010,
    '_sdata': 0x40400000,
}
TASK = {
    'kickos_image_header': 0x40000200,
    '__kickos_app_rom_start': 0x40000000,
    '__kickos_app_rom_end': 0x40010000,
    '__kickos_app_sram_start': 0x40200000,
    '__kickos_app_sram_end': 0x40201000,
    '__kickos_appbss_start': 0x40200a10,
    '__kickos_appbss_end': 0x40200fe8,
    '_kickos_heap_start': 0x40200fe8,
    '_kickos_heap_limit': 0x40201000,
    '__kickos_tls_segment_start': 0x401075c0,
    '__kickos_tls_segment_end': 0x401075c8,
    '__kickos_tdata_start': 0x401075c0,
    '__kickos_tdata_end': 0x401075c4,
    '__kickos_tbss_start': 0x401075c4,
    '__kickos_tbss_end': 0x401075c8,
    '_sdata': 0x40400000,
}


def program(values):
    return ('program', 'raw', dict(values), [], ())


def stated(lines):
    out = {}
    for line in lines:
        name, _, value = line.rstrip(';').partition(' = ')
        out[name] = value
    return out


class KernelExports(unittest.TestCase):
    def exports(self, programs, pe=False, base=0):
        rows = image_link.embed_names(EMBED)
        lines, asserts, names = image_link.kernel_exports(rows, programs, pe, base)
        return stated(lines), asserts, names

    def test_a_copied_bound_is_image_zero_s_whatever_follows(self):
        got, _, names = self.exports([program(INIT), program(TASK), program(TASK)])
        for name in ('__kickos_appbss_start', '__kickos_appbss_end', '_kickos_heap_start',
                     '_kickos_heap_limit', '__kickos_tls_segment_start',
                     '__kickos_tls_segment_end'):
            self.assertEqual(got[name], '0x%x' % INIT[name], name)
            self.assertIn(name, names)
        self.assertEqual(got['__kickos_image0_header'], '0x%x' % INIT['kickos_image_header'])

    def test_an_aliased_bound_is_image_zero_s(self):
        got, _, _ = self.exports([program(INIT), program(TASK)])
        self.assertEqual(got['__kickos_tbss_end'],
                         '0x%x + __kickos_image_kernel_alias' % INIT['__kickos_tbss_end'])
        got, _, _ = self.exports([program(INIT), program(TASK)], pe=True, base=0x40000000)
        self.assertEqual(got['__kickos_tdata_start'], '__kickos_image0_load_start + 0x%x'
                         % (INIT['__kickos_tdata_start'] - 0x40000000))

    def test_a_least_or_most_bound_spans_every_program(self):
        got, _, _ = self.exports([program(INIT), program(TASK)])
        self.assertEqual(got['__kickos_app_rom_end'], '0x%x' % TASK['__kickos_app_rom_end'])
        self.assertEqual(got['__kickos_app_sram_end'], '0x%x' % TASK['__kickos_app_sram_end'])
        self.assertEqual(got['__kickos_app_sram_start'], '0x%x' % INIT['__kickos_app_sram_start'])

    def test_a_checked_word_is_asserted_once_per_program(self):
        _, asserts, _ = self.exports([program(INIT), program(TASK)])
        self.assertEqual(len([a for a in asserts if a.startswith('ASSERT(_sdata == ')]), 2)


if __name__ == '__main__':
    unittest.main()
