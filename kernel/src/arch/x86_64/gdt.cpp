#include <cosmos/gdt.hpp>

namespace gdt {

struct PACKED Table {
    Entry null_desc;
    Entry kcode;
    Entry kdata;
    Entry udata;
    Entry ucode;
    TssEntry tss;
};

alignas(16) static Table g_gdt;
alignas(16) static Tss g_tss;

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

void init(u64 double_fault_stack_top, u64 nmi_stack_top) {
    __builtin_memset(&g_gdt, 0, sizeof(g_gdt));
    set_entry(g_gdt.kcode, 0, 0xFFFFF, 0x9A, 0xA0);
    set_entry(g_gdt.kdata, 0, 0xFFFFF, 0x92, 0xC0);
    set_entry(g_gdt.udata, 0, 0xFFFFF, 0xF2, 0xC0);
    set_entry(g_gdt.ucode, 0, 0xFFFFF, 0xFA, 0xA0);

    __builtin_memset(&g_tss, 0, sizeof(g_tss));
    g_tss.ist1 = double_fault_stack_top;
    g_tss.ist2 = nmi_stack_top;
    g_tss.iomap_base = sizeof(Tss);

    u64 tss_base = reinterpret_cast<u64>(&g_tss);
    g_gdt.tss.length    = sizeof(Tss) - 1;
    g_gdt.tss.base_low  = tss_base & 0xFFFF;
    g_gdt.tss.base_mid  = (tss_base >> 16) & 0xFF;
    g_gdt.tss.flags1    = 0x89;
    g_gdt.tss.flags2    = 0x00;
    g_gdt.tss.base_high = (tss_base >> 24) & 0xFF;
    g_gdt.tss.base_upper = static_cast<u32>(tss_base >> 32);
    g_gdt.tss.reserved  = 0;

    Descriptor desc{ sizeof(g_gdt) - 1, reinterpret_cast<u64>(&g_gdt) };
    gdt_flush(&desc, KCODE_SEL, KDATA_SEL);
    tss_flush(TSS_SEL);
}

}
