#include <cosmos/keymap.hpp>
#include <cosmos/klog.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>

using namespace input;

static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-72s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

struct Keys {
    Key k[64];
    u32 n = 0;
    void add(const Key& key) { if (n < 64) k[n++] = key; }
};

static Keys scan(Set1Decoder& d, std::initializer_list<u8> bytes) {
    Keys out;
    for (u8 b : bytes) {
        Key key;
        if (d.feed(b, &key)) out.add(key);
    }
    return out;
}

static Keys ansi(AnsiDecoder& d, const char* s, u32 n, bool idle_at_end = false) {
    Keys out;
    for (u32 i = 0; i < n; ++i) {
        Key tmp[2];
        u32 c = d.feed(static_cast<u8>(s[i]), tmp);
        for (u32 j = 0; j < c; ++j) out.add(tmp[j]);
    }
    if (idle_at_end) {
        Key tmp[2];
        u32 c = d.idle(tmp);
        for (u32 j = 0; j < c; ++j) out.add(tmp[j]);
    }
    return out;
}

static Keys ansi(AnsiDecoder& d, const char* s, bool idle_at_end = false) {
    u32 n = 0;
    while (s[n]) ++n;
    return ansi(d, s, n, idle_at_end);
}

static bool only(const Keys& k, const Key& want) { return k.n == 1 && k.k[0] == want; }

static void set1_tests() {
    Set1Decoder d;
    expect(only(scan(d, {0x1E, 0x9E}), text_key('a')), "set1: a press yields one key and the release none");
    expect(scan(d, {0x1E}).n == 1 && scan(d, {0x9E}).n == 0, "set1: only presses produce events");
    expect(only(scan(d, {0x2A, 0x1E, 0xAA}), text_key('A', MOD_SHIFT)), "set1: shift makes capitals");
    expect(only(scan(d, {0x1E}), text_key('a')), "set1: shift release returns to lower case");
    expect(only(scan(d, {0x36, 0x02, 0xB6}), text_key('!', MOD_SHIFT)), "set1: right shift and the number row");
    expect(only(scan(d, {0x1D, 0x2E, 0x9D}), text_key('c', MOD_CTRL)), "set1: control letters report the letter with the control modifier");
    expect(only(scan(d, {0x38, 0x21, 0xB8}), text_key('f', MOD_ALT)), "set1: alt letters carry the alt modifier");
    expect(only(scan(d, {0xE0, 0x38, 0x21, 0xE0, 0xB8}), text_key('f', MOD_ALTGR)), "set1: the extended alt is AltGr");
    expect(scan(d, {0x3A}).n == 0 && only(scan(d, {0x1E}), text_key('A')), "set1: caps lock capitalises letters");
    expect(only(scan(d, {0x2A, 0x1E, 0xAA}), text_key('a', MOD_SHIFT)), "set1: shift cancels caps lock for letters");
    expect(only(scan(d, {0x02}), text_key('1')), "set1: caps lock leaves digits alone");
    scan(d, {0x3A});
    expect(only(scan(d, {0x1E}), text_key('a')), "set1: caps lock toggles off again");
    expect(only(scan(d, {0x1C}), sym_key(Sym::Enter)), "set1: enter");
    expect(only(scan(d, {0x0E}), sym_key(Sym::Backspace)), "set1: backspace");
    expect(only(scan(d, {0x0F}), sym_key(Sym::Tab)), "set1: tab");
    expect(only(scan(d, {0x01}), sym_key(Sym::Escape)), "set1: escape");
    expect(only(scan(d, {0x39}), text_key(' ')), "set1: space");
    expect(only(scan(d, {0xE0, 0x48}), sym_key(Sym::Up)), "set1: extended up arrow");
    expect(only(scan(d, {0xE0, 0x53}), sym_key(Sym::Delete)), "set1: extended delete");
    expect(only(scan(d, {0xE0, 0x1C}), sym_key(Sym::Enter)), "set1: keypad enter");
    expect(only(scan(d, {0xE0, 0x35}), text_key('/')), "set1: keypad divide");
    expect(only(scan(d, {0x3B}), sym_key(Sym::F1)) && only(scan(d, {0x44}), sym_key(Sym::F10)), "set1: F1 and F10");
    expect(only(scan(d, {0x57}), sym_key(Sym::F11)) && only(scan(d, {0x58}), sym_key(Sym::F12)), "set1: F11 and F12");
    expect(scan(d, {0xE0, 0x2A, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0xAA}).n == 1, "set1: fake shifts around extended keys are ignored");
    expect(scan(d, {0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5}).n == 0 && only(scan(d, {0x1E}), text_key('a')), "set1: the pause sequence is swallowed whole");
    expect(scan(d, {0xFA, 0xFE, 0x00, 0xFF}).n == 0, "set1: acknowledgements and overrun bytes are ignored");
    expect(only(scan(d, {0x4B}), text_key('4')), "set1: keypad digits with num lock");
    scan(d, {0x45});
    expect(only(scan(d, {0x4B}), sym_key(Sym::Left)), "set1: keypad becomes navigation with num lock off");
    expect(only(scan(d, {0x37}), text_key('*')), "set1: keypad multiply");
    expect(only(scan(d, {0xE0, 0x1D, 0x1E, 0xE0, 0x9D}), text_key('a', MOD_CTRL)), "set1: the right control key counts as control");
    expect(only(scan(d, {0x29}), text_key('`')) && only(scan(d, {0x2A, 0x29, 0xAA}), text_key('~', MOD_SHIFT)), "set1: backtick and tilde");
    expect(only(scan(d, {0x2B}), text_key('\\')) && only(scan(d, {0x2A, 0x2B, 0xAA}), text_key('|', MOD_SHIFT)), "set1: backslash and pipe");

    bool all_known = true;
    Set1Decoder e;
    const char* expected_plain = "1234567890-=qwertyuiop[]asdfghjkl;'`";
    u8 codes[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
                  0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};
    for (u32 i = 0; i < sizeof(codes); ++i) {
        Key k;
        if (!e.feed(codes[i], &k) || k.cp != static_cast<u8>(expected_plain[i])) all_known = false;
        e.feed(codes[i] | 0x80, &k);
    }
    expect(all_known, "set1: every letter, digit and punctuation key on the main rows");
}

static void ansi_tests() {
    AnsiDecoder d;
    expect(only(ansi(d, "a"), text_key('a')), "ansi: plain text");
    expect(only(ansi(d, "\r"), sym_key(Sym::Enter)), "ansi: carriage return is enter");
    expect(ansi(d, "\r\n").n == 1, "ansi: a CR LF pair is a single enter");
    expect(only(ansi(d, "\n"), sym_key(Sym::Enter)), "ansi: a bare line feed is enter");
    expect(only(ansi(d, "\x7f"), sym_key(Sym::Backspace)) && only(ansi(d, "\x08"), sym_key(Sym::Backspace)), "ansi: delete and backspace bytes are backspace");
    expect(only(ansi(d, "\t"), sym_key(Sym::Tab)), "ansi: tab");
    expect(only(ansi(d, "\x03"), text_key('c', MOD_CTRL)) && only(ansi(d, "\x04"), text_key('d', MOD_CTRL)), "ansi: control bytes become control letters");
    expect(only(ansi(d, "\x1b[A"), sym_key(Sym::Up)) && only(ansi(d, "\x1b[D"), sym_key(Sym::Left)), "ansi: CSI arrows");
    expect(only(ansi(d, "\x1bOA"), sym_key(Sym::Up)) && only(ansi(d, "\x1bOP"), sym_key(Sym::F1)), "ansi: SS3 arrows and F1");
    expect(only(ansi(d, "\x1b[3~"), sym_key(Sym::Delete)) && only(ansi(d, "\x1b[5~"), sym_key(Sym::PageUp)), "ansi: tilde sequences");
    expect(only(ansi(d, "\x1b[15~"), sym_key(Sym::F5)) && only(ansi(d, "\x1b[24~"), sym_key(Sym::F12)), "ansi: function keys");
    expect(only(ansi(d, "\x1b[1;5C"), sym_key(Sym::Right, MOD_CTRL)), "ansi: modified arrows carry the modifier");
    expect(only(ansi(d, "\x1b[1;3D"), sym_key(Sym::Left, MOD_ALT)) && only(ansi(d, "\x1b[1;2A"), sym_key(Sym::Up, MOD_SHIFT)), "ansi: alt and shift modifiers");
    expect(only(ansi(d, "\x1b[Z"), sym_key(Sym::Tab, MOD_SHIFT)), "ansi: back tab");
    expect(only(ansi(d, "\x1bx"), text_key('x', MOD_ALT)), "ansi: escape then a letter is alt plus the letter");
    expect(only(ansi(d, "\x1b", true), sym_key(Sym::Escape)), "ansi: a lone escape is released by the idle timeout");
    expect(ansi(d, "\x1b[A\x1b[B", false).n == 2, "ansi: back to back sequences");
    expect(ansi(d, "\x1b[999999999999999999A").n == 1, "ansi: absurdly long parameters do not break the parser");
    expect(ansi(d, "\x1b[?25h").n == 0 && only(ansi(d, "z"), text_key('z')), "ansi: unknown sequences are dropped and the parser recovers");
    expect(only(ansi(d, "\xE2\x82\xB9"), text_key(0x20B9)), "ansi: UTF-8 input decodes");
    Keys broken = ansi(d, "\xE2\x82" "a");
    expect(broken.n == 2 && broken.k[0].cp == 0xFFFD && broken.k[1].cp == 'a', "ansi: a truncated UTF-8 sequence becomes a replacement then the next key");
    expect(ansi(d, "\x1b[1;5").n == 0 && d.pending(), "ansi: an unfinished sequence stays pending");
    Key tmp[2];
    d.idle(tmp);
    expect(!d.pending(), "ansi: idle clears an unfinished sequence");
    Keys fuzz;
    unsigned seed = 7;
    for (int i = 0; i < 200000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        Key t[2];
        d.feed(static_cast<u8>(seed >> 16), t);
    }
    d.idle(tmp);
    expect(only(ansi(d, "q"), text_key('q')), "ansi: 200000 random bytes leave the decoder usable");
}

static void klog_tests() {
    klog::Ring<64> r;
    expect(r.size() == 0 && r.tail_seq() == 0, "klog: starts empty");
    for (char c : {'h', 'i'}) r.put(c);
    char buf[16];
    u64 seq = 0;
    u32 n = r.read(seq, buf, sizeof(buf));
    expect(n == 2 && buf[0] == 'h' && buf[1] == 'i' && seq == 2, "klog: reads what was written and advances the cursor");
    expect(r.read(seq, buf, sizeof(buf)) == 0, "klog: a caught-up reader gets nothing");
    for (int i = 0; i < 200; ++i) r.put(static_cast<char>('a' + i % 26));
    seq = 0;
    n = r.read(seq, buf, 16);
    expect(n == 16 && seq == r.tail_seq() - 64 + 16, "klog: a reader that fell behind skips to the oldest surviving byte");
    expect(r.dropped(0) == r.tail_seq() - 64, "klog: the number of overwritten bytes is reported");
    u64 s2 = r.tail_seq();
    r.put('!');
    expect(r.read(s2, buf, 16) == 1 && buf[0] == '!', "klog: wrap-around keeps byte order");
    expect(r.size() == 64, "klog: the ring never holds more than its capacity");
}

int main() {
    set1_tests();
    ansi_tests();
    klog_tests();
    return g_failures ? 1 : 0;
}
