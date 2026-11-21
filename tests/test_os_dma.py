"""Exercise DMA through process files, scheduling, syscalls and the real model."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / ("os.exe" if os.name == "nt" else "os")
FAULT_BINARY = ROOT / ("os_fault_tests.exe" if os.name == "nt" else "os_fault_tests")


def program(instructions, priority=70):
    return f"{priority} {len(instructions)}\n" + "\n".join(instructions) + "\n"


class OsDmaTests(unittest.TestCase):
    def run_os(self, programs, *, cpus=1, arrivals=None, config=None,
               binary=BINARY, extra_env=None):
        parent = ROOT / "obj"
        parent.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="os-dma-", dir=parent) as directory:
            work = Path(directory)
            (work / "input" / "proc").mkdir(parents=True)
            arrivals = arrivals or [0] * len(programs)
            for index, text in enumerate(programs):
                (work / "input" / "proc" / f"p{index}").write_text(text)
            if config is None:
                config = f"4 {cpus} {len(programs)}\n4194304 0 0 0 0\n"
                config += "".join(
                    f"{time} p{index} 70\n" for index, time in enumerate(arrivals)
                )
            (work / "input" / "run").write_text(config)
            env = os.environ.copy()
            env.pop("DMA_TRACE", None)
            env.update(extra_env or {})
            return subprocess.run(
                [str(binary), "run"], cwd=work, env=env,
                text=True, capture_output=True, timeout=30,
            )

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout[-4000:] + result.stderr)
        self.assertEqual(result.stderr, "")

    def test_kernel_buffers_and_offsets(self):
        """An unaligned copy crosses pages and leaves both guard bytes alone."""
        text = (ROOT / "input" / "proc" / "dma").read_text()
        expected = [(2, 0), (3, 129), (4095, 254), (4096, 255), (4102, 77), (4103, 0)]
        for cpus in (1, 4):
            with self.subTest(cpus=cpus):
                result = self.run_os([text], cpus=cpus, arrivals=[3])
                self.assert_success(result)
                reads = re.findall(r"READ PID 1 region 1 offset (\d+) value (\d+)", result.stdout)
                self.assertEqual([(int(a), int(b)) for a, b in reads], expected)
                self.assertEqual(result.stdout.count("DMA interrupt:"), 1)
                self.assertIn("status 0 bytes 4100 callbacks 1", result.stdout)
                self.assertIn("region 4 offset 0 value 42", result.stdout)
                self.assertIn("Process 1 finished", result.stdout)

    def test_cpu_progress_and_channel_contention(self):
        """Two owners must finish once each while independent CPU work continues."""
        size = 262144
        instructions = [
            f"alloc {size} 0", f"alloc {size} 1", "alloc 16 2",
            "write 129 0 0", f"write 254 0 {size - 1}",
            f"dma_memcpy 0 1 0 {size}",
        ]
        instructions += ["write 42 2 0", "read 2 0 9"] + ["calc"] * 2048
        instructions += ["read 1 0 9", f"read 1 {size - 1} 9", "free 0", "free 1"]
        result = self.run_os([program(instructions)] * 2, cpus=4)
        self.assert_success(result)
        completions = re.findall(
            r"DMA interrupt: PID (\d+) status 0 bytes 262144 callbacks 1 "
            r"CPU instructions during DMA=(\d+)", result.stdout,
        )
        self.assertEqual(sorted(int(pid) for pid, _ in completions), [1, 2])
        self.assertGreater(sum(int(count) for _, count in completions), 0)
        for pid in (1, 2):
            self.assertIn(f"READ PID {pid} region 1 offset 0 value 129", result.stdout)
            self.assertIn(f"READ PID {pid} region 1 offset {size - 1} value 254", result.stdout)
            self.assertIn(f"Process {pid} finished", result.stdout)
        print("OS DMA completions (PID, active CPU instructions):", completions)

    def test_owner_exits_with_pending_dma(self):
        """A final DMA instruction must retain its process and frames until ACK."""
        text = program(["alloc 262144 0", "alloc 262144 1", "dma_memcpy 0 1 0 262144"])
        result = self.run_os([text], cpus=2)
        self.assert_success(result)
        self.assertEqual(result.stdout.count("DMA interrupt:"), 1)
        self.assertLess(result.stdout.index("DMA interrupt:"), result.stdout.index("Process 1 finished"))

    def test_invalid_instructions(self):
        """Bad syntax, overflow and missing arguments must fail before dispatch."""
        for text in (
            program(["dma_memcpy 0 1 0"]), program(["dma_memcpy 0 1 0 -4"]),
            program(["alloc 18446744073709551616 0"]), program(["calc extra"]),
            program(["unknown"]), "70 4294967296\n", "70 1\n", "-1 0\n",
            "70 0\ncalc\n",
        ):
            with self.subTest(text=text):
                result = self.run_os([text])
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("Dispatched", result.stdout)

    def test_invalid_runtime_arguments(self):
        """Bounds, user/kernel protection and unsupported syscalls return errors."""
        cases = [
            ["alloc 4 0", "alloc 4 1", "dma_memcpy 0 1 0 3"],
            ["alloc 4 0", "alloc 4 1", "dma_memcpy 0 1 1 4"],
            ["kmalloc 4 0", "read 0 0 9"], ["alloc 4194305 0"],
            ["alloc 4 10"], ["syscall 999"],
        ]
        for instructions in cases:
            with self.subTest(instructions=instructions):
                result = self.run_os([program(instructions)])
                self.assertEqual(result.returncode, 1)
                self.assertIn("instruction", result.stderr)
                self.assertNotIn("DMA interrupt:", result.stdout)

    def test_empty_and_legacy_syscall(self):
        """Zero-length programs and syscall 0 retain the starter input syntax."""
        result = self.run_os([program([]), program(["syscall 0"])], cpus=4)
        self.assert_success(result)
        self.assertEqual(result.stdout.count(" finished"), 2)
        self.assertIn("18-sys_dma_memcpy", result.stdout)
        self.assert_success(self.run_os([], cpus=2))

    def test_invalid_configuration(self):
        """Reject wrapped counts, negative arrivals, oversized RAM and extra input."""
        for config in (
            "0 1 0\n4096 0 0 0 0\n", "4 0 0\n4096 0 0 0 0\n",
            "4 4294967297 0\n4096 0 0 0 0\n", "4 1 51\n4096 0 0 0 0\n",
            "4 1 0\n33554433 0 0 0 0\n", "4 1 0\n4096 -1 0 0 0\n",
            "4 1 1\n4096 0 0 0 0\n-1 p0 70\n",
            "4 1 1\n4096 0 0 0 0\n0 p0 140\n",
            "4 1 0\n4096 0 0 0 0\nextra\n",
        ):
            with self.subTest(config=config):
                result = self.run_os([program([])], config=config)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("Dispatched", result.stdout)

    @unittest.skipUnless(FAULT_BINARY.is_file(), "build make os-fault-tests first")
    def test_thread_creation_failures(self):
        """Partial startup must drain published processes and join without hanging."""
        for mask in (1, 2, 4, 8, 24):
            with self.subTest(mask=mask):
                result = self.run_os(
                    [program(["alloc 4 0", "write 42 0 0", "read 0 0 9"])],
                    cpus=2, binary=FAULT_BINARY,
                    extra_env={"OS_TEST_FAIL_THREADS": str(mask)},
                )
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("OS setup failed", result.stderr)
                if mask in (8, 24):
                    self.assertIn("offset 0 value 42", result.stdout)
                    self.assertIn("Process 1 finished", result.stdout)


if __name__ == "__main__":
    unittest.main()
