#pragma once
#include <cosmos/types.hpp>

namespace civil {

struct Date {
    u32 year, month, day, hour, minute, second;
};

struct RtcRaw {
    u8 second, minute, hour, day, month, year;
};

constexpr bool is_leap(u32 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

constexpr u32 days_in_month(u32 y, u32 m) {
    constexpr u8 table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && is_leap(y)) ? 29 : table[m - 1];
}

constexpr i64 days_from_civil(i64 y, u32 m, u32 d) {
    y -= m <= 2 ? 1 : 0;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = static_cast<u32>(y - era * 400);
    u32 mp = m > 2 ? m - 3 : m + 9;
    u32 doy = (153 * mp + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<i64>(doe) - 719468;
}

static_assert(days_from_civil(1970, 1, 1) == 0, "epoch day must be zero");
static_assert(days_from_civil(2000, 1, 1) == 10957, "2000-01-01 is day 10957");

constexpr bool valid(const Date& t) {
    if (t.month < 1 || t.month > 12) return false;
    if (t.day < 1 || t.day > days_in_month(t.year, t.month)) return false;
    return t.hour <= 23 && t.minute <= 59 && t.second <= 59;
}

constexpr u64 to_unix(const Date& t) {
    i64 days = days_from_civil(t.year, t.month, t.day);
    return static_cast<u64>(days) * 86400ull + t.hour * 3600ull + t.minute * 60ull + t.second;
}

constexpr Date from_unix(u64 secs) {
    i64 z = static_cast<i64>(secs / 86400) + 719468;
    u32 rem = static_cast<u32>(secs % 86400);
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    u32 doe = static_cast<u32>(z - era * 146097);
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = static_cast<i64>(yoe) + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u32 mp = (5 * doy + 2) / 153;
    u32 d = doy - (153 * mp + 2) / 5 + 1;
    u32 m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    return { static_cast<u32>(y), m, d, rem / 3600, (rem / 60) % 60, rem % 60 };
}

constexpr bool bcd_ok(u8 v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }
constexpr u32 from_bcd(u8 v) { return (v >> 4) * 10u + (v & 0x0F); }

inline bool decode_rtc(const RtcRaw& r, bool binary, bool hour24, Date* out) {
    u8 hour_raw = hour24 ? r.hour : static_cast<u8>(r.hour & 0x7F);
    bool pm = !hour24 && (r.hour & 0x80);
    if (!binary && !(bcd_ok(r.second) && bcd_ok(r.minute) && bcd_ok(hour_raw) &&
                     bcd_ok(r.day) && bcd_ok(r.month) && bcd_ok(r.year)))
        return false;
    auto dec = [&](u8 v) -> u32 { return binary ? v : from_bcd(v); };
    u32 hour = dec(hour_raw);
    if (!hour24) {
        if (hour < 1 || hour > 12) return false;
        hour = hour % 12 + (pm ? 12 : 0);
    }
    Date t{ 2000 + dec(r.year), dec(r.month), dec(r.day), hour, dec(r.minute), dec(r.second) };
    if (!valid(t)) return false;
    *out = t;
    return true;
}

inline void format_iso(const Date& t, char* out) {
    auto put = [&](u32 pos, u32 v, u32 digits) {
        for (u32 i = digits; i-- > 0;) { out[pos + i] = static_cast<char>('0' + v % 10); v /= 10; }
    };
    put(0, t.year % 10000, 4); out[4] = '-';
    put(5, t.month, 2);        out[7] = '-';
    put(8, t.day, 2);          out[10] = 'T';
    put(11, t.hour, 2);        out[13] = ':';
    put(14, t.minute, 2);      out[16] = ':';
    put(17, t.second, 2);      out[19] = 'Z';
    out[20] = 0;
}

}
