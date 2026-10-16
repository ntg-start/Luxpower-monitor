#pragma once
// Đổi dương lịch -> âm lịch Việt Nam (múi giờ +7).
// Thuật toán của Hồ Ngọc Đức, rút gọn cho ESP32.
#include <cmath>
#include <cstdio>
#include <string>

namespace vnlunar {

static const double TZ = 7.0;

inline long jd_from_date(int dd, int mm, int yy) {
  int a = (14 - mm) / 12;
  int y = yy + 4800 - a;
  int m = mm + 12 * a - 3;
  long jd = dd + (153 * m + 2) / 5 + 365L * y + y / 4 - y / 100 + y / 400 - 32045;
  if (jd < 2299161L) {
    jd = dd + (153 * m + 2) / 5 + 365L * y + y / 4 - 32083;
  }
  return jd;
}

inline void jd_to_date(long jd, int &dd, int &mm, int &yy) {
  long a, b, c;
  if (jd > 2299160L) {
    a = jd + 32044;
    b = (4 * a + 3) / 146097;
    c = a - (b * 146097) / 4;
  } else {
    b = 0;
    c = jd + 32082;
  }
  long d = (4 * c + 3) / 1461;
  long e = c - (1461 * d) / 4;
  long m = (5 * e + 2) / 153;
  dd = (int) (e - (153 * m + 2) / 5 + 1);
  mm = (int) (m + 3 - 12 * (m / 10));
  yy = (int) (b * 100 + d - 4800 + m / 10);
}

// Ngày (số nguyên JD) của kỳ sóc thứ k tính từ 1900-01-01
inline long new_moon_day(int k, double tz) {
  double T = k / 1236.85;
  double T2 = T * T, T3 = T2 * T;
  double dr = M_PI / 180.0;
  double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
  Jd1 += 0.00033 * sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
  double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
  double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
  double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
  double C1 = (0.1734 - 0.000393 * T) * sin(M * dr) + 0.0021 * sin(2 * dr * M);
  C1 -= 0.4068 * sin(Mpr * dr) + 0.0161 * sin(2 * dr * Mpr);
  C1 -= 0.0004 * sin(3 * dr * Mpr);
  C1 += 0.0104 * sin(2 * dr * F) - 0.0051 * sin(dr * (M + Mpr));
  C1 -= 0.0074 * sin(dr * (M - Mpr)) + 0.0004 * sin(dr * (2 * F + M));
  C1 -= 0.0004 * sin(dr * (2 * F - M)) - 0.0006 * sin(dr * (2 * F + Mpr));
  C1 += 0.0010 * sin(dr * (2 * F - Mpr)) + 0.0005 * sin(dr * (2 * Mpr + M));
  double deltat;
  if (T < -11) {
    deltat = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3;
  } else {
    deltat = -0.000278 + 0.000265 * T + 0.000262 * T2;
  }
  double JdNew = Jd1 + C1 - deltat;
  return (long) floor(JdNew + 0.5 + tz / 24.0);
}

// Kinh độ mặt trời, trả về cung 30 độ (0..11)
inline int sun_longitude(long jdn, double tz) {
  double T = (jdn - 2451545.5 - tz / 24.0) / 36525.0;
  double T2 = T * T;
  double dr = M_PI / 180.0;
  double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
  double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
  double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * sin(dr * M);
  DL += (0.019993 - 0.000101 * T) * sin(dr * 2 * M) + 0.000290 * sin(dr * 3 * M);
  double L = L0 + DL;
  L = L - 360.0 * floor(L / 360.0);
  return (int) floor(L / 30.0);
}

inline long lunar_month_11(int yy, double tz) {
  long off = jd_from_date(31, 12, yy) - 2415021L;
  int k = (int) floor(off / 29.530588853);
  long nm = new_moon_day(k, tz);
  int sunLong = sun_longitude(nm, tz);
  if (sunLong >= 9) {
    nm = new_moon_day(k - 1, tz);
  }
  return nm;
}

inline int leap_month_offset(long a11, double tz) {
  int k = (int) floor((a11 - 2415021.076998695) / 29.530588853 + 0.5);
  int last;
  int i = 1;
  int arc = sun_longitude(new_moon_day(k + 1, tz), tz);
  do {
    last = arc;
    i++;
    arc = sun_longitude(new_moon_day(k + i, tz), tz);
  } while (arc != last && i < 14);
  return i - 1;
}

// Kết quả: ngày/tháng/năm âm + cờ tháng nhuận
inline void solar_to_lunar(int dd, int mm, int yy, int &ld, int &lm, int &ly, int &leap) {
  long dayNumber = jd_from_date(dd, mm, yy);
  int k = (int) floor((dayNumber - 2415021.076998695) / 29.530588853);
  long monthStart = new_moon_day(k + 1, TZ);
  if (monthStart > dayNumber) {
    monthStart = new_moon_day(k, TZ);
  }
  long a11 = lunar_month_11(yy, TZ);
  long b11 = a11;
  int lunarYear;
  if (a11 >= monthStart) {
    lunarYear = yy;
    a11 = lunar_month_11(yy - 1, TZ);
  } else {
    lunarYear = yy + 1;
    b11 = lunar_month_11(yy + 1, TZ);
  }
  long lunarDay = dayNumber - monthStart + 1;
  int diff = (int) ((monthStart - a11) / 29);
  int lunarLeap = 0;
  int lunarMonth = diff + 11;
  if (b11 - a11 > 365) {
    int leapMonthDiff = leap_month_offset(a11, TZ);
    if (diff >= leapMonthDiff) {
      lunarMonth = diff + 10;
      if (diff == leapMonthDiff) {
        lunarLeap = 1;
      }
    }
  }
  if (lunarMonth > 12) lunarMonth -= 12;
  if (lunarMonth >= 11 && diff < 4) lunarYear -= 1;
  ld = (int) lunarDay;
  lm = lunarMonth;
  ly = lunarYear;
  leap = lunarLeap;
}

// Không dấu: màn hình dùng font montserrat (không có glyph tiếng Việt)
static const char *CAN[] = {"Canh", "Tan", "Nham", "Quy", "Giap",
                            "At",   "Binh", "Dinh", "Mau", "Ky"};
static const char *CHI[] = {"Than", "Dau", "Tuat", "Hoi", "Ty",  "Suu",
                            "Dan",  "Mao", "Thin", "Ti",  "Ngo", "Mui"};

inline std::string can_chi(int ly) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%s %s", CAN[ly % 10], CHI[ly % 12]);
  return std::string(buf);
}

}  // namespace vnlunar

// Bảng 7 thanh cho đồng hồ LED: bit a=0 b=1 c=2 d=3 e=4 f=5 g=6
static const uint8_t SEG_TBL[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
