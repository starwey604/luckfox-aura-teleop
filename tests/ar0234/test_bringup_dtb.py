#!/usr/bin/env python3
"""Validate the compiled bring-up FDT rather than only grepping DTS source."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
DTB = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / 'build/ar0234-driver-check/kernel/arch/arm64/boot/dts/rockchip/rv1126b-luckfox-aura-ar0234.dtb'


def read_fdt(path):
    blob = path.read_bytes()
    header = struct.unpack('>10I', blob[:40])
    if header[0] != 0xd00dfeed or header[1] > len(blob):
        raise ValueError('invalid FDT header')
    offset, strings = header[2:4]
    nodes, stack = {}, []
    while True:
        token = struct.unpack_from('>I', blob, offset)[0]
        offset += 4
        if token == 1:
            end = blob.index(0, offset)
            stack.append(blob[offset:end].decode())
            offset = (end + 4) & ~3
            nodes['/' + '/'.join(filter(None, stack))] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, name_offset = struct.unpack_from('>II', blob, offset)
            offset += 8
            start = strings + name_offset
            name = blob[start:blob.index(0, start)].decode()
            nodes['/' + '/'.join(filter(None, stack))][name] = blob[offset:offset + size]
            offset = (offset + size + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            return nodes
        else:
            raise ValueError(f'unknown FDT token {token}')


def words(data):
    return struct.unpack('>' + 'I' * (len(data) // 4), data)


def string(data):
    return data.rstrip(b'\0').decode()


class BringupDeviceTreeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.nodes = read_fdt(DTB)
        cls.bus = string(cls.nodes['/aliases']['i2c4'])
        cls.sensor = cls.bus + '/camera@10'
        cls.handles = {words(p['phandle'])[0]: n for n, p in cls.nodes.items() if 'phandle' in p}

    def test_sensor_address_clock_and_module_supply(self):
        sensor = self.nodes[self.sensor]
        self.assertEqual(sensor['compatible'], b'decxin,3478v1\0onnn,ar0234cs\0')
        self.assertEqual(string(sensor['status']), 'okay')
        self.assertEqual(words(sensor['reg']), (0x10,))
        self.assertEqual(words(self.nodes[self.bus]['clock-frequency']), (100000,))
        osc = self.nodes[self.handles[words(sensor['clocks'])[0]]]
        self.assertEqual(osc['compatible'], b'fixed-clock\0')
        self.assertEqual(words(osc['clock-frequency']), (24000000,))
        supply = self.nodes[self.handles[words(sensor['vdd-supply'])[0]]]
        self.assertEqual(words(supply['regulator-min-microvolt']), (3300000,))
        for key in ('reset-gpios', 'pwdn-gpios', 'avdd-supply', 'dovdd-supply', 'dvdd-supply'):
            self.assertNotIn(key, sensor)

    def test_sensor_and_receiver_links_are_reciprocal_and_two_lane(self):
        endpoint = self.nodes[self.sensor + '/port/endpoint']
        receiver = self.nodes[self.handles[words(endpoint['remote-endpoint'])[0]]]
        self.assertEqual(words(receiver['remote-endpoint']), words(endpoint['phandle']))
        for node in (endpoint, receiver):
            self.assertEqual(words(node['data-lanes']), (1, 2))
            self.assertEqual(words(node['clock-lanes']), (0,))
            self.assertNotIn('clock-noncontinuous', node)
        self.assertEqual(struct.unpack('>Q', endpoint['link-frequencies']), (450000000,))

    def test_old_sensor_nodes_are_removed_and_second_isp_disabled(self):
        for props in self.nodes.values():
            compatible = props.get('compatible', b'')
            self.assertNotIn(b'mia1321', compatible)
            self.assertNotIn(b'sony,imx415', compatible)
        isp1 = [p for n, p in self.nodes.items() if n.endswith('/rkisp-vir1')]
        self.assertEqual(len(isp1), 1)
        self.assertEqual(string(isp1[0]['status']), 'disabled')

    def test_primary_ethernet_and_emmc_remain_enabled(self):
        eth = [p for n, p in self.nodes.items() if 'ethernet@' in n and string(p.get('status', b'')) == 'okay']
        emmc = [p for p in self.nodes.values()
                if b'rockchip,rv1126b-dw-mshc' in p.get('compatible', b'')
                and 'non-removable' in p and words(p.get('bus-width', b'')) == (8,)]
        self.assertTrue(eth)
        self.assertTrue(emmc)
        self.assertEqual(string(emmc[0]['status']), 'okay')


if __name__ == '__main__':
    unittest.main(verbosity=2)
