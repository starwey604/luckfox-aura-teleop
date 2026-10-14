#!/usr/bin/env python3
"""Check IRQ timing interpretation and reject unsafe recording combinations."""
from pathlib import Path
import runpy
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
tool = runpy.run_path(str(ROOT / 'tools/test-codec-latency'))


class CodecToolsTest(unittest.TestCase):
    def test_wall_and_hardware_counter_are_kept_separate(self):
        log = ('6,12,555,-;kmpp: mpp_time_diff_with_hw_time:2514: '
               '21f40000.rkvenc:0 session 900:5 time: 2600 us hw 2500 us\r\n'
               '6,13,556,-;kmpp: mpp_time_diff_with_hw_time:2514: '
               '21f40000.rkvenc:0 session 900:6 time: 3000 us hw 2700 us\r\n'
               '6,14,557,-;kmpp: 21f40000.rkvenc:0 session 900:5 time: 4000 us\n'
               '6,15,558,-;other: 21f50000.rkvdec:0 session 900:5 time: 999 us hw 999 us\n')
        wall, hardware = tool['encoder_samples'](log)
        self.assertEqual(wall, {'900:5': [2.6], '900:6': [3.0]})
        self.assertEqual(hardware, {'900:5': [2.5], '900:6': [2.7]})

    def test_extra_completed_tasks_are_not_discarded(self):
        log = '\n'.join('21f40000.rkvenc:0 session 9:5 time: 2600 us hw 2500 us'
                        for _ in range(121))
        wall, hardware = tool['encoder_samples'](log)
        self.assertEqual(len(wall['9:5']), 121)
        self.assertEqual(tool['distribution'](hardware['9:5'])['samples'], 121)

    def test_percentiles_are_sample_ranks_not_histogram_bins(self):
        data = tool['distribution']([100., 1., 2., 3.])
        self.assertEqual(data['mean'], 26.5)
        self.assertEqual(data['p50'], 2.)
        self.assertEqual(data['p99'], 100.)

    def test_recording_cannot_silently_change_minimal_or_single_camera(self):
        for flags in (['--camera', '0'], ['--camera', 'dual', '--minimal']):
            result = subprocess.run([str(ROOT / 'tools/test-aura-software-baseline'),
                                     *flags, '--record-h264', '--output', '/unused'],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2)
            self.assertIn('H.264 recording requires non-minimal dual launcher', result.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)
