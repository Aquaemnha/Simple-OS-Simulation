# Build và kiểm thử DMA

Cần GNU make, GCC/G++, pthread, shell tương thích POSIX và Python 3.9+
cho kiểm thử CLI. Mã C dùng GNU C11; C++ dùng C++17 để cấp phát đúng
alignment 64 byte của `Vdma_top`.

Mô hình C++ được cấp đã có trong repo, không cần sinh lại từ RTL. Runtime
đã kiểm thử là [Verilator 5.020](https://github.com/verilator/verilator/tree/v5.020),
tag `v5.020`, commit `5c5314b39cd888f427807d626e1502cbf222c292`.
`VERILATOR_ROOT` phải chứa `include/verilated.h`, `verilated_config.h`
và các tệp runtime `.cpp`. Chưa xác nhận tương thích với phiên bản khác.

## Linux hoặc VM môn học

Nếu runtime 5.020 đã được cài ở `/usr/share/verilator`:

```sh
make OBJ=obj/dma os dma dma-tests dma-cpu-tests -j4
./dma_testbench
python3 -m unittest discover -s tests -p test_dma_cli.py -v
```

Nếu cài ở nơi khác, thêm `VERILATOR_ROOT=/duong/dan/verilator` vào lệnh
make. Các lệnh trên chưa được chạy trên Linux trong phiên này; kết quả
thực tế được lưu là của môi trường Windows dưới đây.

## Windows: môi trường đã chạy

Dùng [w64devkit 2.11.0](https://github.com/skeeto/w64devkit/releases/tag/v2.11.0)
portable, GCC/G++ 16.2.0 và pthread. Bản tải `w64devkit-x64-2.11.0.7z.exe`
có SHA-256:

```text
54c2bb9962e8a30c8270e34c5efc3d5fee579f53087fa7d435b95fbba2af7b2b
```

Trong PowerShell, tại thư mục repo:

```powershell
$env:PATH = 'D:\Personal\Cache\Agents\hk261-dma\toolchain\w64devkit\bin;' + $env:PATH
make SHELL=sh OBJ=obj/review VERILATOR_ROOT=D:/Personal/Cache/Agents/hk261-dma/verilator-v5.020 os dma dma-tests dma-cpu-tests -j4
./dma_testbench.exe
& 'D:\Personal\Projects\KeHoach_DoanVien_2026\.venv\Scripts\python.exe' -m unittest discover -s tests -p test_dma_cli.py -v
```

Đây là các đường dẫn đã dùng trên máy làm bài. Máy khác thay đường dẫn
toolchain, runtime và Python bằng vị trí tương ứng. PATH chỉ đổi trong
phiên shell hiện tại. Khi đổi compiler, dùng thư mục object khác, chẳng
hạn `OBJ=obj/toolchain-moi`, để không trộn object của hai môi trường.

### Nếu lấy runtime trực tiếp từ Git

Bản Verilator đã cài bằng hệ thống build chuẩn có sẵn
`verilated_config.h`. Checkout Git thô chỉ có template `.h.in`.
Trên Windows, có thể lấy phần runtime của đúng tag như sau nếu chưa có:

```powershell
git clone --depth 1 --branch v5.020 --filter=blob:none --sparse https://github.com/verilator/verilator.git D:/Personal/Cache/Agents/hk261-dma/verilator-v5.020
git -C D:/Personal/Cache/Agents/hk261-dma/verilator-v5.020 sparse-checkout set include
$runtime = 'D:\Personal\Cache\Agents\hk261-dma\verilator-v5.020'
$template = [IO.File]::ReadAllText("$runtime\include\verilated_config.h.in")
$config = $template.Replace('@PACKAGE_NAME@', 'Verilator').Replace('@PACKAGE_VERSION@', '5.020 2024-01-01').Replace('@VERILATOR_VERSION_INTEGER@', '5020000')
[IO.File]::WriteAllText("$runtime\include\verilated_config.h", $config, [Text.Encoding]::ASCII)
```

Các giá trị trên dành riêng cho tag 5.020. Đây là phần thay metadata
phiên bản từ template upstream, không sửa hoạt động runtime hoặc mô hình
DMA. Makefile đặt `-DVL_TIME_CONTEXT` để dùng thời gian từ context và
không cần hàm legacy `sc_time_stamp()`.

## Các lệnh kiểm thử có ý nghĩa gì?

| Lệnh/target | Nội dung |
| --- | --- |
| `dma` rồi `./dma_testbench` | Chuyển 512 byte mặc định, so toàn bộ đích và nguồn; in kết quả callback |
| `dma-tests` | Tự build/chạy: biên descriptor, endian, RAM chung CPU/DMA, reservation, 4 người gửi, shutdown và restart |
| `dma-cpu-tests` | Tự build/chạy CPU, libmem, syscall thật và engine; mapper/bảng trang thuộc bài kiểm thử |
| `python -m unittest discover -s tests -p test_dma_cli.py -v` | 3 nhóm: dữ liệu, tham số sai, trace và lỗi mở trace |
| `os` | Biên dịch toàn OS; không tự chạy demo OS và không chứng minh MMU/scheduler đã hoàn thiện |

Standalone còn nhận `source destination length`, là các số nguyên ở hệ
thập phân hoặc có tiền tố `0x`. RAM standalone có 1 MiB; length phải khác
0 và chia hết cho 4, hai buffer không chồng nhau.

```powershell
./dma_testbench.exe 1 8193 4
./dma_testbench.exe 0 1048572 4
$env:DMA_TRACE = 'obj/copy.vcd'
./dma_testbench.exe
Remove-Item Env:\DMA_TRACE
```

Mở VCD bằng trình xem waveform nếu cần. Quan sát `ch_done` và
`ch_bytes_transferred`; chân `irq` bị tắt trong mô hình được cấp.

## Kiểm tra cảnh báo cho phần DMA

Các lệnh sau đã đạt trên toolchain Windows nêu trên:

```powershell
gcc -Wall -Wextra -Werror -std=gnu11 -fsyntax-only -Iinclude src/dma.c src/sysbus.c src/sys_dma.c tests/dma_tests.c tests/dma_cpu_tests.c
g++ -Wall -Wextra -Werror -std=gnu++17 -DVL_TIME_CONTEXT -DDMA_MAIN=1 -fsyntax-only -iquote include -isystem D:/Personal/Cache/Agents/hk261-dma/verilator-v5.020/include -isystem D:/Personal/Cache/Agents/hk261-dma/verilator-v5.020/include/vltstd src/dpi_rtl.cpp src/dma_c_testbench.cpp
```

Header của runtime ngoài dự án được đánh dấu `-isystem` chỉ trong lượt
kiểm tra này. Build thông thường vẫn hiển thị cảnh báo `dllimport` từ
`svdpi.h` và cảnh báo ép con trỏ sang `unsigned long` trong mã CALC của
starter trên Windows 64 bit. Không coi toàn bộ starter là đã sạch cảnh
báo và không tắt cảnh báo chung để che chúng.

## Lỗi môi trường đã xác minh

MinGW GCC 14.2 cũ trên máy chạy được một lần sao chép, nhưng crash khi
hủy đối tượng C++ thread-local lúc pthread kết thúc. Chương trình tối
thiểu chỉ có `pthread_create/join` và `thread_local std::queue<int>`
cũng gặp lỗi. Chương trình tối thiểu và kiểm thử vòng đời DMA đều đạt
với w64devkit; không vá mô hình để né lỗi này.

Testbench cũ còn kéo kiểu `BYTE` của Windows vào xung đột với typedef
của môn học. Đã bỏ các include Verilator không dùng ở testbench;
wrapper và mô hình vẫn liên kết runtime thật.
