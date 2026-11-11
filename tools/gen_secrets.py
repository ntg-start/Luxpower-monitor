#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Sinh secrets.yaml cho VLux-Monitor theo MAC của từng mạch.

Mật khẩu KHÔNG được lưu ở đâu cả: chúng luôn dẫn xuất lại được từ
(master key + MAC), nên hỗ trợ kỹ thuật chỉ cần biết MAC là tra ra.

    python tools/gen_secrets.py --init-master         # tạo master.key — CHỈ MỘT LẦN
    python tools/gen_secrets.py                       # tự dò cổng COM, đọc MAC, ghi secrets.yaml
    python tools/gen_secrets.py --port COM5
    python tools/gen_secrets.py --mac 7C:D5:44:3C:2E:D0    # không cần cắm mạch
    python tools/gen_secrets.py --mac 7C:... --show        # chỉ in, không ghi đè file

CẢNH BÁO: mất master.key = mất quyền OTA của TOÀN BỘ máy đã bán ra.
Sao lưu nó ở nơi an toàn, và đừng bao giờ commit vào git.

Lưu ý cho hỗ trợ kỹ thuật: dùng MAC CƠ SỞ (base MAC) của chip — cũng chính là
MAC mà mạch dùng khi nối WiFi, tra được trên router. MAC của chế độ AP là
base MAC + 1, KHÔNG dùng để dẫn xuất.
"""

import argparse
import hashlib
import hmac
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MASTER_PATH = ROOT / "master.key"
SECRETS_PATH = ROOT / "secrets.yaml"

# Bỏ I, O, 0, 1 để không nhầm khi đọc/gõ tay
ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

# ---------------------------------------------------------------------------
#  MẬT KHẨU GIAO DIỆN CHO NGƯỜI DÙNG CUỐI
#
#  None          -> sinh theo MAC, mỗi máy một mật khẩu riêng (an toàn)
#  chuỗi cố định -> mọi máy dùng chung một mật khẩu dễ nhớ (tiện cho người dùng)
#
#  CẢNH BÁO: chuỗi cố định nghĩa là mật khẩu mặc định dùng chung cho TOÀN BỘ
#  sản phẩm — ai biết một máy là biết tất cả. Anh (PSTI Act) và EU (Cyber
#  Resilience Act) CẤM mật khẩu mặc định dùng chung trên thiết bị kết nối bán
#  cho người tiêu dùng. Muốn quay về chế độ an toàn: đặt cả hai thành None.
#
#  ota_password LUÔN sinh theo MAC và không nên hạ xuống: nó cho phép ghi đè
#  firmware từ xa, lại chỉ do công cụ đọc nên không ai phải gõ tay.
# ---------------------------------------------------------------------------
FIXED_WEB_PASSWORD = "123456"    # None = sinh theo MAC
FIXED_AP_PASSWORD = "12345678"   # None = sinh theo MAC; WPA2 bắt buộc >= 8 ký tự

MAC_RE = re.compile(r"\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\b")


def norm_mac(mac: str) -> str:
    """Chuẩn hoá về dạng AA:BB:CC:DD:EE:FF (chữ hoa)."""
    hexs = re.sub(r"[^0-9A-Fa-f]", "", mac)
    if len(hexs) != 12:
        raise ValueError(f"MAC không hợp lệ: {mac!r}")
    hexs = hexs.upper()
    return ":".join(hexs[i:i + 2] for i in range(0, 12, 2))


def derive(master: bytes, mac: str, purpose: str, length: int) -> str:
    """HMAC-SHA256(master, 'purpose|MAC') -> chuỗi base32 rút gọn."""
    dig = hmac.new(master, f"{purpose}|{mac}".encode("utf-8"), hashlib.sha256).digest()
    n = int.from_bytes(dig, "big")
    out = []
    for _ in range(length):
        n, r = divmod(n, len(ALPHABET))
        out.append(ALPHABET[r])
    return "".join(out)


def load_master() -> bytes:
    env = os.environ.get("VLUX_MASTER_KEY")
    if env:
        return bytes.fromhex(env.strip())
    if MASTER_PATH.exists():
        return bytes.fromhex(MASTER_PATH.read_text(encoding="utf-8").strip())
    sys.exit(
        f"Chưa có master key.\n"
        f"  - Tạo mới:  python {Path(__file__).name} --init-master\n"
        f"  - Hoặc đặt biến môi trường VLUX_MASTER_KEY=<64 ký tự hex>\n"
        f"  (file mong đợi: {MASTER_PATH})"
    )


def init_master() -> None:
    if MASTER_PATH.exists():
        sys.exit(f"ĐÃ TỒN TẠI: {MASTER_PATH}\nXoá tay nếu thật sự muốn tạo lại "
                 f"(sẽ làm hỏng OTA của mọi máy đã bán).")
    key = os.urandom(32).hex()
    MASTER_PATH.write_text(key + "\n", encoding="utf-8")
    print(f"Đã tạo {MASTER_PATH}")
    print("\n!!! SAO LƯU NGAY file này ra nơi an toàn (trình quản lý mật khẩu / két).")
    print("!!! Mất nó = mất quyền OTA của toàn bộ máy đã bán ra.")
    print("!!! Đã có trong .gitignore — tuyệt đối không commit.\n")


def autodetect_port() -> str:
    try:
        from serial.tools import list_ports
    except ImportError:
        sys.exit("Thiếu pyserial. Chạy trong venv có esphome, hoặc truyền --port.")
    ports = list(list_ports.comports())
    if not ports:
        sys.exit("Không thấy cổng COM nào. Cắm cáp USB rồi thử lại, hoặc truyền --port.")
    # Ưu tiên các chip USB-serial hay dùng trên CYD: CH340, CP210x, FTDI
    known = {(0x1A86, 0x7523), (0x1A86, 0x55D4), (0x10C4, 0xEA60), (0x0403, 0x6001)}
    for p in ports:
        if (p.vid, p.pid) in known:
            print(f"Dùng cổng {p.device}  ({p.description})")
            return p.device
    print(f"Dùng cổng {ports[0].device}  ({ports[0].description})")
    return ports[0].device


def read_mac(port: str) -> str:
    """Đọc MAC qua esptool. esptool v5 dùng 'read-mac', bản cũ dùng 'read_mac'."""
    last = ""
    for cmd in ("read-mac", "read_mac"):
        argv = [sys.executable, "-m", "esptool", "--port", port, cmd]
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=90)
        except FileNotFoundError:
            sys.exit("Không gọi được esptool. Chạy script bằng python của venv esphome.")
        except subprocess.TimeoutExpired:
            sys.exit(f"esptool treo quá 90 giây trên {port}.")
        out = (r.stdout or "") + (r.stderr or "")
        last = out
        m = MAC_RE.findall(out)
        if m:
            # Dòng đầu tiên thường là 'MAC: xx:xx:...' của chính chip
            return norm_mac(m[0])
    print(last, file=sys.stderr)
    sys.exit(f"Không đọc được MAC từ {port}. Kiểm tra cáp/driver, hoặc truyền --mac thủ công.")


def build_secrets(master: bytes, mac: str) -> dict:
    if FIXED_AP_PASSWORD is not None and len(FIXED_AP_PASSWORD) < 8:
        sys.exit(
            f"FIXED_AP_PASSWORD = {FIXED_AP_PASSWORD!r} chỉ có {len(FIXED_AP_PASSWORD)} ký tự.\n"
            "WPA2 bắt buộc tối thiểu 8 — ESPHome sẽ từ chối build. Sửa trong "
            f"{Path(__file__).name}."
        )
    ap = FIXED_AP_PASSWORD if FIXED_AP_PASSWORD is not None else derive(master, mac, "ap", 12)
    web = FIXED_WEB_PASSWORD if FIXED_WEB_PASSWORD is not None else derive(master, mac, "web", 16)
    return {
        "ap_password": ap,
        "ota_password": derive(master, mac, "ota", 20),  # luôn theo MAC
        "web_user": "admin",
        "web_password": web,
    }


def render(mac: str, s: dict) -> str:
    ap_src = "cố định cho mọi máy" if FIXED_AP_PASSWORD is not None else "sinh theo MAC"
    web_src = "cố định cho mọi máy" if FIXED_WEB_PASSWORD is not None else "sinh theo MAC"
    return (
        "# TỰ ĐỘNG SINH — đừng sửa tay, đừng commit (đã có trong .gitignore).\n"
        f"# Mạch: {mac}\n"
        f"#   ap_password : {ap_src}\n"
        f"#   web_password: {web_src}\n"
        "#   ota_password: sinh theo MAC\n"
        "# Dẫn xuất từ master.key; mất file này không sao, chạy lại:\n"
        f"#     python tools/gen_secrets.py --mac {mac}\n"
        "\n"
        f'ap_password: "{s["ap_password"]}"\n'
        f'ota_password: "{s["ota_password"]}"\n'
        f'web_user: "{s["web_user"]}"\n'
        f'web_password: "{s["web_password"]}"\n'
    )


def main() -> None:
    ap = argparse.ArgumentParser(description="Sinh secrets.yaml theo MAC của mạch.")
    ap.add_argument("--init-master", action="store_true", help="tạo master.key mới (chỉ một lần)")
    ap.add_argument("--mac", help="MAC của mạch, bỏ qua bước đọc qua USB")
    ap.add_argument("--port", help="cổng COM (mặc định: tự dò)")
    ap.add_argument("--show", action="store_true", help="chỉ in ra, không ghi file")
    ap.add_argument("--out", type=Path, default=SECRETS_PATH, help="đường dẫn secrets.yaml")
    args = ap.parse_args()

    if args.init_master:
        init_master()
        return

    master = load_master()
    try:
        mac = norm_mac(args.mac) if args.mac else read_mac(args.port or autodetect_port())
    except ValueError as e:
        sys.exit(f"Lỗi: {e}\nDạng hợp lệ: 7C:D5:44:3C:2E:D0 hoặc 7c-d5-44-3c-2e-d0")
    secrets = build_secrets(master, mac)
    text = render(mac, secrets)

    print(f"\nMAC          : {mac}")
    print(f"ap_password  : {secrets['ap_password']}")
    print(f"ota_password : {secrets['ota_password']}")
    print(f"web_user     : {secrets['web_user']}")
    print(f"web_password : {secrets['web_password']}\n")

    if args.show:
        print("(--show: không ghi file)")
        return

    args.out.write_text(text, encoding="utf-8")
    print(f"Đã ghi {args.out}")


if __name__ == "__main__":
    main()
