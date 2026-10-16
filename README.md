# VLux‑Monitor (ESPHome)

Màn hình giám sát inverter **Luxpower** cho board **CYD 2.8"** (ESP32‑2432S028R),
viết lại bằng **ESPHome + LVGL** với giao diện hiện đại, thay cho firmware Arduino đơn sắc gốc.

- **Phần cứng:** ESP32‑2432S028R – màn 2.8" ILI9341 320×240 + cảm ứng điện trở XPT2046.
- **Nguồn dữ liệu:** kết nối tới **dongle WiFi Luxpower** trên cùng mạng LAN, đọc thanh ghi
  qua giao thức **LXP over TCP** (component `luxpower_tcp` tự viết — port lại từ firmware gốc).
- **Chạy độc lập:** có **captive portal** để khai báo WiFi; không cần Home Assistant.
- **Giao diện:** LVGL – dashboard tối **2 trang** (chạm màn hình để chuyển):
  - *Trang 1 – Tổng quan:* vòng cung SOC pin, thẻ PV / GRID / LOAD, pin & kWh hôm nay,
    đèn trạng thái `LIVE`/`OFFLINE`.
  - *Trang 2 – Chi tiết:* điện áp & tần số lưới, PV1/PV2, điện mua/bán hôm nay,
    áp pin, nhiệt độ.

### Kiến trúc component (quan trọng)

`luxpower_tcp` là **state machine không chặn** (`IDLE → CONNECTING → CONNECTED`), toàn bộ
socket I/O chạy trong `loop()` theo từng mẩu nhỏ. Điều này cần thiết vì đây là thiết bị có
màn hình: nếu chặn main loop thì LVGL ngừng vẽ và cảm ứng không phản hồi.

- Kết nối non-blocking + **backoff tăng dần** (1s → 30s) khi lỗi.
- Buffer nhận **giữ lại frame chưa đủ** giữa các vòng loop (không mất dữ liệu).
- **Tự dò layout reply**: một số dongle chèn byte đếm kiểu Modbus ở offset 34 — code
  phát hiện và bù trừ tự động (nếu sai sẽ lệch 1 byte → số liệu rác).
- Mất kết nối ⇒ **xoá cờ hợp lệ của register** để không hiển thị số liệu cũ.

> Đây là bản **viết lại** (không phải decompile). Firmware gốc là app Arduino/ESP‑IDF tùy biến;
> ESPHome có kiến trúc khác nên toàn bộ được dựng mới, nhưng **giao thức Luxpower được port
> đúng theo kết quả reverse‑engineering** firmware gốc (request 38 byte, function `0xC2`
> TranslatedData, đọc input‑register bank 0–39 / 40–79, thêm bank 160–170).

---

## Cấu trúc

```
vlux-esphome/
├── vlux-monitor.yaml              # cấu hình ESPHome chính (WiFi, display, LVGL UI, sensors)
├── secrets.yaml                   # (tự tạo) mật khẩu AP, OTA… — xem secrets.yaml.example
├── secrets.yaml.example
└── components/
    └── luxpower_tcp/              # custom component: client LXP/TCP tới dongle
        ├── __init__.py            # schema cấu hình + codegen
        ├── luxpower_tcp.h
        └── luxpower_tcp.cpp
```

## Yêu cầu

- **ESPHome ≥ 2024.11** (cần LVGL). Cài bằng: `pip install esphome` hoặc dùng Docker.
- Dongle Luxpower đã cắm, cùng mạng WiFi với thiết bị, biết **IP** (nên đặt DHCP reservation)
  và **serial** của dongle + inverter (10 ký tự, in trên tem hoặc xem trong app LuxPower).

## Thiết lập nhanh

1. Tạo `secrets.yaml` từ mẫu:
   ```bash
   cp secrets.yaml.example secrets.yaml
   # sửa ap_password, ota_password, dongle_ip, dongle_serial, inverter_serial
   ```
2. Kiểm tra cấu hình:
   ```bash
   esphome config vlux-monitor.yaml
   ```
3. Nạp lần đầu qua USB:
   ```bash
   esphome run vlux-monitor.yaml
   ```
   Các lần sau có thể nạp **OTA** qua WiFi.
4. Lần đầu bật, nếu chưa có WiFi: thiết bị tạo AP **`VLux-Monitor`** (captive portal).
   Nối vào AP đó bằng điện thoại → trang khai báo WiFi sẽ tự hiện.

## Tự dò dongle — không cần cấu hình gì

Port đúng theo firmware gốc (đã đối chiếu Ghidra), gồm 2 bước y hệt:

1. **UDP broadcast (nhanh, ~1 giây)** — gửi chuỗi `HF-A11ASSISTANT` tới
   `255.255.255.255:48899`. Dongle Luxpower dùng module WiFi **HF-A11 (High-Flying)**
   nên trả về `"IP,MAC,TÊN"`; component lấy **IP** và **10 ký tự đầu của TÊN làm `dongle_sn`**.
   *(Trong firmware gốc: `FUN_400d5f6c`, cổng `DAT_400d0508 = 48899`.)*
2. **Quét dải /24 (dự phòng, ~15 giây)** — nếu UDP không có phản hồi (bản gốc ghi rõ
   *"Khong thay phan hoi (binh thuong voi dongle Luxpower)"*), quét octet cuối 1→254
   trên cổng **8000**, 8 kết nối song song. *(Bản gốc: `FUN_400d64f0`,
   cổng `DAT_400d0538 = 0x1F40 = 8000`.)*
3. **Học `inv_sn`** — sau khi nối được, chờ frame dongle đẩy ra rồi bóc serial inverter
   (offset 22–31). Đây là bước *"doc passive frames cho SN"* của bản gốc.
3. **Nhớ lại** — IP + 2 serial được lưu vào flash (tương đương `lux_ip`/`dongle_sn`/`inv_sn`
   trong NVS bản gốc), lần khởi động sau nối thẳng, không quét lại.
4. **Tự phục hồi** — nếu IP cũ không còn đúng (dongle đổi DHCP), sau 2 lần lỗi nó tự quét lại.
   Nếu nối được một máy nào đó nhưng 45 giây không thấy frame Luxpower nào, nó bỏ và dò tiếp.

Trang **3/3** trên màn hình hiển thị IP + 2 serial đã dò được.

### Muốn cố định (bỏ qua bước dò cho nhanh)

```yaml
luxpower_tcp:
  host: 192.168.1.50
  dongle_serial: "BA12345678"
  inverter_serial: "1234567890"
```

Hoặc tắt hẳn dò tự động: `auto_discover: false`.

## Cổng kết nối

`port: 8000` — **xác nhận từ firmware gốc** (`DAT_400d0538 = 0x1F40`). Cổng UDP dò tìm là
`48899` (cố định theo giao thức HF-A11).

Nếu quét hết dải ở 8000 không thấy, component còn tự thử lại toàn dải với `4346` như biện
pháp dự phòng cuối. Cổng nào dùng được sẽ **lưu vào flash** cùng IP, lần sau nối thẳng.

> `host` (nếu điền) phải là **địa chỉ IP**, không nhận tên miền — component không phân giải DNS.

## Giao thức Luxpower (tóm tắt từ RE)

Mỗi chu kỳ, thiết bị gửi 3 lệnh **ReadInput** tới dongle:

| Bank | Register | Nội dung |
|------|----------|----------|
| 0 | 0–39   | PV, pin, lưới, tải, tần số, năng lượng ngày |
| 1 | 40–79  | nhiệt độ, BMS, thống kê |
| 2 | 160–170| vùng cài đặt (holding) |

Khung request (38 byte, `0x26`):

```
A1 1A | proto(2) | len(2) | 01 C2 | dongle_serial[10] |
       data_len(2) | 00 04 | inv_serial[10] | reg(2) | count(2) | CRC16
```

`0x04` = ReadInput, `0xC2` = TranslatedData. Phản hồi được bóc thành mảng
register (u16 LE) rồi scale (áp ×0.1 V, tần số ×0.01 Hz, năng lượng ×0.1 kWh…).

Ánh xạ register chính (xác nhận từ firmware gốc):

| Reg | Sensor | Scale |
|----|--------|-------|
| 1–3 | v_pv_1/2/3 | ×0.1 V |
| 4 | v_bat | ×0.1 V |
| 5 (byte thấp) | soc | % |
| 7–9 | p_pv_1/2/3 | W |
| 10 | p_charge | W |
| 11 | p_discharge | W |
| 15 | f_ac | ×0.01 Hz |
| 26 | p_to_grid | W |
| 27 | p_to_user | W |

> Một số offset (nhiệt độ, năng lượng ngày) theo chuẩn LXP input‑register và có thể
> cần hiệu chỉnh theo đời inverter — xem `luxpower_tcp.cpp`, bảng `REGISTER_MAP`.

## Tùy biến giao diện

Toàn bộ UI nằm trong khối `lvgl:` của `vlux-monitor.yaml`. Màu sắc/theme khai ở `styles`.
Mỗi số liệu là một `label`/`arc` được cập nhật trong `sensor: … on_value:`.

## Đã kiểm chứng trên phần cứng thật ✅

Chạy thật với dongle `DG51603498` / inverter `51930V2563`: tự dò ra dongle, tự học serial,
đọc được đầy đủ SOC / áp pin / công suất PV / tải / lưới / tần số / nhiệt độ / kWh ngày.
Cân bằng năng lượng khớp (PV = sạc + tải + hao hụt).

### Giao thức thực tế (đã kiểm chứng trên thiết bị)

Dongle Luxpower **CÓ trả lời** lệnh ReadInput — nhưng chỉ khi khung request đúng tuyệt đối.
Khung dưới đây sao chép từng byte từ `FUN_400d2f74` của firmware gốc:

```
[0]=A1 [1]=1A
[2]=1  [3]=0      <-- protocol = 1   (KHÔNG phải 2 hay 5; sai byte này dongle im lặng)
[4]=20 [5]=0      <-- frame_length = 32
[6]=1  [7]=C2
[8..17]  dongle_sn (10 byte ASCII)
[18]=12 [19]=0    <-- data_len = 18
[20]=0            <-- data address = 0
[21]=4            <-- device_function = ReadInput
[22..31] inverter_sn (10 byte ASCII)
[32,33] reg_start   [34,35] count
[36,37] CRC16-modbus trên [20..36)  (16 byte)
```

Component hỏi 2 dải mỗi chu kỳ: **0–39** (dữ liệu chính) và **40–79** (nhiệt độ ở reg 64–67).
Trả lời dài 117 byte (40 thanh ghi). Frame `reg_start=40` là bằng chứng chắc chắn rằng
dongle trả lời, vì nó **chỉ tự đẩy** các frame `reg_start` 0 / 125 / 250.

Dongle **cũng tự đẩy** frame không cần hỏi:
| device_function | register | nội dung |
|---|---|---|
| `0x03` | 0–79 | cấu hình — **phải bỏ qua**, nếu không sẽ đè lên dữ liệu sống |
| `0x04` | 0–124 | dữ liệu sống |
| `0x04` | 125–249, 250–374 | hầu hết số 0 |

Chi tiết khác:
- Reply **có byte đếm kiểu Modbus** tại offset 34 (= `data_len − 17`) → giá trị bắt đầu ở **offset 35**.
- CRC16 tính trên `buf[20 .. 20+data_len−2]` — khớp.
- Thỉnh thoảng có frame `0x04` toàn số 0 — phải bỏ qua kẻo màn hình nhảy về 0.
- Dongle đóng kết nối sau vài giây; cứ nối lại, không phải lỗi.

### Hiệu năng thực đo
`update_interval: 3s` + `RECONNECT_GAP_MS = 1000` cho **~1.1 giây/lần cập nhật**
(54 frame/phút), chạy ổn định không reboot.


## Giấy phép / lưu ý

Project phục vụ mục đích cá nhân, tương tác với thiết bị bạn sở hữu. Giao thức Luxpower
được dựng lại từ việc phân tích firmware của chính thiết bị bạn có.
