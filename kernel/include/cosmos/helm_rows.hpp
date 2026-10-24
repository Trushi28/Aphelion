#pragma once
#include <cosmos/helm.hpp>

namespace helm::rows {

struct View {
    const char* p;
    u32 n;
};

struct Token {
    u32 name_start;
    u32 name_len;
    u32 value_start;
    u32 value_len;
};

u64 body_start(const u8* line, u64 len);
bool next_token(const u8* line, u64 len, u64* pos, Token* out);
bool field(const u8* line, u64 len, const char* name, View* out);

bool as_number(View v, bool* negative, u64* magnitude);
int compare(View a, View b);

struct Set {
    u8* data;
    u64 size;
    u64 capacity;
    u32 count;
    bool failed;
};

void add(Set& s, const u8* line, u64 len);
void release(Set& s);
void print_table(Writer& w, const Set& s);

}
