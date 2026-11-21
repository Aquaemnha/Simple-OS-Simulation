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
./dma_testbench
./dma_testbench 1 8193 4
./dma_testbench 0 1048572 4
```

On Windows, use w64devkit 2.11.0 (GCC 16.2.0) and `make SHELL=sh os dma`.
Executables have the `.exe` suffix. Use a new object directory when changing
compilers, for example `make SHELL=sh OBJ=obj/build os dma`.

## Tests

```sh
make dma-tests dma-cpu-tests
python -m unittest discover -s tests -p test_dma_cli.py -v
```

The CPU test supplies its own memory mapper. The OS DMA syscall requires
`krnl_t::dma_map_buffers` and returns `-ENOSYS` until a mapper is registered.
Scheduler and MMU code still contain unimplemented starter sections.

## License

See [LICENSE](LICENSE) for the course's educational-use terms and
[README.course.md](README.course.md) for the original starter instructions.
