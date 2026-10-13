#!/usr/bin/env python3
"""Exercise patch dependencies, partial upgrades and refusal before mutation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PatchStackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bundle = self.root / "teleop"
        self.sdk = self.root / "sdk"
        (self.sdk / ".repo").mkdir(parents=True)
        (self.bundle / "tools").mkdir(parents=True)
        (self.bundle / "sdk").mkdir()
        for tool in ("sdk-patch", "verify-sdk-baseline", "select-teleop-board"):
            shutil.copy2(ROOT / "tools" / tool, self.bundle / "tools" / tool)
        self.env = {**os.environ, "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
                    "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.invalid"}
        self.bases = {}
        self.tips = {}
        self.prefixes = {}
        for name in ("first", "second"):
            repo = self.sdk / name
            repo.mkdir()
            self.git(name, "init", "-q")
            (repo / "config").write_text("baseline\n")
            self.git(name, "add", "config")
            self.git(name, "commit", "-qm", "baseline")
            self.bases[name] = self.git(name, "rev-parse", "HEAD").strip()
            (repo / "config").write_text("first patch\n")
            self.git(name, "commit", "-qam", "patch 1")
            self.prefixes[name] = self.git(name, "rev-parse", "HEAD").strip()
            (repo / "config").write_text("dependent second patch\n")
            (repo / "new-file").write_text("saved\n")
            self.git(name, "add", ".")
            self.git(name, "commit", "-qm", "patch 2")
            self.tips[name] = self.git(name, "rev-parse", "HEAD^{tree}").strip()
            patch_dir = self.bundle / "sdk/patches" / name
            patch_dir.mkdir(parents=True)
            self.git(name, "format-patch", "--no-numbered", "-o", str(patch_dir), self.bases[name] + "..HEAD")
            self.git(name, "checkout", "--detach", self.bases[name])
        (self.bundle / "sdk/series.tsv").write_text("".join(
            f"{name}|{base}|teleop/test\n" for name, base in self.bases.items()))
        (self.bundle / "sdk/manifest.lock.xml").write_text(
            "<manifest>" + "".join(f'<project path="{name}" revision="{base}"/>'
                                    for name, base in self.bases.items()) + "</manifest>")

    def git(self, name, *args):
        return subprocess.check_output(["git", "-C", str(self.sdk / name), *args],
                                       env=self.env, text=True, stderr=subprocess.PIPE)

    def run_tool(self, command, success=True, tool="sdk-patch"):
        argv = [str(self.bundle / "tools" / tool)]
        if command:
            argv.append(command)
        result = subprocess.run([*argv, str(self.sdk)], env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def assert_baseline(self, name="first"):
        self.assertEqual(self.git(name, "rev-parse", "HEAD").strip(), self.bases[name])

    def test_dependent_series_and_repeat_apply(self):
        self.run_tool("check")
        self.assert_baseline()
        self.run_tool("apply")
        for name in self.bases:
            self.assertEqual(self.git(name, "rev-parse", "HEAD^{tree}").strip(), self.tips[name])
        before = self.git("first", "rev-parse", "HEAD")
        self.run_tool("apply")
        self.assertEqual(before, self.git("first", "rev-parse", "HEAD"))

    def test_partial_upgrade(self):
        self.git("first", "checkout", "--detach", self.prefixes["first"])
        self.run_tool("apply")
        self.assertEqual(self.git("first", "rev-parse", "HEAD^{tree}").strip(), self.tips["first"])

    def test_later_dirty_project_refuses_before_first_mutation(self):
        (self.sdk / "second/config").write_text("not committed\n")
        self.run_tool("apply", success=False)
        self.assert_baseline()

    def test_divergent_head_refuses(self):
        (self.sdk / "second/config").write_text("unrelated commit\n")
        self.git("second", "commit", "-qam", "unrelated")
        self.run_tool("apply", success=False)
        self.assert_baseline()

    def test_untracked_collision_refuses(self):
        (self.sdk / "second/new-file").write_text("my unsaved file\n")
        self.run_tool("apply", success=False)
        self.assert_baseline()
        self.assertEqual((self.sdk / "second/new-file").read_text(), "my unsaved file\n")

    def test_broken_later_patch_refuses(self):
        patch = sorted((self.bundle / "sdk/patches/second").glob("*.patch"))[-1]
        patch.write_text(patch.read_text().replace("-first patch", "-wrong context"))
        self.run_tool("apply", success=False)
        self.assert_baseline()

    def test_manifest_revision_mismatch(self):
        self.run_tool("", tool="verify-sdk-baseline")
        manifest = self.bundle / "sdk/manifest.lock.xml"
        manifest.write_text(manifest.read_text().replace(self.bases["second"], "0" * 40))
        self.run_tool("", success=False, tool="verify-sdk-baseline")

    def test_board_selection_with_relative_sdk_path(self):
        board = self.sdk / "project/cfg/BoardConfig_Aura/BoardConfig-EMMC-RK801-Luckfox_Aura-Teleop-AR0234-Stereo-Buildroot.mk"
        board.parent.mkdir(parents=True)
        board.write_text("# fixture\n")
        subprocess.run([str(self.bundle / "tools/select-teleop-board"), "sdk", "ar0234-stereo"],
                       cwd=self.root, check=True, capture_output=True)
        self.assertEqual((self.sdk / ".BoardConfig.mk").resolve(strict=True), board)
        candidate = board.with_name(board.name.replace("Stereo-Buildroot", "Stereo-K1-Buildroot"))
        candidate.write_text("# candidate fixture\n")
        subprocess.run([str(self.bundle / "tools/select-teleop-board"), "sdk", "ar0234-stereo-k1"],
                       cwd=self.root, check=True, capture_output=True)
        self.assertEqual((self.sdk / ".BoardConfig.mk").resolve(strict=True), candidate)


if __name__ == "__main__":
    unittest.main()
