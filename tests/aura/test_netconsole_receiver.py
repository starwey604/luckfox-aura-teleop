#!/usr/bin/env python3
"""Host receiver self-test: real UDP, fake SSH/configfs and interface discovery."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]

class NetconsoleReceiverTest(unittest.TestCase):
    def test_packet_capture_and_target_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bindir = root / 'bin'; bindir.mkdir()
            ip = bindir / 'ip'
            ip.write_text('''#!/usr/bin/env python3
import json
print(json.dumps([{'address':'38:05:25:37:c8:9a','addr_info':[{'local':'127.0.0.1'}]}]))
'''); ip.chmod(0o755)
            ssh = bindir / 'ssh'
            ssh.write_text('''#!/usr/bin/env python3
import os,re,socket,sys
from pathlib import Path
command=sys.argv[-1]
with Path(os.environ['CALLS']).open('a') as f: f.write(command+'\\n')
if 'target_created=1' in command:
 print('target_created=1\\nold_loglevel=4',flush=True)
 marker=re.search(r'teleop-netconsole-ready-\\d+',command)[0]
 port=int(re.search(r'printf "%s\\\\n" (\\d+) > [^\\n]+/remote_port',command)[1])
 with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
  s.sendto(('6,1,123,-;'+marker+'\\n').encode(),('127.0.0.1',port))
'''); ssh.chmod(0o755)
            # Reserve an ephemeral port briefly to avoid hardcoded port clashes.
            import socket
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(('127.0.0.1',0)); port = probe.getsockname()[1]
            output = root / 'kernel.txt'
            stdout = root / 'stdout'
            env = dict(os.environ, PATH=str(bindir)+':'+os.environ['PATH'], CALLS=str(root/'calls'))
            env.pop('SSHPASS', None)
            with stdout.open('w') as log:
                child = subprocess.Popen([str(ROOT/'tools/capture-aura-kernel-log'), '--local-ip','127.0.0.1',
                         '--board-ip','127.0.0.1','--port',str(port),'--output',str(output)],
                         env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic()+10
                    while 'capture is ready' not in stdout.read_text():
                        if child.poll() is not None or time.monotonic()>deadline:
                            self.fail(stdout.read_text())
                        time.sleep(0.02)
                    child.terminate()
                    self.assertEqual(child.wait(timeout=5),0,stdout.read_text())
                finally:
                    if child.poll() is None: child.kill(); child.wait()
            metadata = json.loads(output.with_suffix('.txt.json').read_text())
            self.assertTrue(metadata['marker_received'])
            self.assertEqual(metadata['cleanup_exit'],0)
            self.assertIn(b'teleop-netconsole-ready-',output.read_bytes())
            self.assertIn('rmdir /sys/kernel/config/netconsole/teleop_', (root/'calls').read_text())
            self.assertIn('dmesg -n 4', (root/'calls').read_text())

if __name__ == '__main__': unittest.main(verbosity=2)
