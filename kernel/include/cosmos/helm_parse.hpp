#pragma once
#include <cosmos/types.hpp>
#include <cosmos/utf8.hpp>

namespace helm {
namespace parse {

constexpr u32 MAX_WORDS = 96;
constexpr u32 MAX_STAGES = 16;
constexpr u32 MAX_PIPES = 8;
constexpr u32 POOL_BYTES = 6144;
constexpr u32 MAX_LINE_BYTES = 1024;

constexpr char MARK_STAR = 1;
constexpr char MARK_ANY = 2;
constexpr char MARK_VAR = 3;
constexpr char MARK_END = 4;

enum class Error : u8 {
    Ok,
    TooLong,
    TooMany,
    Unterminated,
    BadEscape,
    BadRedirect,
    EmptyCommand,
    BadOperator,
    BadText,
};

enum class Role : u8 { Arg, In, Out, Append };
enum class Link : u8 { First, Seq, And, Or };

struct Word {
    u32 off;
    u32 len;
    u32 stage;
    Role role;
};

struct Stage {
    u32 args;
    bool has_in;
    bool has_out;
};

struct Pipe {
    u32 first_stage;
    u32 stage_count;
    Link link;
};

struct Parsed {
    Word words[MAX_WORDS];
    u32 word_count;
    char pool[POOL_BYTES];
    u32 pool_len;
    Stage stages[MAX_STAGES];
    u32 stage_count;
    Pipe pipes[MAX_PIPES];
    u32 pipe_count;
    u32 error_at;

    const char* text(const Word& w) const { return pool + w.off; }
};

inline const char* describe(Error e) {
    switch (e) {
        case Error::Ok: return "ok";
        case Error::TooLong: return "line or word is too long";
        case Error::TooMany: return "too many words, stages or commands on one line";
        case Error::Unterminated: return "unterminated quote";
        case Error::BadEscape: return "bad escape sequence";
        case Error::BadRedirect: return "redirection needs a file name";
        case Error::EmptyCommand: return "missing command";
        case Error::BadOperator: return "a single & is not supported, use && to chain";
        case Error::BadText: return "line contains a control character or invalid UTF-8";
    }
    return "error";
}

namespace detail {

inline bool is_space(char c) { return c == ' ' || c == '\t'; }
inline bool is_meta(char c) { return c == ';' || c == '|' || c == '&' || c == '<' || c == '>'; }
inline bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
inline int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool valid_utf8(const char* s, u32 n) {
    u32 i = 0;
    while (i < n) {
        u8 b = static_cast<u8>(s[i]);
        u32 need = 0, cp = 0, min = 0;
        if (b < 0x80) { ++i; continue; }
        if (b >= 0xC2 && b <= 0xDF) { need = 1; cp = b & 0x1F; min = 0x80; }
        else if (b >= 0xE0 && b <= 0xEF) { need = 2; cp = b & 0x0F; min = 0x800; }
        else if (b >= 0xF0 && b <= 0xF4) { need = 3; cp = b & 0x07; min = 0x10000; }
        else return false;
        for (u32 k = 1; k <= need; ++k) {
            if (i + k >= n) return false;
            u8 c = static_cast<u8>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (cp < min || !utf8::valid_scalar(cp)) return false;
        i += need + 1;
    }
    return true;
}

struct Builder {
    Parsed& p;
    const char* line;
    u32 i;
    Error err;

    bool fail(Error e) { if (err == Error::Ok) { err = e; p.error_at = i; } return false; }

    bool put(char c) {
        if (p.pool_len + 1 >= POOL_BYTES) return fail(Error::TooLong);
        p.pool[p.pool_len++] = c;
        return true;
    }

    bool put_cp(u32 cp) {
        char e[4];
        u32 n = utf8::encode(cp, e);
        for (u32 k = 0; k < n; ++k)
            if (!put(e[k])) return false;
        return true;
    }

    bool variable() {
        ++i;
        char c = line[i];
        if (c == '?') {
            ++i;
            return put(MARK_VAR) && put('?') && put(MARK_END);
        }
        if (c == '{') {
            u32 j = i + 1;
            u32 n = 0;
            while (is_name_char(line[j])) { ++j; ++n; }
            if (line[j] != '}' || n == 0) return fail(Error::BadEscape);
            if (!put(MARK_VAR)) return false;
            for (u32 k = i + 1; k < j; ++k)
                if (!put(line[k])) return false;
            i = j + 1;
            return put(MARK_END);
        }
        if (!is_name_char(c)) return put('$');
        if (!put(MARK_VAR)) return false;
        while (is_name_char(line[i])) {
            if (!put(line[i])) return false;
            ++i;
        }
        return put(MARK_END);
    }

    bool double_quoted() {
        ++i;
        for (;;) {
            char c = line[i];
            if (!c) return fail(Error::Unterminated);
            if (c == '"') { ++i; return true; }
            if (c == '$') { if (!variable()) return false; continue; }
            if (c != '\\') { if (!put(c)) return false; ++i; continue; }
            char n = line[i + 1];
            if (!n) return fail(Error::Unterminated);
            i += 2;
            switch (n) {
                case '"': case '\\': case '$': if (!put(n)) return false; break;
                case 'n': if (!put('\n')) return false; break;
                case 't': if (!put('\t')) return false; break;
                case 'e': if (!put('\x1b')) return false; break;
                case 'u': {
                    if (line[i] != '{') return fail(Error::BadEscape);
                    u32 j = i + 1, digits = 0, cp = 0;
                    while (hex_value(line[j]) >= 0) {
                        cp = (cp << 4) | static_cast<u32>(hex_value(line[j]));
                        ++j;
                        if (++digits > 6) return fail(Error::BadEscape);
                    }
                    if (line[j] != '}' || digits == 0 || !utf8::valid_scalar(cp) || cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
                        return fail(Error::BadEscape);
                    i = j + 1;
                    if (!put_cp(cp)) return false;
                    break;
                }
                default:
                    if (!put('\\') || !put(n)) return false;
                    break;
            }
        }
    }

    bool word(u32 stage, Role role) {
        if (p.word_count >= MAX_WORDS) return fail(Error::TooMany);
        Word& w = p.words[p.word_count];
        w.off = p.pool_len;
        w.stage = stage;
        w.role = role;
        for (;;) {
            char c = line[i];
            if (!c || is_space(c) || is_meta(c)) break;
            if (c == '\'') {
                ++i;
                while (line[i] && line[i] != '\'') {
                    if (!put(line[i])) return false;
                    ++i;
                }
                if (!line[i]) return fail(Error::Unterminated);
                ++i;
            } else if (c == '"') {
                if (!double_quoted()) return false;
            } else if (c == '\\') {
                if (!line[i + 1]) return fail(Error::Unterminated);
                if (!put(line[i + 1])) return false;
                i += 2;
            } else if (c == '$') {
                if (!variable()) return false;
            } else if (c == '*') {
                if (!put(MARK_STAR)) return false;
                ++i;
            } else if (c == '?') {
                if (!put(MARK_ANY)) return false;
                ++i;
            } else {
                if (!put(c)) return false;
                ++i;
            }
        }
        if (p.pool_len + 1 >= POOL_BYTES) return fail(Error::TooLong);
        w.len = p.pool_len - w.off;
        p.pool[p.pool_len++] = 0;
        ++p.word_count;
        return true;
    }
};

}

inline Error parse(const char* line, Parsed& out) {
    out.word_count = 0;
    out.pool_len = 0;
    out.stage_count = 0;
    out.pipe_count = 0;
    out.error_at = 0;

    u32 len = 0;
    while (line[len]) {
        if (len >= MAX_LINE_BYTES) return Error::TooLong;
        u8 b = static_cast<u8>(line[len]);
        if ((b < 0x20 && b != '\t') || b == 0x7F) return Error::BadText;
        ++len;
    }
    if (!detail::valid_utf8(line, len)) return Error::BadText;

    detail::Builder b{out, line, 0, Error::Ok};
    Link next_link = Link::First;
    u32 stage = 0;
    bool stage_open = true;
    u32 pipe_first_stage = 0;
    out.stages[0] = Stage{0, false, false};
    out.stage_count = 1;

    auto close_pipe = [&](Link following) -> bool {
        if (out.stages[stage].args == 0) {
            b.fail(Error::EmptyCommand);
            return false;
        }
        if (out.pipe_count >= MAX_PIPES) {
            b.fail(Error::TooMany);
            return false;
        }
        out.pipes[out.pipe_count++] = Pipe{pipe_first_stage, stage + 1 - pipe_first_stage, next_link};
        next_link = following;
        stage_open = false;
        return true;
    };

    auto open_stage = [&]() -> bool {
        if (out.stage_count >= MAX_STAGES) {
            b.fail(Error::TooMany);
            return false;
        }
        stage = out.stage_count;
        out.stages[out.stage_count++] = Stage{0, false, false};
        stage_open = true;
        return true;
    };

    for (;;) {
        while (detail::is_space(line[b.i])) ++b.i;
        char c = line[b.i];
        if (!c) break;
        if (c == '#') break;
        if (c == ';') {
            if (!close_pipe(Link::Seq)) return b.err;
            pipe_first_stage = out.stage_count;
            ++b.i;
            if (!open_stage()) return b.err;
            continue;
        }
        if (c == '&') {
            if (line[b.i + 1] != '&') { b.fail(Error::BadOperator); return b.err; }
            if (!close_pipe(Link::And)) return b.err;
            pipe_first_stage = out.stage_count;
            b.i += 2;
            if (!open_stage()) return b.err;
            continue;
        }
        if (c == '|') {
            if (line[b.i + 1] == '|') {
                if (!close_pipe(Link::Or)) return b.err;
                pipe_first_stage = out.stage_count;
                b.i += 2;
                if (!open_stage()) return b.err;
                continue;
            }
            if (out.stages[stage].args == 0) { b.fail(Error::EmptyCommand); return b.err; }
            ++b.i;
            if (!open_stage()) return b.err;
            continue;
        }
        if (c == '<' || c == '>') {
            Role role = Role::In;
            ++b.i;
            if (c == '>') {
                role = Role::Out;
                if (line[b.i] == '>') { role = Role::Append; ++b.i; }
            }
            while (detail::is_space(line[b.i])) ++b.i;
            char t = line[b.i];
            if (!t || detail::is_meta(t)) { b.fail(Error::BadRedirect); return b.err; }
            Stage& st = out.stages[stage];
            if ((role == Role::In && st.has_in) || (role != Role::In && st.has_out)) { b.fail(Error::BadRedirect); return b.err; }
            if (!b.word(stage, role)) return b.err;
            if (role == Role::In) st.has_in = true; else st.has_out = true;
            continue;
        }
        if (!b.word(stage, Role::Arg)) return b.err;
        ++out.stages[stage].args;
    }
    if (stage_open) {
        Stage& st = out.stages[stage];
        if (st.args > 0) {
            if (!close_pipe(Link::First)) return b.err;
        } else if (st.has_in || st.has_out || stage > pipe_first_stage || next_link == Link::And || next_link == Link::Or) {
            b.fail(Error::EmptyCommand);
            return b.err;
        }
    }
    return Error::Ok;
}

}
}
