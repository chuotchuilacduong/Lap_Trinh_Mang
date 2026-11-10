# Mô phỏng điều khiển hệ thống chăm sóc vườn cây tự động

Gồm 2 chương trình viết bằng C (socket POSIX):

| Chương trình | Vai trò |
|---|---|
| `bin/garden_sim` | Mô phỏng 1 vườn: môi trường đất (độ ẩm, N, P, K) và các thiết bị **cảm biến**, **tưới nước**, **bón phân NPK**, **đèn quang hợp**. Mỗi thiết bị có ID, mật khẩu, cổng TCP riêng. |
| `bin/controller` | Ứng dụng điều khiển cho người dùng (menu dòng lệnh): quản lý vườn, quét, kết nối, điều khiển thiết bị. |

## Biên dịch và chạy

```bash
cd project
make

# Terminal 1: chạy mô phỏng vườn 1 (có thể chạy thêm vườn 2 ở terminal khác)
./bin/garden_sim config/garden1.conf          # tùy chọn: -s <phút mô phỏng/giây> -p <cổng quét>
./bin/garden_sim config/garden2.conf

# Terminal 2: chạy ứng dụng điều khiển
./bin/controller                               # tùy chọn: -b <địa chỉ broadcast> -p <cổng quét> -d <thư mục dữ liệu>
./bin/controller -d data2                      # app thứ 2 với App ID khác
```

Mật khẩu mặc định: vườn 1 là `123456`, vườn 2 là `abcdef` (xem `config/*.conf`).

Dữ liệu sinh ra khi chạy (đã có trong `.gitignore`, xoá bằng `make reset-data`):

- `state/<ID>.state` – mật khẩu, trạng thái nguồn, thông số của từng thiết bị (giữ lại khi khởi động lại).
- `logs/<ID>.log` – log hoạt động của từng thiết bị.
- `data/app_id`, `data/gardens.txt` – App ID và danh sách vườn/thiết bị/token của ứng dụng.

## Kịch bản demo nhanh

1. Chạy `garden_sim config/garden1.conf`, chạy `controller`.
2. **2. Quét thiết bị** → thấy 4 thiết bị → thêm vào vườn mới (tạo vườn ngay khi thêm).
3. **3. Kết nối thiết bị** → nhập mật khẩu `123456` → nhận token.
4. **6. Thiết lập thông số**, **7. Điều khiển trực tiếp**, **8. Xem thông tin**, **9. Xem log**.
5. **1. Quản lý vườn → 3. Xem chi tiết vườn** để xem tổng quan trạng thái mọi thiết bị.
6. Mở app thứ 2 (`-d data2`), kết nối cùng thiết bị, rồi đổi mật khẩu từ app 1 → token của app 2 bị huỷ, app 2 phải kết nối lại.

Đồng hồ mô phỏng mặc định chạy 10 phút mô phỏng / 1 giây thực (1 ngày ≈ 2,4 phút), nên có thể quan sát
tưới theo lịch, tưới khi độ ẩm < Hmin, bón phân khi thiếu dinh dưỡng, đèn bật/tắt theo lịch ngay trên terminal của `garden_sim`.

## Mô hình mô phỏng

- Mỗi phút mô phỏng: độ ẩm giảm ~0,08 %, N/P/K giảm ~0,02–0,03 mg/kg (có nhiễu ngẫu nhiên).
- **Cảm biến**: ghi nhận độ ẩm, N, P, K mỗi `T` phút (`interval`).
- **Tưới nước** (2 cơ chế đồng thời): tưới `V` lít vào các thời điểm cố định nếu độ ẩm ≤ `Hmax`; tưới `V` lít khi độ ẩm < `Hmin`. 1 lít làm tăng 1,5 % độ ẩm.
- **Bón phân**: khi N < `Nmin` hoặc P < `Pmin` hoặc K < `Kmin` thì bón `C × V` gam phân hoà trong `V` lít nước.
- **Đèn**: phát sáng với công suất `P` W trong các khung giờ đã lập lịch hoặc khi được bật thủ công; tính điện năng tiêu thụ.
- Các cơ chế tự động theo ngưỡng có thời gian chờ 15 phút giữa 2 lần để tránh lặp liên tục.
- Khi thiết bị bị **tắt** (POWER OFF): không chạy cơ chế tự động, từ chối điều khiển trực tiếp (mã 409).

## Giao thức

Mọi thông điệp là văn bản, mỗi thông điệp 1 dòng kết thúc bằng `\r\n`.
Phản hồi có dạng `<mã> <nội dung>`.

### Quét (UDP, cổng 9999)

```
App  -> broadcast : SCAN <app_id>
Thiết bị -> App   : DEVICE <device_id> <SENSOR|PUMP|FERTILIZER|LIGHT> <tcp_port> <ON|OFF>
```

### Điều khiển (TCP, cổng riêng của từng thiết bị)

| Lệnh | Phản hồi thành công | Ý nghĩa |
|---|---|---|
| `CONNECT <app_id> <password>` | `200 <token> <device_id> <type>` | Xác thực, nhận token gắn với App ID |
| `CHPASS <token> <cũ> <mới>` | `200 ...` | Đổi mật khẩu, huỷ token của các app khác |
| `POWER <token> ON\|OFF` | `200 ...` | Bật / tắt thiết bị |
| `INFO <token>` | `200 key=value ...` | Lấy thông tin, thông số, trạng thái |
| `SET <token> key=value ...` | `200 ...` | Thiết lập thông số (kiểm tra hết rồi mới áp dụng) |
| `RESET <token>` | `200 ...` | Khởi tạo lại thông số mặc định |
| `RUN <token> [tham số]` | `200 ...` | Điều khiển trực tiếp |
| `LOG <token> [n]` | `210 <k>` + k dòng | Lấy n dòng log gần nhất |
| `QUIT` | `200 Tam biet` | Đóng kết nối |

Thông số `SET` theo loại thiết bị:

| Loại | Khoá | `RUN` |
|---|---|---|
| SENSOR | `interval` (T phút) | đo ngay |
| PUMP | `hmin`, `hmax`, `vol` (lít), `times=06:00,17:00` | `RUN <token> [lít]` tưới ngay |
| FERTILIZER | `nmin`, `pmin`, `kmin`, `conc` (g/lít), `vol` (lít) | `RUN <token> [lít]` bón ngay |
| LIGHT | `watt`, `times=18:00-22:00,04:00-06:00` | `RUN <token> <phút>` bật thủ công (0 = huỷ) |

Mã lỗi: `400` sai cú pháp/tham số, `401` token không hợp lệ, `403` sai mật khẩu,
`404` lệnh không hỗ trợ, `409` thiết bị đang tắt, `500` thiết bị quá tải.

## Đối chiếu yêu cầu

| Yêu cầu | Cài đặt |
|---|---|
| Xử lý truyền dòng | `common/net.c`: `StreamBuf` gom byte từ `recv()`, tách từng dòng theo `\r\n`, xử lý cả trường hợp nửa thông điệp / nhiều thông điệp dính nhau / dòng quá dài |
| Cơ chế vào/ra socket trên server | `device/garden_sim.c`: 1 vòng lặp `poll()` phục vụ đồng thời socket UDP quét, socket lắng nghe của mọi thiết bị và mọi kết nối TCP |
| Xác thực truy cập điều khiển | `CONNECT` kiểm tra mật khẩu, sinh token ngẫu nhiên (`/dev/urandom`) gắn với App ID; mọi lệnh khác phải kèm token |
| Khởi tạo thông số cho thiết bị | Thiết bị nạp thông số từ `state/` hoặc dùng mặc định khi khởi động; `RESET` khởi tạo lại từ app |
| Quét thiết bị | UDP broadcast `SCAN`, thiết bị trả `DEVICE ...`, app hiển thị loại + ID và thêm vào vườn |
| Kết nối thiết bị | Menu 3 (hoặc tự động khi chọn thiết bị chưa xác thực) |
| Đổi mật khẩu | Menu 4, lệnh `CHPASS` |
| Điều khiển bật tắt | Menu 5, lệnh `POWER` |
| Thông số bộ phận tưới nước | Menu 6: Hmin, Hmax, V, lịch tưới |
| Thông số bộ phận bón phân | Menu 6: Nmin, Pmin, Kmin, C, V |
| Thông số đèn quang hợp | Menu 6: công suất P, lịch chiếu sáng |
| Điều khiển trực tiếp | Menu 7, lệnh `RUN` (song song với cơ chế lập lịch) |
| Lấy thông tin, thông số từng loại thiết bị | Menu 8, lệnh `INFO` |
| Ghi log hoạt động trên mỗi thiết bị | `logs/<ID>.log`; xem từ app bằng menu 9 (`LOG`) |
| Quản lý thông tin vườn cây | Menu 1: tạo/xoá vườn, xem chi tiết, chuyển/xoá thiết bị; lưu ở `data/gardens.txt` |

## Cấu trúc mã nguồn

```
project/
├── Makefile
├── common/        protocol.h (mã lệnh, mã phản hồi), net.c/h (đệm dòng, gửi/nhận, kết nối)
├── device/        garden_sim.c (vòng lặp poll, quét UDP), device.c/h (logic thiết bị, xử lý lệnh)
├── controller/    controller.c (menu người dùng), garden_store.c/h (lưu vườn & thiết bị)
└── config/        garden1.conf, garden2.conf (danh sách thiết bị cần mô phỏng)
```
