/* Aphelion linker script. Higher-half kernel per the Limine convention
   (top 2GB of the address space); Limine's kernel_address_request tells us
   at runtime what physical address this actually got loaded at. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_main)

PHDRS
{
    requests PT_LOAD FLAGS((1 << 2) | (1 << 1)) ;
    text     PT_LOAD FLAGS((1 << 2) | (1 << 0)) ;
    rodata   PT_LOAD FLAGS((1 << 2)) ;
    data     PT_LOAD FLAGS((1 << 2) | (1 << 1)) ;
}

SECTIONS
{
    . = 0xffffffff80000000;

    __kernel_start = .;

    .requests_start_marker : { KEEP(*(.requests_start_marker)) } :requests
    .requests               : { KEEP(*(.requests)) }             :requests
    .requests_end_marker    : { KEEP(*(.requests_end_marker)) }  :requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : { *(.text .text.*) } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : { *(.data .data.*) } :data

    .bss : { *(COMMON) *(.bss .bss.*) } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.note .note.*) *(.comment) }
}
