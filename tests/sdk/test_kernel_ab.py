#!/usr/bin/env python3
"""Offline A/B aggregation and real SDK module-staging regression checks."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("AURA_SDK_ROOT", ROOT.parent / "aura-sdk"))
loader = importlib.machinery.SourceFileLoader("kernel_ab", str(ROOT / "tools/test-aura-kernel-ab"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ab = importlib.util.module_from_spec(spec)
loader.exec_module(ab)


class KernelABTests(unittest.TestCase):
    def test_saved_images_dry_run_and_layout_refusal(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            tool = path / "sdk/tools/linux/Linux_Upgrade_Tool/upgrade_tool"
            tool.parent.mkdir(parents=True)
            tool.write_text("#!/bin/sh\necho SHOULD_NOT_EXECUTE >&2\nexit 99\n")
            tool.chmod(0o755)
            images = path / "saved-images"; images.mkdir()
            for name in ("download.bin", "env.img", "idblock.img", "uboot.img", "boot.img", "userdata.img", "oem.img", "rootfs.img"):
                (images / name).write_text("fixture\n")
            layout = "blkdevparts=mmcblk0:32K(env),512K@32K(idblock),4M(uboot),11M(boot),1G(userdata),2G(oem),-(rootfs)"
            (images / "env.img").write_text(layout + "\n")
            command = [str(ROOT / "tools/flash-aura-emmc"), "--dry-run", "--sdk", str(path / "sdk"), "--images", str(images)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(str(images / "rootfs.img"), result.stdout)
            self.assertNotIn("SHOULD_NOT_EXECUTE", result.stdout + result.stderr)
            (images / "env.img").write_text("wrong layout\n")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unexpected partition layout", result.stderr)
            self.assertNotIn("SHOULD_NOT_EXECUTE", result.stdout + result.stderr)

    def test_config_comparison_ignores_build_header_but_checks_disabled_symbols(self):
        config = "# Linux A\nCONFIG_PREEMPT=y\n# CONFIG_SOUND is not set\nCONFIG_LOCALVERSION=\"\"\n"
        self.assertEqual(ab.config_values(config), ab.config_values(config.replace("Linux A", "Linux B")))
        self.assertNotEqual(ab.config_values(config), ab.config_values(config.replace("# CONFIG_SOUND is not set", "CONFIG_SOUND=y")))

    def test_summary_weights_samples_and_keeps_run_percentile_range(self):
        runs = [{"metrics": ab.parse_metrics("metric: stage=sender.udp samples=2 mean=1 p95=2 max=3")},
                {"metrics": ab.parse_metrics("metric: stage=sender.udp samples=6 mean=3 p95=5 max=6")}]
        result = ab.summarize(runs)["sender.udp"]
        self.assertEqual(result["samples"], 8)
        self.assertEqual(result["weighted_mean_ms"], 2.5)
        self.assertEqual(result["run_p95_range_ms"], [2, 5])
        self.assertEqual(result["max_ms"], 6)
        with self.assertRaises(ValueError):
            ab.parse_metrics("metric: stage=x samples=1\nmetric: stage=x samples=2")

    def test_actual_sysdrv_module_copy_removes_disabled_modules(self):
        source = (SDK / "sysdrv/Makefile").read_text()
        chunk = re.search(r"ifneq \(\$\(OUTPUT_ROOTFS_SOURCE_DIR\),\)\n\trm -rf .*?/kernel_drv_ko\nendif\n\t\$\(call MAROC_COPY_PKG_TO_SYSDRV_OUTPUT,.*?SELF\)", source).group()
        macro = re.search(r"define MAROC_COPY_PKG_TO_SYSDRV_OUTPUT\n.*?\nendef", (SDK / "sysdrv/Makefile.param").read_text(), re.S).group()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            current = path / "new/kernel_drv_ko"; current.mkdir(parents=True)
            (current / "kmpp.ko").write_text("new")
            stage = path / "staging/kernel_drv_ko"; stage.mkdir(parents=True)
            (stage / "old_sensor.ko").write_text("old")
            (stage.parent / "rootfs.tar").write_text("keep")
            makefile = path / "Makefile"
            makefile.write_text(f"SHELL=/bin/bash\nOUTPUT_ROOTFS_SOURCE_DIR={stage.parent}\nSYSDRV_KERNEL_MOD_PATH={current}\n" + macro + "\nall:\n" + chunk + "\n")
            subprocess.run(["make", "-f", str(makefile)], check=True, capture_output=True)
            self.assertEqual(sorted(item.name for item in stage.iterdir()), ["kmpp.ko"])
            self.assertEqual((stage / "kmpp.ko").read_text(), "new")
            self.assertEqual((stage.parent / "rootfs.tar").read_text(), "keep")

    def test_actual_oem_packaging_removes_old_modules(self):
        source = (SDK / "project/build.sh").read_text()
        copy_function = re.search(r"function __COPY_FILES\(\)\n\{.*?\n\}", source, re.S).group()
        start = source.index("\t_install_dir=$_target_dir/usr\n", source.index("function __PACKAGE_RESOURCES"))
        end = source.index("\t__COPY_FILES $RK_PROJECT_PATH_APP/bin", start)
        chunk = source[start:end]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp); new = path / "sysdrv/kernel_drv_ko"; new.mkdir(parents=True)
            (new / "kmpp.ko").write_text("new")
            old = path / "oem/usr/ko"; old.mkdir(parents=True)
            (old / "old_sensor.ko").write_text("old")
            lib = path / "oem/usr/lib"; lib.mkdir(); (lib / "librockit.so").write_text("keep")
            script = copy_function + f"\nRK_PROJECT_PATH_SYSDRV={shlex.quote(str(new.parent))}\n_target_dir={shlex.quote(str(path / 'oem'))}\n" + chunk
            subprocess.run(["bash", "-e", "-c", script], check=True, capture_output=True)
            self.assertEqual(sorted(item.name for item in old.iterdir()), ["kmpp.ko"])
            self.assertEqual((lib / "librockit.so").read_text(), "keep")


if __name__ == "__main__":
    unittest.main()
