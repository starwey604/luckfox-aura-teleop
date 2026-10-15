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
        for name in ('FPS', 'VERTICAL_BLANKING', 'EXPOSURE', 'ANALOGUE_GAIN', 'DIGITAL_GAIN',
                     'BITRATE_KBPS', 'WRAP_LINES', 'OUTPUT_MODE', 'H264_DIR', 'GOP', 'SLICE_CTUS',
                     'RC_MODE', 'QP_I', 'QP_P', 'PROFILE', 'ENTROPY', 'TRANSFORM', 'DEBLOCK', 'MAX_REENCODE', 'CODEC_REPORT'):
            self.env.pop(name, None)

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
        self.assertIn('fps=120 bitrate_kbps=30000', result.stdout)
        self.assertIn('vertical_blanking=506 exposure=735 analogue_gain=48 digital_gain=512', result.stdout)
        self.assertIn('gop=15 slice_ctus=0 output_mode=frame entropy=cavlc transform=4x4', result.stdout)

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
        self.assertIn('--fps 120', calls)
        self.assertIn('--bitrate-kbps 30000', calls)
        self.assertIn('--gop 15 --slice-ctus 0', calls)
        self.assertIn('--entropy cavlc --transform 4x4', calls)
        self.assertNotIn('--rc-mode', calls)
        self.assertEqual(calls.count('exposure=735,analogue_gain=48,digital_gain=512,test_pattern=0'), 2)
        self.assertEqual(calls.count('vertical_blanking=506'), 2)

    def test_slice_output_control(self):
        result = self.run_launcher('--frames', '3', OUTPUT_MODE='slice', SLICE_CTUS='480')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('--output-mode slice', self.calls.read_text())
        result = self.run_launcher('--frames', '3', OUTPUT_MODE='slice')
        self.assertEqual(result.returncode, 2)
        self.assertIn('requires SLICE_CTUS > 0', result.stderr)
        result = self.run_launcher('--frames', '3', OUTPUT_MODE='unknown')
        self.assertEqual(result.returncode, 2)

    def test_codec_overrides_are_individual_arguments(self):
        result = self.run_launcher('--frames', '3', GOP='1', SLICE_CTUS='480', RC_MODE='fixqp',
                                   QP_I='26', QP_P='30', ENTROPY='cavlc', TRANSFORM='4x4',
                                   DEBLOCK='off', MAX_REENCODE='0', CODEC_REPORT='1')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        for flag in ('--gop 1', '--slice-ctus 480', '--rc-mode fixqp', '--qp-i 26', '--qp-p 30',
                     '--entropy cavlc', '--transform 4x4', '--deblock off', '--max-reencode 0', '--codec-report'):
            self.assertIn(flag, calls)

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

    def test_overrides_restore_previous_timing_before_exposure(self):
        result = self.run_launcher('--frames', '3', VERTICAL_BLANKING='762', FPS='100',
                                   EXPOSURE='442', ANALOGUE_GAIN='32', DIGITAL_GAIN='256', BITRATE_KBPS='20000',
                                   ENTROPY='cabac', TRANSFORM='auto', SLICE_CTUS='120')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = self.calls.read_text().splitlines()
        for index in (0, 1):
            prefix = f'control -d {self.base}/dev/v4l-subdev{index}'
            blank = next(n for n, line in enumerate(lines) if prefix in line and 'vertical_blanking=762' in line)
            expose = next(n for n, line in enumerate(lines) if prefix in line and 'exposure=442,analogue_gain=32,digital_gain=256' in line)
            self.assertLess(blank, expose)
        self.assertIn('--fps 100', self.calls.read_text())
        self.assertIn('--bitrate-kbps 20000', self.calls.read_text())
        self.assertIn('--entropy cabac --transform auto', self.calls.read_text())
        self.assertIn('--slice-ctus 120', self.calls.read_text())

    def test_capture_only_runs_both_diagnostics(self):
        result = self.run_launcher('--capture-only', '--frames', '3')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls.read_text()
        self.assertIn('diagnostic 0 --capture', calls)
        self.assertIn('diagnostic 1 --capture', calls)
        self.assertNotIn('sender', calls)


if __name__ == '__main__':
    unittest.main(verbosity=2)
