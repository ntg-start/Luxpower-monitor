# Nạp firmware VLux-Monitor

> ✅ **Nạp được ngay, không cần cấu hình gì.** Thiết bị **tự quét LAN tìm dongle Luxpower**,
> **tự học** serial dongle + serial inverter từ frame dongle đẩy ra, rồi **nhớ vào flash**.
> WiFi khai báo qua captive portal lúc chạy.
>
> Mật khẩu AP/OTA/web lấy từ `secrets.yaml` lúc build — bản bin này dùng giá trị mẫu
> (`ap_password: vlux12345`). Muốn đổi thì sửa `secrets.yaml` rồi build lại:
> `esphome compile vlux-monitor.yaml`.

## File nào dùng khi nào

| File | Offset | Dùng khi |
|------|--------|----------|
| **`vlux-monitor-factory.bin`** | `0x0` | **Nạp mới / nạp sạch** — gộp sẵn bootloader + bảng phân vùng + otadata + app. Chỉ cần 1 lệnh. |
| `vlux-monitor-ota.bin` | — | Cập nhật **OTA** qua WiFi hoặc web `http://<ip>/` |
| `vlux-monitor-app.bin` | `0x10000` | Chỉ nạp phần app (khi bootloader/partition đã đúng) |
| `bootloader.bin` | `0x1000` | nạp rời |
| `partition-table.bin` | `0x8000` | nạp rời |
| `ota_data_initial.bin` | `0x9000` | nạp rời |

Thông số flash: `--flash-mode dio --flash-size 4MB --flash-freq 40m`, chip `esp32`.

## Cách 1 — Nạp 1 file duy nhất (khuyến nghị)

```bash
esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m \
  0x0 vlux-monitor-factory.bin
```

Nếu muốn xoá sạch flash trước (khuyến nghị khi thay firmware gốc):

```bash
esptool --chip esp32 --port /dev/ttyUSB0 erase-flash
```

> Ubuntu: nếu báo lỗi quyền, thêm user vào nhóm `dialout`:
> `sudo usermod -aG dialout $USER` rồi đăng xuất/đăng nhập lại.
> Dùng esptool có sẵn trong venv: `cd .. && . .venv/bin/activate`.

## Cách 2 — Nạp rời từng phần

```bash
esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0x9000  ota_data_initial.bin \
  0x10000 vlux-monitor-app.bin
```

## Cách 3 — Để ESPHome tự làm (dễ nhất)

```bash
cd ..
. .venv/bin/activate
esphome run vlux-monitor.yaml          # tự compile + chọn cổng + nạp + xem log
```

## Cách 4 — Nạp từ trình duyệt

Mở <https://web.esphome.io> bằng **Chrome/Edge**, cắm board qua USB, chọn
`vlux-monitor-factory.bin`. Không cần cài gì.

## Sau khi nạp

1. Màn hình bật, hiện dashboard với giá trị `--`.
2. Thiết bị tạo WiFi AP **`VLux-Monitor`** (mật khẩu `vlux12345`).
   Nối vào bằng điện thoại → trang khai báo WiFi tự mở.
3. Khai báo WiFi xong, thiết bị **tự quét mạng tìm dongle** (~15 giây),
   rồi **tự học 2 serial** từ frame dongle đẩy ra.
4. **Chạm màn hình** để chuyển trang. Trang **3/3** hiện IP + 2 serial dò được.
   Góc trên phải: **`LIVE`** (xanh) = đã nối dongle, **`OFFLINE`** (đỏ) = chưa.
5. Xem log chi tiết:
   ```bash
   esphome logs vlux-monitor.yaml
   ```
   Các dòng đáng chú ý:
   ```
   Đã gửi broadcast 'HF-A11ASSISTANT' tới 255.255.255.255:48899, chờ phản hồi...
   Thấy dongle: IP=192.168.1.50  tên=BA12345678
   Lấy dongle SN từ broadcast: BA12345678
   Đã kết nối dongle tại 192.168.1.50:8000
   Đã học inverter SN: 1234567890
   Đủ 2 serial — bắt đầu hỏi dữ liệu.
   Frame func=0x04 reg=0..39 (40 reg, values@34)
   ```
   Cổng TCP là **8000**, cổng UDP dò tìm là **48899** — cả hai lấy trực tiếp từ
   firmware gốc. Nếu dongle không trả lời broadcast (bản gốc ghi là *bình thường*),
   component tự chuyển sang quét dải `/24` — vẫn tìm ra, chỉ chậm hơn (~15 giây).

Bảng chẩn đoán lỗi: xem phần cuối `../README.md`.
