#!/usr/bin/env python3
"""Exercise preflight, camera IDs, controls and child cleanup without hardware."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StereoLauncherTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        self.calls = self.base / 'calls'
        self.calls.touch()
        for index, bus in enumerate((7, 8)):
            node = self.base / f'sys/class/video4linux/v4l-subdev{index}'
            node.mkdir(parents=True)
            (node / 'name').write_text(f'm0{index}_b_ar0234 {bus}-0010\n')
        self.write('diagnostic', '''#!/bin/sh
camera=; mode=; report=
while [ "$#" -gt 0 ]; do
 case "$1" in
 --camera) camera=$2; shift 2 ;; --output) report=$2; shift 2 ;;
 --probe|--capture) mode=$1; shift ;; *) shift ;;
 esac
done
echo "diagnostic $camera $mode" >> "$CALLS"
if [ "$camera" = "${MISSING_CAMERA:-none}" ]; then exit 3; fi
mkdir -p "$report"
printf 'selected adapter=%s, client=/sys/... driver=ar0234\n' "$((camera + 7))" > "$report/report.txt"
''')
        self.write('v4l2-ctl', '#!/bin/sh\necho "control $*" >> "$CALLS"\n')
        self.write('sender', '''#!/usr/bin/env python3
import os, signal, sys, time
from pathlib import Path
camera = 'stereo'
Path(os.environ['REPORT_DIR'], 'pid' + camera).write_text(str(os.getpid()))
with open(os.environ['CALLS'], 'a') as f: f.write('sender ' + ' '.join(sys.argv[1:]) + '\\n')
signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
print('aura-sender: VI 1/1/0', flush=True)
if os.environ.get('FAIL_CAMERA') == '1':
    time.sleep(0.2)
    sys.exit(4)
if sys.argv[sys.argv.index('--frames') + 1] == '0':
    while True: time.sleep(0.05)
time.sleep(0.3)
''')
        self.env = dict(os.environ, PATH=str(self.bin) + ':' + os.environ['PATH'],
                        SYS_ROOT=str(self.base / 'sys'), DEV_ROOT=str(self.base / 'dev'),
                        PROC_ROOT=str(self.base / 'proc'), CALLS=str(self.calls),
                        REPORT_DIR=str(self.base / 'report'), DIAGNOSTIC=str(self.bin / 'diagnostic'),
                        SENDER=str(self.bin / 'sender'))

    def write(self, name, content):
        path = self.bin / name
        path.write_text(content)
        path.chmod(0o755)

    def run_launcher(self, *args, **env):
        return subprocess.run(['/bin/sh', str(ROOT / 'tools/run-ar0234-stereo'), *args],
                              env=self.env | env, text=True, capture_output=True, timeout=5)

    def test_dry_run_checks_both_without_controls_or_senders(self):
        result = self.run_launcher('--dry-run')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertIn('diagnostic 0 --probe', calls)
        self.assertIn('diagnostic 1 --probe', calls)
        self.assertNotIn('control', calls)
        self.assertNotIn('sender', calls)

    def test_missing_second_camera_changes_neither_camera(self):
        result = self.run_launcher('--frames', '3', MISSING_CAMERA='1')
        self.assertEqual(result.returncode, 3)
        self.assertNotIn('control', self.calls.read_text())
        self.assertNotIn('sender', self.calls.read_text())

    def test_one_sender_uses_dual_and_720p(self):
        result = self.run_launcher('--frames', '3')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        for index in (0, 1):
            self.assertIn(f'control -d {self.base}/dev/v4l-subdev{index}', calls)
        self.assertIn('sender --dual --width 1280 --height 720', calls)
        self.assertEqual(calls.count('--wrap-lines 0'), 1)
        self.assertIn('--output-mode frame', calls)
        self.assertEqual(calls.count('exposure=735,analogue_gain=32,digital_gain=512,test_pattern=0'), 2)

    def test_slice_output_control(self):
        result = self.run_launcher('--frames', '3', OUTPUT_MODE='slice')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('--output-mode slice', self.calls.read_text())
        result = self.run_launcher('--frames', '3', OUTPUT_MODE='unknown')
        self.assertEqual(result.returncode, 2)

    def test_recordings_use_independent_paths(self):
        directory = str(self.base / 'h264')
        result = self.run_launcher('--frames', '3', H264_DIR=directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'--output-h264 {directory}/camera{{camera}}.h264', self.calls.read_text())

    def test_sender_failure_is_returned(self):
        result = self.run_launcher(FAIL_CAMERA='1')
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        pid = int((self.base / 'report/pidstereo').read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_optional_sensor_timing_is_applied_before_exposure(self):
        result = self.run_launcher('--frames', '3', VERTICAL_BLANKING='80')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = self.calls.read_text().splitlines()
        for index in (0, 1):
            prefix = f'control -d {self.base}/dev/v4l-subdev{index}'
            blank = next(n for n, line in enumerate(lines) if prefix in line and 'vertical_blanking=80' in line)
            expose = next(n for n, line in enumerate(lines) if prefix in line and 'exposure=735' in line)
            self.assertLess(blank, expose)

    def test_capture_only_runs_both_diagnostics(self):
        result = self.run_launcher('--capture-only', '--frames', '3')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertIn('diagnostic 0 --capture', calls)
        self.assertIn('diagnostic 1 --capture', calls)
        self.assertNotIn('sender', calls)


if __name__ == '__main__':
    unittest.main(verbosity=2)
