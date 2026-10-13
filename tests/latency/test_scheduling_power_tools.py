#!/usr/bin/env python3
"""Policy restoration tests; no hardware or privileged writes."""
import importlib.machinery
import importlib.util
from pathlib import Path
import shlex
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader('scheduling_power_tools', str(ROOT / 'tools/test-scheduling-power-ab'))
spec = importlib.util.spec_from_loader(loader.name, loader)
module = importlib.util.module_from_spec(spec)
loader.exec_module(module)


class FakeBoard:
    def __init__(self):
        self.values = {
            '/proc/sys/kernel/random/boot_id': 'same-boot', '/sys/class/thermal/thermal_zone0/temp': '44000',
            module.VENC + '/governor': 'venc_ondemand', module.VENC + '/min_freq': '396000000',
            module.VENC + '/max_freq': '550000000', module.VENC + '/cur_freq': '550000000',
            '/proc/irq/71/smp_affinity_list': '0-3', '/proc/irq/71/effective_affinity_list': '0'}
        self.writes = []
        self.fail_write_once = False

    def __call__(self, command):
        if command == 'cat /proc/interrupts': return '71: 100 0 0 0 GICv2 eth0'
        words = shlex.split(command)
        if words[0] == 'cat': return '\n'.join(self.values[path] for path in words[1:])
        if words[0] == 'printf':
            value, path = words[2], words[4]
            self.values[path] = value
            self.writes.append((path, value))
            if path.endswith('/smp_affinity_list') and value in ('0', '1'):
                self.values['/proc/irq/71/effective_affinity_list'] = value
            # A wide mask deliberately retains the currently effective CPU.
            if self.fail_write_once:
                self.fail_write_once = False
                raise RuntimeError('injected write/readback transport failure')
            return ''
        raise AssertionError(command)


class RestorationTest(unittest.TestCase):
    def test_venc_restored_after_camera_exception(self):
        board = FakeBoard(); policy = module.BoardPolicy(board, 71)
        with self.assertRaisesRegex(RuntimeError, 'camera failed'):
            with policy.treatment('venc-floor', 'B', 1):
                self.assertEqual(board.values[module.VENC + '/min_freq'], '550000000')
                raise RuntimeError('camera failed')
        policy.restored()

    def test_partial_application_restores_original_value(self):
        board = FakeBoard(); policy = module.BoardPolicy(board, 71)
        board.fail_write_once = True
        with self.assertRaisesRegex(RuntimeError, 'injected'):
            with policy.treatment('venc-floor', 'B', 1): self.fail('Should not yield')
        policy.restored()

    def test_irq_effective_cpu_is_restored_before_wide_mask(self):
        board = FakeBoard(); policy = module.BoardPolicy(board, 71)
        with policy.treatment('gmac-irq', 'B', 1):
            self.assertEqual(board.values['/proc/irq/71/effective_affinity_list'], '1')
        self.assertEqual([value for _, value in board.writes], ['1', '0', '0-3'])
        policy.restored()

    def test_baseline_has_no_mutations_and_detects_drift(self):
        board = FakeBoard(); policy = module.BoardPolicy(board, 71)
        with policy.treatment('venc-floor', 'A', 1): pass
        self.assertEqual(board.writes, [])
        board.values[module.VENC + '/min_freq'] = '550000000'
        with self.assertRaisesRegex(RuntimeError, 'not restored'):
            with policy.treatment('venc-floor', 'A', 1): self.fail('Should not yield')

    def test_pm_qos_file_is_closed_on_exception(self):
        state = {'value': 2000000000}
        def write(fd, data):
            self.assertEqual(fd, 42)
            state['value'] = module.struct.unpack('=i', data)[0]
            return 4
        def close(fd):
            self.assertEqual(fd, 42)
            state['value'] = 2000000000
        with mock.patch.object(module, 'cpu_latency_value', side_effect=lambda: state['value']), \
             mock.patch.object(module.os, 'open', return_value=42), \
             mock.patch.object(module.os, 'write', side_effect=write), \
             mock.patch.object(module.os, 'close', side_effect=close) as closed:
            with self.assertRaisesRegex(RuntimeError, 'camera failure'):
                with module.idle_constraint(True, 50) as record:
                    self.assertEqual(record['applied_us'], 50)
                    raise RuntimeError('camera failure')
            closed.assert_called_once()
            self.assertEqual(record['restored_us'], 2000000000)

    def test_pm_qos_no_contrast_refuses_to_start(self):
        with mock.patch.object(module, 'cpu_latency_value', return_value=10), \
             mock.patch.object(module.os, 'open') as opened:
            with self.assertRaisesRegex(RuntimeError, 'no A/B contrast'):
                with module.idle_constraint(True, 50): self.fail('Should not yield')
            opened.assert_not_called()


if __name__ == '__main__': unittest.main()
