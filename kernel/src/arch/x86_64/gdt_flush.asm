bits 64
section .text

global gdt_flush
gdt_flush:
    lgdt [rdi]

    movzx eax, dx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    movzx eax, si
    push rax
    lea rcx, [rel .reload_cs]
    push rcx
    retfq
.reload_cs:
    ret

global tss_flush
tss_flush:
    ltr di
    ret

section .note.GNU-stack noalloc noexec nowrite
