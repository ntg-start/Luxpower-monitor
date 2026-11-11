"""Thành phần phụ trợ cho trang Cài đặt trên màn hình.

Không tạo entity nào — nhiệm vụ duy nhất là:
  1. Yêu cầu WiFiComponent GIỮ LẠI kết quả quét sau khi nối xong. Mặc định nó
     giải phóng vùng nhớ này ngay khi kết nối thành công, khiến danh sách SSID
     trong trang Cài đặt rỗng. Đây là hàm chỉ gọi được lúc sinh mã, không gọi
     được từ lambda trong YAML — nên mới cần component riêng.
  2. Nhúng header chứa các hàm tiện ích dùng trong lambda của LVGL.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import wifi

CODEOWNERS = ["@vlux"]
DEPENDENCIES = ["wifi"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    wifi.request_wifi_scan_results()
    cg.add_global(
        cg.RawStatement('#include "esphome/components/vlux_settings/vlux_settings.h"')
    )
