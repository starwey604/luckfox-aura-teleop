#!/usr/bin/env python3
"""Native VAAPI/Vulkan pipeline tests, including intentional downstream stalls."""
import json
import os
from pathlib import Path
import re
import resource
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
VIEWER = Path(os.environ.get('TELEOP_TEST_VIEWER', ROOT / 'build/host/bin/host-viewer'))
REPORT_DIR = os.environ.get('TELEOP_TEST_REPORT_DIR')


class ViewerPipelineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if Path('/run/.containerenv').exists() or Path('/.dockerenv').exists():
            raise RuntimeError('Run these tests natively on CachyOS')
        cls.temporary = tempfile.TemporaryDirectory(prefix='teleop-pipeline-')
        cls.path = Path(cls.temporary.name)
        sample = cls.path / 'sample.h264'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                        'testsrc2=size=320x180:rate=100', '-frames:v', '60', '-c:v', 'libx264',
                        '-preset', 'ultrafast', '-tune', 'zerolatency', '-x264-params',
                        'bframes=0:keyint=15:scenecut=0', '-f', 'h264', str(sample)],
                       check=True, timeout=30)
        metadata = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_packets',
            '-show_entries', 'packet=pos,size,flags', '-of', 'json', str(sample)], text=True))['packets']
        data = sample.read_bytes()
        cls.frames = [(data[int(p['pos']):int(p['pos']) + int(p['size'])], 'K' in p['flags'])
                      for p in metadata]

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def start(self, name, args=(), env=None):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        path = self.path / (name + '.txt')
        log = path.open('w')
        process = subprocess.Popen([str(VIEWER), '--port', str(port), '--timeout-ms', '3000',
                                    '--window-width', '640', '--window-height', '480', *args],
                                   env=dict(os.environ, TELEOP_METRICS='1', **(env or {})),
                                   stdout=log, stderr=subprocess.STDOUT)
        self.addCleanup(self.cleanup, process, log)
        deadline = time.monotonic() + 10
        while True:
            with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
                try:
                    probe.bind(('::', port))
                except OSError:
                    break
            if process.poll() is not None or time.monotonic() > deadline:
                self.fail('Viewer did not bind: ' + path.read_text())
            time.sleep(.01)
        # Socket binds before per-stream VAAPI creation. Do not flood its startup.
        time.sleep(.15)
        return process, path, port

    @staticmethod
    def cleanup(process, log):
        try:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        finally:
            log.close()

    def finish(self, process, path, expected=0):
        self.assertEqual(process.wait(timeout=15), expected, path.read_text())
        text = path.read_text()
        if REPORT_DIR:
            output = Path(REPORT_DIR)
            output.mkdir(parents=True, exist_ok=True)
            (output / path.name).write_text(text)
        return text

    def send(self, port, count, streams=(0, 1), interval=.01, invalid=False, flip=False):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
            deadline = time.monotonic()
            for index in range(count):
                payload, key = self.frames[index % len(self.frames)]
                if invalid:
                    payload = b'not an H264 access unit'
                for stream in streams:
                    fragments = (len(payload) + 1159) // 1160
                    for part in reversed(range(fragments)):
                        offset = part * 1160
                        fragment = payload[offset:offset + 1160]
                        flags = (1 if key else 0) | (0 if flip and index else 2)
                        header = struct.pack('!IBBHHHIHHIIHHQ', 0x41555241, 1, 40, flags,
                            stream, 0, index, part, fragments, len(payload), offset, len(fragment), 0, 0)
                        source.sendto(header + fragment, ('127.0.0.1', port))
                deadline += interval
                time.sleep(max(0, deadline - time.monotonic()))

    def test_display_stall_does_not_block_receive_or_decode(self):
        ages = {}
        for mode in ('sync', 'async'):
            process, path, port = self.start('display-stall-' + mode, [
                '--dual-stream', '--frames', '120', '--pipeline', mode,
                '--diagnostic-present-delay-ms', '100',
                *(['--queue-report-ms', '50'] if mode == 'async' else [])])
            self.send(port, 60)
            text = self.finish(process, path)
            self.assertIn('access_units=120', text)
            self.assertEqual(text.count('decoder: frames=60 hardware_frames=60'), 2)
            self.assertIn('malformed=0 duplicates=0 dropped=0', text)
            self.assertIn('rxq_overflow=0', text)
            ages[mode] = float(re.search(r'kernel_rx_age_max_ms=([\d.]+)', text)[1])
            if mode == 'async':
                self.assertIn('queue-sample:', text)
                for stream in (0, 1):
                    row = re.search(rf'queue: stream={stream} .*', text)[0]
                    self.assertIn('overflows=0', row)
                    self.assertRegex(row, r'mailbox_replaced=[1-9]\d*')
                    self.assertIn('last_decoded=60 last_presented=60', row)
                presentations = int(re.search(r'presenter: frames=(\d+)', text)[1])
                self.assertLess(presentations, 30)
        self.assertGreater(ages['sync'], 100)
        self.assertLess(ages['async'], ages['sync'] / 5)
        print('display stall kernel RX max ms:', ages)

    def test_encoded_overflow_is_explicit_and_bounded(self):
        process, path, port = self.start('encoded-overflow', [
            '--headless', '--frames', '0', '--dual-stream', '--decode-queue-capacity', '1',
            '--diagnostic-decode-delay-ms', '50'])
        self.send(port, 12, interval=.005)
        text = self.finish(process, path, 1)
        self.assertIn('encoded queue capacity exceeded', text)
        self.assertIn('refusing to discard H.264 reference pictures', text)
        self.assertRegex(text, r'peak_depth=1 .*overflows=1')

    def test_batch_capacities_preserve_dual_vaapi_and_packet_timestamps(self):
        for capacity in (1, 8, 16, 32):
            process, path, port = self.start(f'batch-{capacity}', [
                '--headless', '--dual-stream', '--frames', '24', '--rx-batch-size', str(capacity)])
            self.send(port, 12, interval=.02)
            text = self.finish(process, path)
            self.assertEqual(text.count('decoder: frames=12 hardware_frames=12'), 2)
            self.assertIn('malformed=0 duplicates=0 dropped=0', text)
            timestamps = int(re.search(r'kernel_timestamps=(\d+)', text)[1])
            datagrams = int(re.search(r'rx-syscalls: .*datagrams=(\d+)', text)[1])
            self.assertEqual(timestamps, datagrams)
            self.assertIn(f'batch_capacity={capacity}', text)
            self.assertIn('rxq_overflow=0', text)

    def test_worker_cpu_affinity_is_applied_without_changing_default_scheduler(self):
        allowed = sorted(os.sched_getaffinity(0))
        if len(allowed) < 3: self.skipTest('Need three allowed CPUs')
        process, path, port = self.start('worker-affinity', [
            '--headless', '--dual-stream', '--frames', '24', '--rx-cpu', str(allowed[0]),
            '--decode0-cpu', str(allowed[1]), '--decode1-cpu', str(allowed[2])])
        self.send(port, 12, interval=.02)
        text = self.finish(process, path)
        for role, cpu in zip(('rx', 'dec0', 'dec1'), allowed):
            self.assertRegex(text, rf'thread-policy: role={role} .*policy=other priority=0 .*affinity={cpu} cpu={cpu} requested_cpu={cpu}')
        self.assertEqual(text.count('decoder: frames=12 hardware_frames=12'), 2)

    def test_invalid_worker_cpu_fails_and_joins_peers(self):
        invalid = next(cpu for cpu in range(1024) if cpu not in os.sched_getaffinity(0))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        result = subprocess.run([str(VIEWER), '--port', str(port), '--headless', '--dual-stream',
            '--rx-cpu', str(invalid)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('requested CPU is outside inherited allowed affinity', result.stdout)

    def test_rx_round_robin_does_not_elevate_decoder_workers(self):
        if resource.getrlimit(resource.RLIMIT_RTPRIO)[0] < 10: self.skipTest('SCHED_RR requires RLIMIT_RTPRIO>=10')
        cpu = min(os.sched_getaffinity(0))
        process, path, port = self.start('worker-rr', ['--headless', '--dual-stream', '--frames', '24',
                                                     '--rx-cpu', str(cpu), '--rx-rr-priority', '10'])
        self.send(port, 12, interval=.02)
        text = self.finish(process, path)
        self.assertRegex(text, r'thread-policy: role=rx .*policy=rr priority=10')
        for role in ('dec0', 'dec1'):
            self.assertRegex(text, rf'thread-policy: role={role} .*policy=other priority=0')
        self.assertEqual(text.count('decoder: frames=12 hardware_frames=12'), 2)

    def test_rt_permission_failure_is_explicit_and_joins_peers(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        def deny_rt(): resource.setrlimit(resource.RLIMIT_RTPRIO, (0, 0))
        result = subprocess.run([str(VIEWER), '--port', str(port), '--headless', '--dual-stream',
            '--rx-rr-priority', '10'], preexec_fn=deny_rt, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=5)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('rx: SCHED_RR:', result.stdout)

    @staticmethod
    def cpu_latency_value():
        with open('/dev/cpu_dma_latency', 'rb', buffering=0) as node:
            return struct.unpack('=i', node.read(4))[0]

    def test_cpu_latency_request_released_after_normal_exit(self):
        if not os.access('/dev/cpu_dma_latency', os.R_OK | os.W_OK): self.skipTest('Need PM QoS node access')
        before = self.cpu_latency_value()
        if before <= 50: self.skipTest('Existing stricter PM QoS request')
        process, path, port = self.start('cpu-qos-normal', ['--headless', '--frames', '12', '--cpu-latency-us', '50'])
        self.assertEqual(self.cpu_latency_value(), 50)
        self.send(port, 12, streams=(0,), interval=.02)
        text = self.finish(process, path)
        self.assertIn('cpu-latency: requested_us=50 effective_us=50', text)
        self.assertEqual(self.cpu_latency_value(), before)

    def test_cpu_latency_request_released_after_worker_error(self):
        if not os.access('/dev/cpu_dma_latency', os.R_OK | os.W_OK): self.skipTest('Need PM QoS node access')
        before = self.cpu_latency_value()
        if before <= 50: self.skipTest('Existing stricter PM QoS request')
        process, path, port = self.start('cpu-qos-error', ['--headless', '--dual-stream', '--frames', '0',
                                                        '--cpu-latency-us', '50'])
        self.send(port, 1, streams=(0,), invalid=True)
        self.finish(process, path, 1)
        self.assertEqual(self.cpu_latency_value(), before)

    def test_one_stream_is_not_held_for_idle_peer(self):
        process, path, port = self.start('idle-peer', ['--dual-stream', '--frames', '12'])
        self.send(port, 12, streams=(0,), interval=.02)
        text = self.finish(process, path)
        self.assertIn('viewer stream 0: decoder: frames=12 hardware_frames=12', text)
        self.assertIn('viewer stream 1: decoder: frames=0 hardware_frames=0', text)
        self.assertRegex(text, r'queue: stream=0 .*displayed=[1-9]\d* .*last_presented=12')

    def test_window_close_cancels_hour_long_receive_wait(self):
        process, path, _ = self.start('window-close', ['--frames', '0', '--timeout-ms', '3600000'],
                                     {'TELEOP_TEST_SDL_QUIT_AFTER_MS': '500'})
        begin = time.monotonic()
        text = self.finish(process, path)
        self.assertLess(time.monotonic() - begin, 3)
        self.assertIn('reason=window-close', text)

    def test_idle_signal_joins_all_workers(self):
        process, path, _ = self.start('idle-signal', ['--headless', '--dual-stream', '--frames', '0',
                                                    '--timeout-ms', '3600000'])
        process.send_signal(signal.SIGINT)
        text = self.finish(process, path)
        self.assertIn('reason=signal', text)

    def test_signal_can_cancel_a_busy_receive_queue(self):
        process, path, port = self.start('busy-signal', [
            '--transport-only', '--frames', '0', '--rx-batch-size', '32'])
        finished = threading.Event()
        def flood():
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                while not finished.is_set():
                    source.sendto(b'invalid-packet', ('127.0.0.1', port))
        producer = threading.Thread(target=flood)
        producer.start()
        try:
            time.sleep(.05)
            begin = time.monotonic()
            process.send_signal(signal.SIGINT)
            text = self.finish(process, path)
            self.assertLess(time.monotonic() - begin, 2)
            self.assertIn('reason=signal', text)
        finally:
            finished.set()
            producer.join(timeout=2)

    def test_timeout_joins_all_workers(self):
        process, path, _ = self.start('timeout', ['--headless', '--dual-stream', '--frames', '0',
                                                '--timeout-ms', '300'])
        text = self.finish(process, path, 1)
        self.assertIn('network inactivity timeout', text)

    def test_decoder_failure_cancels_idle_peer_and_rx(self):
        process, path, port = self.start('decoder-failure', ['--headless', '--dual-stream', '--frames', '0'])
        self.send(port, 1, streams=(0,), invalid=True)
        text = self.finish(process, path, 1)
        self.assertIn('host-viewer:', text)
        self.assertNotIn('network inactivity timeout', text)

    def test_contract_change_cancels_workers(self):
        process, path, port = self.start('contract-change', ['--headless', '--dual-stream', '--frames', '0'])
        self.send(port, 2, streams=(0,), interval=.03, flip=True)
        text = self.finish(process, path, 1)
        self.assertIn('H.264 AU contract changed midstream', text)

    def test_oversized_datagram_is_observed_and_rejected(self):
        process, path, port = self.start('truncated', ['--transport-only', '--frames', '1'])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
            source.sendto(b'x' * 1400, ('127.0.0.1', port))
        self.send(port, 1, streams=(0,))
        text = self.finish(process, path)
        self.assertIn('access_units=1', text)
        self.assertIn('malformed=1', text)
        self.assertIn('truncated=1', text)

    def test_software_frames_use_same_mailbox_and_window(self):
        process, path, port = self.start('software-window', ['--software-decode', '--frames', '12'])
        self.send(port, 12, streams=(0,), interval=.02)
        text = self.finish(process, path)
        self.assertIn('decoder: frames=12 hardware_frames=0', text)
        self.assertIn('last_decoded=12 last_presented=12', text)

    def test_worker_initialization_failure_joins_peer(self):
        begin = time.monotonic()
        result = subprocess.run([str(VIEWER), '--headless', '--dual-stream',
                                 '--vaapi-device', '/dev/teleop-test-no-device'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=5)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertLess(time.monotonic() - begin, 3)
        self.assertIn('host-viewer:', result.stdout)

    def test_acquired_swapchain_failure_closes_frame_and_joins_workers(self):
        process, path, port = self.start('acquire-failure', ['--dual-stream', '--frames', '0'],
            {'TELEOP_TEST_FAIL_AFTER_SWAPCHAIN_ACQUIRE': '1'})
        begin = time.monotonic()
        self.send(port, 1)
        text = self.finish(process, path, 1)
        self.assertIn('injected failure after swapchain acquire', text)
        self.assertLess(time.monotonic() - begin, 3)


if __name__ == '__main__':
    unittest.main()
