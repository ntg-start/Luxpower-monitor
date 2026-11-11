#include "luxpower_tcp.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/network/util.h"
#include <cerrno>
#include <cstring>

namespace esphome {
namespace luxpower_tcp {

static const char *const TAG = "luxpower_tcp";

struct Bank { uint16_t start; uint16_t count; };
static const Bank BANKS[] = {{0, 40}, {40, 40}};
static const int8_t NUM_BANKS = 2;

static const uint8_t LXP_PREFIX0 = 0xA1;
static const uint8_t LXP_PREFIX1 = 0x1A;
static const uint8_t TCP_FUNC_TRANSLATED = 0xC2;
static const uint8_t TCP_FUNC_HEARTBEAT = 0xC1;
static const uint8_t DEV_FUNC_READ_INPUT = 0x04;
static const uint8_t DEV_FUNC_READ_HOLD = 0x03;

static const uint32_t CONNECT_TIMEOUT_MS = 4000;
static const uint32_t NOT_DONGLE_MS = 8000;
static const uint32_t STALE_MS = 60000;        // quá lâu không có dữ liệu -> offline
static const uint32_t SCAN_SLOT_TIMEOUT_MS = 2000;  // dongle đẩy frame ngay sau khi nối   // thời gian thử mỗi IP khi quét
static const size_t MAX_READ_PER_LOOP = 512;


// ------------------------------------------------------------------ setup ---

// Cổng thay thế: hai cổng Luxpower thường gặp là 4346 và 8000.
uint16_t LuxpowerTcp::alt_port_() const {
  if (this->port_ == 4346)
    return 8000;
  if (this->port_ == 8000)
    return 4346;
  return 0;  // cổng tuỳ chỉnh -> không tự đổi
}

void LuxpowerTcp::setup() {
  this->regs_.fill(0);
  this->reg_valid_.fill(false);
  this->cur_port_ = this->port_;

  // ưu tiên giá trị khai trong YAML (nếu có)
  this->active_ip_ = this->cfg_host_;
  this->dongle_sn_ = this->cfg_dongle_sn_;
  this->inv_sn_ = this->cfg_inv_sn_;

  // nạp cache từ flash
  this->pref_ = global_preferences->make_preference<LuxCache>(fnv1_hash("luxpower_tcp_cache_v2"));
  LuxCache c{};
  if (this->pref_.load(&c) && c.valid == 1) {
    if (c.port != 0)
      this->cur_port_ = c.port;  // cổng đã dùng được lần trước
    c.ip[sizeof(c.ip) - 1] = 0;
    c.dongle_sn[sizeof(c.dongle_sn) - 1] = 0;
    c.inv_sn[sizeof(c.inv_sn) - 1] = 0;
    if (c.ip[0])
      this->cache_ip_ = c.ip;  // IP đã từng cho dữ liệu — luôn giữ làm phương án quay về
    if (this->active_ip_.empty() && c.ip[0])
      this->active_ip_ = c.ip;
    if (this->dongle_sn_.empty() && c.dongle_sn[0])
      this->dongle_sn_ = c.dongle_sn;
    if (this->inv_sn_.empty() && c.inv_sn[0])
      this->inv_sn_ = c.inv_sn;
    ESP_LOGI(TAG, "Nạp cache: ip=%s cổng=%u dongle=%s inv=%s", this->active_ip_.c_str(),
             this->cur_port_, this->dongle_sn_.c_str(), this->inv_sn_.c_str());
  }
  this->set_online_(false);
  // đưa giá trị cache lên text_sensor để trang 3 không trống
  if (this->ts_ip_ != nullptr && !this->active_ip_.empty())
    this->ts_ip_->publish_state(this->active_ip_);
  if (this->ts_dsn_ != nullptr && !this->dongle_sn_.empty())
    this->ts_dsn_->publish_state(this->dongle_sn_);
  if (this->ts_isn_ != nullptr && !this->inv_sn_.empty())
    this->ts_isn_->publish_state(this->inv_sn_);
}

void LuxpowerTcp::dump_config() {
  ESP_LOGCONFIG(TAG, "Luxpower TCP:");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  Host: %s", this->cfg_host_.empty() ? "(tự dò)" : this->cfg_host_.c_str());
  ESP_LOGCONFIG(TAG, "  Dongle SN: %s",
                this->cfg_dongle_sn_.empty() ? "(tự học)" : this->cfg_dongle_sn_.c_str());
  ESP_LOGCONFIG(TAG, "  Inverter SN: %s",
                this->cfg_inv_sn_.empty() ? "(tự học)" : this->cfg_inv_sn_.c_str());
  LOG_UPDATE_INTERVAL(this);
}

void LuxpowerTcp::save_cache_() {
  LuxCache c{};
  // Chỉ ghi IP đã THỰC SỰ cho dữ liệu, để một lần dò nhầm không xoá mất IP tốt trong flash
  strncpy(c.ip, this->cache_ip_.empty() ? this->active_ip_.c_str() : this->cache_ip_.c_str(),
          sizeof(c.ip) - 1);
  strncpy(c.dongle_sn, this->dongle_sn_.c_str(), sizeof(c.dongle_sn) - 1);
  strncpy(c.inv_sn, this->inv_sn_.c_str(), sizeof(c.inv_sn) - 1);
  c.port = this->cur_port_;
  c.valid = 1;
  this->pref_.save(&c);
}

// -------------------------------------------------------------- khung LXP ---

uint16_t LuxpowerTcp::crc16_modbus_(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
  }
  return crc;
}

// Khung ReadInput 38 byte — sao chép chính xác FUN_400d2f74 của firmware gốc.
// Điểm mấu chốt: protocol = 1 (không phải 2 hay 5). Sai byte này dongle im lặng.
size_t LuxpowerTcp::build_read_request_(uint8_t *out, uint16_t start, uint16_t count) {
  memset(out, 0, 38);
  out[0] = LXP_PREFIX0;
  out[1] = LXP_PREFIX1;
  out[2] = 1;    // protocol = 1  <-- firmware gốc
  out[3] = 0;
  out[4] = 0x20; // frame_length = 32
  out[5] = 0;
  out[6] = 1;    // address
  out[7] = TCP_FUNC_TRANSLATED;
  for (int i = 0; i < 10; i++)
    out[8 + i] = (i < (int) this->dongle_sn_.size()) ? (uint8_t) this->dongle_sn_[i] : 0;
  out[18] = 0x12;  // data_len = 18
  out[19] = 0;
  out[20] = 0;     // data address = 0
  out[21] = DEV_FUNC_READ_INPUT;
  for (int i = 0; i < 10; i++)
    out[22 + i] = (i < (int) this->inv_sn_.size()) ? (uint8_t) this->inv_sn_[i] : 0;
  out[32] = start & 0xFF;
  out[33] = start >> 8;
  out[34] = count & 0xFF;
  out[35] = count >> 8;
  uint16_t crc = crc16_modbus_(out + 20, 16);
  out[36] = crc & 0xFF;
  out[37] = crc >> 8;
  return 38;
}

void LuxpowerTcp::send_next_request_() {
  if (this->next_bank_ < 0 || this->next_bank_ >= NUM_BANKS)
    return;
  if (!this->serials_known_() || !this->sock_)
    return;
  // Dongle chỉ giữ kết nối khoảng 1,1-1,5 giây rồi TỰ ĐÓNG, và nó không tự đẩy
  // dữ liệu 0x04 — phải hỏi mới có. Nên:
  //   * gửi ngay khi link chắc chắn đã established (đã nhận byte đầu tiên) thay vì
  //     chờ một mốc thời gian cố định — getpeername() trên lwIP báo "đã nối" ngay
  //     khi vừa gửi SYN nên không thể tin, nhưng byte nhận được thì không nói dối;
  //   * gửi HẾT các bank trong CÙNG một vòng loop. Trên CYD có LVGL, mỗi vòng loop
  //     có thể kéo dài hàng trăm ms — tách mỗi bank một vòng là trượt khỏi cửa sổ
  //     1,3 giây và mất bank sau.
  if (!this->link_ready_)
    return;
  while (this->next_bank_ >= 0 && this->next_bank_ < NUM_BANKS) {
    uint8_t req[38];
    size_t n =
        build_read_request_(req, BANKS[this->next_bank_].start, BANKS[this->next_bank_].count);
    ssize_t w = this->sock_->write(req, n);
    if (w < 0) {
      // ENOTCONN/EINPROGRESS: lwIP báo "đã nối" qua getpeername ngay khi vừa gửi SYN,
      // nên có thể tới đây trước khi bắt tay xong. Chưa mất bank nào — cứ thử lại
      // vòng sau. Nếu thật sự không nối được thì NOT_DONGLE_MS sẽ dọn sau 8 giây.
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN ||
          errno == EINPROGRESS || errno == EALREADY)
        return;
      ESP_LOGW(TAG, "write() lỗi errno=%d", errno);
      this->close_socket_("lỗi gửi request");
      return;
    }
    ESP_LOGV(TAG, "TX req bank=%d reg=%u cnt=%u w=%d", this->next_bank_,
             BANKS[this->next_bank_].start, BANKS[this->next_bank_].count, (int) w);
    this->last_send_ = millis();
    if (++this->next_bank_ >= NUM_BANKS) {
      this->next_bank_ = -1;
      break;
    }
  }
}

// ---------------------------------------------------------------- helpers ---

void LuxpowerTcp::set_online_(bool v) {
  if (v == this->online_state_ && this->online_ != nullptr && this->online_->has_state())
    return;
  this->online_state_ = v;
  if (this->online_ != nullptr)
    this->online_->publish_state(v);
}

void LuxpowerTcp::invalidate_regs_() { this->reg_valid_.fill(false); }

void LuxpowerTcp::close_socket_(const char *reason) {
  if (this->sock_) {
    this->sock_->close();
    this->sock_ = nullptr;
    ESP_LOGW(TAG, "Đóng kết nối: %s", reason);
  }
  // Phải đọc hai cờ này TRƯỚC khi xoá
  bool was_connected = (this->state_ == State::CONNECTED);
  bool silent = was_connected && !this->got_valid_frame_;
  this->state_ = State::IDLE;
  this->rx_len_ = 0;
  this->got_valid_frame_ = false;
  this->link_ready_ = false;
  if (was_connected) {
    if (silent) {
      if (this->silent_sessions_ < 8)
        this->silent_sessions_++;
    } else {
      this->silent_sessions_ = 0;
    }
  }
  // Dongle chỉ có ~5 socket (đo thực tế). Khi pool cạn — còn socket treo từ phiên
  // trước (ESP32 mất điện đột ngột không gửi FIN) hoặc từ client khác — nó vẫn bắt
  // tay TCP bình thường rồi ĐÓNG NGAY mà không gửi byte nào. Nối lại dồn dập lúc đó
  // chỉ giữ cho nó cạn mãi; phải giãn dần để nó kịp thu hồi socket chết.
  if (this->silent_sessions_ >= 3) {
    uint32_t b = 1000u * (1u << (this->silent_sessions_ - 2));  // 2s,4s,8s... tối đa 60s
    if (b > 60000u)
      b = 60000u;
    this->retry_after_ = millis() + b;
    ESP_LOGW(TAG, "Dongle nhận kết nối nhưng im lặng %u lần liên tiếp — nhiều khả năng cạn "
                  "socket, chờ %u ms cho nó hồi.",
             this->silent_sessions_, (unsigned) b);
    // Chặn kịch bản bám nhầm: một máy khác cũng mở cổng 8000 rồi đóng ngay sẽ giữ
    // ta ở đây mãi. Im lặng quá nhiều lần mà chưa bao giờ cho dữ liệu -> bỏ IP,
    // quay lại dò. Không áp dụng cho host khai trong YAML hay IP tốt trong flash.
    if (this->silent_sessions_ >= 6 && !this->ever_got_data_ && this->cfg_host_.empty() &&
        this->auto_discover_ && this->active_ip_ != this->cache_ip_) {
      ESP_LOGW(TAG, "%s im lặng quá nhiều lần và chưa từng cho dữ liệu — dò lại.",
               this->active_ip_.c_str());
      this->active_ip_.clear();
      this->silent_sessions_ = 0;
    }
    return;
  }
  // Dongle Luxpower đóng kết nối sau mỗi vài frame — đó là hành vi BÌNH THƯỜNG.
  // Nếu IP này đã từng cho dữ liệu, nối lại ngay (1 giây), không backoff dài.
  if (this->ever_got_data_) {
    this->retry_after_ = millis() + this->reconnect_gap_;
  } else {
    this->set_online_(false);
    this->invalidate_regs_();
    uint32_t backoff = 1000u * (1u << (this->connect_fails_ < 4 ? this->connect_fails_ : 4));
    this->retry_after_ = millis() + backoff;
  }
}

// Lấy prefix /24 của mạng hiện tại, ví dụ "192.168.1." + octet cuối của chính mình
bool LuxpowerTcp::local_prefix_(std::string &prefix, uint8_t &own_octet) {
  for (auto &ip : network::get_ip_addresses()) {
    char buf[network::IP_ADDRESS_BUFFER_SIZE];
    ip.str_to(buf);
    std::string s(buf);
    if (s.empty() || s == "0.0.0.0" || s.find(':') != std::string::npos)
      continue;  // bỏ IPv6 / chưa có IP
    size_t dot = s.rfind('.');
    if (dot == std::string::npos)
      continue;
    prefix = s.substr(0, dot + 1);
    own_octet = (uint8_t) atoi(s.c_str() + dot + 1);
    return true;
  }
  return false;
}

// ------------------------------------------- dò bằng UDP broadcast (HF-A11) ---
// Firmware gốc: udp.begin(48899); gửi "HF-A11ASSISTANT" tới 255.255.255.255:48899;
// module WiFi HF-A11 của dongle trả về chuỗi "IP,MAC,TÊN" (tên chứa serial dongle).

static const uint16_t HF_PORT = 48899;
static const char *const HF_PROBE = "HF-A11ASSISTANT";
static const uint32_t UDP_WAIT_MS = 3000;  // bản gốc chờ 2999 ms

bool LuxpowerTcp::parse_hf_reply_(const char *s, std::string &ip, std::string &name) {
  std::string str(s);
  size_t c1 = str.find(',');
  if (c1 == std::string::npos)
    return false;
  size_t c2 = str.find(',', c1 + 1);
  if (c2 == std::string::npos)
    return false;
  ip = str.substr(0, c1);
  name = str.substr(c2 + 1);
  while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' ' ||
                           name.back() == '\0'))
    name.pop_back();
  // bản gốc yêu cầu trường tên dài hơn 7 ký tự
  if (name.size() <= 7)
    return false;
  if (ip.size() < 7 || ip.size() > 15 || ip.find('.') == std::string::npos)
    return false;
  return true;
}

void LuxpowerTcp::udp_discover_begin_() {
  this->udp_ = socket::socket_ip(SOCK_DGRAM, IPPROTO_UDP);
  if (!this->udp_) {
    ESP_LOGW(TAG, "Không tạo được socket UDP (errno=%d) — bỏ qua broadcast, quét dải.", errno);
    this->scan_begin_();
    return;
  }
  this->udp_->setblocking(false);
  int on = 1;
  this->udp_->setsockopt(SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

  struct sockaddr_storage ba {};
  socklen_t bl = socket::set_sockaddr_any((struct sockaddr *) &ba, sizeof(ba), HF_PORT);
  if (bl == 0 || this->udp_->bind((struct sockaddr *) &ba, bl) != 0) {
    ESP_LOGW(TAG, "Không bind được UDP %u, chuyển sang quét dải.", HF_PORT);
    this->udp_ = nullptr;
    this->scan_begin_();
    return;
  }

  struct sockaddr_storage da {};
  socklen_t dl =
      socket::set_sockaddr((struct sockaddr *) &da, sizeof(da), "255.255.255.255", HF_PORT);
  ssize_t w = this->udp_->sendto(HF_PROBE, strlen(HF_PROBE), 0, (struct sockaddr *) &da, dl);
  if (w < 0) {
    ESP_LOGW(TAG, "Gửi broadcast thất bại (errno=%d), chuyển sang quét dải.", errno);
    this->udp_ = nullptr;
    this->scan_begin_();
    return;
  }
  ESP_LOGI(TAG, "Đã gửi broadcast '%s' tới 255.255.255.255:%u, chờ phản hồi...", HF_PROBE, HF_PORT);
  this->udp_started_ = millis();
  this->state_ = State::DISCOVER_UDP;
}

void LuxpowerTcp::udp_discover_step_() {
  char buf[256];
  struct sockaddr_storage src {};
  socklen_t sl = sizeof(src);
  ssize_t n = this->udp_->recvfrom(buf, sizeof(buf) - 1, (struct sockaddr *) &src, &sl);
  if (n > 0) {
    buf[n] = 0;
    std::string ip, name;
    if (parse_hf_reply_(buf, ip, name)) {
      ESP_LOGI(TAG, "Thấy dongle: IP=%s  tên=%s", ip.c_str(), name.c_str());
      if (this->dongle_sn_.empty() && name.size() >= 10) {
        this->dongle_sn_ = name.substr(0, 10);
        ESP_LOGI(TAG, "Lấy dongle SN từ broadcast: %s", this->dongle_sn_.c_str());
        if (this->ts_dsn_ != nullptr)
          this->ts_dsn_->publish_state(this->dongle_sn_);
      }
      this->udp_ = nullptr;
      this->active_ip_ = ip;
      this->cur_port_ = this->port_;
      this->start_connect_(ip);
      return;
    }
    ESP_LOGV(TAG, "Bỏ qua gói UDP lạ: %s", buf);
  }
  if (millis() - this->udp_started_ > UDP_WAIT_MS) {
    ESP_LOGI(TAG, "Không thấy phản hồi UDP (bình thường với dongle Luxpower) — quét dải mạng.");
    this->udp_ = nullptr;
    this->scan_begin_();
  }
}

// ----------------------------------------------------------------- quét ----

void LuxpowerTcp::scan_abort_() {
  for (auto &sl : this->slots_) {
    if (sl.active && sl.sock) {
      sl.sock->close();
      sl.sock = nullptr;
    }
    sl.active = false;
  }
}

void LuxpowerTcp::scan_begin_() {
  uint8_t own = 0;
  if (!this->local_prefix_(this->scan_prefix_, own)) {
    ESP_LOGW(TAG, "Chưa có IP, hoãn quét.");
    this->retry_after_ = millis() + 3000;
    return;
  }
  this->scan_skip_ = own;
  this->scan_next_ = 1;
  this->cand_count_ = 0;
  this->port_tries_ = 0;
  this->scan_abort_();
  this->state_ = State::SCANNING;
  ESP_LOGI(TAG, "Dò dongle trên %s0/24 cổng %u ...", this->scan_prefix_.c_str(), this->cur_port_);
}

void LuxpowerTcp::scan_step_() {
  // 1) Kiểm tra các slot đang thử.
  //    KHÔNG dựa vào getpeername(): trên lwIP nó trả về thành công ngay khi connect
  //    vừa khởi động, nên mọi IP đều trông như "nối được".
  //    Chỉ nhận một IP khi nó THỰC SỰ đẩy ra frame bắt đầu bằng A1 1A.
  for (auto &sl : this->slots_) {
    if (!sl.active)
      continue;
    if (sl.sock) {
      uint8_t tmp[128];
      ssize_t n = sl.sock->read(tmp, sizeof(tmp));
      if (n > 0) {
        bool is_lxp = false;
        for (ssize_t k = 0; k + 1 < n; k++) {
          if (tmp[k] == LXP_PREFIX0 && tmp[k + 1] == LXP_PREFIX1) {
            is_lxp = true;
            break;
          }
        }
        std::string ip = this->scan_prefix_ + std::to_string(sl.octet);
        if (is_lxp) {
          ESP_LOGI(TAG, "Tìm thấy DONGLE tại %s:%u (đã nhận frame LXP)", ip.c_str(),
                   this->cur_port_);
          auto s = std::move(sl.sock);
          sl.active = false;
          this->scan_abort_();
          this->adopt_socket_(std::move(s), ip);
          // giữ lại các byte vừa đọc để không mất frame đầu
          size_t keep = (size_t) n < LUX_RX_BUF ? (size_t) n : LUX_RX_BUF;
          memcpy(this->rx_buf_, tmp, keep);
          this->rx_len_ = keep;
          this->link_ready_ = true;  // đã có byte thật từ IP này
          this->drain_frames_();
          return;
        }
        ESP_LOGD(TAG, "%s mở cổng %u nhưng không phải Luxpower — bỏ qua", ip.c_str(),
                 this->cur_port_);
        sl.sock->close();
        sl.sock = nullptr;
        sl.active = false;
        continue;
      }
      if (n == 0) {
        // read()==0 chỉ xảy ra khi phiên ĐÃ established rồi bị peer đóng. Nhận kết
        // nối rồi đóng ngay mà không gửi byte nào chính là chữ ký của dongle đang
        // cạn socket — không được kết luận "không phải Luxpower".
        if ((size_t) this->cand_count_ < sizeof(this->cand_octets_))
          this->cand_octets_[this->cand_count_++] = sl.octet;
        ESP_LOGD(TAG, "%s%u nhận kết nối rồi đóng ngay (0 byte) — ghi làm ứng viên.",
                 this->scan_prefix_.c_str(), sl.octet);
        sl.sock->close();
        sl.sock = nullptr;
        sl.active = false;
        continue;
      }
    }
    if (millis() - sl.started > SCAN_SLOT_TIMEOUT_MS) {
      if (sl.sock)
        sl.sock->close();
      sl.sock = nullptr;
      sl.active = false;
    }
  }

  // 2) nạp thêm IP mới vào slot trống
  for (auto &sl : this->slots_) {
    if (sl.active)
      continue;
    while (this->scan_next_ <= 254 && this->scan_next_ == this->scan_skip_)
      this->scan_next_++;
    if (this->scan_next_ > 254)
      break;
    uint8_t octet = (uint8_t) this->scan_next_++;
    std::string ip = this->scan_prefix_ + std::to_string(octet);

    auto s = socket::socket_ip(SOCK_STREAM, IPPROTO_TCP);
    if (!s) {
      ESP_LOGW(TAG, "Hết socket khi quét (errno=%d) — giảm số kết nối song song.", errno);
      break;
    }
    s->setblocking(false);
    struct sockaddr_storage addr {};
    socklen_t alen = socket::set_sockaddr((struct sockaddr *) &addr, sizeof(addr), ip, this->cur_port_);
    if (alen == 0)
      continue;
    int r = s->connect((struct sockaddr *) &addr, alen);
    if (r != 0 && errno != EINPROGRESS && errno != EALREADY)
      continue;  // IP này hỏng ngay, bỏ qua
    sl.sock = std::move(s);
    sl.octet = octet;
    sl.started = millis();
    sl.active = true;
  }

  // log tiến độ mỗi 64 IP
  if ((this->scan_next_ % 64) == 0 && this->scan_next_ <= 254) {
    ESP_LOGD(TAG, "Quét ... %s%u", this->scan_prefix_.c_str(), this->scan_next_);
  }

  // 3) quét xong mà không thấy gì
  bool any_active = false;
  for (auto &sl : this->slots_)
    any_active |= sl.active;
  if (this->scan_next_ > 254 && !any_active) {
    uint16_t alt = this->alt_port_();
    this->port_tries_++;
    if (alt != 0 && this->port_tries_ < 2) {
      // Quét hết dải ở cổng này mà không thấy -> tự thử cổng còn lại
      this->cur_port_ = (this->cur_port_ == this->port_) ? alt : this->port_;
      ESP_LOGW(TAG, "Không thấy dongle ở cổng %u — thử lại toàn dải với cổng %u.",
               (this->cur_port_ == alt) ? this->port_ : alt, this->cur_port_);
      this->scan_next_ = 1;
      return;  // quét tiếp ngay với cổng mới
    }
    ESP_LOGW(TAG, "Quét hết %s0/24 ở cả cổng %u và %u mà không thấy dongle.",
             this->scan_prefix_.c_str(), this->port_, alt);
    this->port_tries_ = 0;
    this->cur_port_ = this->port_;
    this->state_ = State::IDLE;
    // Quét trượt KHÔNG có nghĩa dongle biến mất: lúc cạn socket nó bắt tay TCP rồi
    // đóng ngay, không kịp đẩy frame nào nên bị loại ở bước nhận diện. Quay về IP
    // đã lưu trong flash thay vì ngồi chờ rồi quét lại vô ích.
    if (this->cand_count_ > 0) {
      // Ưu tiên ứng viên "nhận rồi đóng ngay" — gần như chắc chắn là dongle đang bận
      this->active_ip_ = this->scan_prefix_ + std::to_string(this->cand_octets_[0]);
      for (uint8_t k = 1; k < this->cand_count_; k++)
        this->cand_octets_[k - 1] = this->cand_octets_[k];
      this->cand_count_--;
      ESP_LOGI(TAG, "Thử lại ứng viên im lặng: %s", this->active_ip_.c_str());
      this->retry_after_ = millis() + 3000;
    } else if (!this->cache_ip_.empty()) {
      this->active_ip_ = this->cache_ip_;
      ESP_LOGI(TAG, "Quay lại IP đã lưu trong flash: %s", this->active_ip_.c_str());
      this->retry_after_ = millis() + 5000;
    } else {
      this->retry_after_ = millis() + 30000;
    }
  }
}

// ------------------------------------------------------------- kết nối -----

void LuxpowerTcp::adopt_socket_(std::unique_ptr<socket::Socket> s, const std::string &ip) {
  this->sock_ = std::move(s);
  this->active_ip_ = ip;
  this->state_ = State::CONNECTED;
  this->connect_fails_ = 0;
  this->connected_at_ = millis();
  this->got_valid_frame_ = false;
  this->peer_closed_ = false;
  this->link_ready_ = false;
  this->rx_len_ = 0;
  this->next_bank_ = 0;   // hỏi ngay khi vừa nối
  this->set_online_(true);
  if (this->ts_ip_ != nullptr)
    this->ts_ip_->publish_state(ip);
  ESP_LOGI(TAG, "Đã kết nối dongle tại %s:%u", ip.c_str(), this->cur_port_);
  this->port_tries_ = 0;
  this->save_cache_();  // nhớ IP + cổng ngay, serial học sau
  if (!this->serials_known_())
    ESP_LOGI(TAG, "Chưa biết serial — chờ frame dongle đẩy ra để học...");
}

void LuxpowerTcp::start_connect_(const std::string &ip) {
  auto s = socket::socket_ip(SOCK_STREAM, IPPROTO_TCP);
  if (!s) {
    ESP_LOGE(TAG, "Không tạo được socket TCP (errno=%d) — hết socket trong pool lwIP?", errno);
    this->retry_after_ = millis() + 5000;
    return;
  }
  s->setblocking(false);
  struct sockaddr_storage addr {};
  socklen_t alen = socket::set_sockaddr((struct sockaddr *) &addr, sizeof(addr), ip, this->cur_port_);
  if (alen == 0) {
    ESP_LOGE(TAG, "IP không hợp lệ: '%s' (phải là địa chỉ IP)", ip.c_str());
    this->active_ip_.clear();
    this->retry_after_ = millis() + 5000;
    return;
  }
  ESP_LOGI(TAG, "Kết nối %s:%u ...", ip.c_str(), this->cur_port_);
  int r = s->connect((struct sockaddr *) &addr, alen);
  if (r != 0 && errno != EINPROGRESS && errno != EALREADY) {
    this->connect_fails_++;
    this->retry_after_ = millis() + 3000;
    return;
  }
  this->sock_ = std::move(s);
  this->connect_started_ = millis();
  this->state_ = State::CONNECTING;
}

void LuxpowerTcp::poll_connect_() {
  struct sockaddr_storage pa {};
  socklen_t pl = sizeof(pa);
  if (this->sock_ && this->sock_->getpeername((struct sockaddr *) &pa, &pl) == 0) {
    auto s = std::move(this->sock_);
    this->adopt_socket_(std::move(s), this->active_ip_);
    return;
  }
  if (millis() - this->connect_started_ > CONNECT_TIMEOUT_MS) {
    this->connect_fails_++;
    ESP_LOGW(TAG, "Không kết nối được %s:%u", this->active_ip_.c_str(), this->cur_port_);
    // IP cache có thể đã đổi -> cho phép dò lại
    // IP lấy từ flash đã từng chạy được -> kiên nhẫn hơn nhiều trước khi vứt đi
    uint8_t fail_limit = (!this->cache_ip_.empty() && this->active_ip_ == this->cache_ip_) ? 6 : 2;
    if (this->cfg_host_.empty() && this->auto_discover_ && this->connect_fails_ >= fail_limit) {
      this->active_ip_.clear();
      ESP_LOGI(TAG, "Sẽ dò lại dongle trên mạng.");
    }
    this->close_socket_("connect timeout");
  }
}

// -------------------------------------------------------------------- I/O ---

void LuxpowerTcp::pump_rx_() {
  if (!this->sock_)
    return;  // send_next_request_() có thể vừa đóng socket -> tránh deref NULL
  size_t budget = MAX_READ_PER_LOOP;
  while (budget > 0) {
    if (this->rx_len_ >= LUX_RX_BUF) {
      ESP_LOGW(TAG, "RX buffer đầy, bỏ nửa đầu");
      memmove(this->rx_buf_, this->rx_buf_ + LUX_RX_BUF / 2, LUX_RX_BUF / 2);
      this->rx_len_ = LUX_RX_BUF / 2;
    }
    size_t want = LUX_RX_BUF - this->rx_len_;
    if (want > budget)
      want = budget;
    ssize_t got = this->sock_->read(this->rx_buf_ + this->rx_len_, want);
    if (got > 0) {
      this->link_ready_ = true;  // có byte thật -> TCP đã established, hỏi được rồi
      this->rx_len_ += (size_t) got;
      budget -= (size_t) got;
      continue;
    }
    if (got == 0) {
      // Dongle đóng. KHÔNG đóng socket ngay: dữ liệu vừa đọc vẫn nằm trong rx_buf_
      // và close_socket_() sẽ xoá nó. Đánh dấu để loop() giải mã xong rồi mới đóng.
      this->peer_closed_ = true;
      return;
    }
    break;
  }
}

void LuxpowerTcp::learn_serials_(const uint8_t *buf) {
  bool changed = false;
  auto clean = [](const uint8_t *p) {
    std::string s;
    for (int i = 0; i < 10; i++) {
      char c = (char) p[i];
      if (c >= 32 && c < 127)
        s += c;
    }
    return s;
  };
  if (this->dongle_sn_.empty()) {
    std::string s = clean(buf + 8);  // serial dongle nằm ở offset 8..17
    if (s.size() == 10) {
      this->dongle_sn_ = s;
      changed = true;
      ESP_LOGI(TAG, "Đã học dongle SN: %s", s.c_str());
      if (this->ts_dsn_ != nullptr)
        this->ts_dsn_->publish_state(s);
    }
  }
  if (this->inv_sn_.empty()) {
    std::string s = clean(buf + 22);  // serial inverter trong data frame, offset 22..31
    if (s.size() == 10) {
      this->inv_sn_ = s;
      changed = true;
      ESP_LOGI(TAG, "Đã học inverter SN: %s", s.c_str());
      if (this->ts_isn_ != nullptr)
        this->ts_isn_->publish_state(s);
    }
  }
  if (changed && this->serials_known_()) {
    ESP_LOGI(TAG, "Đủ 2 serial — bắt đầu hỏi dữ liệu.");
    this->save_cache_();
  }
}

void LuxpowerTcp::drain_frames_() {
  size_t i = 0;
  while (i + 6 <= this->rx_len_) {
    if (this->rx_buf_[i] != LXP_PREFIX0 || this->rx_buf_[i + 1] != LXP_PREFIX1) {
      i++;
      continue;
    }
    uint16_t flen = this->rx_buf_[i + 4] | (this->rx_buf_[i + 5] << 8);
    size_t total = 6 + (size_t) flen;
    if (total < 20 || total > LUX_RX_BUF) {
      i++;
      continue;
    }
    if (i + total > this->rx_len_)
      break;

    const uint8_t *f = this->rx_buf_ + i;
    this->got_valid_frame_ = true;
    ESP_LOGV(TAG, "RX frame: len=%u tcp_fn=0x%02X dev_fn=0x%02X reg=%u",
             (unsigned) total, f[7], total >= 22 ? f[21] : 0,
             total >= 34 ? (unsigned) (f[32] | (f[33] << 8)) : 0);

    // Học serial từ BẤT KỲ frame nào dongle đẩy ra (kể cả heartbeat)
    if (!this->serials_known_() && total >= 32)
      this->learn_serials_(f);

    if (f[7] == TCP_FUNC_TRANSLATED) {
      if (this->parse_frame_(f, total)) {
        // Frame dữ liệu hợp lệ -> publish ngay (dongle tự đẩy, ta không hỏi)
        this->last_data_ms_ = millis();
        this->ever_got_data_ = true;
        this->silent_sessions_ = 0;
        this->cache_ip_ = this->active_ip_;  // IP này cho dữ liệu thật -> đáng tin
        this->frames_total_count_++;
        if (this->frames_total_ != nullptr)
          this->frames_total_->publish_state(this->frames_total_count_);
        this->connect_fails_ = 0;
        this->set_online_(true);
        // Van tiết lưu: publish tối đa 1 lần/giây. Mỗi lần publish kéo theo 18 sensor
        // + 18 lambda cập nhật LVGL (str_sprintf cấp phát chuỗi) -> publish quá dày
        // làm phân mảnh heap và treo thiết bị sau vài phút.
        if (millis() - this->last_publish_ms_ >= 1000) {
          this->last_publish_ms_ = millis();
          this->publish_all_();
        }
        if (!this->cached_ok_) {
          this->cached_ok_ = true;
          this->save_cache_();
        }
      }
    } else if (f[7] == TCP_FUNC_HEARTBEAT) {
      ESP_LOGV(TAG, "Heartbeat từ dongle");
    }
    i += total;
  }
  if (i > 0) {
    memmove(this->rx_buf_, this->rx_buf_ + i, this->rx_len_ - i);
    this->rx_len_ -= i;
  }
}

bool LuxpowerTcp::parse_frame_(const uint8_t *buf, size_t len) {
  if (len < 36)
    return false;
  uint16_t data_len = buf[18] | (buf[19] << 8);
  size_t dend = 20 + (size_t) data_len;
  if (data_len < 18 || dend > len)
    return false;

  uint8_t dev_func = buf[21];
  if (dev_func == DEV_FUNC_READ_HOLD) {
    // Frame cấu hình (holding register) — KHÔNG ghi đè vùng input register
    ESP_LOGV(TAG, "Bỏ qua frame holding (0x03)");
    return false;
  }
  if (dev_func != DEV_FUNC_READ_INPUT)
    return false;

  uint16_t crc_rx = buf[dend - 2] | (buf[dend - 1] << 8);
  uint16_t crc_calc = crc16_modbus_(buf + 20, (size_t) data_len - 2);
  if (crc_rx != crc_calc) {
    ESP_LOGD(TAG, "CRC lệch (rx=0x%04X calc=0x%04X) — vẫn parse", crc_rx, crc_calc);
  }

  uint16_t reg_start = buf[32] | (buf[33] << 8);

  // Tự dò layout: có/không có byte đếm kiểu Modbus ở offset 34
  size_t vstart, vlen;
  if (data_len >= 19 && buf[34] == (uint8_t) (data_len - 17) && ((data_len - 17) % 2) == 0) {
    vstart = 35;
    vlen = (size_t) data_len - 17;
  } else {
    vstart = 34;
    vlen = (size_t) data_len - 16;
  }
  if (vstart + vlen + 2 > len)
    return false;

  size_t num_regs = vlen / 2;
  // Dongle thỉnh thoảng đẩy frame toàn số 0 -> bỏ, tránh nhảy về 0 trên màn hình
  bool any_nonzero = false;
  for (size_t k = 0; k < num_regs && !any_nonzero; k++)
    any_nonzero = (buf[vstart + k * 2] | buf[vstart + k * 2 + 1]) != 0;
  if (!any_nonzero) {
    ESP_LOGD(TAG, "Bỏ qua frame 0x04 TOÀN SỐ 0: reg_start=%u n=%u dlen=%u b34=%u",
             reg_start, (unsigned) num_regs, (unsigned) data_len, buf[34]);
    return false;
  }
  for (size_t k = 0; k < num_regs; k++) {
    uint16_t r = (uint16_t) (reg_start + k);
    if (r >= LUX_MAX_REG)
      break;
    size_t off = vstart + k * 2;
    this->regs_[r] = buf[off] | (buf[off + 1] << 8);
    this->reg_valid_[r] = true;
  }
  ESP_LOGD(TAG, "Frame func=0x%02X reg=%u..%u (%u reg, values@%u)", dev_func, reg_start,
           (unsigned) (reg_start + num_regs - 1), (unsigned) num_regs, (unsigned) vstart);
  return true;
}

// ------------------------------------------------------------------- loop ---

void LuxpowerTcp::loop() {
  if (!network::is_connected()) {
    if (this->sock_)
      this->close_socket_("mất mạng");
    this->udp_ = nullptr;
    this->scan_abort_();
    return;
  }

  switch (this->state_) {
    case State::IDLE:
      if ((int32_t) (millis() - this->retry_after_) < 0)
        break;
      if (!this->active_ip_.empty()) {
        this->start_connect_(this->active_ip_);  // IP đã biết (YAML hoặc cache)
      } else if (this->auto_discover_) {
        this->udp_discover_begin_();  // bước 1: broadcast; thất bại -> quét dải
      } else {
        ESP_LOGW(TAG, "Không có host và auto_discover tắt.");
        this->retry_after_ = millis() + 30000;
      }
      break;

    case State::DISCOVER_UDP:
      this->udp_discover_step_();
      break;

    case State::SCANNING:
      this->scan_step_();
      break;

    case State::CONNECTING:
      this->poll_connect_();
      break;

    case State::CONNECTED: {
      // ĐỌC TRƯỚC, HỎI SAU: byte đầu tiên chứng minh TCP đã established thật, và
      // frame dongle tự đẩy ra là nguồn để học 2 serial. Dongle KHÔNG tự đẩy dữ
      // liệu 0x04 — phải hỏi mới có, và chỉ có ~1,3 giây để hỏi.
      this->pump_rx_();
      this->drain_frames_();          // giải mã TRƯỚC khi đóng, tránh mất frame
      if (this->peer_closed_) {
        this->peer_closed_ = false;
        this->close_socket_("dongle đóng kết nối");
        break;
      }
      // Dự phòng: dongle không đẩy gì thì sau 200 ms vẫn cứ hỏi.
      if (!this->link_ready_ && millis() - this->connected_at_ >= 200)
        this->link_ready_ = true;
      this->send_next_request_();
      if (this->state_ != State::CONNECTED || !this->sock_)
        break;                        // gửi lỗi -> socket đã đóng, dừng vòng này

      // Nối được nhưng không phải dongle (không frame LXP nào) -> bỏ, dò tiếp
      if (!this->got_valid_frame_ && millis() - this->connected_at_ > NOT_DONGLE_MS) {
        ESP_LOGW(TAG, "%s không gửi frame Luxpower nào.", this->active_ip_.c_str());
        // Giữ lại IP đã từng cho dữ liệu: im lặng thường chỉ là dongle cạn socket,
        // vứt IP đi sẽ kéo theo một vòng quét /24 dài vô ích.
        bool trusted = this->ever_got_data_ || this->active_ip_ == this->cache_ip_;
        if (this->cfg_host_.empty() && !trusted)
          this->active_ip_.clear();
        this->close_socket_("không phải dongle");
      }
      break;
    }
  }
}

// update() = nhịp hỏi dữ liệu (update_interval trong YAML).
void LuxpowerTcp::update() {
  if (this->state_ == State::CONNECTED && this->serials_known_() && this->next_bank_ < 0)
    this->next_bank_ = 0;   // xếp hàng hỏi lại
  if (this->last_data_ms_ == 0)
    return;
  if (millis() - this->last_data_ms_ > STALE_MS) {
    if (this->online_state_) {
      ESP_LOGW(TAG, "Quá %u giây không có dữ liệu từ dongle.", (unsigned) (STALE_MS / 1000));
      this->set_online_(false);
      this->invalidate_regs_();
    }
  }
}

// ---------------------------------------------------------------- publish ---

void LuxpowerTcp::publish_all_() {
  float pv1 = has_reg_(7) ? reg_u16_(7) : 0;
  float pv2 = has_reg_(8) ? reg_u16_(8) : 0;
  float pv3 = has_reg_(9) ? reg_u16_(9) : 0;
  if (pv1_power_ && has_reg_(7))
    pv1_power_->publish_state(pv1);
  if (pv2_power_ && has_reg_(8))
    pv2_power_->publish_state(pv2);
  if (pv3_power_ && has_reg_(9))
    pv3_power_->publish_state(pv3);
  if (pv_power_ && has_reg_(7))
    pv_power_->publish_state(pv1 + pv2 + pv3);

  if (battery_voltage_ && has_reg_(4))
    battery_voltage_->publish_state(reg_u16_(4) * 0.1f);
  if (battery_soc_ && has_reg_(5))
    battery_soc_->publish_state(reg_u16_(5) & 0xFF);

  float charge = has_reg_(10) ? reg_u16_(10) : 0;
  float discharge = has_reg_(11) ? reg_u16_(11) : 0;
  if (battery_power_ && (has_reg_(10) || has_reg_(11)))
    battery_power_->publish_state(charge - discharge);

  float to_grid = has_reg_(26) ? reg_u16_(26) : 0;
  float to_user = has_reg_(27) ? reg_u16_(27) : 0;
  if (grid_power_ && (has_reg_(26) || has_reg_(27)))
    grid_power_->publish_state(to_user - to_grid);

  float p_inv = has_reg_(16) ? reg_u16_(16) : 0;
  if (load_power_ && (has_reg_(16) || has_reg_(27)))
    load_power_->publish_state(p_inv + to_user);

  if (grid_voltage_ && has_reg_(12))
    grid_voltage_->publish_state(reg_u16_(12) * 0.1f);
  if (grid_freq_ && has_reg_(15))
    grid_freq_->publish_state(reg_u16_(15) * 0.01f);

  if (pv_today_ && has_reg_(28)) {
    float e = reg_u16_(28) + (has_reg_(29) ? reg_u16_(29) : 0) + (has_reg_(30) ? reg_u16_(30) : 0);
    pv_today_->publish_state(e * 0.1f);
  }
  if (charge_today_ && has_reg_(33))
    charge_today_->publish_state(reg_u16_(33) * 0.1f);
  if (discharge_today_ && has_reg_(34))
    discharge_today_->publish_state(reg_u16_(34) * 0.1f);
  if (grid_export_today_ && has_reg_(36))
    grid_export_today_->publish_state(reg_u16_(36) * 0.1f);
  if (grid_import_today_ && has_reg_(37))
    grid_import_today_->publish_state(reg_u16_(37) * 0.1f);

  // Nhiệt độ: reg 64..67, giá trị thô theo °C (xác nhận từ dữ liệu thật: 33/32/42/42)
  if (temp_inner_ && has_reg_(64))
    temp_inner_->publish_state(reg_s16_(64));
  if (temp_radiator_ && has_reg_(65))
    temp_radiator_->publish_state(reg_s16_(65));
}

}  // namespace luxpower_tcp
}  // namespace esphome
