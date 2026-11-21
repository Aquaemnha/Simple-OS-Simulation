# Simple-OS-Simulation
Simple OS simulation — MLQ scheduling, 64-bit paging, user/kernel protection, and asynchronous DMA with Verilator.

## DMA branch

The DMA driver runs the supplied Verilator engine on a separate pthread,
sharing simulated RAM with CPU memory accesses. The driver and CPU/syscall
tests pass; the full OS still needs the group's MMU mapper and remaining
scheduler/MM work before an end-to-end demonstration.

- [Vietnamese explanation and integration contract](docs/dma.md)
- [Word handoff](docs/DMA-Co-processor.docx)
- [Build and test commands](docs/build.md)
- [Recorded results and limitations](docs/dma-results.txt)
- [Raw verification evidence and rerun command](docs/verification.md)
- [Assignment requirements: verified and still missing](docs/requirements.md)
- [Course starter provenance](docs/starter.md)
