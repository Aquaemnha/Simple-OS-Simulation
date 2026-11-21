# Simple OS

Operating system simulator for CO2018, HK261, based on the supplied
`hk261_ossim_demacOprx` starter. The DMA driver runs the provided Verilator
model on a pthread and shares simulated RAM with CPU memory accesses.

## Build

Requires GNU make, GCC/G++, pthread, a POSIX shell and Verilator 5.020
runtime headers and sources. C uses GNU C11; C++ uses C++17.

Set `VERILATOR_ROOT` to the runtime directory, then run:

```sh
make os dma
./os os_dma
./dma_testbench
./dma_testbench 1 8193 4
./dma_testbench 0 1048572 4
```

On Windows, use w64devkit 2.11.0 (GCC 16.2.0) and `make SHELL=sh os dma`.
Executables have the `.exe` suffix. Use a new object directory when changing
compilers, for example `make SHELL=sh OBJ=obj/build os dma`.

## Tests

```sh
make dma-tests dma-cpu-tests runtime-tests os-fault-tests
python -m unittest discover -s tests -p 'test_*.py' -v
```

The CPU test uses the five-level MM64 page tables, PID registry and syscall
path. It checks process isolation, user/kernel permissions, contiguous DMA
spans, RAM exhaustion and CPU progress before the real model reports completion.
The OS tests run process files on one or more CPUs, check copied bytes and
completion counts, and inject thread-creation failures. `os_fault_tests` is a
separate test executable; the normal OS has no fault-injection code.

## DMA demo

`input/proc/dma` copies a user buffer into kernel memory, submits
`dma_memcpy 2 3 3 4100`, performs independent memory operations, then copies
the result back to user memory. The expected destination values are 129, 254,
255 and 77 at offsets 3, 4095, 4096 and 4102. The guard bytes at offsets 2
and 4103 stay zero. Completion reports status 0, 4100 bytes and one callback.

CPU instructions during DMA are counted from worker execution until the
model's `ch_done` callback. This count varies with host thread scheduling;
it is not a speedup measurement. The CPU handles the retained completion
between instructions. The supplied model disables its physical `irq` pin;
notification uses its existing DPI callback.

## Current limits

- One DMA channel and one retained completion. A busy instruction is retried
  without advancing its program counter; independent processes can run.
- Transfers require nonoverlapping, resident, physically contiguous spans.
  Length must be a nonzero multiple of four; unaligned byte addresses work.
  DMA buffers stay pinned until the engine stops accessing them.
- MM64 uses 4 KiB pages and the starter's 13-bit frame field, limiting mapped
  RAM to 32 MiB. Missing/swapped pages and RAM exhaustion return errors;
  page replacement is not implemented. Kernel virtual holes are not reused.
- Configurations support up to 50 CPUs and 50 processes, with nondecreasing
  arrival times. Runtime priority comes from the configuration file.

MMU output reports actual translation calls, page-table entry reads and
currently allocated table bytes. Kernel table storage is shared; per-process
table bytes exclude that shared branch. These are simulator counters, not
hardware cache-miss or timing measurements.

## License

See [LICENSE](LICENSE) for the course's educational-use terms and
[README.course.md](README.course.md) for the original starter instructions.
