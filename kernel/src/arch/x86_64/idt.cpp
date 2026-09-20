#include <cosmos/idt.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/framebuffer.hpp>
#include <cosmos/cpu.hpp>

extern "C" u64 isr_stub_table[41];

namespace idt {

struct PACKED Gate {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
};
struct PACKED Descriptor { u16 limit; u64 base; };

alignas(16) static Gate g_idt[256];
static Handler g_handlers[256];

static void set_gate(u8 vec, u64 isr, u16 selector, u8 ist, u8 type_attr) {
    g_idt[vec].offset_low  = isr & 0xFFFF;
    g_idt[vec].selector    = selector;
    g_idt[vec].ist         = ist;
    g_idt[vec].type_attr   = type_attr;
    g_idt[vec].offset_mid  = (isr >> 16) & 0xFFFF;
    g_idt[vec].offset_high = static_cast<u32>(isr >> 32);
    g_idt[vec].reserved    = 0;
}

static const char* exception_name(u64 v) {
    static const char* names[32] = {
        "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
        "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
        "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
        "Page Fault", "Reserved", "x87 FP Exception", "Alignment Check",
        "Machine Check", "SIMD FP Exception", "Virtualization Exception",
        "Control Protection Exception", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
        "VMM Communication", "Security Exception", "Reserved"
    };
    return v < 32 ? names[v] : "Unknown";
}

static void default_panic(Frame* f) {
    u64 cr2; asm volatile("mov %%cr2, %0" : "=r"(cr2));
    serial::printf("\n!!! UNHANDLED INTERRUPT: %s (vector %lu, error %lu)\n",
                    exception_name(f->vector), f->vector, f->error_code);
    serial::printf("    rip=%p cs=%lx rflags=%lx rsp=%p cr2=%p\n",
                    f->rip, f->cs, f->rflags, f->rsp, cr2);
    if (fb::ready()) {
        fb::printf(0xFF4040, "\n!!! KERNEL PANIC: %s (vec %d)\n",
                    exception_name(f->vector), static_cast<int>(f->vector));
        fb::printf(0xFF4040, "    rip=%x cr2=%x\n",
                    static_cast<unsigned>(f->rip), static_cast<unsigned>(cr2));
    }
    cpu::hang();
}

extern "C" void isr_dispatch(Frame* f) {
    Handler h = g_handlers[f->vector];
    if (h) h(f);
    else default_panic(f);
}

void set_handler(u8 vector, Handler h) { g_handlers[vector] = h; }

static void load_idtr() {
    Descriptor desc{ sizeof(g_idt) - 1, reinterpret_cast<u64>(g_idt) };
    asm volatile("lidt %0" :: "m"(desc));
}

void init() {
    static bool built = false;
    if (built) { load_idtr(); return; }
    built = true;

    for (auto& h : g_handlers) h = nullptr;
    for (auto& g : g_idt) __builtin_memset(&g, 0, sizeof(g));

    constexpr u8 TYPE_INTR64 = 0x8E;

    for (int v = 0; v < 32; ++v) {
        u8 ist = 0;
        if (v == 8) ist = gdt::IST_DOUBLE_FAULT;
        if (v == 2) ist = gdt::IST_NMI;
        set_gate(static_cast<u8>(v), isr_stub_table[v], gdt::KCODE_SEL, ist, TYPE_INTR64);
    }
    for (int v = 32; v < 40; ++v)
        set_gate(static_cast<u8>(v), isr_stub_table[v], gdt::KCODE_SEL, 0, TYPE_INTR64);
    set_gate(0xFF, isr_stub_table[40], gdt::KCODE_SEL, 0, TYPE_INTR64);

    load_idtr();
}

}
