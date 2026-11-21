# Hồ sơ chạy kiểm thử DMA

Ngày tại Việt Nam: 22/11/2026. Các timestamp trong manifest dùng UTC;
`2026-11-21T19:28:39Z` tương ứng khoảng 02:28:39 ngày 22/11 ở UTC+7.

**Chưa đạt toàn bộ BTL.** [Bảng yêu cầu](requirements.md) phân biệt phần
DMA đã kiểm thử với các chức năng OS còn thiếu.

## Lượt chạy đã lưu

| Lượt | Kết quả |
| --- | --- |
| [19:28:05 UTC](verification/20261121T192805117504Z/manifest.json) | Trình xác minh ban đầu chọn nhầm GCC 14.2 theo PATH của tiến trình cha và không tìm thấy make. Build không chạy; không chạy binary cũ để thay thế. Giữ nguyên log lỗi. |
| [19:28:39 UTC](verification/20261121T192839434814Z/manifest.json) | Sau khi sửa cách chọn executable trên Windows: 14 lệnh đầu trả 0; mẫu OS ở lệnh 15 bị timeout 5 giây. DMA đạt các kiểm tra đã chạy; toàn OS chưa đạt. |

Trình xác minh ban đầu chỉ đặt PATH cho tiến trình con. Trên Windows,
điều đó không quyết định executable mà `CreateProcess` chọn. Đã sửa
`tests/verify_dma.py` để tìm đường dẫn executable bằng PATH được yêu
cầu trước khi gọi. Manifest lượt sau ghi đường dẫn thực tế của từng
executable. Không có thay đổi C/C++ giữa hai lượt này.

## Kết quả lượt sau

| Kiểm tra | Số liệu/kết quả đọc từ log | Log nguyên bản |
| --- | --- | --- |
| Build mới `os` và `dma` | Exit 0; object nằm trong thư mục `obj/verify-...` mới | [stdout](verification/20261121T192839434814Z/07-fresh-build.stdout.txt), [stderr và cảnh báo](verification/20261121T192839434814Z/07-fresh-build.stderr.txt) |
| DMA C tests | 3 nhóm đạt; bộ đếm vòng CPU 4426 | [stdout](verification/20261121T192839434814Z/08-dma-tests.stdout.txt) |
| CPU → syscall → mapper test → engine → RAM | 65.536 lệnh CALC sau submit; checksum RAM độc lập 8.355.840 | [stdout](verification/20261121T192839434814Z/09-cpu-tests.stdout.txt), [stderr](verification/20261121T192839434814Z/09-cpu-tests.stderr.txt) |
| CLI unittest | 3 nhóm đạt, thời gian unittest tự báo 0,097 giây | [stderr của unittest](verification/20261121T192839434814Z/10-cli-tests.stderr.txt) |
| Standalone mặc định | 512 byte, status 0, 1 callback; so toàn bộ dữ liệu đạt | [stdout](verification/20261121T192839434814Z/11-default-copy.stdout.txt) |
| Copy lệch hàng và word cuối RAM | Cả hai exit 0, kiểm tra dữ liệu đạt | [lệch hàng](verification/20261121T192839434814Z/12-unaligned-copy.stdout.txt), [word cuối](verification/20261121T192839434814Z/13-last-word-copy.stdout.txt) |
| VCD thực tế | `ch_done`: 1 cạnh lên; counter tối đa 512; `irq` chỉ có giá trị 0 | [VCD gzip](verification/20261121T192839434814Z/copy.vcd.gz), [stdout](verification/20261121T192839434814Z/14-traced-copy.stdout.txt), số đọc nằm trong manifest |
| Toàn OS | Không kết thúc trước giới hạn 5 giây; trình xác minh dừng tiến trình | [manifest](verification/20261121T192839434814Z/manifest.json), stdout/stderr của mục 15 rỗng |

Các kiểm tra cú pháp C/C++ với `-Wall -Wextra -Werror` và kiểm tra mô
hình không đổi đều trả 0, được ghi trong các mục 04–06 của manifest.
Build thường vẫn có cảnh báo từ runtime và starter; không xóa cảnh báo
khỏi stderr để tạo kết quả sạch giả.

Số vòng CPU là quan sát sau submit đến khi test lấy completion. Nó có
thể bao gồm thời gian sự kiện đã sẵn sàng nhưng CPU chưa lấy. Không
dùng số này để khẳng định mọi vòng đều trùng với thời gian DMA truyền,
hay để tính phần trăm tăng tốc. Mapper test không chứng minh MMU thật.

Timeout OS là kết quả chưa đạt, không chuyển thành PASS chỉ vì script
chủ động dừng tiến trình. Hai luồng stdout/stderr rỗng do quá trình bị
dừng cũng không chứng minh OS chưa thực hiện lệnh nào; C stdio có thể
còn giữ dữ liệu trong buffer.

## Chạy lại

Từ thư mục repo, sau khi có môi trường theo [build.md](build.md):

```powershell
& 'D:\Personal\Projects\KeHoach_DoanVien_2026\.venv\Scripts\python.exe' tests/verify_dma.py --verilator-root D:/Personal/Cache/Agents/hk261-dma/verilator-v5.020 --toolchain-bin D:/Personal/Cache/Agents/hk261-dma/toolchain/w64devkit/bin --assignment 'D:/Personal/Study/HCMUT data/H_ di_u hành/hk261_ossim_demacOprx.pdf'
```

Máy khác thay các đường dẫn. `--assignment` là tùy chọn, chỉ ghi hash
PDF để nhận diện đề; không gửi nội dung PDF lên repo. Mỗi lượt tạo một
thư mục bằng timestamp mới; không ghi đè lượt trước. Nếu build hỏng,
script bỏ qua chạy executable để tránh sử dụng binary cũ. Khi mẫu OS
timeout, mã thoát cuối của script là 1 dù các kiểm tra DMA đã đạt.

## Kiểm tra tính nguyên vẹn

Manifest ghi commit nền, trạng thái working tree lúc bắt đầu, SHA-256
99 tệp nguồn/đầu vào/trình xác minh, hash binary và hash từng artifact.
Trình xác minh được thêm ở working tree và được ghi hash riêng; không
giả nhận rằng lúc chạy cây làm việc sạch. Đối chiếu `sources_sha256`
khi cần xác nhận một tệp cụ thể đúng với tệp đã chạy.

Các tệp trong `docs/verification/` được Git giữ nguyên byte bằng
`.gitattributes`, để không đổi CRLF/LF của log sau khi clone. VCD được
nén bằng gzip; giải nén sẽ khôi phục đúng byte có hash `vcd.raw_sha256`.
SHA-256 kiểm tra tệp có thay đổi sau khi bàn giao, không tự chứng minh
nội dung log hay toàn chương trình đúng.
