bits 64
default rel

%define SYS_EXIT 0
%define SYS_YIELD 1
%define SYS_GETTID 2
%define SYS_DEBUG_WRITE 3
%define SYS_SET_GS_BASE 4
%define SYS_GET_GS_BASE 5

%macro EXIT 1
    mov edi, %1
    xor eax, eax
    syscall
    ud2
%endmacro

%macro CHECK 2
    mov rax, %2
    cmp %1, rax
    jne .bad
%endmacro

%macro BLOB 1
global ublob_%1
global ublob_%1_end
ublob_%1:
%endmacro

%macro ENDBLOB 1
ublob_%1_end:
%endmacro

section .rodata
align 16

BLOB hello
    mov eax, 9999
    syscall
    cmp rax, -1
    jne .bad
    mov eax, SYS_GETTID
    syscall
    test rax, rax
    jle .bad
    mov eax, SYS_DEBUG_WRITE
    lea rdi, [rel .msg]
    mov esi, .msg_end - .msg
    syscall
    cmp rax, .msg_end - .msg
    jne .bad
    EXIT 0
.bad:
    EXIT 2
.msg:
    db "hello from ring 3", 10
.msg_end:
ENDBLOB hello

align 16
BLOB exit42
    EXIT 42
ENDBLOB exit42

align 16
BLOB null
    xor eax, eax
    mov rax, [rax]
    ud2
ENDBLOB null

align 16
BLOB readkernel
    mov rax, 0xFFFFFFFF80000000
    mov rax, [rax]
    ud2
ENDBLOB readkernel

align 16
BLOB jumpkernel
    mov rax, 0xFFFFFFFF80000000
    jmp rax
ENDBLOB jumpkernel

align 16
BLOB privileged
    hlt
    ud2
ENDBLOB privileged

align 16
BLOB divzero
    xor edx, edx
    xor ecx, ecx
    mov eax, 1
    div ecx
    ud2
ENDBLOB divzero

align 16
BLOB badptr
    mov eax, SYS_DEBUG_WRITE
    mov rdi, 0xFFFF800000000000
    mov esi, 8
    syscall
    cmp rax, -2
    jne .bad

    mov eax, SYS_DEBUG_WRITE
    mov rdi, 0x00007FFFFFFFFFFC
    mov esi, 8
    syscall
    cmp rax, -2
    jne .bad

    mov eax, SYS_DEBUG_WRITE
    xor edi, edi
    mov esi, 4
    syscall
    cmp rax, -2
    jne .bad

    mov eax, SYS_DEBUG_WRITE
    mov rdi, 0xFFFFFFFFFFFFFFF8
    mov esi, 16
    syscall
    cmp rax, -2
    jne .bad

    mov eax, SYS_DEBUG_WRITE
    lea rdi, [rel ublob_badptr]
    mov rsi, -1
    syscall
    cmp rax, -3
    jne .bad

    mov eax, SYS_DEBUG_WRITE
    mov edi, 0x00001000
    mov esi, 4
    syscall
    cmp rax, -2
    jne .bad

    mov eax, SYS_DEBUG_WRITE
    lea rdi, [rel ublob_badptr]
    xor esi, esi
    syscall
    test rax, rax
    jnz .bad
    EXIT 0
.bad:
    EXIT 1
ENDBLOB badptr

align 16
BLOB registers
    mov qword [rsp - 8], 40
.round:
    mov rbx, 0x1111111111111111
    mov rbp, 0x2222222222222222
    mov rdx, 0x3333333333333333
    mov rsi, 0x4444444444444444
    mov rdi, 0x5555555555555555
    mov r8, 0x6666666666666666
    mov r9, 0x7777777777777777
    mov r10, 0x8888888888888888
    mov r12, 0x9999999999999999
    mov r13, 0xAAAAAAAAAAAAAAAA
    mov r14, 0xBBBBBBBBBBBBBBBB
    mov r15, 0xCCCCCCCCCCCCCCCC
    mov eax, SYS_YIELD
    syscall
    test rax, rax
    jnz .bad
    mov ecx, 300000
.spin:
    dec ecx
    jnz .spin
    CHECK rbx, 0x1111111111111111
    CHECK rbp, 0x2222222222222222
    CHECK rdx, 0x3333333333333333
    CHECK rsi, 0x4444444444444444
    CHECK rdi, 0x5555555555555555
    CHECK r8, 0x6666666666666666
    CHECK r9, 0x7777777777777777
    CHECK r10, 0x8888888888888888
    CHECK r12, 0x9999999999999999
    CHECK r13, 0xAAAAAAAAAAAAAAAA
    CHECK r14, 0xBBBBBBBBBBBBBBBB
    CHECK r15, 0xCCCCCCCCCCCCCCCC
    dec qword [rsp - 8]
    jnz .round
    EXIT 0
.bad:
    EXIT 1
ENDBLOB registers

align 16
BLOB worker
    mov qword [rsp - 8], 3000
.again:
    mov eax, SYS_YIELD
    syscall
    test rax, rax
    jnz .bad
    dec qword [rsp - 8]
    jnz .again
    EXIT 0
.bad:
    EXIT 1
ENDBLOB worker

align 16
BLOB datapage
    mov rax, 0x0000000000800000
    mov qword [rax], 0x1234
    mov rbx, [rax]
    cmp rbx, 0x1234
    jne .bad
    mov byte [rel ublob_datapage], 0
    ud2
.bad:
    EXIT 1
ENDBLOB datapage

align 16
BLOB gsbase
    mov eax, SYS_GET_GS_BASE
    syscall
    test rax, rax
    jnz .bad1
    mov edi, 0x00800040
    mov eax, SYS_SET_GS_BASE
    syscall
    test rax, rax
    jnz .bad2
    mov rax, 0x1122334455667788
    mov [gs:0], rax
    mov rbx, [gs:0]
    cmp rbx, rax
    jne .bad3
    mov ecx, 0x00800040
    mov rdx, [rcx]
    cmp rdx, rax
    jne .bad4
    mov eax, SYS_GET_GS_BASE
    syscall
    cmp rax, 0x00800040
    jne .bad5
    mov eax, SYS_GETTID
    syscall
    mov rbx, [gs:0]
    mov rax, 0x1122334455667788
    cmp rbx, rax
    jne .bad6
    mov qword [rsp - 8], 40
.round:
    mov eax, SYS_YIELD
    syscall
    mov ecx, 300000
.spin:
    dec ecx
    jnz .spin
    mov rbx, [gs:0]
    mov rax, 0x1122334455667788
    cmp rbx, rax
    jne .bad7
    dec qword [rsp - 8]
    jnz .round
    xor edi, edi
    mov eax, SYS_SET_GS_BASE
    syscall
    test rax, rax
    jnz .bad8
    mov eax, SYS_GET_GS_BASE
    syscall
    test rax, rax
    jnz .bad9
    EXIT 0
.bad1:
    EXIT 1
.bad2:
    EXIT 2
.bad3:
    EXIT 3
.bad4:
    EXIT 4
.bad5:
    EXIT 5
.bad6:
    EXIT 6
.bad7:
    EXIT 7
.bad8:
    EXIT 8
.bad9:
    EXIT 9
ENDBLOB gsbase

align 16
BLOB gsbad
    mov rdi, 0x0000800000000000
    mov eax, SYS_SET_GS_BASE
    syscall
    cmp rax, -3
    jne .bad1
    mov rdi, 0xFFFF800000000000
    mov eax, SYS_SET_GS_BASE
    syscall
    cmp rax, -3
    jne .bad2
    mov rdi, -1
    mov eax, SYS_SET_GS_BASE
    syscall
    cmp rax, -3
    jne .bad3
    mov rdi, 0xFFFF7FFFFFFFFFFF
    mov eax, SYS_SET_GS_BASE
    syscall
    cmp rax, -3
    jne .bad4
    mov eax, SYS_GET_GS_BASE
    syscall
    test rax, rax
    jnz .bad5
    mov rdi, 0x00007FFFFFFFFFFF
    mov eax, SYS_SET_GS_BASE
    syscall
    test rax, rax
    jnz .bad6
    mov eax, SYS_GET_GS_BASE
    syscall
    mov rcx, 0x00007FFFFFFFFFFF
    cmp rax, rcx
    jne .bad7
    xor edi, edi
    mov eax, SYS_SET_GS_BASE
    syscall
    test rax, rax
    jnz .bad8
    mov eax, SYS_GET_GS_BASE
    syscall
    test rax, rax
    jnz .bad9
    EXIT 0
.bad1:
    EXIT 1
.bad2:
    EXIT 2
.bad3:
    EXIT 3
.bad4:
    EXIT 4
.bad5:
    EXIT 5
.bad6:
    EXIT 6
.bad7:
    EXIT 7
.bad8:
    EXIT 8
.bad9:
    EXIT 9
ENDBLOB gsbad

align 16
BLOB gsiso
    mov eax, SYS_GETTID
    syscall
    mov r13, rax
    mov r12, rax
    and r12d, 0xFF
    shl r12d, 3
    add r12d, 0x00800000
    mov rdi, r12
    mov eax, SYS_SET_GS_BASE
    syscall
    test rax, rax
    jnz .bad1
    mov [gs:0], r13
    mov qword [rsp - 8], 40
.round:
    mov eax, SYS_YIELD
    syscall
    mov ecx, 300000
.spin:
    dec ecx
    jnz .spin
    mov rbx, [gs:0]
    cmp rbx, r13
    jne .bad2
    mov eax, SYS_GET_GS_BASE
    syscall
    cmp rax, r12
    jne .bad3
    dec qword [rsp - 8]
    jnz .round
    EXIT 0
.bad1:
    EXIT 1
.bad2:
    EXIT 2
.bad3:
    EXIT 3
ENDBLOB gsiso

align 16
BLOB gszero
    mov qword [rsp - 8], 40
.round:
    mov eax, SYS_GET_GS_BASE
    syscall
    test rax, rax
    jnz .bad1
    mov eax, SYS_YIELD
    syscall
    mov ecx, 300000
.spin:
    dec ecx
    jnz .spin
    dec qword [rsp - 8]
    jnz .round
    EXIT 0
.bad1:
    EXIT 1
ENDBLOB gszero

align 16
BLOB nmispin
    mov rbx, 0x1111111111111111
    mov rbp, 0x2222222222222222
    mov r12, 0x9999999999999999
    mov r13, 0xAAAAAAAAAAAAAAAA
    mov r14, 0x0000000000000000
    mov r15, 80000000
.spin:
    inc r14
    dec r15
    jnz .spin
    cmp r14, 80000000
    jne .bad
    CHECK rbx, 0x1111111111111111
    CHECK rbp, 0x2222222222222222
    CHECK r12, 0x9999999999999999
    CHECK r13, 0xAAAAAAAAAAAAAAAA
    EXIT 0
.bad:
    EXIT 1
ENDBLOB nmispin

section .note.GNU-stack noalloc noexec nowrite
