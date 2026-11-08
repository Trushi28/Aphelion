ENTRY(_start)

PHDRS
{
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text.start) *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code

    . = 0x800000;
    .data : { *(.data .data.*) } :data
    .init_array : { *(.init_array*) *(.ctors*) } :data
    .bss : { *(.bss .bss.* COMMON) } :data

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) *(.gnu*) *(.dynamic) *(.got*) *(.plt*) *(.interp) }
}

ASSERT(SIZEOF(.init_array) == 0, "global constructors are not supported in APX programs")
