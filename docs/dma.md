# Co-processor và DMA

Phần DMA chạy bằng mô hình Verilator được cấp trong đề HK261. CPU gửi yêu cầu
sao chép rồi tiếp tục chạy; một pthread riêng điều khiển mô hình DMA. Khi
truyền xong, callback của mô hình ghi nhận sự kiện để CPU xử lý kết quả.

**Trạng thái bàn giao:** driver, bus RAM, syscall theo PID và các bài kiểm
thử DMA đã chạy được. Toàn bộ OS biên dịch được. Đường dịch địa chỉ qua
MMU thật của nhóm còn cần nối vào `krnl_t::dma_map_buffers`; hiện OS chưa
gắn hàm này và trả `-ENOSYS` khi gọi DMA. Kiểm thử CPU dùng bảng ánh xạ
riêng của bài kiểm thử. Bản này chưa phải demo toàn OS hoàn chỉnh.

Bản Word: [DMA-Co-processor.docx](DMA-Co-processor.docx).
Cách build: [build.md](build.md). Kết quả chạy: [dma-results.txt](dma-results.txt).

## 1. Hiểu luồng chạy

Ví dụ CPU cần chuyển một khối dữ liệu từ vùng A sang vùng B. CPU chỉ gửi
vị trí nguồn, vị trí đích và số byte. Sau đó CPU có thể tính toán hoặc làm
việc với vùng nhớ khác. DMA tự đọc và ghi RAM qua bus.

Khi xong, CPU lấy kết quả hoàn tất và xác nhận đã nhận, gọi là ACK. Việc
gửi DMA không gọi `pthread_join` hay chờ cả khối được sao chép. `join` chỉ
được dùng khi tắt dịch vụ, trước khi giải phóng RAM.

```mermaid
sequenceDiagram
    participant CPU
    participant Kernel as Syscall và MMU
    participant Worker as Luồng DMA
    participant RAM as Mô hình Verilator và RAM
    CPU->>Kernel: PID, vùng nguồn, vùng đích, offset, size
    Kernel->>Kernel: Kiểm tra quyền, dịch sang địa chỉ bus
    Kernel->>Worker: dma_submit
    Kernel-->>CPU: Trả trạng thái nhận yêu cầu
    par Công việc độc lập
        CPU->>CPU: Chạy các lệnh tiếp theo
    and Truyền dữ liệu
        Worker->>RAM: Descriptor, clock, đọc/ghi qua DPI
        RAM-->>Worker: Callback khi ch_done lên
    end
    CPU->>Worker: Xử lý sự kiện hoàn tất giữa các lệnh
    Worker-->>CPU: PID, status, số byte, số callback; ACK
```

Trong bản mô phỏng, callback đặt sự kiện; CPU kiểm tra sự kiện ở ranh giới
giữa hai lệnh. Callback không chạy mã CPU trực tiếp và không dùng POSIX
signal. Đây là cơ chế sự kiện hoàn tất của simulator, cần phân biệt với
ngắt phần cứng trên máy thật.

Mô hình được cấp nối `intr_en=0`, nên chân `irq` không được bật. Sự kiện
hoàn tất hiện dùng callback DPI ở cạnh lên của `ch_done`. Không dùng log
`interrupt` để kết luận chân `irq` đã lên mức 1.

## 2. Kiến thức và yêu cầu dùng để hiện thực

Số trang dưới đây là số trang trong trình đọc PDF, không nhất thiết trùng
với số slide in ở chân trang.

| Nguồn trong thư mục môn học | Nội dung áp dụng | Nơi hiện thực |
| --- | --- | --- |
| `hk261_ossim_demacOprx.pdf`, tr. 21–23 | CPU và thiết bị hoạt động bất đồng bộ; xử lý hoàn tất qua interrupt/handler | `src/dma.c`, `src/os.c` |
| Cùng đề, tr. 23 | Địa chỉ bus 64 bit, dữ liệu 32 bit; xử lý từng byte để rõ endianness | `src/sysbus.c` |
| Cùng đề, tr. 24–26 | Bảo vệ tài nguyên chung; syscall theo GNU C; truyền PID qua ranh giới user/kernel | `src/sys_dma.c`, `src/libmem.c` |
| Cùng đề, tr. 26 và 33 | Demo phải dùng DMA engine được cấp, prototype phần mềm chỉ dùng bước đầu | `src/dpi_rtl.cpp`, các tệp `Vdma_top*` được giữ nguyên |
| Cùng đề, tr. 27 | Co-processor gồm bất đồng bộ, CPU qua MMU và DMA truy cập RAM | Kiểm thử driver đạt; CPU qua MMU thật còn cần ghép |
| `ch1_EN_BK_w_come_withComment.pdf`, tr. 21–25, 31 | Interrupt-driven I/O và DMA truyền theo khối | Luồng gửi yêu cầu và nhận hoàn tất |
| `ch4_EN_BK_Threads.pdf`, tr. 21–24 | POSIX thread, tạo và kết thúc luồng | Một worker sở hữu mô hình |
| `ch7_EN_BK_sync2.pdf`, tr. 20–29 | Mutex, condition variable; kiểm tra điều kiện trong vòng `while` | Khóa trạng thái kênh và đánh thức worker |
| `ch9_virMem.pdf`, tr. 71, slide in số 78 | I/O interlock: giữ trang đang I/O khỏi bị thay thế | Reservation và kiểm tra frame đang dùng |

Hai ảnh email và `Screenshot 2026-11-22 003915.png` trong thư mục môn học
nhắc lại: không chặn toàn CPU đến khi DMA xong. Khóa vẫn dùng để bảo vệ
tài nguyên chung hoặc công việc phụ thuộc kết quả DMA. Vì vậy worker nhả
khóa trạng thái trước khi chạy mô hình; khóa RAM chỉ giữ trong từng lần
truy cập ngắn.

Định dạng đối chiếu với [GNU Coding Standards – Writing C](https://www.gnu.org/prep/standards/html_node/Writing-C.html).
Các hàm mới dùng tên có dấu gạch dưới, thụt 2 khoảng trắng, có khoảng
trắng trước dấu `(` và đặt tên hàm ở đầu dòng khi định nghĩa. Phần sửa
nhỏ trong starter giữ cách trình bày sẵn có của tệp.

## 3. Những tệp cần đọc

| Tệp | Công việc |
| --- | --- |
| `include/dma.h`, `src/dma.c` | Nhận descriptor, quản lý một kênh, worker, kết quả hoàn tất |
| `include/sysbus.h`, `src/sysbus.c` | Gắn RAM thật của simulator, giới hạn địa chỉ, ghép/tách word theo byte, bảo vệ buffer |
| `src/dpi_rtl.cpp` | Khởi tạo mô hình, reset, nạp descriptor, cấp clock, timeout, VCD |
| `src/sys_dma.c`, `src/syscall.tbl` | Syscall số 18: kiểm tra tham số, gọi mapper và gửi DMA |
| `src/libmem.c`, `src/cpu.c` | Lệnh `DMA_MEMCPY` đi qua thư viện rồi syscall; giữ nguyên tham số 64 bit |
| `src/os.c` | Khởi động DMA sau khi có RAM, xử lý hoàn tất giữa các lệnh, tắt worker khi kết thúc |
| `src/mm-memphy.c` | CPU và DMA dùng cùng storage; từ chối trả frame đang được DMA giữ |
| `tests/dma_tests.c` | Biên địa chỉ, RAM chung, nhiều người gửi, vòng đời dịch vụ |
| `tests/dma_cpu_tests.c` | Đường CPU → syscall → mapper kiểm thử → engine → RAM |
| `tests/test_dma_cli.py` | Dữ liệu đầy đủ, tham số sai, tạo VCD và lỗi mở VCD |

### Trạng thái một kênh

`IDLE → QUEUED → RUNNING → COMPLETE → IDLE`.

- `IDLE`: có thể nhận yêu cầu mới.
- `QUEUED`: descriptor đã nhận, worker chưa bắt đầu truyền.
- `RUNNING`: worker đang điều khiển mô hình; CPU vẫn chạy công việc độc lập.
- `COMPLETE`: kết quả được giữ lại; CPU lấy kết quả bằng
  `dma_take_completion` để ACK và trả kênh về `IDLE`.

Khi kết quả chưa được ACK, gửi tiếp vẫn trả `-EBUSY`. Nhờ vậy yêu cầu mới
không ghi đè kết quả của PID trước. Shutdown xử lý nốt yêu cầu đã nhận,
join worker rồi mới cho phép thu hồi RAM.

### Ba loại địa chỉ cần phân biệt

| Loại | Ý nghĩa | Ví dụ trong bài kiểm thử CPU |
| --- | --- | --- |
| Địa chỉ ảo | Địa chỉ trong không gian của tiến trình | Nguồn `0x100001000`, đích `0x200003000` |
| Địa chỉ bus | Offset trong RAM mô phỏng, do kernel dịch ra | Nguồn `0`, đích `524288` |
| Con trỏ host | Địa chỉ C/C++ của vùng storage trên máy chạy simulator | Chỉ dùng nội bộ, không đưa vào descriptor |

Địa chỉ ảo lớn hơn 4 GiB trong test giúp bắt lỗi cắt mất 32 bit cao. RAM
test chỉ có 1 MiB; test này không chứng minh đã truy cập RAM vật lý lớn
hơn 4 GiB. Descriptor vẫn giữ nguồn và đích bằng `uint64_t`.

Mỗi lần DMA đọc/ghi 32 bit, bus ghép/tách 4 byte theo little-endian.
Ví dụ các byte `81 23 45 F7` tạo word `0xF7452381`. Không ép một con trỏ
byte lệch hàng thành `uint32_t *`.

## 4. Giao diện và giới hạn

`libmem_dma_memcpy(caller, src, dst, offset, size)` nhận **ID vùng nhớ**
nguồn/đích; cùng `offset` được áp dụng vào đầu của cả hai vùng. Nó gọi
syscall bằng kernel, PID và thanh ghi tham số. PCB không được truyền
sang hàm syscall.

`dma_submit(pid, source_bus, destination_bus, length)` là giao diện bên
kernel, nhận địa chỉ bus đã dịch. Hàm trả 0 khi đã nhận descriptor; kết
quả sao chép đến sau trong `struct dma_result`.

| Trường hợp | Kết quả |
| --- | --- |
| Chưa khởi động dịch vụ | `-ENODEV` |
| Đang có yêu cầu hoặc kết quả chưa ACK | `-EBUSY` |
| Độ dài bằng 0, không chia hết cho 4, lớn hơn `UINT32_MAX` | `-EINVAL` |
| Hai buffer chồng nhau | `-EINVAL` |
| Địa chỉ/độ dài vượt RAM | `-EFAULT` |
| Chưa có mapper MMU | `-ENOSYS` |
| Mô hình lỗi hoặc số callback/số byte sai | `-EIO` |
| Mô hình không hoàn tất trong giới hạn clock | `-ETIMEDOUT` |

Độ dài phải chia hết cho 4 vì mô hình được cấp truyền word 32 bit và
không có byte strobe cho phần dư. Địa chỉ đầu không bắt buộc căn 4 byte;
trường hợp lệch 1 byte đã được thử với engine. Driver không thay việc
truyền của mô hình bằng `memcpy` phần mềm.

Trong khi DMA còn giữ buffer, CPU được đọc nguồn và truy cập vùng RAM
độc lập. Ghi nguồn/đích và đọc đích trả `-EBUSY`. Caller phải xử lý mã
này; không coi một lần đọc lỗi là dữ liệu hợp lệ. Alloc/free của PID có
yêu cầu chưa ACK cũng trả busy để giữ vùng nhớ sống. Đây là chính sách
giữ bộ nhớ đơn giản cho một kênh, có thể hạn chế thao tác cùng PID.

## 5. Phần cần ghép với thành viên bộ nhớ

Trường `krnl_t::dma_map_buffers` trong `include/common.h` là chỗ nhóm
bộ nhớ nối vào. Hàm này nhận kernel, PID, hai ID vùng, offset, độ dài
và hai con trỏ đầu ra chứa địa chỉ bus. `src/sys_dma.c` chỉ gọi
`dma_submit` sau khi mapper trả 0.

Mapper thật cần làm đủ các bước sau:

1. Tra tiến trình bằng PID từ dữ liệu do kernel quản lý; kiểm tra PID
   còn hợp lệ và hai vùng thuộc đúng tiến trình.
2. Kiểm tra offset/size nằm trong cả hai vùng, quyền đọc nguồn và ghi
   đích, cùng quy tắc tách user/kernel của nhóm. Không cho một yêu cầu
   từ user truy cập tùy ý vùng kernel.
3. Dịch từng trang bằng bảng trang nhiều cấp thật. Xác nhận trang đang
   ở RAM; bản driver hiện tại yêu cầu mỗi buffer liên tục về địa chỉ
   vật lý. Nếu frame rời rạc, trả lỗi hoặc bổ sung thiết kế tách descriptor
   sau khi nhóm thống nhất. Hiện chưa có scatter/gather.
4. Trả offset RAM của hai buffer. Không dùng ID vùng, địa chỉ ảo hoặc
   con trỏ host thay cho kết quả dịch.
5. Gắn mapper vào `os.dma_map_buffers` trước khi khởi chạy các CPU.
   `tests/dma_cpu_tests.c` minh họa hợp đồng bằng dữ liệu kiểm thử;
   không chép bảng trang kiểm thử vào OS để thay MMU thật.

Syscall giữ `mmvm_lock` từ lúc gọi mapper đến khi đã đặt reservation.
Mapper **không khóa lại** mutex này và không gọi wrapper cấp phát tự
khóa nó, vì mutex không đệ quy. Allocator, free và page replacement cần
cùng tuân theo khóa này. Thứ tự khóa là MM → trạng thái DMA → RAM.

Khi chọn frame để thay thế hoặc thu hồi, kiểm tra
`sysbus_range_pinned(ram, address, length)` và bỏ qua frame đang dùng.
`MEMPHY_put_freefp` đã trả `-EBUSY` cho frame này; bên gọi phải xử lý
lỗi trước khi sửa bảng trang hoặc đưa frame vào danh sách tự do. Không
được giải phóng trực tiếp storage trong lúc dịch vụ còn chạy.

Ảnh email khuyến khích tổ chức vùng theo nhóm kích thước small/medium/large
kiểu macOS. Mapper làm việc trên vùng nhớ mà nhóm MM thiết kế; driver
không tự áp một layout của học kỳ khác.

## 6. Đã kiểm thử những gì?

Môi trường đã chạy: Windows, w64devkit 2.11.0 (GCC/G++ 16.2.0, pthread),
runtime Verilator 5.020; C11 và C++17. Các tệp generated `Vdma_top*`
giữ nguyên so với starter. C++17 bảo đảm cấp phát đúng alignment 64 byte
của lớp mô hình.

| Kiểm tra | Kết quả và phạm vi |
| --- | --- |
| Build `os`, `dma`, `dma-tests`, `dma-cpu-tests` | Đạt, kể cả build song song từ thư mục object mới |
| Descriptor | Từ chối độ dài sai, tràn 64 bit, vượt RAM, chồng buffer; địa chỉ trên 4 GiB bị từ chối khi RAM test chỉ có 1 MiB |
| Dữ liệu | Đạt với byte có bit cao, địa chỉ lệch hàng, word cuối RAM, khối 256 KiB; so toàn bộ đích và nguồn |
| RAM chung và vòng đời | Đạt với CPU MEMPHY, reservation, frame đang dùng, shutdown đang truyền và khởi động lại |
| Nhiều người gửi | 4 pthread gửi cùng lúc; đúng 1 yêu cầu được nhận; kết quả giữ đến ACK |
| CPU/syscall | Đạt với CPU và syscall thật; mapper là dữ liệu kiểm thử, có địa chỉ ảo trên 4 GiB, quyền trang và frame rời rạc |
| CLI/VCD | 3 nhóm unittest đạt; có kiểm tra thư mục VCD không tồn tại |
| Coding style/cảnh báo | Các tệp C/C++ của phần DMA qua `-Wall -Wextra -Werror` khi kiểm tra cú pháp; header runtime bên ngoài được đánh dấu system trong lượt kiểm tra này |
| Chạy toàn OS | Mẫu `os_1_mlq_paging_small_4K` chưa kết thúc trong 3 giây thử; chưa có kết quả end-to-end đạt |

Một lần chạy standalone chuyển 512 byte, nhận đúng 1 callback, báo
`bytes=512`, kiểm tra dữ liệu đầy đủ đạt. VCD ghi một cạnh lên `ch_done`
và bộ đếm đạt 512. Test CPU chuyển 256 KiB và đã chạy 65.536 lệnh CALC
sau lệnh gửi DMA trong lần chạy được lưu. Các số vòng CPU phụ thuộc
lịch chạy của máy, không dùng làm phép đo tốc độ hay mức tăng hiệu năng.

Log có dòng `DMA submit: PID 41 status -22`: đây là trường hợp cố ý
gửi size vượt 32 bit để kiểm tra từ chối tham số.

Starter còn các TODO ở queue/scheduler, thao tác bảng trang MM64 và
một số hàm đọc/ghi vùng ảo. Vì vậy build OS thành công chưa đủ chứng
minh yêu cầu CPU qua MMU thật trong thang điểm trang 27. Nhóm phải ghép
các phần và chạy lại demo chung trước khi nộp.

## 7. Chạy demo và bàn giao

Làm theo [build.md](build.md), rồi chạy:

```sh
./dma_testbench
./dma_testbench 1 8193 4
./dma_testbench 0 1048572 4
python -m unittest discover -s tests -p test_dma_cli.py -v
```

Trên PowerShell, dùng `./dma_testbench.exe`; thay `python` bằng đường dẫn
Python nếu máy chưa đặt PATH. Không cần viết chương trình CPU đầu vào
cho demo standalone.

Khi trình bày, mở `src/dma.c` để chỉ ra: submit nhận descriptor rồi trả
về; worker nhả khóa trước khi cấp clock; callback giữ sự kiện; CPU ACK
kết quả. Sau đó mở `src/sysbus.c` và kết quả kiểm thử để giải thích địa
chỉ bus, RAM chung và byte order. Cuối cùng chỉ rõ mapper đang là phần
cần ghép trong OS.

Buổi ghép với thành viên 2/3/5 nên chốt: chữ ký mapper và thứ tự khóa;
chính sách trang liên tục/pinning; cách CPU xử lý `-EBUSY`; rồi chạy
chung một tiến trình gửi DMA, một tiến trình tính toán và một trường
hợp truy cập vùng nhớ không được phép. Chỉ đưa kết quả vào báo cáo cuối
khi đã chạy đúng MMU của nhóm.
