#include <cosmos/gdt.hpp>
#include <cosmos/cpu.hpp>

namespace gdt {

struct PACKED Table {
    Entry null_desc;
    Entry kcode;
    Entry kdata;
    Entry udata;
    Entry ucode;
    TssEntry tss;
};

struct alignas(16) CoreState {
    Table gdt;
    Tss tss;
};

struct Seen { u64 gdt_base, tss_base, ist1, ist2; };

alignas(16) static CoreState g_core[MAX_CORES];
alignas(16) static u8 g_df_stack[MAX_CORES][IST_STACK_BYTES];
alignas(16) static u8 g_nmi_stack[MAX_CORES][IST_STACK_BYTES];
static Seen g_seen[MAX_CORES];
static volatile u64 g_seen_mask = 0;

struct PACKED Descriptor { u16 limit; u64 base; };

extern "C" void gdt_flush(Descriptor* desc, u16 code_sel, u16 data_sel);
extern "C" void tss_flush(u16 tss_sel);

static void set_entry(Entry& e, u32 base, u32 limit, u8 access, u8 gran) {
    e.limit_low   = limit & 0xFFFF;
    e.base_low    = base & 0xFFFF;
    e.base_mid    = (base >> 16) & 0xFF;
    e.access      = access;
    e.granularity = (gran & 0xF0) | ((limit >> 16) & 0x0F);
    e.base_high   = (base >> 24) & 0xFF;
}

static u64 df_top(u32 core) { return reinterpret_cast<u64>(&g_df_stack[core][IST_STACK_BYTES]); }
static u64 nmi_top(u32 core) { return reinterpret_cast<u64>(&g_nmi_stack[core][IST_STACK_BYTES]); }

static void record_loaded_state(u32 core) {
    Descriptor loaded{};
    asm volatile("sgdt %0" : "=m"(loaded));
    u16 task_reg = 0;
    asm volatile("str %0" : "=r"(task_reg));

    Seen s{};
    s.gdt_base = loaded.base;
    if (task_reg == TSS_SEL) {
        const auto* e = reinterpret_cast<const TssEntry*>(loaded.base + TSS_SEL);
        s.tss_base = static_cast<u64>(e->base_low) | (static_cast<u64>(e->base_mid) << 16) |
                     (static_cast<u64>(e->base_high) << 24) | (static_cast<u64>(e->base_upper) << 32);
        const auto* t = reinterpret_cast<const Tss*>(s.tss_base);
        s.ist1 = t->ist1;
        s.ist2 = t->ist2;
    }
    g_seen[core] = s;
    __atomic_or_fetch(&g_seen_mask, 1ull << core, __ATOMIC_SEQ_CST);
}

void init(u32 core) {
    if (core >= MAX_CORES) cpu::hang();
    CoreState& c = g_core[core];

    __builtin_memset(&c.gdt, 0, sizeof(c.gdt));
    set_entry(c.gdt.kcode, 0, 0xFFFFF, 0x9A, 0xA0);
    set_entry(c.gdt.kdata, 0, 0xFFFFF, 0x92, 0xC0);
    set_entry(c.gdt.udata, 0, 0xFFFFF, 0xF2, 0xC0);
    set_entry(c.gdt.ucode, 0, 0xFFFFF, 0xFA, 0xA0);

    __builtin_memset(&c.tss, 0, sizeof(c.tss));
    c.tss.ist1 = df_top(core);
    c.tss.ist2 = nmi_top(core);
    c.tss.iomap_base = sizeof(Tss);

    u64 tss_base = reinterpret_cast<u64>(&c.tss);
    c.gdt.tss.length    = sizeof(Tss) - 1;
    c.gdt.tss.base_low  = tss_base & 0xFFFF;
    c.gdt.tss.base_mid  = (tss_base >> 16) & 0xFF;
    c.gdt.tss.flags1    = 0x89;
    c.gdt.tss.flags2    = 0x00;
    c.gdt.tss.base_high = (tss_base >> 24) & 0xFF;
    c.gdt.tss.base_upper = static_cast<u32>(tss_base >> 32);
    c.gdt.tss.reserved  = 0;

    Descriptor desc{ sizeof(c.gdt) - 1, reinterpret_cast<u64>(&c.gdt) };
    gdt_flush(&desc, KCODE_SEL, KDATA_SEL);
    tss_flush(TSS_SEL);
    record_loaded_state(core);
}

u32 cores_seen() {
    u64 m = __atomic_load_n(&g_seen_mask, __ATOMIC_SEQ_CST);
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

bool selftest() {
    u64 mask = __atomic_load_n(&g_seen_mask, __ATOMIC_SEQ_CST);
    if (!(mask & 1)) return false;
    for (u32 i = 0; i < MAX_CORES; ++i) {
        if (!(mask & (1ull << i))) continue;
        const Seen& a = g_seen[i];
        if (a.gdt_base != reinterpret_cast<u64>(&g_core[i].gdt)) return false;
        if (a.tss_base != reinterpret_cast<u64>(&g_core[i].tss)) return false;
        if (a.ist1 != df_top(i) || a.ist2 != nmi_top(i)) return false;
        for (u32 j = 0; j < i; ++j) {
            if (!(mask & (1ull << j))) continue;
            const Seen& b = g_seen[j];
            if (a.gdt_base == b.gdt_base || a.tss_base == b.tss_base) return false;
            if (a.ist1 == b.ist1 || a.ist2 == b.ist2) return false;
        }
    }
    return true;
}

}
