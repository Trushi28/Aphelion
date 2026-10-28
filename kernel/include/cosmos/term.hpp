#pragma once
#include <cosmos/types.hpp>
#include <cosmos/utf8.hpp>

namespace term {

constexpr u32 MAX_COLS = 480;
constexpr u32 MAX_ROWS = 200;
constexpr u32 MAX_PARAMS = 16;
constexpr u32 PALETTE_SIZE = 16;
constexpr u32 TAB_WIDTH = 8;

constexpr u32 ATTR_SHIFT = 21;
constexpr u32 CP_MASK = (1u << ATTR_SHIFT) - 1;

enum Attr : u32 {
    BOLD = 1u << 0,
    DIM = 1u << 1,
    UNDERLINE = 1u << 2,
    REVERSE = 1u << 3,
    WIDE_LEAD = 1u << 4,
    WIDE_TAIL = 1u << 5,
};

struct Cell {
    u32 glyph;
    u32 fg;
    u32 bg;
};

constexpr u32 cell_cp(const Cell& c) { return c.glyph & CP_MASK; }
constexpr u32 cell_attr(const Cell& c) { return c.glyph >> ATTR_SHIFT; }
constexpr u32 make_glyph(u32 cp, u32 attr) { return (cp & CP_MASK) | (attr << ATTR_SHIFT); }

struct Palette {
    u32 ansi[PALETTE_SIZE];
    u32 default_fg;
    u32 default_bg;
};

u32 xterm_256(const Palette& p, u32 index);

class Terminal {
public:
    void init(Cell* storage, u32 cols, u32 rows, const Palette& palette);

    void write(const char* s, u32 n);
    void write(const char* s);
    void put_byte(u8 b);
    void put_cp(u32 cp);

    void set_colors(u32 fg, u32 bg);
    void reset_colors();
    void clear_all();

    u32 cols() const { return cols_; }
    u32 rows() const { return rows_; }
    u32 cursor_col() const { return cx_; }
    u32 cursor_row() const { return cy_; }
    bool cursor_visible() const { return cursor_visible_; }
    const Cell& cell(u32 col, u32 row) const { return grid_[row * cols_ + col]; }
    const Palette& palette() const { return palette_; }

    bool row_dirty(u32 row) const { return (dirty_[row >> 6] >> (row & 63)) & 1; }

    u32 dirty_begin(u32 row) const { return dirty_lo_[row]; }
    u32 dirty_end(u32 row) const { return dirty_hi_[row]; }
    bool any_dirty() const;
    void clear_dirty();
    void mark_all_dirty();
    i32 take_scroll();

    u32 row_text(u32 row, char* out, u32 max) const;

    void set_history(Cell* storage, u32 lines);
    u32 history_count() const { return hist_count_; }
    u32 history_capacity() const { return hist_cap_; }
    u32 view_offset() const { return view_; }
    bool scroll_view(i32 lines);
    bool snap_to_live();
    const Cell& view_cell(u32 col, u32 row) const;

private:
    enum class State : u8 { Ground, Escape, Csi, Osc, OscEscape, Charset };

    Cell blank() const;
    Cell& at(u32 col, u32 row) { return grid_[row * cols_ + col]; }
    void mark_row(u32 row) { mark_span(row, 0, cols_); }

    void mark_span(u32 row, u32 c0, u32 c1) {
        u64 bit = 1ull << (row & 63);
        if (dirty_[row >> 6] & bit) {
            if (c0 < dirty_lo_[row]) dirty_lo_[row] = static_cast<u16>(c0);
            if (c1 > dirty_hi_[row]) dirty_hi_[row] = static_cast<u16>(c1);
        } else {
            dirty_[row >> 6] |= bit;
            dirty_lo_[row] = static_cast<u16>(c0);
            dirty_hi_[row] = static_cast<u16>(c1);
        }
    }

    void mark_cursor() { mark_span(cy_, cx_, cx_ + 1 < cols_ ? cx_ + 1 : cols_); }
    void move_to(u32 col, u32 row);

    void line_feed();
    void reverse_index();
    void carriage_return();
    void scroll_up(u32 n);
    void scroll_down(u32 n);
    void insert_lines(u32 n);
    void delete_lines(u32 n);
    void insert_chars(u32 n);
    void delete_chars(u32 n);
    void erase_chars(u32 n);
    void erase_line(u32 mode);
    void erase_display(u32 mode);
    void fill(u32 col0, u32 row0, u32 col1, u32 row1);
    void break_wide(u32 col, u32 row);
    void new_line_wrap();
    void push_history(u32 n);

    void control(u8 b);
    void escape_final(u8 b);
    void csi_final(u8 b);
    void sgr();
    void save_cursor();
    void restore_cursor();
    void reset();
    u32 param(u32 index, u32 fallback) const;

    Cell* grid_ = nullptr;
    u32 cols_ = 0;
    u32 rows_ = 0;
    u32 cx_ = 0;
    u32 cy_ = 0;
    bool wrap_pending_ = false;
    bool autowrap_ = true;
    bool cursor_visible_ = true;
    u32 top_ = 0;
    u32 bottom_ = 0;
    u32 fg_ = 0;
    u32 bg_ = 0;
    u32 attr_ = 0;
    u32 saved_cx_ = 0;
    u32 saved_cy_ = 0;
    u32 saved_fg_ = 0;
    u32 saved_bg_ = 0;
    u32 saved_attr_ = 0;
    Palette palette_{};
    u64 dirty_[(MAX_ROWS + 63) / 64] = {};
    u16 dirty_lo_[MAX_ROWS] = {};
    u16 dirty_hi_[MAX_ROWS] = {};
    i32 scrolled_ = 0;
    Cell* hist_ = nullptr;
    u32 hist_cap_ = 0;
    u32 hist_cols_ = 0;
    u32 hist_head_ = 0;
    u32 hist_count_ = 0;
    u32 view_ = 0;

    State state_ = State::Ground;
    u32 params_[MAX_PARAMS] = {};
    u32 param_count_ = 0;
    bool param_seen_ = false;
    bool private_ = false;
    utf8::Decoder decoder_{};
};

}
