#!/usr/bin/env python3
"""Exercise the board shell diagnostic with isolated sysfs/media/I2C fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/diagnose-ar0234'


class DiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sys = self.root / 'sys'
        self.dev = self.root / 'dev'
        self.dt = self.root / 'dt'
        self.bin = self.root / 'bin'
        for path in (self.dev, self.dt / 'aliases', self.bin):
            path.mkdir(parents=True)
        adapter = self.sys / 'class/i2c-adapter/i2c-7'
        adapter.mkdir(parents=True)
        controller = self.dt / 'i2c@21c60000'
        controller.mkdir()
        (adapter / 'of_node').symlink_to(controller)
        (adapter / 'name').write_text('rk3x-i2c\n')
        (self.dt / 'aliases/i2c4').write_bytes(b'/i2c@21c60000\0')
        (self.dev / 'i2c-7').touch()
        self.client = self.sys / 'bus/i2c/devices/7-0010'
        self.client.mkdir(parents=True)
        (self.client / 'name').write_text('ar0234\n')
        (self.dev / 'media3').touch()
        (self.dev / 'video24').touch()
        self.calls = self.root / 'calls'
        self.calls.touch()
        self.command('i2cdetect', 'printf "i2c-7 i2c rk3x-i2c\\n"')
        self.command('i2ctransfer', 'printf "i2ctransfer %s\\n" "$*" >> "$CALLS"; printf "%s\\n" "${RESPONSE:-0x0a 0x56}"')
        self.command('dmesg', 'printf "[2.0] ar0234 7-0010 detected\\n"')
        self.command('media-ctl', '''
case "$*" in
 *"-e rkisp_mainpath"*) printf '%s/video24\\n' "$DEV_ROOT" ;;
 *) printf 'driver          rkisp-vir0\\n- entity 1: m00_b_ar0234 7-0010 (1 pad)\\n' ;;
esac''')
        self.command('v4l2-ctl', '''
printf 'v4l2 %s\\n' "$*" >> "$CALLS"
case "$*" in
 *--stream-count=*)
  n=${CAPTURE_COUNT:-3}; i=0
  while [ "$i" -lt "$n" ]; do
   printf 'idx: 0 seq: %s bytesused: 1382400 ts: 1.0\\n' "$i"
   i=$((i+1))
  done ;;
 *--set-fmt-video=*) printf "Width/Height : %s\\nPixel Format : 'NV12'\\n" "${CAPTURE_SIZE:-1280/720}" ;;
 *) printf 'rkisp_mainpath: %s/video24\\n' "$DEV_ROOT" ;;
esac''')
        self.env = dict(os.environ, PATH=str(self.bin) + ':' + os.environ['PATH'],
                        SYS_ROOT=str(self.sys), DEV_ROOT=str(self.dev), DT_ROOT=str(self.dt),
                        PROC_ROOT=str(self.root / 'proc'), CALLS=str(self.calls))

    def command(self, name, body):
        path = self.bin / name
        path.write_text('#!/bin/sh\n' + body + '\n')
        path.chmod(0o755)

    def bound(self, driver='ar0234'):
        path = self.sys / 'bus/i2c/drivers' / driver
        path.mkdir(parents=True)
        (self.client / 'driver').symlink_to(path)

    def split_graph(self, *, reciprocal=True, enabled=True, sensor_bus=7):
        (self.dev / 'media1').touch()
        bridge = self.dt / 'rkcif-mipi-lvds-sditf/port/endpoint'
        isp = self.dt / 'rkisp-vir0/port/endpoint@0'
        for path in (bridge, isp):
            path.mkdir(parents=True, exist_ok=True)
        (bridge / 'phandle').write_bytes(b'\0\0\0\x01')
        (bridge / 'remote-endpoint').write_bytes(b'\0\0\0\x02')
        (isp / 'phandle').write_bytes(b'\0\0\0\x02')
        (isp / 'remote-endpoint').write_bytes(b'\0\0\0\x01' if reciprocal else b'\0\0\0\x03')
        for name, node in [('v4l-subdev4', isp.parents[1]), ('v4l-subdev5', bridge.parents[1])]:
            path = self.sys / 'class/video4linux' / name / 'device'
            path.mkdir(parents=True, exist_ok=True)
            if not (path / 'of_node').is_symlink():
                (path / 'of_node').symlink_to(node)
        flags = 'ENABLED' if enabled else ''
        self.command('media-ctl', f'''
case "$*" in
 *"-e rkisp_mainpath"*) printf '%s/video24\\n' "$DEV_ROOT" ;;
 *"-e rkisp-isp-subdev"*) printf '%s/v4l-subdev4\\n' "$DEV_ROOT" ;;
 *"-e rkcif-mipi-lvds"*) printf '%s/v4l-subdev5\\n' "$DEV_ROOT" ;;
 *media1*) printf 'driver          rkcif\\nmodel           rkcif-mipi-lvds\\n- entity 63: m00_b_ar0234 {sensor_bus}-0010 (1 pad)\\n' ;;
 *) printf 'driver          rkisp-vir0\\n- entity 1: rkisp-isp-subdev\\n<- "rkcif-mipi-lvds":0 [{flags}]\\n' ;;
esac''')

    def run_script(self, *args, **extra_env):
        return subprocess.run(['/bin/sh', str(SCRIPT), '--output', str(self.root / 'report'), *args],
                              env=self.env | extra_env, text=True, capture_output=True, timeout=5)

    def test_default_enumerates_without_i2c_transactions(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('selected adapter=7', result.stdout)
        self.assertNotIn('i2ctransfer', self.calls.read_text())
        self.assertNotIn('--set-fmt-video', self.calls.read_text())

    def test_probe_resolves_dt_alias_and_reads_big_endian_id(self):
        for response, name in [('0x0a 0x56', 'Bayer'), ('0x1a 0x56', 'mono')]:
            with self.subTest(response=response):
                result = self.run_script('--probe', RESPONSE=response)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn(f'PASS: AR0234 {name} ID', result.stdout)
                self.assertIn('i2ctransfer -y 7 w2@0x10 0x30 0x00 r2', self.calls.read_text())

    def test_bound_driver_uses_chip_id_and_never_forces(self):
        self.bound()
        (self.client / 'chip_id').write_text('0x0a56\n')
        result = self.run_script('--probe')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('via driver/runtime PM', result.stdout)
        self.assertNotIn('i2ctransfer', self.calls.read_text())

    def test_other_driver_is_not_overridden(self):
        self.bound('other_sensor')
        result = self.run_script('--probe')
        self.assertEqual(result.returncode, 3, result.stdout)
        self.assertNotIn('i2ctransfer', self.calls.read_text())

    def test_wrong_id_and_bad_bytes_fail(self):
        for response in ('0xff 0xff', 'not a register response'):
            with self.subTest(response=response):
                self.assertEqual(self.run_script('--probe', RESPONSE=response).returncode, 1)

    def test_capture_uses_verified_sensor_graph(self):
        self.bound()
        (self.client / 'chip_id').write_text('0x0a56\n')
        result = self.run_script('--capture', '--frames', '3')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('PASS: captured 3 frames', result.stdout)
        self.assertIn(str(self.dev / 'video24'), self.calls.read_text())

    def test_capture_short_stream_fails(self):
        self.bound()
        (self.client / 'chip_id').write_text('0x0a56\n')
        result = self.run_script('--capture', '--frames', '3', CAPTURE_COUNT='2')
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_capture_follows_verified_split_cif_isp_graphs(self):
        self.bound()
        (self.client / 'chip_id').write_text('0x0a56\n')
        self.split_graph()
        result = self.run_script('--capture', '--frames', '3')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('verified split media bridge:', result.stdout)
        self.assertIn('PASS: captured 3 frames', result.stdout)

    def test_capture_rejects_unrelated_or_disabled_split_bridge(self):
        self.bound()
        (self.client / 'chip_id').write_text('0x0a56\n')
        for kwargs in ({'reciprocal': False}, {'enabled': False}, {'sensor_bus': 8}):
            with self.subTest(kwargs=kwargs):
                self.calls.write_text('')
                self.split_graph(**kwargs)
                result = self.run_script('--capture', '--frames', '3')
                self.assertEqual(result.returncode, 3, result.stdout)
                self.assertNotIn('--set-fmt-video', self.calls.read_text())

    def test_capture_rejects_clipped_dimensions(self):
        self.bound()
        (self.client / 'chip_id').write_text('0x0a56\n')
        result = self.run_script('--capture', '--frames', '3', CAPTURE_SIZE='1280/1080')
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn('--stream-mmap', self.calls.read_text())

    def test_missing_alias_never_probes_default_numeric_bus(self):
        (self.dt / 'aliases/i2c4').unlink()
        result = self.run_script('--probe')
        self.assertEqual(result.returncode, 3, result.stdout)
        self.assertNotIn('i2ctransfer', self.calls.read_text())

    def test_invalid_address_has_no_side_effects(self):
        result = self.run_script('--probe', '--address', '0x80')
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.calls.read_text(), '')


if __name__ == '__main__':
    unittest.main(verbosity=2)
