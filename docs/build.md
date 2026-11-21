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

This branch is also checked with MinGW-w64 GCC/G++ 14.2.0 (POSIX threads)
and Git Bash on Windows:

```sh
mingw32-make SHELL=sh VERILATOR_ROOT=D:/path/to/verilator-v5.020 dma
./dma_testbench.exe
```

The first baseline run copied 512 bytes from bus offset `0x1000` to `0x2000`
and reached the supplied model's completion callback. That baseline driver
prints only the first eight bytes; later tests must compare the full transfer.
The removed testbench includes were unused and pulled Windows' `BYTE` type
into the course header's `BYTE` typedef.
