#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/socket/socket.h"

#include <string>
#include <array>
#include <memory>

namespace esphome {
namespace luxpower_tcp {

static const uint16_t LUX_MAX_REG = 256;
static const size_t LUX_RX_BUF = 1024;
static const uint8_t SCAN_SLOTS = 4;  // phải khớp SCAN_SLOTS trong __init__.py

// Lưu vào flash để lần sau khỏi dò lại (giống lux_ip/dongle_sn/inv_sn trong NVS bản gốc)
struct LuxCache {
  char ip[16];
  char dongle_sn[11];
  char inv_sn[11];
  uint16_t port;
  uint8_t valid;
} __attribute__((packed));

class LuxpowerTcp : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_host(const std::string &h) { this->cfg_host_ = h; }
  void set_port(uint16_t p) { this->port_ = p; }
  void set_dongle_serial(const std::string &s) { this->cfg_dongle_sn_ = s; }
  void set_inverter_serial(const std::string &s) { this->cfg_inv_sn_ = s; }
  void set_auto_discover(bool v) { this->auto_discover_ = v; }
  // chỉnh được lúc chạy để dò giá trị tối ưu mà không cần nạp lại
  void set_reconnect_gap(uint32_t ms) { this->reconnect_gap_ = ms; }
  void set_frames_total(sensor::Sensor *s) { frames_total_ = s; }

  void set_online(binary_sensor::BinarySensor *s) { online_ = s; }
  void set_dongle_ip(text_sensor::TextSensor *s) { ts_ip_ = s; }
  void set_dongle_sn_text(text_sensor::TextSensor *s) { ts_dsn_ = s; }
  void set_inverter_sn_text(text_sensor::TextSensor *s) { ts_isn_ = s; }

  void set_pv_power(sensor::Sensor *s) { pv_power_ = s; }
  void set_pv1_power(sensor::Sensor *s) { pv1_power_ = s; }
  void set_pv2_power(sensor::Sensor *s) { pv2_power_ = s; }
  void set_pv3_power(sensor::Sensor *s) { pv3_power_ = s; }
  void set_battery_voltage(sensor::Sensor *s) { battery_voltage_ = s; }
  void set_battery_soc(sensor::Sensor *s) { battery_soc_ = s; }
  void set_battery_power(sensor::Sensor *s) { battery_power_ = s; }
  void set_grid_power(sensor::Sensor *s) { grid_power_ = s; }
  void set_load_power(sensor::Sensor *s) { load_power_ = s; }
  void set_grid_voltage(sensor::Sensor *s) { grid_voltage_ = s; }
  void set_grid_freq(sensor::Sensor *s) { grid_freq_ = s; }
  void set_pv_today(sensor::Sensor *s) { pv_today_ = s; }
  void set_charge_today(sensor::Sensor *s) { charge_today_ = s; }
  void set_discharge_today(sensor::Sensor *s) { discharge_today_ = s; }
  void set_grid_import_today(sensor::Sensor *s) { grid_import_today_ = s; }
  void set_grid_export_today(sensor::Sensor *s) { grid_export_today_ = s; }
  void set_temp_inner(sensor::Sensor *s) { temp_inner_ = s; }
  void set_temp_radiator(sensor::Sensor *s) { temp_radiator_ = s; }

 protected:
  enum class State : uint8_t { IDLE, DISCOVER_UDP, SCANNING, CONNECTING, CONNECTED };

  // dò tìm bằng UDP broadcast (giao thức HF-A11, giống firmware gốc)
  void udp_discover_begin_();
  void udp_discover_step_();
  static bool parse_hf_reply_(const char *s, std::string &ip, std::string &name);

  // kết nối / dò tìm
  void start_connect_(const std::string &ip);
  void poll_connect_();
  void scan_begin_();
  void scan_step_();
  void scan_abort_();
  void adopt_socket_(std::unique_ptr<socket::Socket> s, const std::string &ip);
  void close_socket_(const char *reason);
  bool local_prefix_(std::string &prefix, uint8_t &own_octet);

  // I/O
  void pump_rx_();
  void drain_frames_();
  void send_next_request_();
  size_t build_read_request_(uint8_t *out, uint16_t start, uint16_t count);
  bool parse_frame_(const uint8_t *buf, size_t len);
  void learn_serials_(const uint8_t *buf);
  void publish_all_();
  void invalidate_regs_();
  void set_online_(bool v);
  void save_cache_();

  static uint16_t crc16_modbus_(const uint8_t *data, size_t len);
  bool serials_known_() const { return !dongle_sn_.empty() && !inv_sn_.empty(); }

  bool has_reg_(uint16_t r) const { return r < LUX_MAX_REG && reg_valid_[r]; }
  uint16_t reg_u16_(uint16_t r) const { return regs_[r]; }
  int16_t reg_s16_(uint16_t r) const { return (int16_t) regs_[r]; }

  // cấu hình
  std::string cfg_host_, cfg_dongle_sn_, cfg_inv_sn_;
  uint16_t port_{8000};        // cổng khai trong YAML
  uint16_t cur_port_{0};       // cổng đang dùng (có thể tự đổi sang cổng thay thế)
  uint8_t port_tries_{0};      // số cổng đã quét hết trong lượt này
  bool auto_discover_{true};
  uint32_t reconnect_gap_{1000};
  uint32_t frames_total_count_{0};
  sensor::Sensor *frames_total_{nullptr};
  uint16_t alt_port_() const;  // cổng thay thế (8000 <-> 4346), 0 nếu không có

  // trạng thái hiện hành
  std::string active_ip_, dongle_sn_, inv_sn_;

  std::unique_ptr<socket::Socket> sock_;
  std::unique_ptr<socket::Socket> udp_;
  uint32_t udp_started_{0};
  State state_{State::IDLE};
  uint32_t connect_started_{0};
  uint32_t retry_after_{0};
  uint8_t connect_fails_{0};
  uint32_t connected_at_{0};
  bool got_valid_frame_{false};

  // quét mạng
  struct ScanSlot {
    std::unique_ptr<socket::Socket> sock;
    uint32_t started;
    uint8_t octet;
    bool active;
  };
  ScanSlot slots_[SCAN_SLOTS];
  std::string scan_prefix_;
  // IP bắt tay TCP được nhưng đóng ngay, không gửi byte nào: chữ ký của dongle
  // đang cạn socket. Giữ lại để thử kỹ sau khi quét xong thay vì loại thẳng.
  uint8_t cand_octets_[6]{};
  uint8_t cand_count_{0};
  uint16_t scan_next_{1};
  uint8_t scan_skip_{0};
  bool tried_cached_{false};

  uint8_t rx_buf_[LUX_RX_BUF];
  size_t rx_len_{0};

  int8_t next_bank_{-1};       // bank đang chờ gửi (-1 = không có)
  uint32_t last_send_{0};
  uint32_t last_publish_ms_{0};
  uint32_t last_data_ms_{0};   // lần cuối nhận frame 0x04 hợp lệ
  bool ever_got_data_{false};  // IP này đã từng cho dữ liệu thật
  bool cached_ok_{false};      // đã lưu IP/serial vào flash chưa
  std::string cache_ip_;       // IP đã từng cho dữ liệu (nạp từ flash) — phương án quay về
  bool link_ready_{false};     // đã nhận byte đầu -> TCP chắc chắn established
  uint8_t silent_sessions_{0}; // số phiên liên tiếp nối được nhưng dongle im lặng
  bool peer_closed_{false};    // dongle đã đóng — đóng SAU khi giải mã hết buffer

  std::array<uint16_t, LUX_MAX_REG> regs_{};
  std::array<bool, LUX_MAX_REG> reg_valid_{};

  ESPPreferenceObject pref_;

  binary_sensor::BinarySensor *online_{nullptr};
  bool online_state_{false};
  text_sensor::TextSensor *ts_ip_{nullptr};
  text_sensor::TextSensor *ts_dsn_{nullptr};
  text_sensor::TextSensor *ts_isn_{nullptr};

  sensor::Sensor *pv_power_{nullptr};
  sensor::Sensor *pv1_power_{nullptr};
  sensor::Sensor *pv2_power_{nullptr};
  sensor::Sensor *pv3_power_{nullptr};
  sensor::Sensor *battery_voltage_{nullptr};
  sensor::Sensor *battery_soc_{nullptr};
  sensor::Sensor *battery_power_{nullptr};
  sensor::Sensor *grid_power_{nullptr};
  sensor::Sensor *load_power_{nullptr};
  sensor::Sensor *grid_voltage_{nullptr};
  sensor::Sensor *grid_freq_{nullptr};
  sensor::Sensor *pv_today_{nullptr};
  sensor::Sensor *charge_today_{nullptr};
  sensor::Sensor *discharge_today_{nullptr};
  sensor::Sensor *grid_import_today_{nullptr};
  sensor::Sensor *grid_export_today_{nullptr};
  sensor::Sensor *temp_inner_{nullptr};
  sensor::Sensor *temp_radiator_{nullptr};
};

}  // namespace luxpower_tcp
}  // namespace esphome
