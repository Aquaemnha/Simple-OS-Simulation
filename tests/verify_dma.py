"""Build and run DMA checks, preserving raw evidence, including failures."""

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def trace_summary(path):
    """Read actual VCD changes; do not infer an IRQ from the DPI callback."""
    names = {"ch_done", "irq", "ch_bytes_transferred"}
    symbols = {}
    values = {}
    maxima = {}
    irq_values = set()
    done_edges = 0
    for line in path.read_text(encoding="ascii").splitlines():
        line = line.strip()
        if line.startswith("$var "):
            fields = line.split()
            if fields[4] in names:
                symbols[fields[3]] = fields[4]
            continue
        if not line or line.startswith(("$", "#")):
            continue
        if line[0] in "01xz":
            bits, symbol = line[0], line[1:]
        elif line[0] == "b":
            bits, symbol = line[1:].split()
        else:
            continue
        name = symbols.get(symbol)
        if name is None or any(c not in "01" for c in bits):
            continue
        value = int(bits, 2)
        if name == "ch_done" and values.get(symbol) == 0 and value == 1:
            done_edges += 1
        if name == "irq":
            irq_values.add(value)
        values[symbol] = value
        maxima[name] = max(maxima.get(name, 0), value)
    if set(symbols.values()) != names:
        raise ValueError("Required top-level signals are missing from VCD")
    return {
        "ch_done_rising_edges": done_edges,
        "max_ch_bytes_transferred": maxima.get("ch_bytes_transferred"),
        "irq_observed_values": sorted(irq_values),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verilator-root", type=Path, required=True)
    parser.add_argument("--toolchain-bin", type=Path)
    parser.add_argument("--assignment", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = (args.output or ROOT / "docs/verification" / stamp).resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.pop("DMA_TRACE", None)
    if args.toolchain_bin:
        env["PATH"] = str(args.toolchain_bin.resolve()) + os.pathsep + env["PATH"]
    runtime = args.verilator_root.resolve()
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=ROOT
    ).decode("utf-8").split("\0")
    source_names = {
        name for name in tracked if name and (
            Path(name).suffix in {".c", ".cpp", ".h", ".py", ".sh", ".tbl"}
            or name == "Makefile" or name.startswith("input/")
        )
    }
    source_names.add("tests/verify_dma.py")
    manifest = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "tested_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "worktree_status_at_start": subprocess.check_output(
            ["git", "status", "--short"], cwd=ROOT, text=True
        ),
        "platform": platform.platform(),
        "python": sys.version,
        "runtime_root": str(runtime),
        "toolchain_bin": str(args.toolchain_bin.resolve())
        if args.toolchain_bin else None,
        "sources_sha256": {
            name: sha256(ROOT / name) for name in sorted(source_names)
        },
        "commands": [],
        "scope": "DMA checks; this runner does not certify the full BTL",
    }
    if args.assignment:
        manifest["assignment_sha256"] = sha256(args.assignment)

    def save():
        (output / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def run(label, argv, timeout=60, extra_env=None):
        """Keep stdout/stderr bytes and actual exit/timeout status separately."""
        child_env = env.copy()
        if extra_env:
            child_env.update(extra_env)
        options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} \
            if os.name == "nt" else {"start_new_session": True}
        result = {"label": label, "argv": argv, "timeout_s": timeout}
        # Windows resolves the executable before applying the child's PATH.
        executable = shutil.which(argv[0], path=child_env["PATH"])
        result["resolved_executable"] = executable
        begin = time.perf_counter()
        try:
            if executable is None:
                raise FileNotFoundError(f"Executable not found: {argv[0]}")
            process = subprocess.Popen(
                [executable, *argv[1:]], cwd=ROOT, env=child_env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, **options,
            )
        except OSError as error:
            stdout, stderr = b"", str(error).encode("utf-8")
            result.update(returncode=None, timed_out=False, launch_error=True)
        else:
            timed_out = False
            try:
                stdout, stderr = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                if os.name == "nt":
                    subprocess.run(
                        ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                        capture_output=True, check=False,
                    )
                else:
                    os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate()
            result.update(returncode=process.returncode, timed_out=timed_out)
        result["elapsed_wall_s"] = round(time.perf_counter() - begin, 6)
        result["stdout"] = f"{label}.stdout.txt"
        result["stderr"] = f"{label}.stderr.txt"
        (output / result["stdout"]).write_bytes(stdout)
        (output / result["stderr"]).write_bytes(stderr)
        result["ok"] = result["returncode"] == 0 and not result["timed_out"]
        manifest["commands"].append(result)
        save()
        print(f'{label}: exit={result["returncode"]} '
              f'timeout={result["timed_out"]} '
              f'wall={result["elapsed_wall_s"]}s', flush=True)
        return result

    def text_of(result):
        return (output / result["stdout"]).read_text(
            encoding="utf-8", errors="replace"
        )

    save()
    run("01-gcc-version", ["gcc", "--version"])
    run("02-gxx-version", ["g++", "--version"])
    run("03-make-version", ["make", "--version"])
    run("04-model-unchanged", [
        "git", "diff", "--exit-code", "f754033", "--",
        "src/Vdma_top*", "include/Vdma_top*",
    ])
    run("05-c-warnings", [
        "gcc", "-Wall", "-Wextra", "-Werror", "-std=gnu11",
        "-fsyntax-only", "-Iinclude", "src/dma.c", "src/sysbus.c",
        "src/sys_dma.c", "tests/dma_tests.c", "tests/dma_cpu_tests.c",
    ])
    run("06-cpp-warnings", [
        "g++", "-Wall", "-Wextra", "-Werror", "-std=gnu++17",
        "-DVL_TIME_CONTEXT", "-DDMA_MAIN=1", "-fsyntax-only",
        "-iquote", "include", "-isystem", (runtime / "include").as_posix(),
        "-isystem", (runtime / "include/vltstd").as_posix(),
        "src/dpi_rtl.cpp", "src/dma_c_testbench.cpp",
    ])
    make = ["make", "SHELL=sh", f"OBJ=obj/verify-{stamp}",
            f"VERILATOR_ROOT={runtime.as_posix()}"]
    build = run("07-fresh-build", make + ["os", "dma", "-j4"], timeout=180)
    if build["ok"]:
        run("08-dma-tests", make + ["dma-tests"], timeout=60)
        cpu = run("09-cpu-tests", make + ["dma-cpu-tests"], timeout=60)
        run("10-cli-tests", [sys.executable, "-m", "unittest", "discover",
                             "-s", "tests", "-p", "test_dma_cli.py", "-v"])
        suffix = ".exe" if os.name == "nt" else ""
        binary = str(ROOT / f"dma_testbench{suffix}")
        for label, inputs in [
            ("11-default-copy", []),
            ("12-unaligned-copy", ["1", "8193", "4"]),
            ("13-last-word-copy", ["0", "1048572", "4"]),
        ]:
            run(label, [binary, *inputs])
        trace = output / "copy.vcd"
        traced = run("14-traced-copy", [binary],
                     extra_env={"DMA_TRACE": str(trace)})
        if traced["ok"] and trace.exists():
            manifest["vcd"] = trace_summary(trace)
            manifest["vcd"]["raw_sha256"] = sha256(trace)
            (output / "copy.vcd.gz").write_bytes(
                gzip.compress(trace.read_bytes(), mtime=0)
            )
            trace.unlink()
            save()
        match = re.search(
            r"CALC instructions=(\d+), independent RAM checksum=(\d+)",
            text_of(cpu),
        )
        if match:
            manifest["cpu_test_observations"] = {
                "calc_instructions_after_submit": int(match[1]),
                "independent_ram_checksum": int(match[2]),
                "interpretation": "Not a speedup or hardware-overlap benchmark",
            }
        run("15-os-sample", [str(ROOT / f"os{suffix}"),
                             "os_1_mlq_paging_small_4K"], timeout=5)
        manifest["binaries_sha256"] = {
            name + suffix: sha256(ROOT / (name + suffix))
            for name in ["os", "dma_testbench", "dma_tests", "dma_cpu_tests"]
        }
    else:
        manifest["skipped_runtime_checks"] = "Fresh build failed; no stale binary run"
    manifest["dma_checks_passed"] = (
        all(c["ok"] for c in manifest["commands"]
            if c["label"] != "15-os-sample")
        and manifest.get("vcd", {}).get("ch_done_rising_edges") == 1
        and manifest.get("vcd", {}).get("max_ch_bytes_transferred") == 512
    )
    manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
    manifest["artifacts_sha256"] = {
        path.name: sha256(path) for path in sorted(output.iterdir())
        if path.name != "manifest.json"
    }
    save()
    print(f"Evidence: {output}", flush=True)
    all_ok = manifest["dma_checks_passed"] and all(
        c["ok"] for c in manifest["commands"]
    )
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
