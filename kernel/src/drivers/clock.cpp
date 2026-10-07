#include <cosmos/clock.hpp>
#include <cosmos/civil.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace clock {

constexpr u16 PORT_INDEX = 0x70;
constexpr u16 PORT_DATA = 0x71;
constexpr u8 REG_SECOND = 0x00;
constexpr u8 REG_MINUTE = 0x02;
constexpr u8 REG_HOUR = 0x04;
constexpr u8 REG_DAY = 0x07;
constexpr u8 REG_MONTH = 0x08;
constexpr u8 REG_YEAR = 0x09;
constexpr u8 REG_STATUS_A = 0x0A;
constexpr u8 REG_STATUS_B = 0x0B;
constexpr u8 REG_STATUS_D = 0x0D;

static u32 g_bsp_id = 0;
static bool g_wall_valid = false;
static u64 g_base_ms = 0;
static u64 g_boot_wall_ms = 0;
static u64 g_ticks = 0;

static u8 cmos(u8 reg) {
    cpu::out8(PORT_INDEX, reg);
    return cpu::in8(PORT_DATA);
}

static bool wait_not_updating() {
    for (u32 i = 0; i < 2000000; ++i) {
        if (!(cmos(REG_STATUS_A) & 0x80)) return true;
        cpu::io_wait();
    }
    return false;
}

static civil::RtcRaw snapshot() {
    civil::RtcRaw r;
    r.second = cmos(REG_SECOND);
    r.minute = cmos(REG_MINUTE);
    r.hour = cmos(REG_HOUR);
    r.day = cmos(REG_DAY);
    r.month = cmos(REG_MONTH);
    r.year = cmos(REG_YEAR);
    return r;
}

static bool same(const civil::RtcRaw& a, const civil::RtcRaw& b) {
    return a.second == b.second && a.minute == b.minute && a.hour == b.hour &&
           a.day == b.day && a.month == b.month && a.year == b.year;
}

static bool read_rtc(civil::Date* out) {
    if (!(cmos(REG_STATUS_D) & 0x80)) return false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!wait_not_updating()) return false;
        civil::RtcRaw a = snapshot();
        if (!wait_not_updating()) return false;
        civil::RtcRaw b = snapshot();
        if (!same(a, b)) continue;
        u8 sb = cmos(REG_STATUS_B);
        return civil::decode_rtc(b, (sb & 0x04) != 0, (sb & 0x02) != 0, out);
    }
    return false;
}

void init(u32 bsp_apic_id) {
    g_bsp_id = bsp_apic_id;
    g_ticks = 0;
    civil::Date d;
    if (!read_rtc(&d)) {
        g_wall_valid = false;
        serial::writeln("[clock] RTC unreadable or reports an invalid time; file mtimes stay 0");
        return;
    }
    u64 ms = civil::to_unix(d) * 1000ull;
    g_base_ms = ms;
    g_boot_wall_ms = ms;
    g_wall_valid = true;
    char iso[21];
    civil::format_iso(d, iso);
    serial::printf("[clock] RTC reads %s (assumed UTC, years 2000-2099)\n", iso);
}

void sync() {
    civil::Date d;
    if (!read_rtc(&d)) return;
    u64 ticks = __atomic_load_n(&g_ticks, __ATOMIC_RELAXED);
    __atomic_store_n(&g_base_ms, civil::to_unix(d) * 1000ull - ticks * TICK_MS, __ATOMIC_RELAXED);
    if (!g_wall_valid) g_boot_wall_ms = civil::to_unix(d) * 1000ull;
    __atomic_store_n(&g_wall_valid, true, __ATOMIC_RELAXED);
}

void tick(u32 apic_id) {
    if (apic_id == g_bsp_id) __atomic_fetch_add(&g_ticks, 1, __ATOMIC_RELAXED);
}

bool wall_valid() { return __atomic_load_n(&g_wall_valid, __ATOMIC_RELAXED); }

u64 uptime_ms() { return __atomic_load_n(&g_ticks, __ATOMIC_RELAXED) * TICK_MS; }

u64 now_ms() {
    if (!wall_valid()) return 0;
    return __atomic_load_n(&g_base_ms, __ATOMIC_RELAXED) + uptime_ms();
}

u64 boot_wall_ms() { return g_boot_wall_ms; }

}
