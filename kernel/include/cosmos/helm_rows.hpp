#pragma once
#include <cosmos/helm.hpp>

namespace helm {

namespace rows {

enum class Kind : u8 { Text, Int, Size, Time, Flag };

constexpr u32 MAX_COLUMNS = 12;
constexpr u32 NAME_BYTES = 24;
constexpr u32 NOTE_BYTES = 160;
constexpr u32 NULL_LEN = 0xFFFFFFFFu;
constexpr u64 LIMIT_BYTES = 8ull << 20;

struct Column {
    char name[NAME_BYTES];
    Kind kind;
};

struct Cell {
    u64 num;
    u32 off;
    u32 len;
};

struct Table {
    Column col[MAX_COLUMNS];
    u32 cols;
    Cell* cells;
    u32 rows;
    u32 capacity;
    char* pool;
    u32 pool_len;
    u32 pool_cap;
    i32 body;
    bool failed;
    char note[NOTE_BYTES];
};

void begin(Table& t);
void release(Table& t);
bool add_column(Table& t, const char* name, Kind kind);
u32 add_row(Table& t);
void set_text(Table& t, u32 row, u32 col, const char* s, u32 n);
void set_text(Table& t, u32 row, u32 col, const char* s);
void set_num(Table& t, u32 row, u32 col, u64 v);
void set_null(Table& t, u32 row, u32 col);
void set_note(Table& t, const char* s);

i32 find_column(const Table& t, const char* name);
bool is_null(const Table& t, u32 row, u32 col);
const char* text_at(const Table& t, u32 row, u32 col, u32* n);
u64 num_at(const Table& t, u32 row, u32 col);

int compare_cells(const Table& a, u32 row_a, u32 col_a, const Table& b, u32 row_b, u32 col_b, bool leading_number);

bool from_text(Table& t, const u8* data, u64 size);
bool copy_schema(Table& dst, const Table& src);
bool copy_row(Table& dst, const Table& src, u32 row);

u32 format_cell(const Table& t, u32 row, u32 col, char* out, u32 cap);

void render(Writer& w, const Table& t);
void flatten(Writer& w, const Table& t);

bool parse_value(Kind kind, const char* text, u64* out);

bool sort_rows(Table& out, const Table& in, const u32* columns, u32 column_count, bool descending, bool leading_number);

}

}
