#!/usr/bin/env python3
"""Native host: validate local AU timestamp association through real VAAPI."""
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
VIEWER = Path(os.environ.get("TELEOP_TEST_VIEWER", ROOT / "build/host-metrics/bin/host-viewer"))


class SoftwareMetricsTest(unittest.TestCase):
    def test_dual_vaapi_timestamp_association_and_disabled_mode(self):
        if Path("/run/.containerenv").exists() or Path("/.dockerenv").exists():
            self.fail("Run VAAPI host tests natively, not in Distrobox")
        with tempfile.TemporaryDirectory(prefix="teleop-metrics-") as directory:
            path = Path(directory)
            sample = path / "sample.h264"
            subprocess.run([
                "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=50",
                "-frames:v", "12", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                "-x264-params", "bframes=0:keyint=12:scenecut=0", "-f", "h264", str(sample)
            ], check=True, timeout=30)
            packets = json.loads(subprocess.check_output([
                "ffprobe", "-v", "error", "-show_packets", "-show_entries", "packet=pos,size",
                "-of", "json", str(sample)
            ], text=True, timeout=10))["packets"]
            self.assertEqual(len(packets), 12)
            data = sample.read_bytes()
            cases = [(False, (), False), (True, (), False), (True, (0, 1), False),
                     (True, (0,), False), (True, (0, 1), True)]
            for case, (enabled, complete_streams, force_parser) in enumerate(cases):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    source.bind(("127.0.0.1", 0))
                    port = source.getsockname()[1]
                environment = dict(os.environ, TELEOP_METRICS="1" if enabled else "0")
                output = path / f"viewer-{case}.txt"
                with output.open("w") as log:
                    command = [
                        str(VIEWER), "--port", str(port), "--dual-stream", "--headless",
                        "--frames", "24", "--timeout-ms", "3000"
                    ]
                    if force_parser: command += ['--force-stream-parser']
                    viewer = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        # Confirm the UDP port is bound before injecting the first IDR.
                        deadline = time.monotonic() + 5
                        while True:
                            with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
                                try:
                                    probe.bind(("::", port))
                                except OSError:
                                    break
                            if viewer.poll() is not None or time.monotonic() > deadline:
                                self.fail("Viewer did not bind UDP port: " + output.read_text())
                            time.sleep(0.01)
                        time.sleep(0.1)
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                            for frame_id, packet in enumerate(packets):
                                offset, size = int(packet["pos"]), int(packet["size"])
                                payload = data[offset:offset + size]
                                count = (size + 1159) // 1160
                                for stream_id in (0, 1):
                                    # Deliberate fragment reordering checks that the metric
                                    # starts at the first accepted datagram, not packet ID 0.
                                    for packet_id in reversed(range(count)):
                                        fragment_offset = packet_id * 1160
                                        fragment = payload[fragment_offset:fragment_offset + 1160]
                                        header = struct.pack(
                                            "!IBBHHHIHHIIHHQ", 0x41555241, 1, 40,
                                            (1 if frame_id == 0 else 0) | (2 if stream_id in complete_streams else 0),
                                            stream_id, 0, frame_id,
                                            packet_id, count, size, fragment_offset, len(fragment), 0, 0)
                                        source.sendto(header + fragment, ("127.0.0.1", port))
                                time.sleep(0.02)
                        self.assertEqual(viewer.wait(timeout=10), 0, output.read_text())
                    finally:
                        if viewer.poll() is None:
                            viewer.terminate()
                            viewer.wait(timeout=5)
                text = output.read_text()
                self.assertIn("access_units=24", text)
                self.assertIn("malformed=0 duplicates=0 dropped=0", text)
                self.assertEqual(text.count("decoder: frames=12 hardware_frames=12"), 2)
                if not enabled:
                    self.assertNotIn("metric: stage=", text)
                    continue
                for stream in (0, 1):
                    for stage in ("parser_wait", "submit_to_frame", "au_ready_to_frame", "reassembly"):
                        pattern = rf"metric: stage=host.stream{stream}\.{stage} samples=12 .*"
                        self.assertRegex(text, pattern, "Lost or mis-associated frame timestamps")
                    match = re.search(rf"stage=host.stream{stream}\.parser_wait .*p50=([\d.]+)", text)
                    if stream in complete_streams and not force_parser:
                        self.assertLess(float(match[1]), 5.0, "Complete AU still waits for next frame")
                        self.assertIn(f'viewer stream {stream}: input=complete-au', text)
                    else:
                        self.assertGreater(float(match[1]), 10.0, "Parser lookahead was not measured")
                        self.assertIn(f'viewer stream {stream}: input=annex-b-stream', text)
                print(text, end="")

    def test_missing_fragment_does_not_submit_partial_au(self):
        if Path('/run/.containerenv').exists() or Path('/.dockerenv').exists():
            self.fail('Run VAAPI host tests natively, not in Distrobox')
        with tempfile.TemporaryDirectory(prefix='teleop-au-loss-') as temporary:
            path = Path(temporary); sample = path / 'idr.h264'
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                            'testsrc2=size=320x180:rate=50', '-frames:v', '6', '-c:v', 'libx264',
                            '-preset', 'ultrafast', '-tune', 'zerolatency', '-x264-params',
                            'bframes=0:keyint=1:slices=4', '-f', 'h264', str(sample)],
                           check=True, timeout=30)
            packets = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_packets',
                '-show_entries', 'packet=pos,size', '-of', 'json', str(sample)], text=True))['packets']
            data = sample.read_bytes()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
            with (path / 'viewer.txt').open('w') as log:
                viewer = subprocess.Popen([str(VIEWER), '--port', str(port), '--headless', '--frames', '5',
                                           '--timeout-ms', '3000'], stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 5
                    while True:
                        with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
                            try: probe.bind(('::', port))
                            except OSError: break
                        self.assertIsNone(viewer.poll())
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.01)
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                        for frame_id, packet in enumerate(packets):
                            offset, size = int(packet['pos']), int(packet['size'])
                            payload = data[offset:offset + size]; count = (size + 1159) // 1160
                            self.assertGreater(count, 1)
                            for packet_id in reversed(range(count)):
                                if frame_id == 2 and packet_id == 1: continue
                                start = packet_id * 1160; fragment = payload[start:start + 1160]
                                header = struct.pack('!IBBHHHIHHIIHHQ', 0x41555241, 1, 40, 3, 0, 0,
                                    frame_id, packet_id, count, size, start, len(fragment), 0, 0)
                                source.sendto(header + fragment, ('127.0.0.1', port))
                            time.sleep(.02)
                    self.assertEqual(viewer.wait(timeout=10), 0)
                finally:
                    if viewer.poll() is None: viewer.terminate(); viewer.wait(timeout=5)
            text = (path / 'viewer.txt').read_text()
            self.assertIn('access_units=5', text)
            self.assertIn('malformed=0 duplicates=0 dropped=1', text)
            self.assertIn('decoder: frames=5 hardware_frames=5', text)
            self.assertNotRegex(text, r'\[h264 @|host-viewer:')


if __name__ == "__main__":
    unittest.main()
