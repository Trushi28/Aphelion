#include <cosmos/term.hpp>

namespace term {

u32 xterm_256(const Palette& p, u32 index) {
    if (index < PALETTE_SIZE) return p.ansi[index];
    if (index < 232) {
        u32 i = index - 16;
        auto level = [](u32 v) -> u32 { return v == 0 ? 0 : 55 + v * 40; };
        return (level(i / 36) << 16) | (level((i / 6) % 6) << 8) | level(i % 6);
    }
    u32 g = 8 + (index - 232) * 10;
    return (g << 16) | (g << 8) | g;
}

void Terminal::init(Cell* storage, u32 cols, u32 rows, const Palette& palette) {
    grid_ = storage;
    cols_ = cols > MAX_COLS ? MAX_COLS : (cols ? cols : 1);
    rows_ = rows > MAX_ROWS ? MAX_ROWS : (rows ? rows : 1);
    palette_ = palette;
    autowrap_ = true;
    cursor_visible_ = true;
    reset();
    clear_all();
}

void Terminal::reset() {
    cx_ = cy_ = 0;
    wrap_pending_ = false;
    top_ = 0;
    bottom_ = rows_ - 1;
    fg_ = palette_.default_fg;
    bg_ = palette_.default_bg;
    attr_ = 0;
    saved_cx_ = saved_cy_ = 0;
    saved_fg_ = fg_;
    saved_bg_ = bg_;
    saved_attr_ = 0;
    state_ = State::Ground;
    param_count_ = 0;
    param_seen_ = false;
    private_ = false;
    decoder_ = utf8::Decoder{};
    autowrap_ = true;
    cursor_visible_ = true;
}

Cell Terminal::blank() const { return Cell{ make_glyph(' ', 0), fg_, bg_ }; }

void Terminal::set_colors(u32 fg, u32 bg) {
    fg_ = fg;
    bg_ = bg;
}

void Terminal::reset_colors() {
    fg_ = palette_.default_fg;
    bg_ = palette_.default_bg;
    attr_ = 0;
}

void Terminal::clear_all() {
    Cell b = Cell{ make_glyph(' ', 0), palette_.default_fg, palette_.default_bg };
    for (u32 i = 0; i < cols_ * rows_; ++i) grid_[i] = b;
    cx_ = cy_ = 0;
    wrap_pending_ = false;
    scrolled_ = 0;
    mark_all_dirty();
}

bool Terminal::any_dirty() const {
    for (u64 w : dirty_) if (w) return true;
    return false;
}

void Terminal::clear_dirty() {
    for (u64& w : dirty_) w = 0;
    scrolled_ = 0;
}

void Terminal::mark_all_dirty() {
    for (u32 r = 0; r < rows_; ++r) mark_row(r);
    scrolled_ = 0;
}

i32 Terminal::take_scroll() {
    i32 s = scrolled_;
    scrolled_ = 0;
    return s;
}

void Terminal::move_to(u32 col, u32 row) {
    mark_cursor();
    cx_ = col >= cols_ ? cols_ - 1 : col;
    cy_ = row >= rows_ ? rows_ - 1 : row;
    wrap_pending_ = false;
    mark_cursor();
}

void Terminal::fill(u32 col0, u32 row0, u32 col1, u32 row1) {
    Cell b = blank();
    for (u32 r = row0; r <= row1 && r < rows_; ++r) {
        for (u32 c = col0; c <= col1 && c < cols_; ++c) at(c, r) = b;

        mark_span(r, col0 > 0 ? col0 - 1 : 0, col1 + 2 < cols_ ? col1 + 2 : cols_);
    }
}

void Terminal::break_wide(u32 col, u32 row) {
    Cell& c = at(col, row);
    u32 a = cell_attr(c);
    Cell b = blank();
    if ((a & WIDE_LEAD) && col + 1 < cols_) at(col + 1, row) = b;
    if ((a & WIDE_TAIL) && col > 0) at(col - 1, row) = b;
}

void Terminal::scroll_up(u32 n) {
    if (n == 0) return;
    u32 height = bottom_ - top_ + 1;
    if (n > height) n = height;

    mark_cursor();
    for (u32 r = top_; r + n <= bottom_; ++r)
        mem::move(&at(0, r), &at(0, r + n), cols_ * sizeof(Cell));
    Cell b = blank();
    for (u32 r = bottom_ + 1 - n; r <= bottom_; ++r)
        for (u32 c = 0; c < cols_; ++c) at(c, r) = b;

    if (top_ == 0 && bottom_ == rows_ - 1) {
        if (static_cast<u32>(scrolled_) + n >= rows_) {
            mark_all_dirty();
            return;
        }
        scrolled_ += static_cast<i32>(n);
        for (u32 r = 0; r + n < rows_; ++r) {
            if (row_dirty(r + n)) mark_row(r);
            else dirty_[r >> 6] &= ~(1ull << (r & 63));
        }
        for (u32 r = rows_ - n; r < rows_; ++r) mark_row(r);
        mark_cursor();
    } else {
        for (u32 r = top_; r <= bottom_; ++r) mark_row(r);
    }
}

void Terminal::scroll_down(u32 n) {
    if (n == 0) return;
    u32 height = bottom_ - top_ + 1;
    if (n > height) n = height;
    for (u32 r = bottom_; r >= top_ + n; --r) {
        mem::move(&at(0, r), &at(0, r - n), cols_ * sizeof(Cell));
        if (r == 0) break;
    }
    Cell b = blank();
    for (u32 r = top_; r < top_ + n; ++r)
        for (u32 c = 0; c < cols_; ++c) at(c, r) = b;
    for (u32 r = top_; r <= bottom_; ++r) mark_row(r);
}

void Terminal::line_feed() {
    mark_cursor();
    if (cy_ == bottom_) scroll_up(1);
    else if (cy_ + 1 < rows_) ++cy_;
    wrap_pending_ = false;
    mark_cursor();
}

void Terminal::reverse_index() {
    mark_cursor();
    if (cy_ == top_) scroll_down(1);
    else if (cy_ > 0) --cy_;
    wrap_pending_ = false;
    mark_cursor();
}

void Terminal::carriage_return() {
    mark_cursor();
    cx_ = 0;
    wrap_pending_ = false;
    mark_cursor();
}

void Terminal::insert_lines(u32 n) {
    if (cy_ < top_ || cy_ > bottom_) return;
    u32 saved_top = top_;
    top_ = cy_;
    scroll_down(n);
    top_ = saved_top;
    cx_ = 0;
    wrap_pending_ = false;
    mark_row(cy_);
}

void Terminal::delete_lines(u32 n) {
    if (cy_ < top_ || cy_ > bottom_) return;
    u32 saved_top = top_;
    top_ = cy_;
    bool full = saved_top == 0 && bottom_ == rows_ - 1 && cy_ == 0;
    if (full) {
        scroll_up(n);
    } else {
        u32 height = bottom_ - top_ + 1;
        if (n > height) n = height;
        for (u32 r = top_; r + n <= bottom_; ++r)
            mem::move(&at(0, r), &at(0, r + n), cols_ * sizeof(Cell));
        Cell b = blank();
        for (u32 r = bottom_ + 1 - n; r <= bottom_; ++r)
            for (u32 c = 0; c < cols_; ++c) at(c, r) = b;
        for (u32 r = top_; r <= bottom_; ++r) mark_row(r);
    }
    top_ = saved_top;
    cx_ = 0;
    wrap_pending_ = false;
    mark_row(cy_);
}

void Terminal::insert_chars(u32 n) {
    if (n > cols_ - cx_) n = cols_ - cx_;
    break_wide(cx_, cy_);
    for (u32 c = cols_ - 1; c >= cx_ + n; --c) {
        at(c, cy_) = at(c - n, cy_);
        if (c == 0) break;
    }
    Cell b = blank();
    for (u32 c = cx_; c < cx_ + n; ++c) at(c, cy_) = b;
    mark_span(cy_, cx_ > 0 ? cx_ - 1 : 0, cols_);
}

void Terminal::delete_chars(u32 n) {
    if (n > cols_ - cx_) n = cols_ - cx_;
    break_wide(cx_, cy_);
    for (u32 c = cx_; c + n < cols_; ++c) at(c, cy_) = at(c + n, cy_);
    Cell b = blank();
    for (u32 c = cols_ - n; c < cols_; ++c) at(c, cy_) = b;
    mark_span(cy_, cx_ > 0 ? cx_ - 1 : 0, cols_);
}

void Terminal::erase_chars(u32 n) {
    if (n > cols_ - cx_) n = cols_ - cx_;
    break_wide(cx_, cy_);
    if (n) break_wide(cx_ + n - 1, cy_);
    fill(cx_, cy_, cx_ + n - 1, cy_);
}

void Terminal::erase_line(u32 mode) {
    if (mode == 0) { break_wide(cx_, cy_); fill(cx_, cy_, cols_ - 1, cy_); }
    else if (mode == 1) { break_wide(cx_, cy_); fill(0, cy_, cx_, cy_); }
    else fill(0, cy_, cols_ - 1, cy_);
}

void Terminal::erase_display(u32 mode) {
    if (mode == 0) {
        erase_line(0);
        if (cy_ + 1 < rows_) fill(0, cy_ + 1, cols_ - 1, rows_ - 1);
    } else if (mode == 1) {
        erase_line(1);
        if (cy_ > 0) fill(0, 0, cols_ - 1, cy_ - 1);
    } else {
        fill(0, 0, cols_ - 1, rows_ - 1);
    }
}

void Terminal::new_line_wrap() {
    carriage_return();
    line_feed();
}

void Terminal::put_cp(u32 cp) {
    u32 w = utf8::width(cp);
    if (w == 0) return;
    if (!utf8::valid_scalar(cp)) cp = utf8::REPLACEMENT;
    if (wrap_pending_) {
        if (autowrap_) new_line_wrap();
        wrap_pending_ = false;
    }
    if (w == 2 && cx_ + 1 >= cols_) {
        if (!autowrap_) return;
        break_wide(cx_, cy_);
        at(cx_, cy_) = blank();
        mark_row(cy_);
        new_line_wrap();
    }
    break_wide(cx_, cy_);
    if (w == 2) break_wide(cx_ + 1, cy_);
    u32 base = attr_ & ~(WIDE_LEAD | WIDE_TAIL);
    at(cx_, cy_) = Cell{ make_glyph(cp, base | (w == 2 ? static_cast<u32>(WIDE_LEAD) : 0u)), fg_, bg_ };
    if (w == 2) at(cx_ + 1, cy_) = Cell{ make_glyph(0, base | WIDE_TAIL), fg_, bg_ };

    mark_span(cy_, cx_ > 0 ? cx_ - 1 : 0, cx_ + w + 2 < cols_ ? cx_ + w + 2 : cols_);
    u32 next = cx_ + w;
    if (next >= cols_) {
        cx_ = cols_ - 1;
        wrap_pending_ = true;
    } else {
        cx_ = next;
    }
    mark_cursor();
}

void Terminal::control(u8 b) {
    switch (b) {
        case '\n':
        case 0x0B:
        case 0x0C:
            carriage_return();
            line_feed();
            break;
        case '\r':
            carriage_return();
            break;
        case '\b':
            mark_cursor();
            if (cx_ > 0) --cx_;
            wrap_pending_ = false;
            mark_cursor();
            break;
        case '\t': {
            u32 next = (cx_ / TAB_WIDTH + 1) * TAB_WIDTH;
            move_to(next >= cols_ ? cols_ - 1 : next, cy_);
            break;
        }
        case 0x1B:
            state_ = State::Escape;
            break;
        default:
            break;
    }
}

u32 Terminal::param(u32 index, u32 fallback) const {
    if (index >= param_count_) return fallback;
    return params_[index] ? params_[index] : fallback;
}

void Terminal::save_cursor() {
    saved_cx_ = cx_;
    saved_cy_ = cy_;
    saved_fg_ = fg_;
    saved_bg_ = bg_;
    saved_attr_ = attr_;
}

void Terminal::restore_cursor() {
    move_to(saved_cx_, saved_cy_);
    fg_ = saved_fg_;
    bg_ = saved_bg_;
    attr_ = saved_attr_;
}

void Terminal::sgr() {
    if (param_count_ == 0) { reset_colors(); return; }
    for (u32 i = 0; i < param_count_; ++i) {
        u32 p = params_[i];
        if (p == 0) reset_colors();
        else if (p == 1) attr_ |= BOLD;
        else if (p == 2) attr_ |= DIM;
        else if (p == 4) attr_ |= UNDERLINE;
        else if (p == 7) attr_ |= REVERSE;
        else if (p == 22) attr_ &= ~(BOLD | DIM);
        else if (p == 24) attr_ &= ~UNDERLINE;
        else if (p == 27) attr_ &= ~REVERSE;
        else if (p >= 30 && p <= 37) fg_ = palette_.ansi[p - 30];
        else if (p == 39) fg_ = palette_.default_fg;
        else if (p >= 40 && p <= 47) bg_ = palette_.ansi[p - 40];
        else if (p == 49) bg_ = palette_.default_bg;
        else if (p >= 90 && p <= 97) fg_ = palette_.ansi[8 + p - 90];
        else if (p >= 100 && p <= 107) bg_ = palette_.ansi[8 + p - 100];
        else if (p == 38 || p == 48) {
            u32* target = p == 38 ? &fg_ : &bg_;
            if (i + 1 < param_count_ && params_[i + 1] == 5 && i + 2 < param_count_) {
                *target = xterm_256(palette_, params_[i + 2] & 0xFF);
                i += 2;
            } else if (i + 1 < param_count_ && params_[i + 1] == 2 && i + 4 < param_count_) {
                *target = ((params_[i + 2] & 0xFF) << 16) | ((params_[i + 3] & 0xFF) << 8) | (params_[i + 4] & 0xFF);
                i += 4;
            } else {
                i = param_count_;
            }
        }
    }
}

void Terminal::csi_final(u8 b) {
    if (private_) {
        bool set = b == 'h';
        if ((b == 'h' || b == 'l') && param_count_ > 0) {
            for (u32 i = 0; i < param_count_; ++i) {
                if (params_[i] == 25) { cursor_visible_ = set; mark_cursor(); }
                else if (params_[i] == 7) autowrap_ = set;
            }
        }
        return;
    }
    u32 n = param(0, 1);
    switch (b) {
        case 'A': move_to(cx_, cy_ >= n ? cy_ - n : 0); break;
        case 'B': move_to(cx_, cy_ + n); break;
        case 'C': move_to(cx_ + n, cy_); break;
        case 'D': move_to(cx_ >= n ? cx_ - n : 0, cy_); break;
        case 'E': move_to(0, cy_ + n); break;
        case 'F': move_to(0, cy_ >= n ? cy_ - n : 0); break;
        case 'G': move_to(param(0, 1) - 1, cy_); break;
        case 'd': move_to(cx_, param(0, 1) - 1); break;
        case 'H':
        case 'f': move_to(param(1, 1) - 1, param(0, 1) - 1); break;
        case 'J': erase_display(param_count_ ? params_[0] : 0); break;
        case 'K': erase_line(param_count_ ? params_[0] : 0); break;
        case 'L': insert_lines(n); break;
        case 'M': delete_lines(n); break;
        case 'P': delete_chars(n); break;
        case '@': insert_chars(n); break;
        case 'X': erase_chars(n); break;
        case 'S': scroll_up(n); break;
        case 'T': scroll_down(n); break;
        case 'm': sgr(); break;
        case 's': save_cursor(); break;
        case 'u': restore_cursor(); break;
        case 'r': {
            u32 t = param(0, 1) - 1;
            u32 bt = param_count_ > 1 && params_[1] ? params_[1] - 1 : rows_ - 1;
            if (bt >= rows_) bt = rows_ - 1;
            if (t < bt) { top_ = t; bottom_ = bt; move_to(0, 0); }
            break;
        }
        default: break;
    }
}

void Terminal::escape_final(u8 b) {
    switch (b) {
        case '[':
            state_ = State::Csi;
            param_count_ = 0;
            param_seen_ = false;
            private_ = false;
            for (u32& p : params_) p = 0;
            return;
        case ']':
            state_ = State::Osc;
            return;
        case '(':
        case ')':
        case '*':
        case '+':
            state_ = State::Charset;
            return;
        case '7': save_cursor(); break;
        case '8': restore_cursor(); break;
        case 'c': reset(); clear_all(); break;
        case 'D': line_feed(); break;
        case 'E': carriage_return(); line_feed(); break;
        case 'M': reverse_index(); break;
        default: break;
    }
    state_ = State::Ground;
}

void Terminal::put_byte(u8 b) {
    switch (state_) {
        case State::Ground: {
            if (b < 0x20 || b == 0x7F) {
                if (decoder_.pending()) {
                    u32 tail[1];
                    if (decoder_.finish(tail)) put_cp(tail[0]);
                }
                if (b != 0x7F) control(b);
                return;
            }
            u32 out[2];
            u32 k = decoder_.feed(b, out);
            for (u32 i = 0; i < k; ++i) put_cp(out[i]);
            return;
        }
        case State::Escape:
            if (b == 0x18 || b == 0x1A) { state_ = State::Ground; return; }
            if (b == 0x1B) return;
            escape_final(b);
            return;
        case State::Charset:
            state_ = State::Ground;
            return;
        case State::Osc:
            if (b == 0x07 || b == 0x18 || b == 0x1A) state_ = State::Ground;
            else if (b == 0x1B) state_ = State::OscEscape;
            return;
        case State::OscEscape:
            state_ = b == '\\' ? State::Ground : State::Osc;
            return;
        case State::Csi:
            if (b == 0x18 || b == 0x1A) { state_ = State::Ground; return; }
            if (b == 0x1B) { state_ = State::Escape; return; }
            if (b >= '0' && b <= '9') {
                if (param_count_ == 0) param_count_ = 1;
                if (!param_seen_) param_seen_ = true;
                u32& p = params_[param_count_ - 1];
                if (p < 100000) p = p * 10 + (b - '0');
                return;
            }
            if (b == ';' || b == ':') {
                if (param_count_ == 0) param_count_ = 1;
                if (param_count_ < MAX_PARAMS) ++param_count_;
                return;
            }
            if (b == '?' || b == '>' || b == '=' || b == '<') { private_ = b == '?'; return; }
            if (b >= 0x20 && b <= 0x2F) return;
            if (b >= 0x40 && b <= 0x7E) {
                csi_final(b);
                state_ = State::Ground;
                return;
            }
            state_ = State::Ground;
            return;
    }
}

void Terminal::write(const char* s, u32 n) {
    for (u32 i = 0; i < n; ++i) put_byte(static_cast<u8>(s[i]));
}

void Terminal::write(const char* s) {
    while (*s) put_byte(static_cast<u8>(*s++));
}

u32 Terminal::row_text(u32 row, char* out, u32 max) const {
    if (row >= rows_ || max == 0) return 0;
    u32 last = 0;
    for (u32 c = 0; c < cols_; ++c) {
        const Cell& cl = cell(c, row);
        u32 a = cell_attr(cl);
        if (a & WIDE_TAIL) continue;
        if (cell_cp(cl) != ' ') last = c + 1;
    }
    u32 n = 0;
    for (u32 c = 0; c < last; ++c) {
        const Cell& cl = cell(c, row);
        if (cell_attr(cl) & WIDE_TAIL) continue;
        char enc[4];
        u32 k = utf8::encode(cell_cp(cl) ? cell_cp(cl) : ' ', enc);
        if (n + k + 1 > max) break;
        for (u32 i = 0; i < k; ++i) out[n++] = enc[i];
    }
    out[n] = 0;
    return n;
}

}
