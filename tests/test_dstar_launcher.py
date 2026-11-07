#!/usr/bin/env python3
"""Model-free launcher regressions. Dry runs never start or download a model."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class PrefillChunkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dstar-launcher-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        shutil.copy2(Path(__file__).resolve().parents[1] / "dstar", self.root / "dstar")
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        # Stub only hardware discovery. Real argument parsing and command
        # construction run unchanged on Linux CI as well as Apple Silicon.
        for name, source in {
            "uname": '#!/bin/sh\ncase "$1" in -s) echo Darwin;; -m) echo arm64;; *) exit 1;; esac\n',
            "sysctl": '#!/bin/sh\ncase "$2" in hw.memsize) echo 68719476736;; iogpu.wired_limit_mb) echo 65536;; *) exit 1;; esac\n',
        }.items():
            p = bin_dir / name
            p.write_text(source)
            p.chmod(0o755)
        models = self.root / "models"
        qdir = models / "IQ3_XXS"
        qdir.mkdir(parents=True)
        for shard in [1, 2]:
            (qdir / f"Qwen3.8-Flash-Next-GSQ-RCO-IQ3_XXS-0000{shard}-of-00002.gguf").touch()
        self.other_model = self.root / "Qwen-other.gguf"
        self.other_model.touch()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("DSTAR_", "DS4_QWEN"))}
        self.env.update(PATH=str(bin_dir) + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
                        HOME=str(self.root), DSTAR_MODELS=str(models), HOST="127.0.0.1", PORT="37944")

    def run_launcher(self, *args, flash=True):
        prefix = ["--quant", "iq3"] if flash else [str(self.other_model)]
        return subprocess.run(["/bin/bash", str(self.root / "dstar"), "serve", *prefix,
                               "--ctx", "65536", "--no-mtp", "--no-vision", "--dry-run", *args],
                              env=self.env, capture_output=True, text=True, timeout=10)

    def chunks(self, output):
        return re.findall(r"--prefill-chunk\s+(\S+)", output)

    def test_flash_default_unchanged(self):
        result = self.run_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.chunks(result.stdout), ["2048"])

    def test_explicit_values_both_model_routes(self):
        for flash in [True, False]:
            for value in ["1", "1024", "2048", "65536"]:
                with self.subTest(flash=flash, value=value):
                    result = self.run_launcher("--prefill-chunk", value, flash=flash)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(self.chunks(result.stdout), [value])

    def test_other_model_retains_its_server_default(self):
        result = self.run_launcher(flash=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.chunks(result.stdout), [])

    def test_invalid_values_rejected_before_startup(self):
        for value in ["", "0", "00", "-1", "65537", "9999999999999999999999", "1.5", "abc", "--port"]:
            with self.subTest(value=value):
                result = self.run_launcher("--prefill-chunk", value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("--prefill-chunk", result.stderr)
                self.assertNotIn("./ds4-server", result.stdout)

    def test_missing_value_rejected(self):
        result = self.run_launcher("--prefill-chunk")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--prefill-chunk needs a value", result.stderr)

    def test_raw_server_argument_forwarding_unchanged(self):
        result = self.run_launcher("--prefill-chunk", "1024", "--", "--summary-slot", "--prefill-chunk", "512")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.chunks(result.stdout), ["1024", "512"])
        self.assertIn("--summary-slot", result.stdout)


if __name__ == "__main__":
    unittest.main()
