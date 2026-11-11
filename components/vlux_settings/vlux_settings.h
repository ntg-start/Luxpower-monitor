#pragma once
// Tiện ích cho trang Cài đặt trên màn hình.
// Lưu ý: toàn bộ chuỗi trả về đều KHÔNG DẤU — font montserrat không có glyph
// tiếng Việt (xem ghi chú ở lunar.h).

#include "esphome/core/helpers.h"
#include "esphome/components/wifi/wifi_component.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace vlux_settings {

/// Danh sách SSID duy nhất từ lần quét gần nhất, sóng mạnh nhất lên đầu.
/// Bỏ mạng ẩn và SSID rỗng. Trùng SSID (nhiều AP cùng tên) chỉ giữ một.
inline std::vector<std::string> ssid_list() {
  std::vector<std::pair<int8_t, std::string>> tmp;
  {
    // Đọc từ main loop nên lock là no-op, vẫn giữ cho đúng giao kèo của API.
    esphome::wifi::ScanResultsLock lock(esphome::wifi::global_wifi_component);
    for (const auto &r : esphome::wifi::global_wifi_component->get_scan_result()) {
      if (r.get_is_hidden())
        continue;
      std::string s = r.get_ssid().str();
      if (s.empty())
        continue;
      bool dup = false;
      for (auto &e : tmp) {
        if (e.second == s) {
          if (r.get_rssi() > e.first)
            e.first = r.get_rssi();
          dup = true;
          break;
        }
      }
      if (!dup)
        tmp.emplace_back(r.get_rssi(), s);
    }
  }
  std::sort(tmp.begin(), tmp.end(),
            [](const std::pair<int8_t, std::string> &a, const std::pair<int8_t, std::string> &b) {
              return a.first > b.first;
            });
  std::vector<std::string> out;
  out.reserve(tmp.size());
  for (auto &e : tmp)
    out.push_back(e.second);
  return out;
}

/// Chuỗi "a\nb\nc" để nạp thẳng vào lv_dropdown_set_options().
inline std::string ssid_options() {
  auto v = ssid_list();
  if (v.empty())
    return std::string("(chua thay mang nao)");
  std::string out;
  for (size_t i = 0; i < v.size(); i++) {
    if (i != 0)
      out += '\n';
    out += v[i];
  }
  return out;
}

/// SSID theo chỉ số đang chọn trong dropdown. Rỗng nếu chỉ số không hợp lệ.
inline std::string ssid_at(uint16_t idx) {
  auto v = ssid_list();
  if (idx < v.size())
    return v[idx];
  return std::string();
}

inline uint16_t ssid_count() { return (uint16_t) ssid_list().size(); }

/// Mã thiết bị suy ra từ MAC, dạng 7CD5-443C-2ED0.
/// Đây là mỏ neo tự nhiên cho cơ chế chống sao chép mạch: MAC nằm trong eFuse
/// của chip, không sửa được bằng phần mềm. Một bản chống sao chép thật sự cần
/// thêm chữ ký từ máy chủ cấp phép — PHẦN ĐÓ CHƯA LÀM.
inline std::string device_id() {
  std::string m = esphome::get_mac_address();  // 12 ký tự hex, không dấu phân cách
  std::string out;
  for (size_t i = 0; i < m.size(); i++) {
    if (i != 0 && (i % 4) == 0)
      out += '-';
    out += (char) std::toupper((unsigned char) m[i]);
  }
  return out;
}

}  // namespace vlux_settings
