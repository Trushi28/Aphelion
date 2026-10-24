#include <cosmos/helm.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/xhci.hpp>
#include <cosmos/usbstor.hpp>
#include <cosmos/bootcfg.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/diskmap.hpp>
#include <cosmos/corona.hpp>
#include <cosmos/input.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/klog.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/civil.hpp>
#include <cosmos/power.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

int usage(Io& io, const char* text) {
    print_error(io, text);
    return 2;
}

Facts g_facts{};

const char* disk_text() {
    if (nebula::disk_is_mounted()) return "Stellar volume mounted";
    return g_facts.disk_note ? g_facts.disk_note : "";
}

u32 lerp(u32 a, u32 b, u32 t, u32 max) { return (a * (max - t) + b * t) / max; }

u32 mix(u32 c0, u32 c1, u32 t, u32 max) {
    u32 r = lerp((c0 >> 16) & 255, (c1 >> 16) & 255, t, max);
    u32 g = lerp((c0 >> 8) & 255, (c1 >> 8) & 255, t, max);
    u32 b = lerp(c0 & 255, c1 & 255, t, max);
    return (r << 16) | (g << 8) | b;
}

struct Text {
    char s[160];
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

void logo(Writer& w) {
    static const char* top[8] = {"\xE2\x96\x84\xE2\x96\x80\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88", "\xE2\x96\x88 \xE2\x96\x88",
                                 "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80", "\xE2\x96\x88  ", "\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88",
                                 "\xE2\x96\x88\xE2\x96\x84 \xE2\x96\x88"};
    static const char* bottom[8] = {"\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80", "\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88",
                                    "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x84", "\xE2\x96\x88\xE2\x96\x84\xE2\x96\x84", "\xE2\x96\x88", "\xE2\x96\x88\xE2\x96\x84\xE2\x96\x88",
                                    "\xE2\x96\x88 \xE2\x96\x80\xE2\x96\x88"};
    for (int line = 0; line < 2; ++line) {
        w.str("  ");
        for (u32 i = 0; i < 8; ++i) {
            w.rgb(mix(palette::AMBER, palette::EMBER, i, 7));
            w.str(line == 0 ? top[i] : bottom[i]);
            w.put(' ');
        }
        w.reset();
        w.put('\n');
    }
}

void panel_row(Writer& w, const char* key, const Text& value, u32 inner) {
    w.rgb(palette::MIST);
    w.str("  \xE2\x94\x82 ");
    w.rgb(palette::AMBER);
    w.pad_to(key, 9);
    w.rgb(palette::TEXT);
    w.bytes(value.s, value.n);
    u32 used = 1 + 9 + value.cells();
    for (u32 i = used; i < inner; ++i) w.put(' ');
    w.rgb(palette::MIST);
    w.str(" \xE2\x94\x82\n");
    w.reset();
}

constexpr u32 PANEL_ROWS = 7;

}

void set_facts(const Facts& f) { g_facts = f; }
const Facts& facts() { return g_facts; }

void print_about(Writer& w) {
    corona::Metrics m = corona::metrics();
    Text v[PANEL_ROWS];
    const char* keys[PANEL_ROWS] = {"kernel", "console", "memory", "input", "storage", "nebula", "power"};

    v[0].str("Aphelion \xC2\xB7 x86_64 \xC2\xB7 ");
    v[0].num(g_facts.suns);
    v[0].str(g_facts.suns == 1 ? " Sun" : " Suns");

    v[1].str("Corona ");
    v[1].num(m.cols);
    v[1].str("\xC3\x97");
    v[1].num(m.rows);
    v[1].str(" \xC2\xB7 JetBrains Mono ");
    v[1].num(m.cell_w);
    v[1].str("\xC3\x97");
    v[1].num(m.cell_h);

    v[2].num(universe::free_bytes() >> 20);
    v[2].str(" MiB free of ");
    v[2].num(universe::total_bytes() >> 20);
    v[2].str(" MiB");

    v[3].str(g_facts.keyboard_ready ? (g_facts.keyboard_irq ? "PS/2 keyboard (interrupt)" : "PS/2 keyboard (polled)") : "no PS/2 keyboard");
    if (g_facts.serial_present) v[3].str(g_facts.serial_irq ? " \xC2\xB7 serial (interrupt)" : " \xC2\xB7 serial (polled)");

    v[4].str(g_facts.storage ? g_facts.storage : "none");
    if (disk_text()[0]) {
        v[4].str(" \xC2\xB7 ");
        v[4].str(disk_text());
    }

    v[5].str("realms");
    for (u32 i = 0; i < nebula::realm_count(); ++i) {
        v[5].put(' ');
        v[5].str(nebula::realm_at(i)->name());
        if (!nebula::realm_at(i)->available()) v[5].str("(off)");
    }

    power::Capabilities caps = power::capabilities();
    v[6].str("reboot ready \xC2\xB7 poweroff ");
    v[6].str(caps.poweroff_supported ? "ready" : "not yet on real hardware");

    u32 inner = 0;
    for (u32 i = 0; i < PANEL_ROWS; ++i) {
        u32 width = 1 + 9 + v[i].cells();
        if (width > inner) inner = width;
    }
    u32 limit = m.cols > 8 ? m.cols - 8 : 1;
    if (inner > limit) inner = limit;

    w.rgb(palette::MIST);
    w.str("  \xE2\x95\xAD");
    w.repeat("\xE2\x94\x80", inner + 2);
    w.str("\xE2\x95\xAE\n");
    for (u32 i = 0; i < PANEL_ROWS; ++i) panel_row(w, keys[i], v[i], inner);
    w.rgb(palette::MIST);
    w.str("  \xE2\x95\xB0");
    w.repeat("\xE2\x94\x80", inner + 2);
    w.str("\xE2\x95\xAF\n");
    w.reset();
}

void print_splash(Writer& w) {
    w.str("\x1b[2J\x1b[H\n");
    logo(w);
    w.rgb(palette::MIST);
    w.str("  a non-POSIX kernel \xC2\xB7 Helm shell \xC2\xB7 type ");
    w.rgb(palette::ICE);
    w.str("help");
    w.rgb(palette::MIST);
    w.str("\n\n");
    w.reset();
    print_about(w);
    w.put('\n');
}

namespace {

const char* state_name(orbital::State s) {
    switch (s) {
        case orbital::State::Ready: return "ready";
        case orbital::State::Running: return "running";
        case orbital::State::Sleeping: return "sleeping";
        case orbital::State::Blocked: return "blocked";
    }
    return "?";
}

int cmd_about(Shell&, Io& io, u32, char**) { print_about(io.out); return 0; }

int cmd_clear(Shell&, Io& io, u32, char**) { io.out.str("\x1b[2J\x1b[H"); return 0; }

int cmd_unicode(Shell&, Io& io, u32, char**) {
    Writer& w = io.out;
    w.str("  latin   Aphelion \xC3\x87" "a va? Gr\xC3\xB6\xC3\x9F" "e \xC5\xBC\xC3\xB3\xC5\x82\xC4\x87\n");
    w.str("  greek   \xCE\x91\xCE\xB2\xCE\xB3 \xCE\xB4\xCE\xB5\xCE\xB6 \xCE\xA9\n");
    w.str("  cyrill  \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80\n");
    w.str("  money   \xE2\x82\xB9 \xE2\x82\xAC $ \xC2\xA3 \xC2\xA5\n");
    w.str("  math    \xE2\x89\xA4 \xE2\x89\xA5 \xE2\x89\xA0 \xE2\x89\x88 \xC2\xB1 \xC3\x97 \xC3\xB7\n");
    w.str("  arrows  \xE2\x86\x90 \xE2\x86\x91 \xE2\x86\x92 \xE2\x86\x93 \xE2\x86\x94\n");
    w.str("  boxes   \xE2\x95\x94\xE2\x95\x90\xE2\x95\x97 \xE2\x95\xAD\xE2\x94\x80\xE2\x95\xAE \xE2\x94\x8C\xE2\x94\xAC\xE2\x94\x90 \xE2\x96\x91\xE2\x96\x92\xE2\x96\x93\xE2\x96\x88\n");
    w.str("  braille \xE2\xA3\xBF\xE2\xA3\xB6\xE2\xA3\xA4\xE2\xA0\x9B\xE2\xA0\x89\n");
    w.str("  shapes  \xE2\x97\x8F \xE2\x97\x8B \xE2\x96\xA0 \xE2\x96\xB2 \xE2\x97\x86 \xE2\x9C\x93 \xE2\x98\x85\n");
    w.rgb(palette::MIST);
    w.str("  wide characters (CJK, emoji) draw the replacement glyph; Indic and Arabic shaping is unsupported\n");
    w.reset();
    return 0;
}

int cmd_colors(Shell&, Io& io, u32, char**) {
    Writer& w = io.out;
    const term::Palette& p = corona::theme();
    w.str("  ");
    for (u32 i = 0; i < 16; ++i) {
        w.str("\x1b[48;2;");
        w.num((p.ansi[i] >> 16) & 255); w.put(';'); w.num((p.ansi[i] >> 8) & 255); w.put(';'); w.num(p.ansi[i] & 255);
        w.str("m  ");
        if (i == 7) { w.reset(); w.str("\n  "); }
    }
    w.reset();
    w.put('\n');
    w.str("  ");
    for (u32 i = 0; i < 32; ++i) {
        w.rgb(mix(palette::AMBER, palette::ICE, i, 31));
        w.str("\xE2\x96\x88");
    }
    w.reset();
    w.put('\n');
    return 0;
}

int emit_probe(Shell& sh, Io& io, const char* path) {
    nebula::Blob b{};
    nebula::Status st = lens::text::read_file(sh.cursor, path, &b);
    if (st != nebula::Status::Ok) {
        print_status(io, "cannot read", path, st);
        return code_of(st);
    }
    io.out.bytes(reinterpret_cast<const char*>(b.data), b.size);
    nebula::release(b);
    return 0;
}

int cmd_ps(Shell& sh, Io& io, u32, char**) {
    if (!io.out.terminal()) return emit_probe(sh, io, "sys:/tasks");
    Writer& w = io.out;
    orbital::SatelliteInfo info[64];
    u32 n = orbital::snapshot(info, 64);
    w.rgb(palette::MIST);
    w.str("   id  state     ring core  switches  name\n");
    w.reset();
    for (u32 i = 0; i < n; ++i) {
        w.str("  ");
        w.pad_num(info[i].id, 3);
        w.str("  ");
        w.pad_to(state_name(info[i].state), 8);
        w.str("  ");
        w.pad_num(static_cast<u64>(info[i].ring), 3);
        w.str("  ");
        w.pad_num(info[i].core, 4);
        w.str("  ");
        w.pad_num(info[i].switches, 8);
        w.str("  ");
        w.str(info[i].name);
        w.put('\n');
    }
    w.rgb(palette::MIST);
    w.num(n);
    w.str(" Satellites\n");
    w.reset();
    return 0;
}

int cmd_cores(Shell& sh, Io& io, u32, char**) {
    if (!io.out.terminal()) return emit_probe(sh, io, "sys:/cpus");
    Writer& w = io.out;
    orbital::CoreInfo info[64];
    u32 n = orbital::cores(info, 64);
    orbital::Stats st = orbital::stats();
    for (u32 i = 0; i < n; ++i) {
        w.str("  Sun ");
        w.num(i);
        w.str("  apic ");
        w.pad_num(info[i].apic_id, 3);
        w.str("  ");
        w.str(info[i].idle ? "idle   " : "running");
        w.str("  queued ");
        w.num(info[i].queued);
        w.str("  ");
        w.str(info[i].running);
        w.put('\n');
    }
    w.rgb(palette::MIST);
    w.str("  preemptions ");
    w.num(st.preemptions);
    w.str("  steals ");
    w.num(st.steals);
    w.str("  ipis ");
    w.num(st.ipis_sent);
    w.str("  boosts ");
    w.num(st.boosts);
    w.put('\n');
    w.reset();
    return 0;
}

int cmd_mem(Shell&, Io& io, u32, char**) {
    Writer& w = io.out;
    kheap::Stats h = kheap::stats();
    w.str("  universe  ");
    w.num(universe::free_bytes() >> 20);
    w.str(" MiB free of ");
    w.num(universe::total_bytes() >> 20);
    w.str(" MiB in ");
    w.num(universe::galaxy_count());
    w.str(" galaxies\n  heap      ");
    w.num(h.live_objects);
    w.str(" objects, ");
    w.num(h.live_bytes);
    w.str(" bytes live, ");
    w.num(h.failed_allocs);
    w.str(" failed allocations\n");
    nebula::Realm* ram = nebula::ram_realm();
    w.str("  ram realm ");
    print_size(w, ram->space_total() - ram->space_free());
    w.str(" used of ");
    print_size(w, ram->space_total());
    w.put('\n');
    return 0;
}

int cmd_uptime(Shell&, Io& io, u32, char**) {
    u64 s = clock::uptime_ms() / 1000;
    Writer& w = io.out;
    w.str("  up ");
    w.num(s / 3600);
    w.str("h ");
    w.num((s / 60) % 60);
    w.str("m ");
    w.num(s % 60);
    w.str("s\n");
    return 0;
}

int cmd_date(Shell&, Io& io, u32, char**) {
    if (!clock::wall_valid()) {
        print_error(io, "the wall clock was not set by the firmware; only uptime is known");
        return 1;
    }
    char iso[24];
    civil::format_iso(civil::from_unix(clock::now_ms() / 1000), iso);
    io.out.str(iso);
    io.out.put('\n');
    return 0;
}

int cmd_tty(Shell& sh, Io& io, u32, char**) {
    lens::Domain& d = sh.cursor.domain;
    lens::Lens con = lens::text::console(sh.cursor);
    if (con.is_null()) {
        print_error(io, "this shell has no console lens");
        return 1;
    }
    lens::Result<lens::Rights> rights = lens::rights_of(d, con);
    char letters[16];
    lens::rights_text(rights.value(), letters, sizeof(letters));
    lens::Wait w{con, lens::Ready::Readable | lens::Ready::Writable, {}};
    (void)lens::await(d, &w, 1, 0);
    Writer& o = io.out;
    o.str("console: kind=");
    o.str(lens::kind_name(lens::kind_of(d, con).value()));
    o.str(" rights=");
    o.str(letters);
    o.str(" readable=");
    o.str(w.got.has(lens::Ready::Readable) ? "yes" : "no");
    o.str(" writable=");
    o.str(w.got.has(lens::Ready::Writable) ? "yes" : "no");
    o.put('\n');
    return 0;
}

int cmd_klog(Shell& sh, Io& io, u32 argc, char** argv) {
    u64 want = 2048;
    if (argc > 2 || (argc == 2 && !parse_u64(argv[1], &want))) {
        print_error(io, "usage: klog [bytes]");
        return 2;
    }
    if (want > 4096) want = 4096;
    nebula::Blob log{};
    nebula::Status st = lens::text::read_file(sh.cursor, "sys:/log", &log);
    if (st != nebula::Status::Ok) {
        print_status(io, "cannot read", "sys:/log", st);
        return code_of(st);
    }
    const char* text = static_cast<const char*>(static_cast<const void*>(log.data));
    u64 total = 0;
    for (u64 i = 0; i < log.size;) {
        u64 j = i;
        while (j < log.size && text[j] != ':') ++j;
        i = j + 2 < log.size ? j + 2 : log.size;
        while (i < log.size && text[i] != '\n') { ++total; ++i; }
        ++total;
        if (i < log.size) ++i;
    }
    u64 skip = total > want ? total - want : 0;
    u64 seen = 0;
    io.out.rgb(palette::MIST);
    for (u64 i = 0; i < log.size && !sh.poll();) {
        u64 j = i;
        while (j < log.size && text[j] != ':') ++j;
        i = j + 2 < log.size ? j + 2 : log.size;
        while (i < log.size && text[i] != '\n') {
            if (seen++ >= skip) io.out.put(text[i]);
            ++i;
        }
        if (seen++ >= skip) io.out.put('\n');
        if (i < log.size) ++i;
    }
    io.out.reset();
    io.out.put('\n');
    nebula::release(log);
    return sh.aborted ? 130 : 0;
}

const char* sym_name(input::Sym s) {
    static const char* names[] = {"", "Enter", "Backspace", "Tab", "Escape", "Up", "Down", "Left", "Right", "Home", "End", "PageUp", "PageDown",
                                  "Insert", "Delete", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    u32 i = static_cast<u32>(s);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

int cmd_keys(Shell&, Io& io, u32, char**) {
    Writer& w = io.out;
    w.rgb(palette::MIST);
    w.str("  press keys; Escape leaves\n");
    w.reset();
    w.flush();
    for (;;) {
        input::Key k;
        input::read(k);
        w.str("  ");
        if (k.ctrl()) w.str("Ctrl+");
        if (k.alt()) w.str("Alt+");
        if (k.shift() && k.sym != input::Sym::None) w.str("Shift+");
        if (k.sym != input::Sym::None) {
            w.rgb(palette::ICE);
            w.str(sym_name(k.sym));
        } else {
            w.rgb(palette::MINT);
            w.cp(k.cp);
            w.reset();
            w.str("  U+");
            const char* hex = "0123456789ABCDEF";
            bool started = false;
            for (int sh = 20; sh >= 0; sh -= 4) {
                u32 d = (k.cp >> sh) & 15;
                if (d || started || sh <= 12) { w.put(hex[d]); started = true; }
            }
        }
        w.reset();
        w.put('\n');
        w.flush();
        if (k.sym == input::Sym::Escape) return 0;
    }
}

int cmd_sleep(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc != 2) { print_error(io, "usage: sleep milliseconds"); return 2; }
    u64 ms;
    if (!parse_u64(argv[1], &ms) || ms > 3600000) { print_error(io, "usage: sleep milliseconds (at most 3600000)"); return 2; }
    while (ms > 0) {
        u64 slice = ms > 50 ? 50 : ms;
        orbital::sleep_ms(static_cast<u32>(slice));
        ms -= slice;
        sh.poll_count |= 63;
        if (sh.poll()) return 130;
    }
    return 0;
}

const char* verdict_text(diskmap::Verdict v) {
    switch (v) {
        case diskmap::Verdict::Match: return "boot stick";
        case diskmap::Verdict::NotOurs: return "other disk (untouched)";
        default: return "not identified";
    }
}

int cmd_usb(Shell&, Io& io, u32, char**) {
    Writer& w = io.out;
    u32 controllers = xhci::controller_count();
    if (controllers == 0) {
        w.str("  no xHCI controller was started");
        w.str(g_facts.usb_mode ? "\n" : " (boot the usb entry, or add storage=usb, to probe USB)\n");
        return 0;
    }
    for (u32 i = 0; i < controllers; ++i) {
        const xhci::ControllerInfo& c = xhci::controller(i);
        w.str("  xhci ");
        w.num(i);
        w.str("  ");
        w.hex(c.vendor, 4);
        w.put(':');
        w.hex(c.device, 4);
        w.str("  ");
        w.str(c.status);
        w.str(", ");
        w.num(c.ports);
        w.str(" ports\n");
    }
    for (u32 i = 0; i < xhci::listed_count(); ++i) {
        const xhci::Listed& d = xhci::listed(i);
        w.str("  port ");
        w.num(d.port);
        w.str("  ");
        w.str(xhci::speed_name(d.speed));
        w.str("  ");
        w.hex(d.vendor, 4);
        w.put(':');
        w.hex(d.product, 4);
        w.str(d.mass_storage ? "  mass storage" : "  not storage");
        if (d.note) { w.str("  "); w.str(d.note); }
        w.put('\n');
    }
    for (u32 i = 0; i < usbstor::lun_count(); ++i) {
        const usbstor::LunInfo& l = usbstor::lun(i);
        w.str("  drive ");
        w.num(i);
        w.str("  ");
        w.str(l.vendor);
        w.put(' ');
        w.str(l.product);
        w.str("  ");
        w.num(l.blocks / 2048);
        w.str(" MiB  ");
        w.str(l.usable ? verdict_text(l.verdict) : (l.note ? l.note : "unusable"));
        w.put('\n');
    }
    return 0;
}

int storage_init_erase(Io& io, const char* typed) {
    Writer& w = io.out;
    if (!g_facts.usb_mode || !usbstor::window_present() || !blockdev::present()) {
        print_error(io, "there is no USB storage window to initialise");
        return 1;
    }
    if (nebula::disk_is_mounted()) {
        print_error(io, "the window already holds a Stellar volume; nothing to do");
        return 1;
    }
    u64 mib = blockdev::capacity_sectors() / 2048;
    u64 given = 0;
    if (!typed || !parse_u64(typed, &given) || given != mib) {
        w.rgb(palette::SAND);
        w.str("  this erases the ");
        w.num(mib);
        w.str(" MiB window after the boot image, never the image itself.\n  to confirm type: ");
        w.reset();
        w.rgb(palette::ICE);
        w.str("storage init erase ");
        w.num(mib);
        w.reset();
        w.put('\n');
        return 1;
    }
    stellar::Status why = stellar::Status::Ok;
    if (!stellar::format(blockdev::capacity_sectors(), &why)) {
        w.rgb(palette::SAND);
        w.str("  format failed: ");
        w.str(stellar::status_name(why));
        w.reset();
        w.put('\n');
        return 1;
    }
    nebula::disk_set_mounted(true);
    w.rgb(palette::MINT);
    w.str("  window formatted and mounted\n");
    w.reset();
    return 0;
}

int cmd_storage(Shell&, Io& io, u32 argc, char** argv) {
    Writer& w = io.out;
    if (argc >= 2) {
        if (argc >= 3 && same(argv[1], "init") && same(argv[2], "erase") && argc <= 4)
            return storage_init_erase(io, argc == 4 ? argv[3] : nullptr);
        return usage(io, "usage: storage [init erase <MiB>]");
    }
    w.str("  mode      ");
    w.str(bootcfg::name(bootcfg::get().storage));
    w.put('\n');
    if (g_facts.usb_mode) {
        if (!usbstor::window_present()) {
            w.str("  window    none: ");
            w.str(usbstor::window_status());
            w.str("\n  tip       write the image to the stick in DD mode (Rufus), on a stick larger than the image\n");
        } else {
            const usbstor::LunInfo* l = usbstor::window_lun();
            w.str("  stick     ");
            w.str(l->vendor);
            w.put(' ');
            w.str(l->product);
            w.str(", ");
            w.num(l->blocks / 2048);
            w.str(" MiB, boot image ");
            w.num((l->image_bytes + 1048575) >> 20);
            w.str(" MiB\n  window    ");
            w.num(l->tail_sectors / 2048);
            w.str(" MiB from sector ");
            w.num(l->tail_start);
            w.put('\n');
        }
    }
    w.str("  device    ");
    w.str(g_facts.storage ? g_facts.storage : "none");
    w.put('\n');
    w.str("  volume    ");
    if (nebula::disk_is_mounted()) {
        w.str("Stellar, mounted, ");
        print_size(w, nebula::disk_realm()->space_free());
        w.str(" free of ");
        print_size(w, nebula::disk_realm()->space_total());
    } else {
        w.str("not mounted");
    }
    w.put('\n');
    if (disk_text()[0]) {
        w.rgb(palette::MIST);
        w.str("  ");
        w.str(disk_text());
        w.reset();
        w.put('\n');
    }
    return 0;
}

void settle_disk() {
    if (nebula::disk_is_mounted() && blockdev::present()) blockdev::flush();
}

int cmd_reboot(Shell&, Io& io, u32, char**) {
    io.out.str("  restarting...\n");
    io.out.flush();
    settle_disk();
    power::reboot();
}

int cmd_poweroff(Shell&, Io& io, u32, char**) {
    io.out.str("  powering off...\n");
    io.out.flush();
    settle_disk();
    if (!power::poweroff()) {
        io.out.rgb(palette::SAND);
        io.out.str("  poweroff is not available on this platform yet; it is safe to hold the power button\n");
        io.out.reset();
        return 1;
    }
    return 0;
}

constexpr Command COMMANDS[] = {
    {"about", "about", "the info panel again", &cmd_about},
    {"clear", "clear", "clear the screen (also Ctrl+L)", &cmd_clear},
    {"unicode", "unicode", "a glyph coverage sample", &cmd_unicode},
    {"colors", "colors", "the Corona palette", &cmd_colors},
    {"ps", "ps", "Satellites known to Orbital; piped, one parseable row each", &cmd_ps},
    {"cores", "cores", "per-core scheduler state; piped, one parseable row each", &cmd_cores},
    {"mem", "mem", "Universe, kernel heap and ram realm counters", &cmd_mem},
    {"uptime", "uptime", "time since boot", &cmd_uptime},
    {"date", "date", "wall clock time in UTC when the firmware set one", &cmd_date},
    {"klog", "klog [bytes]", "tail of the kernel log", &cmd_klog},
    {"tty", "tty", "the console lens this shell holds: rights and readiness", &cmd_tty},
    {"keys", "keys", "show decoded key events, Esc leaves", &cmd_keys},
    {"sleep", "sleep milliseconds", "wait", &cmd_sleep},
    {"usb", "usb", "xHCI controllers, devices and drives", &cmd_usb},
    {"storage", "storage [init erase <MiB>]", "the storage device and volume; in usb mode, format the window", &cmd_storage},
    {"reboot", "reboot", "flush the disk and restart", &cmd_reboot},
    {"poweroff", "poweroff", "flush the disk and power off where the platform allows it", &cmd_poweroff},
};

constexpr Table TABLE{"system", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_sys_commands() { register_table(&TABLE); }

}
