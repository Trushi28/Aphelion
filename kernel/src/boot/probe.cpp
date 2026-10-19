#include <cosmos/probe.hpp>
#include <cosmos/corona.hpp>
#include <cosmos/input.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/klog.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/power.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/utf8.hpp>

namespace probe {

namespace {

constexpr u32 MAX_LINE = 160;
constexpr u32 HISTORY = 16;
constexpr u32 PROMPT_CELLS = 11;
constexpr u32 OUT_BYTES = 1024;

Facts g_facts{};

struct Out {
    char buf[OUT_BYTES];
    u32 n = 0;

    void flush() {
        if (!n) return;
        corona::write(buf, n);
        serial::console_write(buf, n);
        n = 0;
    }
    void put(char c) {
        if (n == OUT_BYTES) flush();
        buf[n++] = c;
    }
    void str(const char* s) { while (*s) put(*s++); }
    void bytes(const char* s, u32 len) { for (u32 i = 0; i < len; ++i) put(s[i]); }
    void cp(u32 c) {
        char e[4];
        u32 k = utf8::encode(c, e);
        bytes(e, k);
    }
    void num(u64 v) {
        char t[24];
        u32 i = 0;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (i) put(t[--i]);
    }
    void pad_num(u64 v, u32 width) {
        u32 digits = 1;
        for (u64 x = v; x >= 10; x /= 10) ++digits;
        for (u32 i = digits; i < width; ++i) put(' ');
        num(v);
    }
    void repeat(const char* s, u32 count) { for (u32 i = 0; i < count; ++i) str(s); }
    void rgb(u32 c) {
        str("\x1b[38;2;"); num((c >> 16) & 255); put(';'); num((c >> 8) & 255); put(';'); num(c & 255); put('m');
    }
    void reset() { str("\x1b[0m"); }
};

Out g_out;

u32 lerp(u32 a, u32 b, u32 t, u32 max) {
    return (a * (max - t) + b * t) / max;
}

u32 mix(u32 c0, u32 c1, u32 t, u32 max) {
    u32 r = lerp((c0 >> 16) & 255, (c1 >> 16) & 255, t, max);
    u32 g = lerp((c0 >> 8) & 255, (c1 >> 8) & 255, t, max);
    u32 b = lerp(c0 & 255, c1 & 255, t, max);
    return (r << 16) | (g << 8) | b;
}

struct Text {
    char s[128];
    u32 n = 0;
    void put(char c) { if (n + 1 < sizeof(s)) { s[n++] = c; s[n] = 0; } }
    void str(const char* p) { while (*p) put(*p++); }
    void num(u64 v) {
        char t[24];
        u32 i = 0;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (i) put(t[--i]);
    }
    u32 cells() const { return utf8::cells(s, n); }
};

void logo() {
    static const char* top[8] = {"\xE2\x96\x84\xE2\x96\x80\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88", "\xE2\x96\x88 \xE2\x96\x88",
                                 "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80", "\xE2\x96\x88  ", "\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88",
                                 "\xE2\x96\x88\xE2\x96\x84 \xE2\x96\x88"};
    static const char* bottom[8] = {"\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88",
                                    "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x84", "\xE2\x96\x88\xE2\x96\x84\xE2\x96\x84", "\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x84\xE2\x96\x88",
                                    "\xE2\x96\x88 \xE2\x96\x80\xE2\x96\x88"};
    for (int line = 0; line < 2; ++line) {
        g_out.str("  ");
        for (u32 i = 0; i < 8; ++i) {
            g_out.rgb(mix(corona::AMBER, corona::EMBER, i, 7));
            g_out.str(line == 0 ? top[i] : bottom[i]);
            g_out.put(' ');
        }
        g_out.reset();
        g_out.put('\n');
    }
}

void row(const char* key, const Text& value, u32 inner) {
    g_out.rgb(corona::MIST);
    g_out.str("  \xE2\x94\x82 ");
    g_out.rgb(corona::AMBER);
    g_out.str(key);
    u32 kc = utf8::cells(key, static_cast<u32>(__builtin_strlen(key)));
    for (u32 i = kc; i < 9; ++i) g_out.put(' ');
    g_out.rgb(corona::TEXT);
    g_out.bytes(value.s, value.n);
    u32 used = 1 + 9 + value.cells();
    for (u32 i = used; i < inner; ++i) g_out.put(' ');
    g_out.rgb(corona::MIST);
    g_out.str(" \xE2\x94\x82\n");
    g_out.reset();
}

void info_panel() {
    corona::Metrics m = corona::metrics();
    Text kernel_line, console_line, memory_line, input_line, storage_line, power_line;

    kernel_line.str("Aphelion \xC2\xB7 x86_64 \xC2\xB7 ");
    kernel_line.num(g_facts.suns);
    kernel_line.str(g_facts.suns == 1 ? " Sun" : " Suns");

    console_line.str("Corona ");
    console_line.num(m.cols);
    console_line.str("\xC3\x97");
    console_line.num(m.rows);
    console_line.str(" \xC2\xB7 JetBrains Mono ");
    console_line.num(m.cell_w);
    console_line.str("\xC3\x97");
    console_line.num(m.cell_h);

    memory_line.num(universe::free_bytes() >> 20);
    memory_line.str(" MiB free of ");
    memory_line.num(universe::total_bytes() >> 20);
    memory_line.str(" MiB");

    input_line.str(g_facts.keyboard_ready ? (g_facts.keyboard_irq ? "PS/2 keyboard (interrupt)" : "PS/2 keyboard (polled)") : "no PS/2 keyboard");
    if (g_facts.serial_present) input_line.str(g_facts.serial_irq ? " \xC2\xB7 serial (interrupt)" : " \xC2\xB7 serial (polled)");

    storage_line.str(g_facts.storage);

    power::Capabilities caps = power::capabilities();
    power_line.str("reboot ready \xC2\xB7 poweroff ");
    power_line.str(caps.poweroff_supported ? "ready" : "not yet on real hardware");

    const Text* values[6] = {&kernel_line, &console_line, &memory_line, &input_line, &storage_line, &power_line};
    const char* keys[6] = {"kernel", "console", "memory", "input", "storage", "power"};
    u32 inner = 0;
    for (u32 i = 0; i < 6; ++i) {
        u32 w = 1 + 9 + values[i]->cells();
        if (w > inner) inner = w;
    }
    u32 limit = m.cols > 8 ? m.cols - 8 : 1;
    if (inner > limit) inner = limit;

    g_out.rgb(corona::MIST);
    g_out.str("  \xE2\x95\xAD");
    g_out.repeat("\xE2\x94\x80", inner + 2);
    g_out.str("\xE2\x95\xAE\n");
    for (u32 i = 0; i < 6; ++i) row(keys[i], *values[i], inner);
    g_out.rgb(corona::MIST);
    g_out.str("  \xE2\x95\xB0");
    g_out.repeat("\xE2\x94\x80", inner + 2);
    g_out.str("\xE2\x95\xAF\n");
    g_out.reset();
}

void splash() {
    g_out.str("\x1b[2J\x1b[H\n");
    logo();
    g_out.rgb(corona::MIST);
    g_out.str("  a non-POSIX kernel \xC2\xB7 console probe \xC2\xB7 type ");
    g_out.rgb(corona::ICE);
    g_out.str("help");
    g_out.rgb(corona::MIST);
    g_out.str("\n\n");
    g_out.reset();
    info_panel();
    g_out.put('\n');
    g_out.flush();
}

void prompt() {
    g_out.str("\r");
    g_out.rgb(corona::AMBER);
    g_out.str("aphelion");
    g_out.rgb(corona::EMBER);
    g_out.str(" \xE2\x96\xB8 ");
    g_out.reset();
}

struct Line {
    u32 cp[MAX_LINE];
    u32 len = 0;
    u32 pos = 0;
    u32 width_limit = 60;

    u32 width_of(u32 from, u32 to) const {
        u32 w = 0;
        for (u32 i = from; i < to; ++i) w += utf8::width(cp[i]);
        return w;
    }
    bool insert(u32 c) {
        u32 w = utf8::width(c);
        if (len >= MAX_LINE || width_of(0, len) + w > width_limit) return false;
        for (u32 i = len; i > pos; --i) cp[i] = cp[i - 1];
        cp[pos++] = c;
        ++len;
        return true;
    }
    void erase_before() {
        if (!pos) return;
        for (u32 i = pos; i < len; ++i) cp[i - 1] = cp[i];
        --pos;
        --len;
    }
    void erase_at() {
        if (pos >= len) return;
        for (u32 i = pos + 1; i < len; ++i) cp[i - 1] = cp[i];
        --len;
    }
    u32 encode(char* out, u32 max) const {
        u32 n = 0;
        for (u32 i = 0; i < len; ++i) {
            char e[4];
            u32 k = utf8::encode(cp[i], e);
            if (n + k >= max) break;
            for (u32 j = 0; j < k; ++j) out[n++] = e[j];
        }
        out[n] = 0;
        return n;
    }
    void set(const char* s, u32 n) {
        len = pos = 0;
        utf8::Decoder d;
        u32 tmp[2];
        for (u32 i = 0; i < n; ++i) {
            u32 k = d.feed(static_cast<u8>(s[i]), tmp);
            for (u32 j = 0; j < k; ++j) insert(tmp[j]);
        }
    }
    void clear() { len = pos = 0; }
};

void redraw(const Line& l) {
    prompt();
    for (u32 i = 0; i < l.len; ++i) g_out.cp(l.cp[i]);
    g_out.str("\x1b[K");
    u32 back = l.width_of(l.pos, l.len);
    if (back) { g_out.str("\x1b["); g_out.num(back); g_out.put('D'); }
    g_out.flush();
}

char g_history[HISTORY][MAX_LINE * 4 + 1];
u32 g_history_count = 0;
u32 g_history_next = 0;

bool same_text(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void remember(const char* s, u32 n) {
    if (!n) return;
    u32 last = (g_history_next + HISTORY - 1) % HISTORY;
    if (g_history_count && same_text(g_history[last], s)) return;
    __builtin_memcpy(g_history[g_history_next], s, n + 1);
    g_history_next = (g_history_next + 1) % HISTORY;
    if (g_history_count < HISTORY) ++g_history_count;
}

const char* recall(u32 back) {
    if (back == 0 || back > g_history_count) return nullptr;
    return g_history[(g_history_next + HISTORY - back) % HISTORY];
}

bool word_is(const char* line, const char* word, const char** rest) {
    u32 i = 0;
    while (word[i]) {
        if (line[i] != word[i]) return false;
        ++i;
    }
    if (line[i] != 0 && line[i] != ' ') return false;
    while (line[i] == ' ') ++i;
    *rest = line + i;
    return true;
}

const char* state_name(orbital::State s) {
    switch (s) {
        case orbital::State::Ready: return "ready";
        case orbital::State::Running: return "running";
        case orbital::State::Sleeping: return "sleeping";
        case orbital::State::Blocked: return "blocked";
    }
    return "?";
}

void cmd_help() {
    static const char* lines[][2] = {
        {"help", "this list"},
        {"about", "the info panel again"},
        {"clear", "clear the screen (also Ctrl+L)"},
        {"echo <text>", "print text, Unicode included"},
        {"unicode", "a glyph coverage sample"},
        {"colors", "the Corona palette"},
        {"ps", "Satellites known to Orbital"},
        {"cores", "per-core scheduler state"},
        {"mem", "Universe and kernel heap counters"},
        {"uptime", "time since boot"},
        {"klog [bytes]", "tail of the kernel log"},
        {"keys", "show decoded key events, Esc leaves"},
        {"reboot", "restart the machine"},
        {"poweroff", "power off where the platform allows it"},
    };
    for (auto& l : lines) {
        g_out.str("  ");
        g_out.rgb(corona::ICE);
        g_out.str(l[0]);
        u32 c = utf8::cells(l[0], static_cast<u32>(__builtin_strlen(l[0])));
        for (u32 i = c; i < 16; ++i) g_out.put(' ');
        g_out.rgb(corona::MIST);
        g_out.str(l[1]);
        g_out.reset();
        g_out.put('\n');
    }
}

void cmd_unicode() {
    g_out.str("  latin   Aphelion Ça va? Größe żółć\n");
    g_out.str("  greek   \xCE\x91\xCE\xB2\xCE\xB3 \xCE\xB4\xCE\xB5\xCE\xB6 \xCE\xA9\n");
    g_out.str("  cyrill  \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80\n");
    g_out.str("  money   \xE2\x82\xB9 \xE2\x82\xAC $ \xC2\xA3 \xC2\xA5\n");
    g_out.str("  math    \xE2\x89\xA4 \xE2\x89\xA5 \xE2\x89\xA0 \xE2\x89\x88 \xC2\xB1 \xC3\x97 \xC3\xB7\n");
    g_out.str("  arrows  \xE2\x86\x90 \xE2\x86\x91 \xE2\x86\x92 \xE2\x86\x93 \xE2\x86\x94\n");
    g_out.str("  boxes   \xE2\x95\x94\xE2\x95\x90\xE2\x95\x97 \xE2\x95\xAD\xE2\x94\x80\xE2\x95\xAE \xE2\x94\x8C\xE2\x94\xAC\xE2\x94\x90 \xE2\x96\x91\xE2\x96\x92\xE2\x96\x93\xE2\x96\x88\n");
    g_out.str("  braille \xE2\xA3\xBF\xE2\xA3\xB6\xE2\xA3\xA4\xE2\xA0\x9B\xE2\xA0\x89\n");
    g_out.str("  shapes  \xE2\x97\x8F \xE2\x97\x8B \xE2\x96\xA0 \xE2\x96\xB2 \xE2\x97\x86 \xE2\x9C\x93 \xE2\x98\x85\n");
    g_out.rgb(corona::MIST);
    g_out.str("  wide characters (CJK, emoji) draw the replacement glyph; Indic and Arabic shaping is unsupported\n");
    g_out.reset();
}

void cmd_colors() {
    const term::Palette& p = corona::theme();
    g_out.str("  ");
    for (u32 i = 0; i < 16; ++i) {
        g_out.str("\x1b[48;2;");
        g_out.num((p.ansi[i] >> 16) & 255); g_out.put(';'); g_out.num((p.ansi[i] >> 8) & 255); g_out.put(';'); g_out.num(p.ansi[i] & 255);
        g_out.str("m  ");
        if (i == 7) { g_out.reset(); g_out.str("\n  "); }
    }
    g_out.reset();
    g_out.put('\n');
    g_out.str("  ");
    for (u32 i = 0; i < 32; ++i) {
        g_out.rgb(mix(corona::AMBER, corona::ICE, i, 31));
        g_out.str("\xE2\x96\x88");
    }
    g_out.reset();
    g_out.put('\n');
}

void cmd_ps() {
    static orbital::SatelliteInfo info[64];
    u32 n = orbital::snapshot(info, 64);
    g_out.rgb(corona::MIST);
    g_out.str("   id  state     ring core  switches  name\n");
    g_out.reset();
    for (u32 i = 0; i < n; ++i) {
        g_out.str("  ");
        g_out.pad_num(info[i].id, 3);
        g_out.str("  ");
        const char* st = state_name(info[i].state);
        g_out.str(st);
        for (u32 k = static_cast<u32>(__builtin_strlen(st)); k < 8; ++k) g_out.put(' ');
        g_out.str("  ");
        g_out.pad_num(static_cast<u64>(info[i].ring), 3);
        g_out.str("  ");
        g_out.pad_num(info[i].core, 4);
        g_out.str("  ");
        g_out.pad_num(info[i].switches, 8);
        g_out.str("  ");
        g_out.str(info[i].name);
        g_out.put('\n');
    }
    g_out.rgb(corona::MIST);
    g_out.num(n);
    g_out.str(" Satellites\n");
    g_out.reset();
}

void cmd_cores() {
    static orbital::CoreInfo info[64];
    u32 n = orbital::cores(info, 64);
    orbital::Stats st = orbital::stats();
    for (u32 i = 0; i < n; ++i) {
        g_out.str("  Sun ");
        g_out.num(i);
        g_out.str("  apic ");
        g_out.pad_num(info[i].apic_id, 3);
        g_out.str("  ");
        g_out.str(info[i].idle ? "idle   " : "running");
        g_out.str("  queued ");
        g_out.num(info[i].queued);
        g_out.str("  ");
        g_out.str(info[i].running);
        g_out.put('\n');
    }
    g_out.rgb(corona::MIST);
    g_out.str("  preemptions ");
    g_out.num(st.preemptions);
    g_out.str("  steals ");
    g_out.num(st.steals);
    g_out.str("  ipis ");
    g_out.num(st.ipis_sent);
    g_out.str("  boosts ");
    g_out.num(st.boosts);
    g_out.put('\n');
    g_out.reset();
}

void cmd_mem() {
    kheap::Stats h = kheap::stats();
    g_out.str("  universe  ");
    g_out.num(universe::free_bytes() >> 20);
    g_out.str(" MiB free of ");
    g_out.num(universe::total_bytes() >> 20);
    g_out.str(" MiB in ");
    g_out.num(universe::galaxy_count());
    g_out.str(" galaxies\n  heap      ");
    g_out.num(h.live_objects);
    g_out.str(" objects, ");
    g_out.num(h.live_bytes);
    g_out.str(" bytes live, ");
    g_out.num(h.failed_allocs);
    g_out.str(" failed allocations\n");
}

void cmd_uptime() {
    u64 ms = clock::uptime_ms();
    u64 s = ms / 1000;
    g_out.str("  up ");
    g_out.num(s / 3600);
    g_out.str("h ");
    g_out.num((s / 60) % 60);
    g_out.str("m ");
    g_out.num(s % 60);
    g_out.str("s\n");
}

u64 parse_number(const char* s, u64 fallback) {
    if (*s < '0' || *s > '9') return fallback;
    u64 v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + static_cast<u64>(*s - '0'); if (v > 65536) return 65536; ++s; }
    return v;
}

void cmd_klog(const char* arg) {
    u64 want = parse_number(arg, 2048);
    if (want > 8192) want = 8192;
    u64 tail = klog::tail();
    u64 seq = tail > want ? tail - want : 0;
    char chunk[256];
    g_out.rgb(corona::MIST);
    for (;;) {
        u32 n = klog::read(seq, chunk, sizeof(chunk));
        if (!n) break;
        for (u32 i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\n' || (c >= 0x20 && c < 0x7F) || (static_cast<u8>(c) >= 0x80)) g_out.put(c);
        }
    }
    g_out.reset();
    g_out.put('\n');
}

const char* sym_name(input::Sym s) {
    static const char* names[] = {"", "Enter", "Backspace", "Tab", "Escape", "Up", "Down", "Left", "Right", "Home", "End", "PageUp", "PageDown",
                                  "Insert", "Delete", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    u32 i = static_cast<u32>(s);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

void cmd_keys() {
    g_out.rgb(corona::MIST);
    g_out.str("  press keys; Escape leaves\n");
    g_out.reset();
    g_out.flush();
    for (;;) {
        input::Key k;
        input::read(k);
        g_out.str("  ");
        if (k.ctrl()) g_out.str("Ctrl+");
        if (k.alt()) g_out.str("Alt+");
        if (k.shift() && k.sym != input::Sym::None) g_out.str("Shift+");
        if (k.sym != input::Sym::None) {
            g_out.rgb(corona::ICE);
            g_out.str(sym_name(k.sym));
        } else {
            g_out.rgb(corona::MINT);
            g_out.cp(k.cp);
            g_out.reset();
            g_out.str("  U+");
            const char* hex = "0123456789ABCDEF";
            bool started = false;
            for (int sh = 20; sh >= 0; sh -= 4) {
                u32 d = (k.cp >> sh) & 15;
                if (d || started || sh <= 12) { g_out.put(hex[d]); started = true; }
            }
        }
        g_out.reset();
        g_out.put('\n');
        g_out.flush();
        if (k.sym == input::Sym::Escape) return;
    }
}

bool run(const char* line) {
    const char* rest = nullptr;
    if (!*line) return true;
    if (word_is(line, "help", &rest)) cmd_help();
    else if (word_is(line, "about", &rest)) info_panel();
    else if (word_is(line, "clear", &rest)) g_out.str("\x1b[2J\x1b[H");
    else if (word_is(line, "echo", &rest)) { g_out.str(rest); g_out.put('\n'); }
    else if (word_is(line, "unicode", &rest)) cmd_unicode();
    else if (word_is(line, "colors", &rest)) cmd_colors();
    else if (word_is(line, "ps", &rest)) cmd_ps();
    else if (word_is(line, "cores", &rest)) cmd_cores();
    else if (word_is(line, "mem", &rest)) cmd_mem();
    else if (word_is(line, "uptime", &rest)) cmd_uptime();
    else if (word_is(line, "klog", &rest)) cmd_klog(rest);
    else if (word_is(line, "keys", &rest)) cmd_keys();
    else if (word_is(line, "reboot", &rest)) {
        g_out.str("  restarting...\n");
        g_out.flush();
        power::reboot();
    } else if (word_is(line, "poweroff", &rest)) {
        g_out.str("  powering off...\n");
        g_out.flush();
        if (!power::poweroff()) {
            g_out.rgb(corona::SAND);
            g_out.str("  poweroff is not available on this platform yet; it is safe to hold the power button\n");
            g_out.reset();
        }
    } else {
        g_out.rgb(corona::SAND);
        g_out.str("  unknown command: ");
        g_out.str(line);
        g_out.reset();
        g_out.str("\n  try ");
        g_out.rgb(corona::ICE);
        g_out.str("help");
        g_out.reset();
        g_out.put('\n');
    }
    return true;
}

void loop(void*) {
    splash();
    Line l;
    corona::Metrics m = corona::metrics();
    l.width_limit = m.cols > PROMPT_CELLS + 4 ? m.cols - PROMPT_CELLS - 3 : 8;
    u32 hist_back = 0;
    char saved[MAX_LINE * 4 + 1] = {0};
    redraw(l);

    for (;;) {
        input::Key k;
        input::read(k);
        bool edited = true;

        if (k.sym == input::Sym::Enter) {
            redraw(l);
            g_out.put('\n');
            char text[MAX_LINE * 4 + 1];
            u32 n = l.encode(text, sizeof(text));
            remember(text, n);
            l.clear();
            hist_back = 0;
            run(text);
            g_out.flush();
            redraw(l);
            continue;
        }
        if (k.sym == input::Sym::Backspace) l.erase_before();
        else if (k.sym == input::Sym::Delete) l.erase_at();
        else if (k.sym == input::Sym::Left) { if (l.pos) --l.pos; }
        else if (k.sym == input::Sym::Right) { if (l.pos < l.len) ++l.pos; }
        else if (k.sym == input::Sym::Home) l.pos = 0;
        else if (k.sym == input::Sym::End) l.pos = l.len;
        else if (k.sym == input::Sym::Up) {
            const char* h = recall(hist_back + 1);
            if (h) {
                if (hist_back == 0) l.encode(saved, sizeof(saved));
                ++hist_back;
                l.set(h, static_cast<u32>(__builtin_strlen(h)));
            }
        } else if (k.sym == input::Sym::Down) {
            if (hist_back > 0) {
                --hist_back;
                const char* h = hist_back ? recall(hist_back) : saved;
                l.set(h, static_cast<u32>(__builtin_strlen(h)));
            }
        } else if (k.sym == input::Sym::None && k.ctrl()) {
            if (k.cp == 'c') {
                redraw(l);
                g_out.str("^C\n");
                l.clear();
                hist_back = 0;
            } else if (k.cp == 'l') {
                g_out.str("\x1b[2J\x1b[H");
            } else if (k.cp == 'a') l.pos = 0;
            else if (k.cp == 'e') l.pos = l.len;
            else if (k.cp == 'u') { l.clear(); }
            else if (k.cp == 'k') l.len = l.pos;
            else if (k.cp == 'd' && l.len == 0) {
                g_out.str("\n  use ");
                g_out.rgb(corona::ICE);
                g_out.str("poweroff");
                g_out.reset();
                g_out.str(" or ");
                g_out.rgb(corona::ICE);
                g_out.str("reboot");
                g_out.reset();
                g_out.put('\n');
            } else edited = false;
        } else if (k.sym == input::Sym::Tab) {
            l.insert(' ');
        } else if (k.is_text() && !k.alt() && k.cp >= 0x20) {
            l.insert(k.cp);
        } else {
            edited = false;
        }
        if (edited) redraw(l);
    }
}

}

void set_facts(const Facts& f) { g_facts = f; }

bool spawn() {
    return orbital::spawn("helm-probe", &loop, nullptr) != nullptr;
}

}
