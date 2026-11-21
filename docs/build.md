# Build environment

The supplied generated DMA model builds with the official Verilator 5.020
runtime (tag `v5.020`, commit `5c5314b39cd888f427807d626e1502cbf222c292`).
The model is supplied as C++; regenerating it is not required.

Linux/course VM:

```sh
make dma
./dma_testbench
```

Set `VERILATOR_ROOT=/path/to/verilator` if the runtime is outside
`/usr/share/verilator`. It must contain `include/verilated.h` and
`include/verilated_config.h`.

On Windows, use the portable w64devkit 2.11.0 toolchain (GCC/G++ 16.2.0,
POSIX threads). Keep its `bin` first on the shell's PATH:

```sh
make SHELL=sh VERILATOR_ROOT=D:/path/to/verilator-v5.020 dma dma-tests
./dma_testbench.exe
```

The standalone driver compares every transferred byte and checks that the
source is unchanged. `dma-tests` checks descriptor errors, shared CPU/DMA
RAM, concurrent submission, retained completion, shutdown and restart.

The older installed MinGW GCC 14.2 toolchain passed a single copy but crashed
when destroying C++ thread-local objects at thread exit. A separate program
containing only `pthread_create/join` and a `thread_local std::queue<int>`
reproduced the failure. That program and the DMA lifecycle tests passed with
w64devkit. No generated model or Verilator runtime behavior was patched.

When switching compilers, use a separate object directory, for example
`OBJ=obj/w64devkit`, rather than mixing objects from different toolchains.

The removed testbench includes were unused and pulled Windows' `BYTE` type
into the course header's `BYTE` typedef.
