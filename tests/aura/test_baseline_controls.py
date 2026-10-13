#!/usr/bin/env python3
"""Execute generated sensor-control shell against fake sysfs; no hardware."""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
controls_script = runpy.run_path(str(ROOT / 'tools/test-aura-software-baseline'))['controls_script']


class SensorControlsTest(unittest.TestCase):
    def run_controls(self, camera, vblank=None, missing=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sysfs = root / 'video nodes'; sysfs.mkdir()
            for index, name in enumerate(['m00_b_ar0234 4-0010', 'm01_b_ar0234 3-0010',
                                          'm02_b_ar0234 5-0010', 'm03_b_mia1321 4-0060']):
                if missing and index == 1: continue
                node = sysfs / f'v4l-subdev{index}'; node.mkdir()
                (node / 'name').write_text(name + '\n')
            bindir = root / 'bin'; bindir.mkdir()
            fake = bindir / 'v4l2-ctl'
            fake.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$CONTROL_LOG"\n')
            fake.chmod(0o755)
            env = dict(os.environ, PATH=str(bindir) + ':' + os.environ['PATH'],
                       CONTROL_LOG=str(root / 'calls'))
            result = subprocess.run(['sh', '-c', controls_script(camera, vblank, sysfs)],
                                    env=env, text=True, capture_output=True)
            calls = (root / 'calls').read_text().splitlines() if (root / 'calls').exists() else []
            return result, calls

    def test_exact_camera_selection(self):
        for camera, nodes in [('0', [0]), ('1', [1]), ('dual', [0, 1])]:
            with self.subTest(camera=camera):
                result, calls = self.run_controls(camera)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(calls), len(nodes))
                for call, node in zip(calls, nodes):
                    self.assertIn(f'-d /dev/v4l-subdev{node} ', call)
                    self.assertIn('--set-ctrl=exposure=735,', call)

    def test_vblank_precedes_exposure(self):
        result, calls = self.run_controls('dual', 80)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 4)
        for index in (0, 2):
            self.assertIn('--set-ctrl=vertical_blanking=80', calls[index])
            self.assertIn('--set-ctrl=exposure=735,', calls[index + 1])

    def test_missing_second_sensor_fails_before_sender(self):
        result, _ = self.run_controls('dual', missing=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('Expected 2 AR0234 sensor nodes, found 1', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
