#pragma once
#include <cosmos/types.hpp>
#include <cosmos/utf8.hpp>

namespace input {

enum Mod : u8 {
    MOD_SHIFT = 1u << 0,
    MOD_CTRL = 1u << 1,
    MOD_ALT = 1u << 2,
    MOD_ALTGR = 1u << 3,
};

enum class Sym : u16 {
    None = 0,
    Enter, Backspace, Tab, Escape,
    Up, Down, Left, Right,
    Home, End, PageUp, PageDown, Insert, Delete,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

struct Key {
    u32 cp = 0;
    Sym sym = Sym::None;
    u8 mods = 0;

    constexpr bool is_text() const { return sym == Sym::None && cp != 0; }
    constexpr bool ctrl() const { return mods & MOD_CTRL; }
    constexpr bool alt() const { return mods & MOD_ALT; }
    constexpr bool shift() const { return mods & MOD_SHIFT; }
    constexpr bool operator==(const Key& o) const { return cp == o.cp && sym == o.sym && mods == o.mods; }
};

constexpr Key text_key(u32 cp, u8 mods = 0) { return Key{cp, Sym::None, mods}; }
constexpr Key sym_key(Sym s, u8 mods = 0) { return Key{0, s, mods}; }

namespace detail {

constexpr char ROW_NUM[] = "1234567890-=";
constexpr char ROW_NUM_S[] = "!@#$%^&*()_+";
constexpr char ROW_Q[] = "qwertyuiop[]";
constexpr char ROW_Q_S[] = "QWERTYUIOP{}";
constexpr char ROW_A[] = "asdfghjkl;'`";
constexpr char ROW_A_S[] = "ASDFGHJKL:\"~";
constexpr char ROW_Z[] = "zxcvbnm,./";
constexpr char ROW_Z_S[] = "ZXCVBNM<>?";

constexpr char us_char(u8 code, bool shifted) {
    if (code >= 0x02 && code <= 0x0D) return (shifted ? ROW_NUM_S : ROW_NUM)[code - 0x02];
    if (code >= 0x10 && code <= 0x1B) return (shifted ? ROW_Q_S : ROW_Q)[code - 0x10];
    if (code >= 0x1E && code <= 0x29) return (shifted ? ROW_A_S : ROW_A)[code - 0x1E];
    if (code == 0x2B) return shifted ? '|' : '\\';
    if (code >= 0x2C && code <= 0x35) return (shifted ? ROW_Z_S : ROW_Z)[code - 0x2C];
    if (code == 0x39) return ' ';
    return 0;
}

constexpr bool is_lower(u32 c) { return c >= 'a' && c <= 'z'; }

}

class Set1Decoder {
public:
    bool feed(u8 byte, Key* out) {
        if (skip_ > 0) { --skip_; return false; }
        if (byte == 0xE0) { extended_ = true; return false; }
        if (byte == 0xE1) { skip_ = 5; extended_ = false; return false; }
        if (byte == 0x00 || byte == 0xFF || byte == 0xFA || byte == 0xFE) { extended_ = false; return false; }

        bool release = byte & 0x80;
        u8 code = byte & 0x7F;
        bool ext = extended_;
        extended_ = false;

        if (ext) return extended_key(code, release, out);

        switch (code) {
            case 0x2A: lshift_ = !release; return false;
            case 0x36: rshift_ = !release; return false;
            case 0x1D: lctrl_ = !release; return false;
            case 0x38: lalt_ = !release; return false;
            default: break;
        }
        if (release) return false;

        if (code == 0x3A) { caps_ = !caps_; return false; }
        if (code == 0x45) { num_ = !num_; return false; }
        if (code == 0x46) return false;

        u8 mods = current_mods();
        switch (code) {
            case 0x01: *out = sym_key(Sym::Escape, mods); return true;
            case 0x0E: *out = sym_key(Sym::Backspace, mods); return true;
            case 0x0F: *out = sym_key(Sym::Tab, mods); return true;
            case 0x1C: *out = sym_key(Sym::Enter, mods); return true;
            case 0x37: *out = text_key('*', mods & ~MOD_SHIFT); return true;
            case 0x4A: *out = text_key('-', mods & ~MOD_SHIFT); return true;
            case 0x4E: *out = text_key('+', mods & ~MOD_SHIFT); return true;
            default: break;
        }
        if (code >= 0x3B && code <= 0x44) { *out = sym_key(static_cast<Sym>(static_cast<u16>(Sym::F1) + (code - 0x3B)), mods); return true; }
        if (code == 0x57) { *out = sym_key(Sym::F11, mods); return true; }
        if (code == 0x58) { *out = sym_key(Sym::F12, mods); return true; }
        if (code >= 0x47 && code <= 0x53) return keypad(code, mods, out);

        bool shifted = lshift_ || rshift_;
        char base = detail::us_char(code, false);
        if (!base) return false;
        bool letter = detail::is_lower(static_cast<u8>(base));
        char c;
        if (letter) c = (shifted != caps_) ? static_cast<char>(base - 32) : base;
        else c = detail::us_char(code, shifted);
        u32 cp = static_cast<u8>(c);
        if ((mods & MOD_CTRL) && letter) cp = static_cast<u8>(base);
        *out = text_key(cp, mods);
        return true;
    }

    bool shift_down() const { return lshift_ || rshift_; }
    bool ctrl_down() const { return lctrl_ || rctrl_; }
    bool alt_down() const { return lalt_ || ralt_; }
    bool caps_on() const { return caps_; }
    bool num_on() const { return num_; }

private:
    u8 current_mods() const {
        u8 m = 0;
        if (lshift_ || rshift_) m |= MOD_SHIFT;
        if (lctrl_ || rctrl_) m |= MOD_CTRL;
        if (lalt_) m |= MOD_ALT;
        if (ralt_) m |= MOD_ALTGR;
        return m;
    }

    bool keypad(u8 code, u8 mods, Key* out) {
        static constexpr char digits[] = {'7', '8', '9', 0, '4', '5', '6', 0, '1', '2', '3', '0', '.'};
        static constexpr Sym navs[] = {Sym::Home, Sym::Up, Sym::PageUp, Sym::None, Sym::Left, Sym::None, Sym::Right, Sym::None,
                                       Sym::End, Sym::Down, Sym::PageDown, Sym::Insert, Sym::Delete};
        u8 i = code - 0x47;
        if (num_ && digits[i]) { *out = text_key(static_cast<u8>(digits[i]), mods & ~MOD_SHIFT); return true; }
        if (!num_ && navs[i] != Sym::None) { *out = sym_key(navs[i], mods); return true; }
        return false;
    }

    bool extended_key(u8 code, bool release, Key* out) {
        switch (code) {
            case 0x1D: rctrl_ = !release; return false;
            case 0x38: ralt_ = !release; return false;
            case 0x2A: case 0x36: return false;
            default: break;
        }
        if (release) return false;
        u8 mods = current_mods();
        Sym s = Sym::None;
        switch (code) {
            case 0x48: s = Sym::Up; break;
            case 0x50: s = Sym::Down; break;
            case 0x4B: s = Sym::Left; break;
            case 0x4D: s = Sym::Right; break;
            case 0x47: s = Sym::Home; break;
            case 0x4F: s = Sym::End; break;
            case 0x49: s = Sym::PageUp; break;
            case 0x51: s = Sym::PageDown; break;
            case 0x52: s = Sym::Insert; break;
            case 0x53: s = Sym::Delete; break;
            case 0x1C: s = Sym::Enter; break;
            case 0x35: *out = text_key('/', mods & ~MOD_SHIFT); return true;
            default: return false;
        }
        *out = sym_key(s, mods);
        return true;
    }

    bool extended_ = false;
    u8 skip_ = 0;
    bool lshift_ = false, rshift_ = false;
    bool lctrl_ = false, rctrl_ = false;
    bool lalt_ = false, ralt_ = false;
    bool caps_ = false;
    bool num_ = true;
};

class AnsiDecoder {
public:
    u32 feed(u8 byte, Key out[2]) {
        switch (state_) {
            case State::Ground: return ground(byte, out);
            case State::Escape: return escape(byte, out);
            case State::Csi: return csi(byte, out);
            case State::Ss3: return ss3(byte, out);
        }
        return 0;
    }

    u32 idle(Key out[2]) {
        u32 n = 0;
        if (state_ == State::Escape) {
            out[n++] = sym_key(Sym::Escape);
        }
        state_ = State::Ground;
        u32 tail[1];
        if (utf8_.finish(tail)) out[n++] = text_key(utf8::REPLACEMENT);
        return n;
    }

    bool pending() const { return state_ != State::Ground || utf8_.pending(); }

private:
    enum class State : u8 { Ground, Escape, Csi, Ss3 };
    static constexpr u32 MAX_PARAMS = 4;

    u32 ground(u8 b, Key out[2]) {
        if (utf8_.pending() && (b & 0xC0) != 0x80) {
            u32 n = 0;
            out[n++] = text_key(utf8::REPLACEMENT);
            utf8_ = utf8::Decoder{};
            u32 more = ground_fresh(b, out + n);
            return n + more;
        }
        return ground_fresh(b, out);
    }

    u32 ground_fresh(u8 b, Key out[2]) {
        bool was_cr = last_cr_;
        last_cr_ = false;
        if (b == 0x1B) { utf8_ = utf8::Decoder{}; state_ = State::Escape; return 0; }
        if (b == '\r') { last_cr_ = true; out[0] = sym_key(Sym::Enter); return 1; }
        if (b == '\n') { if (was_cr) return 0; out[0] = sym_key(Sym::Enter); return 1; }
        if (b == '\t') { out[0] = sym_key(Sym::Tab); return 1; }
        if (b == 0x7F || b == 0x08) { out[0] = sym_key(Sym::Backspace); return 1; }
        if (b == 0) return 0;
        if (b < 0x20) { out[0] = text_key('a' + b - 1, MOD_CTRL); return 1; }
        u32 cps[2];
        u32 k = utf8_.feed(b, cps);
        u32 n = 0;
        for (u32 i = 0; i < k; ++i) out[n++] = text_key(cps[i]);
        return n;
    }

    u32 escape(u8 b, Key out[2]) {
        if (b == '[') { state_ = State::Csi; count_ = 0; seen_ = false; for (auto& p : params_) p = 0; return 0; }
        if (b == 'O') { state_ = State::Ss3; return 0; }
        state_ = State::Ground;
        if (b == 0x1B) { state_ = State::Escape; out[0] = sym_key(Sym::Escape); return 1; }
        if (b == 0x7F || b == 0x08) { out[0] = sym_key(Sym::Backspace, MOD_ALT); return 1; }
        if (b == '\r' || b == '\n') { out[0] = sym_key(Sym::Enter, MOD_ALT); return 1; }
        if (b >= 0x20 && b < 0x7F) { out[0] = text_key(b, MOD_ALT); return 1; }
        return 0;
    }

    static u8 xterm_mods(u32 p) {
        if (p < 2) return 0;
        u32 m = p - 1;
        u8 r = 0;
        if (m & 1) r |= MOD_SHIFT;
        if (m & 2) r |= MOD_ALT;
        if (m & 4) r |= MOD_CTRL;
        return r;
    }

    u32 csi(u8 b, Key out[2]) {
        if (b >= '0' && b <= '9') {
            if (count_ < MAX_PARAMS) params_[count_] = params_[count_] * 10 + (b - '0');
            if (params_[count_ < MAX_PARAMS ? count_ : MAX_PARAMS - 1] > 65535) params_[count_ < MAX_PARAMS ? count_ : MAX_PARAMS - 1] = 65535;
            seen_ = true;
            return 0;
        }
        if (b == ';') {
            if (count_ + 1 < MAX_PARAMS) ++count_;
            seen_ = true;
            return 0;
        }
        if (b >= 0x20 && b <= 0x3F) return 0;
        state_ = State::Ground;
        u32 used = count_ + (seen_ ? 1 : 0);
        u32 first = used > 0 ? params_[0] : 0;
        u8 mods = used > 1 ? xterm_mods(params_[1]) : 0;
        Sym s = Sym::None;
        switch (b) {
            case 'A': s = Sym::Up; break;
            case 'B': s = Sym::Down; break;
            case 'C': s = Sym::Right; break;
            case 'D': s = Sym::Left; break;
            case 'H': s = Sym::Home; break;
            case 'F': s = Sym::End; break;
            case 'Z': out[0] = sym_key(Sym::Tab, MOD_SHIFT); return 1;
            case '~': s = tilde(first); break;
            default: break;
        }
        if (s == Sym::None) return 0;
        out[0] = sym_key(s, mods);
        return 1;
    }

    static Sym tilde(u32 n) {
        switch (n) {
            case 1: case 7: return Sym::Home;
            case 2: return Sym::Insert;
            case 3: return Sym::Delete;
            case 4: case 8: return Sym::End;
            case 5: return Sym::PageUp;
            case 6: return Sym::PageDown;
            case 11: return Sym::F1;
            case 12: return Sym::F2;
            case 13: return Sym::F3;
            case 14: return Sym::F4;
            case 15: return Sym::F5;
            case 17: return Sym::F6;
            case 18: return Sym::F7;
            case 19: return Sym::F8;
            case 20: return Sym::F9;
            case 21: return Sym::F10;
            case 23: return Sym::F11;
            case 24: return Sym::F12;
            default: return Sym::None;
        }
    }

    u32 ss3(u8 b, Key out[2]) {
        state_ = State::Ground;
        Sym s = Sym::None;
        switch (b) {
            case 'A': s = Sym::Up; break;
            case 'B': s = Sym::Down; break;
            case 'C': s = Sym::Right; break;
            case 'D': s = Sym::Left; break;
            case 'H': s = Sym::Home; break;
            case 'F': s = Sym::End; break;
            case 'P': s = Sym::F1; break;
            case 'Q': s = Sym::F2; break;
            case 'R': s = Sym::F3; break;
            case 'S': s = Sym::F4; break;
            default: return 0;
        }
        out[0] = sym_key(s);
        return 1;
    }

    State state_ = State::Ground;
    u32 params_[MAX_PARAMS] = {};
    u32 count_ = 0;
    bool seen_ = false;
    bool last_cr_ = false;
    utf8::Decoder utf8_{};
};

}
