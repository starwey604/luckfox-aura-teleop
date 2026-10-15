#!/usr/bin/env python3
"""Validate bitstream evidence and one-factor parameter cases without hardware."""
from pathlib import Path
import runpy
import unittest

tool = runpy.run_path(str(Path(__file__).resolve().parents[2] / 'tools/test-aura-codec-parameters'))


class CodecParametersTest(unittest.TestCase):
    def test_trace_header_values_are_not_bit_positions(self):
        text = ('[trace_headers @ 0x1] 8 profile_idc 01000010 = 66\n'
                '[trace_headers @ 0x1] 10 entropy_coding_mode_flag 0 = 0\n'
                '[trace_headers @ 0x1] 11 disable_deblocking_filter_idc 010 = 1\n')
        values = tool['header_values'](text)
        self.assertEqual(values['profile_idc'], [66])
        self.assertEqual(values['entropy_coding_mode_flag'], [0])
        self.assertEqual(values['disable_deblocking_filter_idc'], [1])
        tool['validate_headers']('baseline-profile', values, [])

    def test_silent_hardware_fallback_is_rejected(self):
        values = {'profile_idc': [100], 'entropy_coding_mode_flag': [1],
                  'transform_8x8_mode_flag': [1], 'disable_deblocking_filter_idc': [0]}
        for case in ('cavlc', 'baseline-profile', '4x4', 'deblock-off'):
            with self.assertRaises(RuntimeError): tool['validate_headers'](case, values, [])
        with self.assertRaises(RuntimeError):
            tool['validate_headers']('gop1', values, [{'flags': 'K_'}, {'flags': '__'}])

    def test_wire_budget_includes_each_datagram(self):
        self.assertEqual(tool['wire_bytes'](1160), 1266)
        self.assertEqual(tool['wire_bytes'](1161), 1373)

    def test_partial_or_ignored_slice_configuration_is_rejected(self):
        line = '[trace_headers @ 0x1] 8 first_mb_in_slice 1 = {}\n'
        text = ''.join(line.format(n) for n in range(0, 3600, 480))
        self.assertEqual(tool['validate_slice_positions'](text * 2, 480, 2), 8)
        with self.assertRaises(RuntimeError): tool['validate_slice_positions'](text, 480, 2)
        with self.assertRaises(RuntimeError): tool['validate_slice_positions'](text, 0, 1)

    def test_gop_and_quality_first_change_one_factor(self):
        self.assertEqual(tool['CASES']['baseline'], [])
        self.assertEqual(tool['CASES']['gop1'], ['--gop', '1'])
        self.assertNotIn('--gop', tool['CASES']['fixqp26'])
        self.assertNotIn('--rc-mode', tool['CASES']['cbr60'])


if __name__ == '__main__': unittest.main(verbosity=2)
