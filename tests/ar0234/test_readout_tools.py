#!/usr/bin/env python3
"""Check real procfs formatting, sample filtering and physical exposure conversion."""
from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]
tool = runpy.run_path(str(ROOT / "tools/test-ar0234-readout"))
baseline = runpy.run_path(str(ROOT / "tools/test-aura-software-baseline"))


class ReadoutToolsTest(unittest.TestCase):
    def test_microsecond_remainder_is_not_a_decimal_fraction(self):
        text = "@@SAMPLE\n@@CAMERA 0\nlanes:2\nframe amount:10\nsingle readout:9.8 ms\n"
        self.assertEqual(tool["proc_samples"](text)["0"][0]["readout_ms"], 9.008)

    def test_idle_startup_zero_and_repeated_frames_are_not_counted(self):
        text = "@@SAMPLE\n@@CAMERA 0\nMonitor Mode:idle\n"
        for frame, value in ((1, "9.700"), (10, "0.0"), (11, "9.750"), (11, "9.760"), (14, "9.780")):
            text += f"@@SAMPLE\n@@CAMERA 0\nlanes:2\nframe amount:{frame}\nsingle readout:{value} ms\n"
        data = tool["proc_samples"](text)
        self.assertEqual([s["observed_frame"] for s in data["0"]], [11, 14])
        self.assertEqual([s["readout_ms"] for s in data["0"]], [9.750, 9.780])
        self.assertEqual(data["1"], [])

    def test_dual_streams_are_separate(self):
        text = "@@SAMPLE\n@@CAMERA 0\nlanes:2\nframe amount:10\nsingle readout:9.792 ms\n"
        text += "@@CAMERA 1\nlanes:4\nframe amount:12\nsingle readout:4.896 ms\n"
        data = tool["proc_samples"](text)
        self.assertEqual(data["0"][0]["readout_ms"], 9.792)
        self.assertEqual(data["1"][0]["readout_ms"], 4.896)
        self.assertEqual(data["1"][0]["lanes"], 4)

    def test_exposure_uses_measured_mode_row_units_and_refuses_out_of_range(self):
        sensor = {"row_us": 13.6, "controls": {"exposure": {"min": 2, "max": 735}}}
        self.assertEqual(tool["exposure_lines"](sensor, 1000), 74)
        self.assertEqual(tool["exposure_lines"](sensor, 3000), 221)
        sensor["row_us"] = 6.8
        self.assertEqual(tool["exposure_lines"](sensor, 1000), 147)
        self.assertEqual(tool["exposure_lines"](sensor, 3000), 441)
        with self.assertRaises(RuntimeError): tool["exposure_lines"](sensor, 100000)

    def test_baseline_propagates_exposure_without_changing_default(self):
        controls = baseline["controls_script"]
        self.assertIn("exposure=735,", controls("dual", None))
        self.assertIn("exposure=74,", controls("dual", None, exposure=74))
        self.assertIn("vertical_blanking=762", controls("dual", 762, exposure=147))

    def test_no_readout_must_fail_instead_of_claiming_zero_latency(self):
        with self.assertRaises(RuntimeError): tool["distribution"]([])

    def test_only_exact_known_bsp_advisory_is_classified_separately(self):
        line = "4,657,645234719,-;rkcif-mipi-lvds: Warning: vblank need >= 1000us if isp work in online, cur 286 us"
        advisories, critical = tool["kernel_issues"](line + "\n")
        self.assertEqual(advisories, [line])
        self.assertFalse(critical)
        self.assertTrue(tool["kernel_issues"](line + "\nWARNING: CPU0 dma_buf_put\n")[1])
        self.assertTrue(tool["kernel_issues"]("rkcif: Warning: something else\n")[1])


if __name__ == "__main__": unittest.main(verbosity=2)
