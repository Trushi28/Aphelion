#include <cosmos/bootcfg.hpp>
#include <cosmos/serial.hpp>

namespace bootcfg {

namespace {

Config g_config;

struct Span {
    const char* p;
    u32 n;
};

bool equals(Span s, const char* lit) {
    u32 i = 0;
    for (; i < s.n; ++i)
        if (lit[i] == 0 || lit[i] != s.p[i]) return false;
    return lit[i] == 0;
}

void reject(Span word, Config& c, const char* why) {
    char text[48];
    u32 n = word.n < sizeof(text) - 1 ? word.n : static_cast<u32>(sizeof(text) - 1);
    for (u32 i = 0; i < n; ++i) text[i] = word.p[i];
    text[n] = 0;
    serial::printf("[bootcfg] ignoring '%s': %s\n", text, why);
    ++c.rejected;
}

void apply(Span word, Config& c, bool& storage_set) {
    u32 eq = 0;
    while (eq < word.n && word.p[eq] != '=') ++eq;
    if (eq == word.n) { reject(word, c, "not a key=value word"); return; }
    Span key{ word.p, eq };
    Span value{ word.p + eq + 1, word.n - eq - 1 };

    if (equals(key, "mode")) {
        if (equals(value, "shell")) c.mode = Mode::Shell;
        else if (equals(value, "test")) c.mode = Mode::Test;
        else reject(word, c, "mode is shell or test");
    } else if (equals(key, "storage")) {
        if (equals(value, "auto")) { c.storage = Storage::Auto; storage_set = true; }
        else if (equals(value, "off")) { c.storage = Storage::Off; storage_set = true; }
        else if (equals(value, "usb")) { c.storage = Storage::Usb; storage_set = true; }
        else reject(word, c, "storage is auto, off or usb");
    } else if (equals(key, "timer")) {
        if (equals(value, "auto")) c.timer = TimerPref::Auto;
        else if (equals(value, "hpet")) c.timer = TimerPref::Hpet;
        else if (equals(value, "pm")) c.timer = TimerPref::Pm;
        else if (equals(value, "pit")) c.timer = TimerPref::Pit;
        else reject(word, c, "timer is auto, hpet, pm or pit");
    } else {
        reject(word, c, "unknown key");
    }
}

constexpr bool blank(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

}

Config parse(const char* cmdline) {
    Config c;
    bool storage_set = false;
    if (cmdline) {
        const char* p = cmdline;
        for (;;) {
            while (blank(*p)) ++p;
            if (!*p) break;
            const char* start = p;
            while (*p && !blank(*p)) ++p;
            apply(Span{ start, static_cast<u32>(p - start) }, c, storage_set);
        }
    }
    if (!storage_set) c.storage = c.mode == Mode::Shell ? Storage::Off : Storage::Auto;
    return c;
}

void set(const Config& c) { g_config = c; }
const Config& get() { return g_config; }

const char* name(Mode m) { return m == Mode::Shell ? "shell" : "test"; }
const char* name(Storage s) {
    switch (s) {
        case Storage::Off: return "off";
        case Storage::Usb: return "usb";
        default: return "auto";
    }
}

const char* name(TimerPref t) {
    switch (t) {
        case TimerPref::Hpet: return "hpet";
        case TimerPref::Pm: return "pm";
        case TimerPref::Pit: return "pit";
        default: return "auto";
    }
}

}
