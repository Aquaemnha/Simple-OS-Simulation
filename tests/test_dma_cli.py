"""Run the real standalone model with malformed descriptors and trace failures."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / ("dma_testbench.exe" if os.name == "nt" else "dma_testbench")


class DmaCliTests(unittest.TestCase):
    def run_dma(self, *args, trace=None):
        env = os.environ.copy()
        env.pop("DMA_TRACE", None)
        if trace is not None:
            env["DMA_TRACE"] = str(trace)
        return subprocess.run(
            [str(BINARY), *args], cwd=ROOT, env=env,
            text=True, capture_output=True, timeout=30,
        )

    def test_complete_data(self):
        """Short, unaligned and last-word copies must use the real callback."""
        for args in [(), ("1", "8193", "4"), ("0", "1048572", "4")]:
            with self.subTest(args=args):
                result = self.run_dma(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("callbacks=1", result.stdout)
                self.assertIn("Full data and unchanged source: PASS", result.stdout)

    def test_invalid_input(self):
        """Reject overflow, overlap, invalid size and malformed CLI input."""
        cases = [
            ("" , "8192", "4"), ("-1", "8192", "4"),
            ("18446744073709551616", "8192", "4"),
            ("1x", "8192", "4"), ("0x", "8192", "4"),
            ("0", "8192", "0"), ("0", "8192", "3"),
            ("0", "0", "4"), ("0", "3", "4"),
            ("0", "1048573", "4"), ("0", "8192"),
        ]
        for args in cases:
            with self.subTest(args=args):
                result = self.run_dma(*args)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertNotIn("submit:", result.stdout)

    def test_trace_and_failed_open(self):
        """A trace-open failure must return an error and join the worker."""
        parent = (ROOT / "obj").resolve()
        self.assertTrue(parent.is_relative_to(ROOT))
        parent.mkdir(exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix="dma-cli-", dir=parent)).resolve()
        self.assertTrue(directory.is_relative_to(parent))
        try:
            trace = directory / "copy.vcd"
            result = self.run_dma("1", "8193", "4", trace=trace)
            self.assertEqual(result.returncode, 0, result.stderr)
            text = trace.read_text()
            self.assertIn("$enddefinitions", text)
            self.assertIn("ch_done", text)
            self.assertIn("ch_bytes_transferred", text)
            result = self.run_dma(trace=directory / "missing" / "copy.vcd")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("DMA submission failed:", result.stderr)
        finally:
            shutil.rmtree(directory)


if __name__ == "__main__":
    unittest.main()
