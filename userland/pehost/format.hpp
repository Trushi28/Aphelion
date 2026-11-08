#pragma once
#include <cosmos/types.hpp>

namespace pehost::fmt {

using VaList = __builtin_ms_va_list;

namespace detail {

constexpr u32 BUFFER = 128;

template <class S>
struct Out {
    S& sink;
    char buf[BUFFER];
    u32 length = 0;
    u64 total = 0;

    explicit Out(S& s) : sink(s) {}

    void flush() {
        if (length) sink.put(buf, length);
        length = 0;
    }

    void put(char c) {
        if (length == BUFFER) flush();
        buf[length++] = c;
        ++total;
    }

    void pad(char c, u64 n) {
        while (n--) put(c);
    }

    void text(const char* s, u64 n) {
        for (u64 i = 0; i < n; ++i) put(s[i]);
    }
};

struct Spec {
    bool left = false, plus = false, space = false, alt = false, zero = false;
    i64 width = 0;
    i64 precision = -1;
    u32 bits = 32;
    char conv = 0;
};

inline u64 string_length(const char* s, i64 limit) {
    u64 n = 0;
    while (s[n] && (limit < 0 || static_cast<i64>(n) < limit)) ++n;
    return n;
}

}

template <class S>
i64 format(S& sink, const char* f, VaList args) {
    detail::Out<S> out(sink);
    while (*f) {
        if (*f != '%') {
            out.put(*f++);
            continue;
        }
        ++f;
        detail::Spec sp;
        for (;; ++f) {
            if (*f == '-') sp.left = true;
            else if (*f == '+') sp.plus = true;
            else if (*f == ' ') sp.space = true;
            else if (*f == '#') sp.alt = true;
            else if (*f == '0') sp.zero = true;
            else break;
        }
        if (*f == '*') {
            i32 w = __builtin_va_arg(args, i32);
            if (w < 0) {
                sp.left = true;
                w = -w;
            }
            sp.width = w;
            ++f;
        } else {
            while (*f >= '0' && *f <= '9') {
                if (sp.width < 100000000) sp.width = sp.width * 10 + (*f - '0');
                ++f;
            }
        }
        if (*f == '.') {
            ++f;
            sp.precision = 0;
            if (*f == '*') {
                i32 p = __builtin_va_arg(args, i32);
                sp.precision = p < 0 ? -1 : p;
                ++f;
            } else {
                while (*f >= '0' && *f <= '9') {
                    if (sp.precision < 100000000) sp.precision = sp.precision * 10 + (*f - '0');
                    ++f;
                }
            }
        }
        if (f[0] == 'h' && f[1] == 'h') {
            sp.bits = 8;
            f += 2;
        } else if (f[0] == 'h') {
            sp.bits = 16;
            ++f;
        } else if (f[0] == 'l' && f[1] == 'l') {
            sp.bits = 64;
            f += 2;
        } else if (f[0] == 'l') {
            sp.bits = 32;
            ++f;
        } else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') {
            sp.bits = 64;
            f += 3;
        } else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') {
            sp.bits = 32;
            f += 3;
        } else if (f[0] == 'z' || f[0] == 't' || f[0] == 'j' || f[0] == 'I') {
            sp.bits = 64;
            ++f;
        }
        sp.conv = *f;
        if (*f) ++f;

        switch (sp.conv) {
            case '%':
                out.put('%');
                break;
            case 'c': {
                char c = static_cast<char>(__builtin_va_arg(args, i32));
                u64 pad = sp.width > 1 ? static_cast<u64>(sp.width - 1) : 0;
                if (!sp.left) out.pad(' ', pad);
                out.put(c);
                if (sp.left) out.pad(' ', pad);
                break;
            }
            case 's': {
                const char* s = __builtin_va_arg(args, const char*);
                if (!s) s = "(null)";
                u64 n = detail::string_length(s, sp.precision);
                u64 pad = static_cast<u64>(sp.width) > n ? static_cast<u64>(sp.width) - n : 0;
                if (!sp.left) out.pad(' ', pad);
                out.text(s, n);
                if (sp.left) out.pad(' ', pad);
                break;
            }
            case 'd':
            case 'i':
            case 'u':
            case 'o':
            case 'x':
            case 'X':
            case 'p': {
                bool is_signed = sp.conv == 'd' || sp.conv == 'i';
                u32 base = sp.conv == 'o' ? 8 : (sp.conv == 'x' || sp.conv == 'X' || sp.conv == 'p') ? 16 : 10;
                bool upper = sp.conv == 'X' || sp.conv == 'p';
                u64 mag;
                bool negative = false;
                if (sp.conv == 'p') {
                    mag = reinterpret_cast<u64>(__builtin_va_arg(args, void*));
                    sp.precision = 16;
                    sp.alt = false;
                } else if (sp.bits == 64) {
                    u64 raw = __builtin_va_arg(args, u64);
                    negative = is_signed && static_cast<i64>(raw) < 0;
                    mag = negative ? 0 - raw : raw;
                } else {
                    u32 raw = __builtin_va_arg(args, u32);
                    if (sp.bits == 16) raw &= 0xFFFF;
                    if (sp.bits == 8) raw &= 0xFF;
                    if (is_signed) {
                        i32 v = sp.bits == 16 ? static_cast<i16>(raw) : sp.bits == 8 ? static_cast<i8>(raw) : static_cast<i32>(raw);
                        negative = v < 0;
                        mag = negative ? 0 - static_cast<u64>(static_cast<i64>(v)) : static_cast<u64>(v);
                    } else {
                        mag = raw;
                    }
                }
                char digits[24];
                u32 nd = 0;
                const char* alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
                if (!(mag == 0 && sp.precision == 0)) {
                    do {
                        digits[nd++] = alphabet[mag % base];
                        mag /= base;
                    } while (mag);
                }
                u64 zeros = sp.precision > static_cast<i64>(nd) ? static_cast<u64>(sp.precision) - nd : 0;
                char prefix[2];
                u32 np = 0;
                if (is_signed) {
                    if (negative) prefix[np++] = '-';
                    else if (sp.plus) prefix[np++] = '+';
                    else if (sp.space) prefix[np++] = ' ';
                } else if (sp.alt && base == 16 && nd != 0 && !(nd == 1 && digits[0] == '0')) {
                    prefix[np++] = '0';
                    prefix[np++] = upper ? 'X' : 'x';
                } else if (sp.alt && base == 8 && zeros == 0 && (nd == 0 || digits[nd - 1] != '0')) {
                    zeros = 1;
                }
                u64 body = np + zeros + nd;
                u64 pad = static_cast<u64>(sp.width) > body ? static_cast<u64>(sp.width) - body : 0;
                if (sp.zero && !sp.left && sp.precision < 0) {
                    zeros += pad;
                    pad = 0;
                }
                if (!sp.left) out.pad(' ', pad);
                out.text(prefix, np);
                out.pad('0', zeros);
                while (nd) out.put(digits[--nd]);
                if (sp.left) out.pad(' ', pad);
                break;
            }
            default:
                out.flush();
                return -1;
        }
    }
    out.flush();
    return static_cast<i64>(out.total);
}

}
