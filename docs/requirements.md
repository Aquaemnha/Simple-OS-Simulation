# Đối chiếu yêu cầu BTL HK261

**Kết luận hiện tại: chưa đáp ứng toàn bộ BTL và chưa đủ để nộp như một
bản hoàn chỉnh.** Phần DMA có bằng chứng kiểm thử ở mức module; điều đó
không thay cho việc kiểm tra scheduler, MMU, tách user/kernel và demo OS.
Không quy đổi số test đạt thành điểm tự chấm.

Đề đối chiếu: `hk261_ossim_demacOprx.pdf`, đặc biệt trang PDF 26–27.
Nguồn đã kiểm tra là commit `cc92ca36936c93b421eb5c700bdfb4f4da95dedb`.
Các tệp C/C++ chưa thay đổi trong đợt xác minh này.

## Yêu cầu và bằng chứng

| Yêu cầu từ đề | Trạng thái thực tế | Bằng chứng hoặc phần còn thiếu |
| --- | --- | --- |
| MLQ theo chính sách và slot, tr. 12–13, 26 | Chưa đạt | `src/queue.c` còn enqueue rỗng, dequeue trả NULL; `get_mlq_proc` trong `src/sched.c` chưa lấy tiến trình và thiếu nhả khóa. Chưa có Gantt từ một lần chạy đúng. |
| Bộ nhớ ảo, cấp phát/free/read/write, tr. 13–20, 26 | Chưa đạt | `pg_getval`/`pg_setval` trong `src/libmem.c` còn phần đọc/ghi vật lý chưa hiện thực. Chưa chạy được bài kiểm thử vùng ảo đầy đủ của OS. |
| Tách user/kernel, hạn chế truy cập và copy qua boundary, tr. 6–7, 14–15, 26 | Chưa đạt | Các hàm copy_from_user/copy_to_user và thao tác vùng kernel còn TODO. Quyền trang trong fixture DMA không chứng minh bảo vệ user/kernel của OS thật. |
| Syscall truyền PID thay vì PCB, GNU C, tr. 26 | Đã kiểm tra cho syscall DMA; chưa xác minh toàn hệ thống | `src/sys_dma.c` nhận kernel, PID, registers; test gọi đường thật. `src/sys_mem.c` vẫn tạo PCB giả và chưa tra PID đúng. Các tệp DMA qua kiểm tra cảnh báo nghiêm ngặt, không có kết luận toàn starter sạch lỗi. |
| Paging nhiều cấp địa chỉ 64 bit, tr. 16–19, 26 | Chưa đạt | `pte_get_entry` trong `src/mm64.c` chưa đi qua cây trang thật và trả địa chỉ biến cục bộ sau khi ép kiểu. Test mapper DMA là fixture riêng. |
| Thống kê số truy cập nhớ, dung lượng cấu trúc bảng trang và giải thích thiết kế, tr. 26 | Chưa có số liệu hợp lệ | Chưa có lần chạy MMU thật để đo. Không dùng số vòng CPU, dung lượng RAM test hoặc số trang fixture làm thống kê thay thế. |
| CPU tiếp tục làm việc sau khi gửi co-processor, tr. 21–22, 26–27 | Có bằng chứng ở mức module | Worker riêng; test CPU chạy các lệnh CALC sau submit, không join tại lệnh DMA. Số vòng đo đến lúc lấy completion không chứng minh mọi vòng đều trùng thời gian thiết bị truyền. Chưa có demo cùng scheduler/MMU thật. |
| Dùng DMA engine Verilator được cấp, tr. 26, 33 | Đã kiểm tra ở mức module | Build/link mô hình thật, chuyển và so toàn bộ dữ liệu. `git diff` xác nhận `Vdma_top*` không đổi so với starter import. |
| DMA truy cập RAM qua địa chỉ bus 64 bit/data 32 bit, tr. 23, 27 | Có bằng chứng ở mức module | RAM dùng chung với CPU MEMPHY; thử endian, lệch hàng, word cuối và 256 KiB. Địa chỉ trên 4 GiB bị từ chối khi RAM test chỉ 1 MiB; chưa chứng minh truy cập RAM vật lý trên 4 GiB. |
| CPU truy cập thông qua MMU thật, tr. 27 | Chưa đạt | `os.dma_map_buffers` chưa được gắn; syscall trả `-ENOSYS` khi thiếu mapper. Không tính bảng trang test là MMU thật. |
| Interrupt/handler khi hoàn tất, tr. 21–23 | Có callback/handler mô phỏng; chưa có bằng chứng chân IRQ hay POSIX signal | VCD: `ch_done` lên 1 lần, counter 512; `irq` luôn 0 vì `intr_en=0` trong mô hình được cấp. Bản này dùng sự kiện DPI được CPU xử lý giữa các lệnh, khác với đường POSIX signal mà ảnh hướng dẫn liên hệ tới Lab2. |
| Đồng bộ tài nguyên chung, tr. 24–25 | Đã kiểm thử phần DMA; toàn OS chưa đạt | Kiểm tra 4 pthread gửi, giữ kết quả đến ACK, RAM reservation, shutdown/restart. Không có bằng chứng toàn OS không còn race/deadlock; scheduler có đường khóa chưa hoàn chỉnh. |
| Build và chạy hệ thống, tr. 24 | Build đạt; chạy toàn OS chưa đạt | Fresh object build trả 0. Mẫu `os_1_mlq_paging_small_4K` bị timeout 5 giây; mã 1 sau đó do trình kiểm thử kết thúc tiến trình, không phải OS tự hoàn thành. |
| Báo cáo: Gantt, trạng thái cấp phát, sơ đồ dịch địa chỉ, kết quả và questionnaire, tr. 26–27 | Chưa đủ báo cáo nhóm | Word hiện có là giải thích/bàn giao DMA. Chưa có Gantt, thống kê MMU thật và bộ đáp án toàn nhóm. |
| Gói `assignment_STUDENTID.zip` và nộp LMS, tr. 27 | Chưa thực hiện | Chưa có bản OS hoàn chỉnh và thông tin mã số sinh viên cho gói nộp. Push GitHub không thay thế nộp LMS. |

Page replacement là phần tùy yêu cầu giảng viên theo trang 26; không tự
coi nó là bắt buộc hoặc tự xác nhận đã được miễn. Việc giữ trang đang
DMA vẫn cần thiết nếu nhóm có đường thay thế/thu hồi trang.

## Questionnaire còn cần tổng hợp

Đề có các câu hỏi sau; tài liệu DMA chưa thay cho đáp án báo cáo nhóm:

1. Trang 9: ưu/nhược của giao diện syscall thống nhất cho file, memory và I/O.
2. Trang 11: phát hiện và xử lý syscall chạy quá lâu.
3. Trang 13: lợi ích của từng chính sách MLQ được nêu trong đề.
4. Trang 17: động cơ kết hợp segmentation với paging.
5. Trang 18: lợi ích khi mở rộng paging phân cấp thành N mức.
6. Trang 19: ưu/nhược của paging và cấp phát liên tục.
7. Trang 25: hậu quả thiếu đồng bộ, kèm ví dụ từ kết quả Simple OS nếu có.

## Log và cách chạy lại

Xem [hồ sơ xác minh](verification.md), gồm lệnh, stdout/stderr nguyên
bản, mã thoát, timeout, hash nguồn/binary và VCD nén. Không đổi dữ liệu
để biến một mục chưa đạt thành đã đạt.

Phần ghép MMU của thành viên 2/3 và demo chung với thành viên 5 được mô
tả trong [hướng dẫn DMA](dma.md#5-phần-cần-ghép-với-thành-viên-bộ-nhớ).
