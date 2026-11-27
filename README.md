# Simple OS (ENGLISH BELOW)

## Kiến Trúc Tổng Quan (Architecture Overview)
Dự án mô phỏng hệ điều hành đa nhiệm hỗ trợ kiến trúc Multi-CPU (tối đa 50 CPU chạy đồng thời bằng `pthread`), lập lịch đa cấp MLQ (Multi-Level Queue), không gian địa chỉ 64-bit với phân trang 5 cấp (MM64), và co-processor DMA bất đồng bộ tích hợp với mô hình Verilator phần cứng.

## Thiết Kế Vùng Nhớ Phân Loại Theo MacOS (Section 2.2.1)
Thay vì sử dụng thiết kế phân vùng phẳng Linux-style cũ, hệ thống áp dụng cơ chế phân bổ bộ nhớ kiểu MacOS Virtual Memory Regions nhằm tối ưu hoá cho DMA và quản lý phân mảnh:
- **Small Region (VMA 0)**: Dành cho các đối tượng dữ liệu nhỏ ($\le 1024$ bytes).
- **Medium Region (VMA 1)**: Dành cho các khối dữ liệu kích thước trung bình ($1025 - 4096$ bytes).
- **Large Region (VMA 2)**: Dành cho các khối dữ liệu kích thước lớn ($> 4096$ bytes).
- **Kernel Region**: Tách biệt hoàn toàn ở không gian địa chỉ cao (`MM64_KERNEL_BASE = 0xffff800000000000`), bảo vệ nghiêm ngặt quyền truy cập từ User Space thông qua các syscall kiểm tra phân quyền (`copy_from_user`, `copy_to_user`).

## Cơ Chế Tương Tác Bất Đồng Bộ Của DMA Co-Processor (Section 2.3)
Tuân thủ nghiêm ngặt mô hình tương tác I/O chuẩn:
- **Non-blocking Dispatch**: Lệnh `dma_submit` chỉ nạp yêu cầu (PID, source, destination, length), dành trước bus RAM và báo hiệu cho DMA worker thread chạy ngầm, ngay lập tức trả về cho CPU tiếp tục thực thi các chỉ lệnh tiếp theo mà **không block CPU**.
- **CPU Độc Lập Thực Thi**: Trong thời gian DMA đang thực hiện truyền dữ liệu, các CPU vẫn tiếp tục fetch và run hàng nghìn lệnh của các process khác (được xác thực thông qua biến đếm `cpu_instructions` trong kết quả test).
- **Interrupt Handling**: Khi phần cứng DMA hoàn tất, hàm callback `dma_done_callback()` phát tín hiệu ngắt. Tại ranh giới giữa các time slot/chỉ lệnh, CPU gọi `handle_dma_interrupt()` để xử lý sự kiện ngắt và giải phóng tài nguyên. Hệ thống không sử dụng bất kỳ cơ chế "cheat" đồng bộ nào làm treo CPU.

## Cơ Chế Đồng Bộ & Phòng Ngừa Race Condition (Multi-CPU Synchronization)
Toàn bộ các tài nguyên dùng chung trong hệ thống được bảo vệ bằng các khóa độc lập:
- `queue_lock`: Đồng bộ truy cập hàng đợi MLQ khi nhiều CPU đồng thời lấy (`get_proc`) hoặc trả (`put_proc`) process.
- `mmvm_lock`: Bảo vệ toàn vẹn bảng phân trang 5 cấp, bảng process registry, danh sách khung trang (`free_fp_list`, `used_fp_list`).
- `memory_lock`: Bảo vệ quyền truy cập đọc/ghi vật lý vào RAM bus và các dải địa chỉ vật lý đang dành riêng cho DMA, chống xung đột giữa CPU bus và DMA bus.
- `channel_lock`: Quản lý vòng đời trạng thái kênh DMA (`DMA_IDLE`, `DMA_QUEUED`, `DMA_RUNNING`, `DMA_COMPLETE`) giữa CPU thread và DMA worker thread.
- Các mutex được giải phóng ngay sau vùng tranh chấp, tuân thủ thứ tự lock phân cấp nhằm ngăn chặn hoàn toàn nguy cơ Deadlock.

## Hướng Dẫn Biên Dịch & Chạy Kiểm Thử
- **Biên dịch hệ điều hành và DMA testbench**:
  ```sh
  make os dma
  ```
- **Chạy toàn bộ test suite (Python Unittest & C Testbench)**:
  ```sh
  python3 -m unittest discover -s tests -p 'test_*.py' -v
  make dma-cpu-tests
  ```
- **Chạy mô phỏng hệ điều hành với cấu hình mẫu**:
  ```sh
  ./os os_dma
  ./os os_1_singleCPU_mlq_paging
  ```

# README.md
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