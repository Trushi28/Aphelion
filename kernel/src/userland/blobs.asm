bits 64
default rel

%define SYS_EXIT 0
%define SYS_YIELD 1
%define SYS_GETTID 2
%define SYS_DEBUG_WRITE 3
%define SYS_SET_GS_BASE 4
%define SYS_GET_GS_BASE 5
%define SYS_SET_FS_BASE 6
%define SYS_GET_FS_BASE 7
%define SYS_THREAD_CREATE 8
%define SYS_THREAD_EXIT 9
%define SYS_THREAD_JOIN 10
%define SYS_HANDLE_CLOSE 11
%define SYS_HANDLE_INFO 12
%define SYS_HANDLE_NARROW 13
%define SYS_CHANNEL_CREATE 14
%define SYS_CHANNEL_SEND 15
%define SYS_CHANNEL_RECV 16
%define SYS_HANDLE_WAIT 17
%define SYS_VMO_CREATE 18
%define SYS_VM_MAP 19
%define SYS_VM_UNMAP 20
%define SYS_VM_PROTECT 21
%define SYS_VMO_SIZE 22
%define SYS_FILE_OPEN 23
%define SYS_FILE_READ 24
%define SYS_FILE_WRITE 25
%define SYS_FILE_STAT 26
%define SYS_FILE_CREATE 27
%define SYS_DIR_ENTRY 28
%define SYS_PIPE_CREATE 29
%define SYS_PROCESS_CREATE 30
%define SYS_PROCESS_STATUS 31
%define SYS_PROCESS_KILL 32
%define INFO_PROC 0x3030103
%define INFO_END 0x1030103
%define INFO_VMO 0x2030303
%define INFO_VMO_RO 0x2030001
%define INFO_VMO_RO_DELEGATE 0x2030101
%define WIN 0x100000000000
%define DATA 0x800000
%define STK0 0x801100

%macro EXIT 1
    mov edi, %1
    xor eax, eax
    syscall
    ud2
%endmacro

%macro SYSC 1
    mov eax, %1
    syscall
%endmacro

%macro EXPECT 3
    mov r9, %2
    cmp %1, r9
    je %%ok
    EXIT %3
%%ok:
%endmacro

%macro FILLX 1
    lea rax, [r12 + %1]
    mov [rsp - 128], rax
    mov [rsp - 120], rax
    movdqu xmm %+ %1, [rsp - 128]
%endmacro

%macro FILLY 1
    lea rax, [r12 + %1]
    mov [rsp - 128], rax
    mov [rsp - 120], rax
    not rax
    mov [rsp - 112], rax
    mov [rsp - 104], rax
    vmovdqu ymm %+ %1, [rsp - 128]
%endmacro

%macro VERIFYX 1
    movdqu [rsp - 128], xmm %+ %1
    lea rax, [r12 + %1]
    cmp rax, [rsp - 128]
    je %%lo
    EXIT (1 + %1)
%%lo:
    cmp rax, [rsp - 120]
    je %%hi
    EXIT (17 + %1)
%%hi:
%endmacro

%macro VERIFYY 1
    vmovdqu [rsp - 128], ymm %+ %1
    lea rax, [r12 + %1]
    cmp rax, [rsp - 128]
    je %%a
    EXIT (1 + %1)
%%a:
    cmp rax, [rsp - 120]
    je %%b
    EXIT (17 + %1)
%%b:
    not rax
    cmp rax, [rsp - 112]
    je %%c
    EXIT (33 + %1)
%%c:
    cmp rax, [rsp - 104]
    je %%d
    EXIT (49 + %1)
%%d:
%endmacro

%macro VECPRELUDE 0
    SYSC SYS_GETTID
    mov r13, rax
    mov r12, rax
    mov r9, 0x9E3779B97F4A7C15
    imul r12, r9
    mov ecx, 0x037F
    mov edx, 0x027F
    test r13b, 1
    cmovnz ecx, edx
    mov r15d, ecx
    mov [rsp - 160], cx
    fldcw [rsp - 160]
    mov r14d, 0x1F80
    mov eax, r13d
    and eax, 3
    shl eax, 13
    or r14d, eax
    mov [rsp - 164], r14d
    ldmxcsr [rsp - 164]
    mov ebp, r13d
    and ebp, 0xFFFF
    add ebp, 0x100000
    mov [rsp - 176], rbp
    fild qword [rsp - 176]
%endmacro

%macro VECSPIN 1
    mov qword [rsp - 8], 40
%%round:
    SYSC SYS_YIELD
    mov ecx, 300000
%%spin:
    dec ecx
    jnz %%spin
    %1
    dec qword [rsp - 8]
    jnz %%round
%endmacro

%macro VECSCALARS 0
    fnstcw [rsp - 160]
    movzx eax, word [rsp - 160]
    EXPECT rax, r15, 80
    stmxcsr [rsp - 164]
    mov eax, [rsp - 164]
    EXPECT rax, r14, 81
%endmacro

%macro VECSSE_CHECK 0
%assign n 0
%rep 16
    VERIFYX n
%assign n n+1
%endrep
    VECSCALARS
%endmacro

%macro VECAVX_CHECK 0
%assign n 0
%rep 16
    VERIFYY n
%assign n n+1
%endrep
    VECSCALARS
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

align 16
BLOB fsbase
    SYSC SYS_GET_FS_BASE
    EXPECT rax, 0, 1
    SYSC SYS_GET_GS_BASE
    EXPECT rax, 0, 2
    mov edi, 0x00800080
    SYSC SYS_SET_FS_BASE
    EXPECT rax, 0, 3
    mov edi, 0x00800040
    SYSC SYS_SET_GS_BASE
    EXPECT rax, 0, 4
    mov rax, 0x1122334455667788
    mov [fs:0], rax
    mov rdx, 0x8877665544332211
    mov [gs:0], rdx
    mov rbx, [fs:0]
    EXPECT rbx, 0x1122334455667788, 5
    mov rbx, [gs:0]
    EXPECT rbx, 0x8877665544332211, 6
    mov ecx, 0x00800080
    mov rbx, [rcx]
    EXPECT rbx, 0x1122334455667788, 7
    mov ecx, 0x00800040
    mov rbx, [rcx]
    EXPECT rbx, 0x8877665544332211, 8
    SYSC SYS_GET_FS_BASE
    EXPECT rax, 0x00800080, 9
    SYSC SYS_GET_GS_BASE
    EXPECT rax, 0x00800040, 10
    mov qword [rsp - 8], 40
.round:
    SYSC SYS_YIELD
    mov ecx, 300000
.spin:
    dec ecx
    jnz .spin
    mov rbx, [fs:0]
    EXPECT rbx, 0x1122334455667788, 11
    mov rbx, [gs:0]
    EXPECT rbx, 0x8877665544332211, 12
    dec qword [rsp - 8]
    jnz .round
    xor edi, edi
    SYSC SYS_SET_FS_BASE
    EXPECT rax, 0, 13
    SYSC SYS_GET_FS_BASE
    EXPECT rax, 0, 14
    SYSC SYS_GET_GS_BASE
    EXPECT rax, 0x00800040, 15
    EXIT 0
ENDBLOB fsbase

align 16
BLOB fsbad
    mov rdi, 0x0000800000000000
    SYSC SYS_SET_FS_BASE
    EXPECT rax, -3, 1
    mov rdi, 0xFFFF800000000000
    SYSC SYS_SET_FS_BASE
    EXPECT rax, -3, 2
    mov rdi, -1
    SYSC SYS_SET_FS_BASE
    EXPECT rax, -3, 3
    mov rdi, 0xFFFF7FFFFFFFFFFF
    SYSC SYS_SET_FS_BASE
    EXPECT rax, -3, 4
    SYSC SYS_GET_FS_BASE
    EXPECT rax, 0, 5
    mov rdi, 0x00007FFFFFFFFFFF
    SYSC SYS_SET_FS_BASE
    EXPECT rax, 0, 6
    SYSC SYS_GET_FS_BASE
    EXPECT rax, 0x00007FFFFFFFFFFF, 7
    SYSC SYS_GET_GS_BASE
    EXPECT rax, 0, 8
    xor edi, edi
    SYSC SYS_SET_FS_BASE
    EXPECT rax, 0, 9
    EXIT 0
ENDBLOB fsbad

align 16
BLOB fsiso
    SYSC SYS_GETTID
    mov r13, rax
    mov r12, rax
    and r12d, 0xFF
    shl r12d, 3
    add r12d, 0x00800000
    lea r14d, [r12 + 0x800]
    mov r15, r13
    not r15
    mov rdi, r12
    SYSC SYS_SET_GS_BASE
    EXPECT rax, 0, 1
    mov rdi, r14
    SYSC SYS_SET_FS_BASE
    EXPECT rax, 0, 2
    mov [gs:0], r13
    mov [fs:0], r15
    mov qword [rsp - 8], 40
.round:
    SYSC SYS_YIELD
    mov ecx, 300000
.spin:
    dec ecx
    jnz .spin
    mov rbx, [gs:0]
    EXPECT rbx, r13, 3
    mov rbx, [fs:0]
    EXPECT rbx, r15, 4
    SYSC SYS_GET_GS_BASE
    EXPECT rax, r12, 5
    SYSC SYS_GET_FS_BASE
    EXPECT rax, r14, 6
    dec qword [rsp - 8]
    jnz .round
    EXIT 0
ENDBLOB fsiso

align 16
BLOB fszero
    mov qword [rsp - 8], 40
.round:
    SYSC SYS_GET_FS_BASE
    EXPECT rax, 0, 1
    SYSC SYS_GET_GS_BASE
    EXPECT rax, 0, 2
    SYSC SYS_YIELD
    mov ecx, 300000
.spin:
    dec ecx
    jnz .spin
    dec qword [rsp - 8]
    jnz .round
    EXIT 0
ENDBLOB fszero

align 16
BLOB vecsse
    VECPRELUDE
%assign n 0
%rep 16
    FILLX n
%assign n n+1
%endrep
    VECSPIN VECSSE_CHECK
    fistp qword [rsp - 176]
    mov rax, [rsp - 176]
    EXPECT rax, rbp, 82
    EXIT 0
ENDBLOB vecsse

align 16
BLOB vecavx
    mov eax, 1
    xor ecx, ecx
    cpuid
    mov eax, ecx
    and eax, 0x18000000
    cmp eax, 0x18000000
    jne .none
    xor ecx, ecx
    xgetbv
    and eax, 6
    cmp eax, 6
    jne .none
    VECPRELUDE
%assign n 0
%rep 16
    FILLY n
%assign n n+1
%endrep
    VECSPIN VECAVX_CHECK
    fistp qword [rsp - 176]
    mov rax, [rsp - 176]
    EXPECT rax, rbp, 82
    EXIT 0
.none:
    EXIT 78
ENDBLOB vecavx

%macro TSPAWNL 3
    lea rdi, [rel %1]
    mov rsi, %2
    xor edx, edx
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jg %%ok
    EXIT %3
%%ok:
%endmacro

%macro TEXIT 1
    mov edi, %1
    mov eax, SYS_THREAD_EXIT
    syscall
    ud2
%endmacro

%macro TREFUSED 3
    mov rdi, %1
    mov rsi, %2
    xor edx, edx
    mov eax, SYS_THREAD_CREATE
    syscall
    cmp rax, -3
    je %%ok
    EXIT %3
%%ok:
%endmacro

%macro TSPAWN 1
    mov rdi, r13
    mov rsi, STK0 + %1 * 0x100
    mov edx, 100 + %1
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jg %%ok
    EXIT 20 + %1
%%ok:
    mov [r15 + 0x10 + %1 * 8], rax
%endmacro

%macro TJOINEXPECT 2
    mov rdi, [r15 + 0x10 + %1 * 8]
    mov eax, SYS_THREAD_JOIN
    syscall
    cmp rax, %2
    je %%ok
    EXIT 30 + %1
%%ok:
%endmacro

%macro TIDMATCH 1
    mov rax, [r15 + 0x100 + %1 * 8]
    cmp rax, [r15 + 0x10 + %1 * 8]
    je %%ok
    EXIT 50 + %1
%%ok:
%endmacro

%macro DIFFER 3
    mov rax, %1
    cmp rax, %2
    jne %%ok
    EXIT %3
%%ok:
%endmacro

%macro RAXZERO 1
    test rax, rax
    jz %%ok
    TEXIT %1
%%ok:
%endmacro

%macro JOINBAD 2
    mov rdi, %1
    mov eax, SYS_THREAD_JOIN
    syscall
    cmp rax, -3
    je %%ok
    EXIT %2
%%ok:
%endmacro

align 16
BLOB threads
    mov r15, DATA
    lea r13, [rel .child]
    SYSC SYS_GETTID
    mov r14, rax
    SYSC SYS_GET_GS_BASE
    test rax, rax
    jnz .bad1
    SYSC SYS_GET_FS_BASE
    test rax, rax
    jnz .bad2
    TREFUSED 0, STK0, 3
    TREFUSED r13, 0, 4
    TREFUSED r13, 0x0000800000000000, 5
    TREFUSED 0xFFFF800000000000, STK0, 6
    TREFUSED r13, 0xFFFFFFFFFFFFFFF0, 7
    TSPAWN 0
    TSPAWN 1
    TSPAWN 2
    TJOINEXPECT 0, 50
    TJOINEXPECT 1, 51
    TJOINEXPECT 2, 52
    TJOINEXPECT 0, 50
    SYSC SYS_GET_GS_BASE
    test rax, rax
    jnz .bad9
    SYSC SYS_GET_FS_BASE
    test rax, rax
    jnz .bad10
    TIDMATCH 0
    TIDMATCH 1
    TIDMATCH 2
    DIFFER [r15 + 0x10], r14, 60
    DIFFER [r15 + 0x18], r14, 61
    DIFFER [r15 + 0x20], r14, 62
    DIFFER [r15 + 0x10], [r15 + 0x18], 63
    DIFFER [r15 + 0x10], [r15 + 0x20], 64
    DIFFER [r15 + 0x18], [r15 + 0x20], 65
    JOINBAD r14, 70
    JOINBAD 0xFFFF, 71
    JOINBAD 0, 72
    JOINBAD 0x100000001, 73
    EXIT 0
.bad1:
    EXIT 1
.bad2:
    EXIT 2
.bad9:
    EXIT 9
.bad10:
    EXIT 10
.child:
    mov r12, rdi
    mov r15, DATA
    SYSC SYS_GET_GS_BASE
    RAXZERO 1001
    SYSC SYS_GET_FS_BASE
    RAXZERO 1002
    lea rdi, [r12 + 0x900000]
    SYSC SYS_SET_GS_BASE
    RAXZERO 1003
    lea rdi, [r12 + 0xA00000]
    SYSC SYS_SET_FS_BASE
    RAXZERO 1004
    mov r13, 60
.spin:
    SYSC SYS_YIELD
    dec r13
    jnz .spin
    SYSC SYS_GET_GS_BASE
    lea rbx, [r12 + 0x900000]
    cmp rax, rbx
    je .gs_ok
    TEXIT 1005
.gs_ok:
    SYSC SYS_GET_FS_BASE
    lea rbx, [r12 + 0xA00000]
    cmp rax, rbx
    je .fs_ok
    TEXIT 1006
.fs_ok:
    SYSC SYS_GETTID
    mov rbx, r12
    sub rbx, 100
    mov [r15 + 0x100 + rbx * 8], rax
    mov edi, r12d
    sub edi, 50
    mov eax, SYS_THREAD_EXIT
    syscall
    ud2
ENDBLOB threads

align 16
BLOB lastexit
    TSPAWNL .child, STK0, 1
    mov rdi, rax
    mov eax, SYS_THREAD_JOIN
    syscall
    cmp rax, 21
    je .joined
    EXIT 2
.joined:
    TEXIT 44
.child:
    TEXIT 21
ENDBLOB lastexit

align 16
BLOB lastchild
    SYSC SYS_GETTID
    mov rbx, rax
    lea rdi, [rel .child]
    mov rsi, STK0
    mov rdx, rbx
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jg .made
    EXIT 1
.made:
    TEXIT 5
.child:
    mov eax, SYS_THREAD_JOIN
    syscall
    cmp rax, 5
    je .gone
    TEXIT 900
.gone:
    TEXIT 66
ENDBLOB lastchild

align 16
BLOB groupexit
    TSPAWNL .yielder, STK0, 1
    TSPAWNL .busy, STK0 + 0x100, 2
    mov r13, 300
.wait:
    SYSC SYS_YIELD
    dec r13
    jnz .wait
    EXIT 33
.yielder:
    SYSC SYS_YIELD
    jmp .yielder
.busy:
    jmp .busy
ENDBLOB groupexit

align 16
BLOB threadfault
    TSPAWNL .crash, STK0, 1
.loop:
    SYSC SYS_YIELD
    jmp .loop
.crash:
    ud2
ENDBLOB threadfault

align 16
BLOB spinner
    TSPAWNL .yielder, STK0, 1
    TSPAWNL .busy, STK0 + 0x100, 2
.main:
    jmp .main
.yielder:
    SYSC SYS_YIELD
    jmp .yielder
.busy:
    jmp .busy
ENDBLOB spinner

align 16
BLOB joinhang
    TSPAWNL .busy, STK0, 1
    mov rdi, rax
    mov eax, SYS_THREAD_JOIN
    syscall
    EXIT 2
.busy:
    jmp .busy
ENDBLOB joinhang

align 16
BLOB threadlimit
    mov r15, DATA
    lea r13, [rel .waiter]
    xor r12d, r12d
.fill:
    mov rdi, r13
    mov rsi, r12
    shl rsi, 8
    add rsi, STK0
    xor edx, edx
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jle .filled
    mov [r15 + 0x10 + r12 * 8], rax
    inc r12
    cmp r12, 12
    jb .fill
    EXIT 1
.filled:
    cmp rax, -4
    je .refused
    EXIT 2
.refused:
    cmp r12, 7
    je .seven
    EXIT 3
.seven:
    mov qword [r15], 1
    xor ebx, ebx
.joinall:
    mov rdi, [r15 + 0x10 + rbx * 8]
    mov eax, SYS_THREAD_JOIN
    syscall
    cmp rax, 7
    je .joined
    EXIT 4
.joined:
    inc rbx
    cmp rbx, 7
    jb .joinall
    xor r12d, r12d
.more:
    mov rdi, r13
    mov rsi, STK0
    xor edx, edx
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jle .exhausted
    mov rdi, rax
    mov eax, SYS_THREAD_JOIN
    syscall
    cmp rax, 7
    je .again
    EXIT 5
.again:
    inc r12
    cmp r12, 20
    jb .more
    EXIT 6
.exhausted:
    cmp rax, -4
    je .limit
    EXIT 7
.limit:
    cmp r12, 8
    je .done
    EXIT 8
.done:
    EXIT 0
.waiter:
    mov rax, DATA
    cmp qword [rax], 0
    jne .go
    SYSC SYS_YIELD
    jmp .waiter
.go:
    TEXIT 7
ENDBLOB threadlimit

%macro SC 7
    mov rdi, %2
    mov rsi, %3
    mov rdx, %4
    mov r10, %5
    mov r8, %6
    mov r9, %7
    mov eax, %1
    syscall
%endmacro

%macro MEM32 3
    cmp dword [r15 + %1], %2
    je %%ok
    EXIT %3
%%ok:
%endmacro

%macro MEM8 3
    cmp byte [r15 + %1], %2
    je %%ok
    EXIT %3
%%ok:
%endmacro

align 16
BLOB chanecho
    mov r15, DATA
    mov r14, rdi
    SC SYS_CHANNEL_RECV, r14, DATA, 256, DATA+0x200, 4, 1
    EXPECT rax, 0x100000005, 1
    MEM32 0, 0x72726163, 2
    MEM8 4, 'y', 3
    mov r12, [r15 + 0x200]
    SC SYS_HANDLE_INFO, r12, 0, 0, 0, 0, 0
    EXPECT rax, INFO_END, 4
    mov dword [r15 + 0x100], 0x21616976
    SC SYS_CHANNEL_SEND, r12, DATA+0x100, 4, 0, 0, 0
    EXPECT rax, 0, 5
    mov dword [r15 + 0x108], 0x6B636162
    mov [r15 + 0x210], r12
    SC SYS_CHANNEL_SEND, r14, DATA+0x108, 4, DATA+0x210, 1, 0
    EXPECT rax, 0, 6
    SC SYS_HANDLE_INFO, r12, 0, 0, 0, 0, 0
    EXPECT rax, -8, 7
    SC SYS_CHANNEL_SEND, r14, DATA+0x108, 4, DATA+0x210, 1, 0
    EXPECT rax, -8, 8
    EXIT 0
ENDBLOB chanecho

align 16
BLOB chanwait
    mov r15, DATA
    mov r14, rdi
    lea rdi, [rel .waiter]
    mov rsi, STK0
    mov rdx, r14
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jg .made
    EXIT 1
.made:
    SC SYS_CHANNEL_RECV, r14, DATA, 256, 0, 0, 1
    EXIT 2
.waiter:
    mov r15, DATA
    mov [r15 + 0x400], rdi
    mov dword [r15 + 0x408], 1
    mov dword [r15 + 0x40C], 0
    SC SYS_HANDLE_WAIT, DATA+0x400, 1, -1, 0, 0, 0
    EXIT 3
ENDBLOB chanwait

align 16
BLOB peer_a
    mov r15, DATA
    mov r14, rdi
    SC SYS_CHANNEL_RECV, r14, DATA, 256, DATA+0x200, 4, 1
    EXPECT rax, 0x100000002, 1
    mov r12, [r15 + 0x200]
    mov dword [r15 + 0x100], 0x676E6970
    SC SYS_CHANNEL_SEND, r12, DATA+0x100, 4, 0, 0, 0
    EXPECT rax, 0, 2
    SC SYS_CHANNEL_RECV, r12, DATA+0x120, 64, 0, 0, 1
    EXPECT rax, 4, 3
    MEM32 0x120, 0x676E6F70, 4
    EXIT 0
ENDBLOB peer_a

align 16
BLOB peer_b
    mov r15, DATA
    mov r14, rdi
    SC SYS_CHANNEL_RECV, r14, DATA, 256, DATA+0x200, 4, 1
    EXPECT rax, 0x100000002, 1
    mov r12, [r15 + 0x200]
    SC SYS_CHANNEL_RECV, r12, DATA+0x120, 64, 0, 0, 1
    EXPECT rax, 4, 2
    MEM32 0x120, 0x676E6970, 3
    mov dword [r15 + 0x100], 0x676E6F70
    SC SYS_CHANNEL_SEND, r12, DATA+0x100, 4, 0, 0, 0
    EXPECT rax, 0, 4
    EXIT 0
ENDBLOB peer_b

align 16
BLOB chanlimits
    mov r15, DATA
    SC SYS_HANDLE_INFO, 0, 0, 0, 0, 0, 0
    EXPECT rax, -8, 1
    SC SYS_HANDLE_INFO, 0x7777777700000099, 0, 0, 0, 0, 0
    EXPECT rax, -8, 2
    SC SYS_CHANNEL_CREATE, 0xFFFF800000000000, 0, 0, 0, 0, 0
    EXPECT rax, -2, 3
    SC SYS_CHANNEL_CREATE, DATA+0x300, 0, 0, 0, 0, 0
    EXPECT rax, 0, 4
    mov r14, [r15 + 0x300]
    mov r13, [r15 + 0x308]
    SC SYS_HANDLE_INFO, r14, 0, 0, 0, 0, 0
    EXPECT rax, INFO_END, 5
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 257, 0, 0, 0
    EXPECT rax, -9, 6
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 0, DATA+0x210, 5, 0
    EXPECT rax, -9, 7
    mov [r15 + 0x210], r14
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 1, DATA+0x210, 1, 0
    EXPECT rax, -3, 8
    mov [r15 + 0x210], r13
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 1, DATA+0x210, 1, 0
    EXPECT rax, -3, 9
    SC SYS_HANDLE_NARROW, r14, 1, DATA+0x310, 0, 0, 0
    EXPECT rax, 0, 10
    mov rbx, [r15 + 0x310]
    SC SYS_CHANNEL_SEND, rbx, DATA+0x500, 1, 0, 0, 0
    EXPECT rax, -7, 11
    SC SYS_HANDLE_NARROW, r13, 3, DATA+0x318, 0, 0, 0
    EXPECT rax, 0, 12
    mov rbp, [r15 + 0x318]
    mov [r15 + 0x210], rbp
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 1, DATA+0x210, 1, 0
    EXPECT rax, -7, 13
    SC SYS_HANDLE_INFO, rbp, 0, 0, 0, 0, 0
    EXPECT rax, 0x1030003, 14
    SC SYS_HANDLE_CLOSE, rbx, 0, 0, 0, 0, 0
    EXPECT rax, 0, 15
    SC SYS_HANDLE_CLOSE, rbx, 0, 0, 0, 0, 0
    EXPECT rax, -8, 16
    SC SYS_HANDLE_CLOSE, rbp, 0, 0, 0, 0, 0
    EXPECT rax, 0, 17
    SC SYS_CHANNEL_RECV, r13, DATA, 256, 0, 0, 0
    EXPECT rax, -5, 18
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 100, 0, 0, 0
    EXPECT rax, 0, 19
    SC SYS_CHANNEL_RECV, r13, DATA, 8, 0, 0, 0
    EXPECT rax, -9, 20
    SC SYS_CHANNEL_RECV, r13, DATA, 256, 0, 0, 0
    EXPECT rax, 100, 21
    SC SYS_CHANNEL_SEND, r14, 0xFFFF800000000000, 4, 0, 0, 0
    EXPECT rax, -2, 22
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 5, 0, 0, 0
    EXPECT rax, 0, 23
    SC SYS_CHANNEL_RECV, r13, 0xFFFF800000000000, 8, 0, 0, 0
    EXPECT rax, -2, 24
    SC SYS_CHANNEL_RECV, r13, DATA, 256, 0, 0, 0
    EXPECT rax, 5, 25
    SC SYS_CHANNEL_CREATE, DATA+0x340, 0, 0, 0, 0, 0
    EXPECT rax, 0, 26
    mov rbx, [r15 + 0x340]
    mov [r15 + 0x210], rbx
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 3, DATA+0x210, 1, 0
    EXPECT rax, 0, 27
    SC SYS_CHANNEL_RECV, r13, DATA, 256, 0xFFFF800000000000, 4, 0
    EXPECT rax, -2, 28
    SC SYS_CHANNEL_RECV, r13, DATA, 256, DATA+0x220, 4, 0
    EXPECT rax, 0x100000003, 29
    mov rbx, [r15 + 0x220]
    SC SYS_CHANNEL_SEND, rbx, DATA+0x500, 2, 0, 0, 0
    EXPECT rax, 0, 30
    mov rbx, [r15 + 0x348]
    SC SYS_CHANNEL_RECV, rbx, DATA, 256, 0, 0, 0
    EXPECT rax, 2, 31
    mov rbx, [r15 + 0x340]
    SC SYS_HANDLE_INFO, rbx, 0, 0, 0, 0, 0
    EXPECT rax, -8, 32
    xor r12d, r12d
.fill:
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 1, 0, 0, 0
    EXPECT rax, 0, 33
    inc r12
    cmp r12, 16
    jb .fill
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 1, 0, 0, 0
    EXPECT rax, -5, 34
    xor r12d, r12d
.drain:
    SC SYS_CHANNEL_RECV, r13, DATA, 256, 0, 0, 0
    EXPECT rax, 1, 35
    inc r12
    cmp r12, 16
    jb .drain
    SC SYS_CHANNEL_RECV, r13, DATA, 256, 0, 0, 0
    EXPECT rax, -5, 36
    mov [r15 + 0x400], r13
    mov dword [r15 + 0x408], 1
    mov dword [r15 + 0x40C], 0
    SC SYS_HANDLE_WAIT, DATA+0x400, 1, 0, 0, 0, 0
    EXPECT rax, -10, 37
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 1, 0, 0, 0
    EXPECT rax, 0, 38
    SC SYS_HANDLE_WAIT, DATA+0x400, 1, 0, 0, 0, 0
    EXPECT rax, 1, 39
    cmp dword [r15 + 0x40C], 1
    je .got
    EXIT 40
.got:
    mov dword [r15 + 0x408], 0
    SC SYS_HANDLE_WAIT, DATA+0x400, 1, 0, 0, 0, 0
    EXPECT rax, -3, 41
    SC SYS_HANDLE_WAIT, DATA+0x400, 0, 0, 0, 0, 0
    EXPECT rax, -3, 42
    SC SYS_HANDLE_WAIT, DATA+0x400, 33, 0, 0, 0, 0
    EXPECT rax, -3, 43
    SC SYS_HANDLE_WAIT, 0xFFFF800000000000, 1, 0, 0, 0, 0
    EXPECT rax, -2, 44
    SC SYS_CHANNEL_RECV, r13, DATA, 256, 0, 0, 0
    EXPECT rax, 1, 45
    xor r12d, r12d
.make:
    mov rbx, r12
    shl rbx, 4
    add rbx, DATA+0x800
    SC SYS_CHANNEL_CREATE, rbx, 0, 0, 0, 0, 0
    cmp rax, -4
    je .full
    test rax, rax
    jz .made
    EXIT 46
.made:
    inc r12
    cmp r12, 64
    jb .make
    EXIT 47
.full:
    cmp r12, 40
    jae .enough
    EXIT 48
.enough:
    xor ebp, ebp
.release:
    mov rbx, rbp
    shl rbx, 4
    mov rbx, [r15 + rbx + 0x800]
    SC SYS_HANDLE_CLOSE, rbx, 0, 0, 0, 0, 0
    EXPECT rax, 0, 49
    mov rbx, rbp
    shl rbx, 4
    mov rbx, [r15 + rbx + 0x808]
    SC SYS_HANDLE_CLOSE, rbx, 0, 0, 0, 0, 0
    EXPECT rax, 0, 50
    inc rbp
    cmp rbp, r12
    jb .release
    SC SYS_CHANNEL_CREATE, DATA+0x300, 0, 0, 0, 0, 0
    EXPECT rax, 0, 51
    mov r14, [r15 + 0x300]
    mov r13, [r15 + 0x308]
    SC SYS_HANDLE_CLOSE, r13, 0, 0, 0, 0, 0
    EXPECT rax, 0, 52
    SC SYS_CHANNEL_SEND, r14, DATA+0x500, 1, 0, 0, 0
    EXPECT rax, -6, 53
    SC SYS_CHANNEL_RECV, r14, DATA, 256, 0, 0, 0
    EXPECT rax, -6, 54
    EXIT 0
ENDBLOB chanlimits

%macro VMOMK 3
    SC SYS_VMO_CREATE, %1, 0, DATA+0x300, 0, 0, 0
    EXPECT rax, 0, %3
    mov %2, [r15 + 0x300]
%endmacro

align 16
BLOB vmbasic
    mov r15, DATA
    SC SYS_VMO_CREATE, 0, 0, DATA+0x300, 0, 0, 0
    EXPECT rax, -3, 1
    SC SYS_VMO_CREATE, 4097, 0, DATA+0x300, 0, 0, 0
    EXPECT rax, -9, 2
    SC SYS_VMO_CREATE, 4, 1, DATA+0x300, 0, 0, 0
    EXPECT rax, -3, 3
    SC SYS_VMO_CREATE, 4, 0, 0xFFFF800000000000, 0, 0, 0
    EXPECT rax, -2, 4
    VMOMK 4, r14, 5
    SC SYS_HANDLE_INFO, r14, 0, 0, 0, 0, 0
    EXPECT rax, INFO_VMO, 6
    SC SYS_VMO_SIZE, r14, 0, 0, 0, 0, 0
    EXPECT rax, 4, 7
    SC SYS_VMO_SIZE, 0x7777777700000099, 0, 0, 0, 0, 0
    EXPECT rax, -8, 8
    SC SYS_VM_MAP, r14, 0, 0, 4, 3, 0
    EXPECT rax, WIN, 9
    mov r13, rax
    mov rbx, 0x1122334455667788
    mov [r13], rbx
    mov [r13 + 0x3FF8], rbx
    mov rax, [r13]
    cmp rax, rbx
    je .a
    EXIT 10
.a:
    SC SYS_VM_MAP, r14, 0, 0, 4, 1, 0
    EXPECT rax, WIN+0x4000, 11
    mov r12, rax
    mov rax, [r12 + 0x3FF8]
    cmp rax, rbx
    je .b
    EXIT 12
.b:
    mov qword [r13 + 0x1000], 0x5A5A5A5A
    cmp qword [r12 + 0x1000], 0x5A5A5A5A
    je .c
    EXIT 13
.c:
    SC SYS_VM_MAP, r14, WIN+0x1000, 0, 1, 1, 0
    EXPECT rax, -12, 14
    SC SYS_VM_MAP, r14, 0, 0, 1, 7, 0
    EXPECT rax, -7, 15
    SC SYS_VM_MAP, r14, 0, 0, 1, 2, 0
    EXPECT rax, -3, 16
    SC SYS_VM_MAP, r14, 0, 0, 1, 0, 0
    EXPECT rax, -3, 17
    SC SYS_VM_MAP, r14, 0, 0, 1, 8, 0
    EXPECT rax, -3, 18
    SC SYS_VM_MAP, r14, 0, 3, 2, 1, 0
    EXPECT rax, -3, 19
    SC SYS_VM_MAP, r14, 0, 0, 0, 1, 0
    EXPECT rax, -3, 20
    SC SYS_VM_MAP, r14, 0x400000, 0, 1, 1, 0
    EXPECT rax, -3, 21
    SC SYS_VM_MAP, r14, WIN+0x20001, 0, 1, 1, 0
    EXPECT rax, -3, 22
    SC SYS_VM_MAP, r14, 0x7F0000000000, 0, 1, 1, 0
    EXPECT rax, -3, 23
    SC SYS_VM_MAP, 0x7777777700000099, 0, 0, 1, 1, 0
    EXPECT rax, -8, 24
    SC SYS_VM_MAP, 0, 0, 0, 1, 1, 0
    EXPECT rax, -8, 25
    SC SYS_VM_MAP, r14, WIN+0x20000, 0, 1, 1, 0
    EXPECT rax, WIN+0x20000, 26
    SC SYS_VM_UNMAP, WIN+0x20000, 1, 0, 0, 0, 0
    EXPECT rax, 0, 27
    SC SYS_CHANNEL_CREATE, DATA+0x320, 0, 0, 0, 0, 0
    EXPECT rax, 0, 28
    mov rbx, [r15 + 0x320]
    SC SYS_VM_MAP, rbx, 0, 0, 1, 1, 0
    EXPECT rax, -3, 29
    SC SYS_VMO_SIZE, rbx, 0, 0, 0, 0, 0
    EXPECT rax, -3, 30
    SC SYS_HANDLE_NARROW, r14, 1, DATA+0x330, 0, 0, 0
    EXPECT rax, 0, 31
    mov rbx, [r15 + 0x330]
    SC SYS_HANDLE_INFO, rbx, 0, 0, 0, 0, 0
    EXPECT rax, INFO_VMO_RO, 32
    SC SYS_VM_MAP, rbx, 0, 0, 1, 3, 0
    EXPECT rax, -7, 33
    SC SYS_VM_MAP, rbx, 0, 0, 1, 5, 0
    EXPECT rax, -7, 34
    SC SYS_VM_MAP, rbx, 0, 0, 1, 1, 0
    EXPECT rax, WIN+0x8000, 35
    mov rbp, rax
    SC SYS_VM_PROTECT, rbp, 1, 3, 0, 0, 0
    EXPECT rax, -7, 36
    SC SYS_VM_PROTECT, rbp, 1, 1, 0, 0, 0
    EXPECT rax, 0, 37
    SC SYS_VM_PROTECT, rbp, 2, 1, 0, 0, 0
    EXPECT rax, -13, 38
    SC SYS_VM_PROTECT, r13, 4, 7, 0, 0, 0
    EXPECT rax, -7, 39
    SC SYS_VM_PROTECT, r13, 4, 2, 0, 0, 0
    EXPECT rax, -3, 40
    SC SYS_VM_PROTECT, r13, 0, 1, 0, 0, 0
    EXPECT rax, -3, 41
    mov byte [r13], 0xC3
    cmp byte [r12], 0xC3
    je .d
    EXIT 42
.d:
    SC SYS_VM_PROTECT, r13, 1, 5, 0, 0, 0
    EXPECT rax, 0, 43
    call r13
    SC SYS_VM_PROTECT, r13, 1, 3, 0, 0, 0
    EXPECT rax, 0, 44
    mov byte [r13], 0x90
    cmp byte [r12], 0x90
    je .e
    EXIT 45
.e:
    lea rbx, [r13 + 0x1000]
    SC SYS_VM_UNMAP, rbx, 1, 0, 0, 0, 0
    EXPECT rax, 0, 46
    mov rax, [r13]
    mov rax, [r13 + 0x2000]
    SC SYS_VM_UNMAP, rbx, 1, 0, 0, 0, 0
    EXPECT rax, -13, 47
    SC SYS_VM_PROTECT, r13, 4, 1, 0, 0, 0
    EXPECT rax, -13, 48
    SC SYS_VM_UNMAP, r13, 4, 0, 0, 0, 0
    EXPECT rax, -13, 49
    mov rax, [r13 + 0x3FF8]
    SC SYS_VM_UNMAP, r13, 1, 0, 0, 0, 0
    EXPECT rax, 0, 50
    lea rbx, [r13 + 0x2000]
    SC SYS_VM_UNMAP, rbx, 2, 0, 0, 0, 0
    EXPECT rax, 0, 51
    cmp qword [r12 + 0x1000], 0x5A5A5A5A
    je .f
    EXIT 52
.f:
    SC SYS_HANDLE_CLOSE, r14, 0, 0, 0, 0, 0
    EXPECT rax, 0, 53
    SC SYS_VMO_SIZE, r14, 0, 0, 0, 0, 0
    EXPECT rax, -8, 54
    mov rbx, [r15 + 0x330]
    SC SYS_HANDLE_CLOSE, rbx, 0, 0, 0, 0, 0
    EXPECT rax, 0, 60
    cmp qword [r12 + 0x1000], 0x5A5A5A5A
    je .g
    EXIT 55
.g:
    SC SYS_VM_UNMAP, r12, 4, 0, 0, 0, 0
    EXPECT rax, 0, 56
    SC SYS_VM_UNMAP, rbp, 1, 0, 0, 0, 0
    EXPECT rax, 0, 57
    VMOMK 2, r14, 58
    SC SYS_VM_MAP, r14, 0, 0, 2, 3, 0
    EXPECT rax, WIN, 59
    mov qword [rax + 0x1FF8], 0x77
    EXIT 0
ENDBLOB vmbasic

align 16
BLOB vmnx
    mov r15, DATA
    VMOMK 1, r14, 1
    SC SYS_VM_MAP, r14, 0, 0, 1, 3, 0
    EXPECT rax, WIN, 2
    mov r13, rax
    mov byte [r13], 0xC3
    call r13
    EXIT 3
ENDBLOB vmnx

align 16
BLOB vmwro
    mov r15, DATA
    VMOMK 1, r14, 1
    SC SYS_VM_MAP, r14, 0, 0, 1, 1, 0
    EXPECT rax, WIN, 2
    mov r13, rax
    mov rax, [r13]
    mov byte [r13], 1
    EXIT 3
ENDBLOB vmwro

align 16
BLOB vmwrx
    mov r15, DATA
    VMOMK 1, r14, 1
    SC SYS_VM_MAP, r14, 0, 0, 1, 3, 0
    EXPECT rax, WIN, 2
    mov r13, rax
    mov byte [r13], 0xC3
    SC SYS_VM_PROTECT, r13, 1, 5, 0, 0, 0
    EXPECT rax, 0, 3
    call r13
    mov byte [r13], 0x90
    EXIT 4
ENDBLOB vmwrx

align 16
BLOB vmgone
    mov r15, DATA
    VMOMK 2, r14, 1
    SC SYS_VM_MAP, r14, 0, 0, 2, 3, 0
    EXPECT rax, WIN, 2
    mov r13, rax
    mov qword [r13 + 0x1000], 0x42
    SC SYS_VM_UNMAP, r13, 2, 0, 0, 0, 0
    EXPECT rax, 0, 3
    mov rax, [r13 + 0x1000]
    EXIT 4
ENDBLOB vmgone

align 16
BLOB vmlimits
    mov r15, DATA
    VMOMK 1, r14, 1
    xor r12d, r12d
.fill:
    mov rbx, r12
    shl rbx, 13
    mov rax, WIN
    add rbx, rax
    SC SYS_VM_MAP, r14, rbx, 0, 1, 1, 0
    cmp rax, -4
    je .full
    cmp rax, rbx
    je .placed
    EXIT 2
.placed:
    inc r12
    cmp r12, 100
    jb .fill
    EXIT 3
.full:
    cmp r12, 64
    je .exact
    EXIT 4
.exact:
    mov rbx, WIN
    SC SYS_VM_UNMAP, rbx, 1, 0, 0, 0, 0
    EXPECT rax, 0, 5
    mov rbx, WIN
    SC SYS_VM_MAP, r14, rbx, 0, 1, 1, 0
    EXPECT rax, WIN, 6
    xor r12d, r12d
.drain:
    mov rbx, r12
    shl rbx, 13
    mov rax, WIN
    add rbx, rax
    SC SYS_VM_UNMAP, rbx, 1, 0, 0, 0, 0
    EXPECT rax, 0, 7
    inc r12
    cmp r12, 64
    jb .drain
    SC SYS_VM_UNMAP, WIN, 1, 0, 0, 0, 0
    EXPECT rax, -13, 8
    VMOMK 4096, r13, 9
    SC SYS_VM_MAP, r13, 0, 0, 4096, 3, 0
    EXPECT rax, WIN, 10
    mov rbp, rax
    mov rbx, 0x600D600D
    mov [rbp + 0xFFFFF8], rbx
    VMOMK 4096, r12, 11
    SC SYS_VM_MAP, r12, 0, 0, 4096, 1, 0
    EXPECT rax, WIN+0x1000000, 12
    SC SYS_VM_MAP, r14, 0, 0, 1, 1, 0
    EXPECT rax, -4, 13
    SC SYS_VM_UNMAP, rbp, 4096, 0, 0, 0, 0
    EXPECT rax, 0, 14
    SC SYS_VM_MAP, r14, 0, 0, 1, 1, 0
    EXPECT rax, WIN, 15
    EXIT 0
ENDBLOB vmlimits

align 16
BLOB vmstack
    mov r15, DATA
    VMOMK 2, r14, 1
    SC SYS_VM_MAP, r14, 0, 0, 2, 3, 0
    EXPECT rax, WIN, 2
    mov r13, rax
    lea rdi, [rel .thread]
    lea rsi, [r13 + 0x2000 - 16]
    mov rdx, r13
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jg .made
    EXIT 3
.made:
    mov r12, rax
    SC SYS_THREAD_JOIN, r12, 0, 0, 0, 0, 0
    EXPECT rax, 77, 4
    cmp dword [r15 + 0x10], 0x600D
    je .done
    EXIT 5
.done:
    SC SYS_VM_UNMAP, r13, 2, 0, 0, 0, 0
    EXPECT rax, 0, 6
    EXIT 0
.thread:
    mov r15, DATA
    call .sub
    mov dword [r15 + 0x10], 0x600D
    mov edi, 77
    mov eax, SYS_THREAD_EXIT
    syscall
    ud2
.sub:
    push rbx
    push rdi
    pop rdi
    pop rbx
    ret
ENDBLOB vmstack

align 16
BLOB vmshoot
    mov r15, DATA
    VMOMK 1, r14, 1
    SC SYS_VM_MAP, r14, 0, 0, 1, 3, 0
    EXPECT rax, WIN, 2
    mov r13, rax
    mov [r15 + 0x20], r13
    lea rdi, [rel .reader]
    mov rsi, STK0
    xor edx, edx
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jg .made
    EXIT 3
.made:
    mov r12, rax
.wait:
    cmp dword [r15], 1
    je .touched
    SC SYS_YIELD, 0, 0, 0, 0, 0, 0
    jmp .wait
.touched:
    SC SYS_VM_UNMAP, r13, 1, 0, 0, 0, 0
    EXPECT rax, 0, 4
    mov dword [r15 + 4], 1
    SC SYS_THREAD_JOIN, r12, 0, 0, 0, 0, 0
    EXIT 6
.reader:
    mov r15, DATA
    mov r13, [r15 + 0x20]
    mov rax, [r13]
    mov dword [r15], 1
.spin:
    pause
    cmp dword [r15 + 4], 1
    jne .spin
    mov rax, [r13]
    mov edi, 5
    mov eax, SYS_THREAD_EXIT
    syscall
    ud2
ENDBLOB vmshoot

align 16
BLOB vmrace
    mov r15, DATA
    SC SYS_CHANNEL_CREATE, DATA+0x300, 0, 0, 0, 0, 0
    EXPECT rax, 0, 1
    mov r12, [r15 + 0x300]
    VMOMK 1, r14, 2
    lea rdi, [rel .worker]
    mov rsi, STK0
    mov rdx, r12
    mov eax, SYS_THREAD_CREATE
    syscall
    test rax, rax
    jg .made
    EXIT 3
.made:
    mov r13, rax
    xor ebp, ebp
.loop:
    mov rbx, WIN+0x40000
    SC SYS_VM_MAP, r14, rbx, 0, 1, 3, 0
    EXPECT rax, WIN+0x40000, 4
    mov rbx, WIN+0x40000
    SC SYS_VM_UNMAP, rbx, 1, 0, 0, 0, 0
    EXPECT rax, 0, 5
    inc ebp
    cmp ebp, 3000
    jb .loop
.settle:
    cmp dword [r15 + 0x20], 200
    jae .stop
    SC SYS_YIELD, 0, 0, 0, 0, 0, 0
    jmp .settle
.stop:
    mov dword [r15 + 0x10], 1
    SC SYS_THREAD_JOIN, r13, 0, 0, 0, 0, 0
    EXPECT rax, 0, 6
    EXIT 0
.worker:
    mov r15, DATA
    mov r14, rdi
.again:
    mov rbx, WIN+0x40000
    SC SYS_CHANNEL_RECV, r14, rbx, 256, 0, 0, 0
    cmp rax, -5
    je .good
    cmp rax, -2
    je .good
    mov edi, 9
    mov eax, SYS_THREAD_EXIT
    syscall
    ud2
.good:
    inc dword [r15 + 0x20]
    cmp dword [r15 + 0x10], 1
    jne .again
    xor edi, edi
    mov eax, SYS_THREAD_EXIT
    syscall
    ud2
ENDBLOB vmrace

align 16
BLOB vmshare
    mov r15, DATA
    mov r14, rdi
    SC SYS_CHANNEL_RECV, r14, DATA, 256, DATA+0x200, 4, 1
    EXPECT rax, 0x200000003, 1
    mov r12, [r15 + 0x200]
    mov r13, [r15 + 0x208]
    SC SYS_VMO_SIZE, r12, 0, 0, 0, 0, 0
    EXPECT rax, 2, 2
    SC SYS_VM_MAP, r12, 0, 0, 2, 3, 0
    EXPECT rax, WIN, 3
    mov rbp, rax
    mov rbx, 0xA9E110FA9E110FA1
    cmp [rbp], rbx
    je .magic
    EXIT 4
.magic:
    mov rbx, 0x5EED5EED5EED5EED
    mov [rbp + 0x1008], rbx
    SC SYS_VM_MAP, r13, 0, 0, 2, 1, 0
    EXPECT rax, WIN+0x2000, 5
    mov r14, rax
    mov rbx, 0x5EED5EED5EED5EED
    cmp [r14 + 0x1008], rbx
    je .alias
    EXIT 6
.alias:
    SC SYS_VM_MAP, r13, 0, 0, 1, 3, 0
    EXPECT rax, -7, 7
    SC SYS_VM_PROTECT, r14, 2, 3, 0, 0, 0
    EXPECT rax, -7, 8
    SC SYS_HANDLE_INFO, r13, 0, 0, 0, 0, 0
    EXPECT rax, INFO_VMO_RO_DELEGATE, 9
    EXIT 0
ENDBLOB vmshare

align 16
BLOB vmspin
    mov r15, DATA
    VMOMK 16, r14, 1
    SC SYS_VM_MAP, r14, 0, 0, 16, 3, 0
    EXPECT rax, WIN, 2
    mov r13, rax
    SC SYS_HANDLE_CLOSE, r14, 0, 0, 0, 0, 0
    EXPECT rax, 0, 3
    xor eax, eax
.spin:
    mov [r13 + 0xF000], rax
    inc rax
    jmp .spin
ENDBLOB vmspin

align 16
BLOB fileio
    mov r15, DATA
    mov r14, rdi
    mov rax, 'f.txt'
    mov [r15 + 0x100], rax
    mov rax, 'hello'
    mov [r15 + 0x110], rax
    mov rax, 'nope.txt'
    mov [r15 + 0x120], rax
    mov rax, '../f.txt'
    mov [r15 + 0x130], rax
    mov rax, 'sub'
    mov [r15 + 0x140], rax
    mov rax, 'sub/g.tx'
    mov [r15 + 0x150], rax
    mov byte [r15 + 0x158], 't'
    SC SYS_FILE_CREATE, r14, DATA+0x100, 5, 3, 0, DATA+0x300
    EXPECT rax, 0, 1
    mov r13, [r15 + 0x300]
    SC SYS_FILE_WRITE, r13, 0, DATA+0x110, 5, 0, 0
    EXPECT rax, 5, 2
    SC SYS_FILE_READ, r13, 0, DATA+0x180, 16, 0, 0
    EXPECT rax, 5, 3
    MEM32 0x180, 0x6C6C6568, 4
    MEM8 0x184, 'o', 5
    SC SYS_FILE_READ, r13, 2, DATA+0x1A0, 16, 0, 0
    EXPECT rax, 3, 6
    MEM8 0x1A0, 'l', 7
    SC SYS_FILE_READ, r13, 5, DATA+0x1C0, 16, 0, 0
    EXPECT rax, 0, 8
    SC SYS_FILE_READ, r13, 0, DATA+0x1C0, 0, 0, 0
    EXPECT rax, 0, 9
    SC SYS_FILE_READ, r13, 0, 0xFFFF800000000000, 4, 0, 0
    EXPECT rax, -2, 10
    SC SYS_FILE_READ, r13, 0, DATA+0x1C0, 5000, 0, 0
    EXPECT rax, 5, 11
    SC SYS_FILE_STAT, r13, DATA+0x400, 0, 0, 0, 0
    EXPECT rax, 0, 12
    cmp qword [r15 + 0x400], 5
    je .stat_size
    EXIT 13
.stat_size:
    MEM32 0x410, 0, 14
    SC SYS_FILE_STAT, r13, 0xFFFF800000000000, 0, 0, 0, 0
    EXPECT rax, -2, 15
    SC SYS_FILE_STAT, 0x7777777700000099, DATA+0x400, 0, 0, 0, 0
    EXPECT rax, -8, 16
    SC SYS_FILE_CREATE, r14, DATA+0x100, 5, 3, 0, DATA+0x308
    EXPECT rax, -12, 17
    SC SYS_FILE_OPEN, r14, DATA+0x100, 5, 1, DATA+0x310, 0
    EXPECT rax, 0, 18
    mov r12, [r15 + 0x310]
    SC SYS_FILE_WRITE, r12, 0, DATA+0x110, 5, 0, 0
    EXPECT rax, -7, 19
    SC SYS_FILE_OPEN, r14, DATA+0x100, 5, 0x200, DATA+0x318, 0
    EXPECT rax, -7, 20
    SC SYS_FILE_OPEN, r14, DATA+0x120, 8, 1, DATA+0x318, 0
    EXPECT rax, -15, 21
    SC SYS_FILE_OPEN, r14, DATA+0x130, 8, 1, DATA+0x318, 0
    EXPECT rax, -3, 22
    SC SYS_FILE_OPEN, r14, DATA+0x100, 0, 1, DATA+0x318, 0
    EXPECT rax, -3, 23
    SC SYS_FILE_OPEN, r14, DATA+0x100, 256, 1, DATA+0x318, 0
    EXPECT rax, -3, 24
    SC SYS_FILE_OPEN, r14, DATA+0x100, 5, 0x400, DATA+0x318, 0
    EXPECT rax, -3, 25
    SC SYS_FILE_OPEN, r14, 0xFFFF800000000000, 5, 1, DATA+0x318, 0
    EXPECT rax, -2, 26
    SC SYS_FILE_OPEN, r14, DATA+0x100, 5, 1, 0xFFFF800000000000, 0
    EXPECT rax, -2, 27
    SC SYS_FILE_CREATE, r14, DATA+0x140, 3, 0, 1, 0
    EXPECT rax, 0, 28
    SC SYS_FILE_CREATE, r14, DATA+0x150, 9, 3, 0, DATA+0x328
    EXPECT rax, 0, 29
    mov rbx, [r15 + 0x328]
    SC SYS_HANDLE_CLOSE, rbx, 0, 0, 0, 0, 0
    EXPECT rax, 0, 30
    SC SYS_FILE_CREATE, r14, DATA+0x140, 3, 0, 4, 0
    EXPECT rax, -3, 31
    xor ebx, ebx
    xor r12d, r12d
    xor ebp, ebp
.enum:
    SC SYS_DIR_ENTRY, r14, rbx, DATA+0x500, 0, 0, 0
    cmp rax, 1
    je .enum_done
    test rax, rax
    jz .enum_item
    EXIT 32
.enum_item:
    mov eax, [r15 + 0x510]
    add r12, rax
    mov eax, [r15 + 0x514]
    add rbp, rax
    inc rbx
    cmp rbx, 8
    jb .enum
    EXIT 33
.enum_done:
    cmp rbx, 2
    je .enum_count
    EXIT 34
.enum_count:
    cmp r12, 1
    je .enum_kinds
    EXIT 35
.enum_kinds:
    cmp rbp, 8
    je .enum_names
    EXIT 36
.enum_names:
    SC SYS_DIR_ENTRY, r13, 0, DATA+0x500, 0, 0, 0
    EXPECT rax, -16, 37
    SC SYS_DIR_ENTRY, r14, 0, 0xFFFF800000000000, 0, 0, 0
    EXPECT rax, -2, 38
    SC SYS_FILE_STAT, r14, DATA+0x400, 0, 0, 0, 0
    EXPECT rax, 0, 39
    MEM32 0x410, 1, 40
    SC SYS_FILE_READ, r14, 0, DATA+0x1C0, 4, 0, 0
    EXPECT rax, -16, 41
    SC SYS_PIPE_CREATE, 0, DATA+0x600, 0, 0, 0, 0
    EXPECT rax, -3, 42
    SC SYS_PIPE_CREATE, 65537, DATA+0x600, 0, 0, 0, 0
    EXPECT rax, -3, 43
    SC SYS_PIPE_CREATE, 64, 0xFFFF800000000000, 0, 0, 0, 0
    EXPECT rax, -2, 44
    SC SYS_PIPE_CREATE, 64, DATA+0x600, 0, 0, 0, 0
    EXPECT rax, 0, 45
    mov r12, [r15 + 0x600]
    mov rbp, [r15 + 0x608]
    SC SYS_FILE_READ, r12, 0, DATA+0x1C0, 16, 0, 0
    EXPECT rax, -5, 46
    mov dword [r15 + 0x190], 'ping'
    SC SYS_FILE_WRITE, rbp, 0, DATA+0x190, 4, 0, 0
    EXPECT rax, 4, 47
    SC SYS_FILE_WRITE, r12, 0, DATA+0x190, 4, 0, 0
    EXPECT rax, -7, 48
    SC SYS_FILE_READ, rbp, 0, DATA+0x1C0, 4, 0, 0
    EXPECT rax, -7, 49
    mov [r15 + 0x700], r12
    mov dword [r15 + 0x708], 1
    mov dword [r15 + 0x70C], 0
    SC SYS_HANDLE_WAIT, DATA+0x700, 1, 0, 0, 0, 0
    EXPECT rax, 1, 50
    SC SYS_FILE_READ, r12, 0, DATA+0x1C0, 16, 0, 0
    EXPECT rax, 4, 51
    MEM32 0x1C0, 0x676E6970, 52
    SC SYS_FILE_STAT, r12, DATA+0x400, 0, 0, 0, 0
    EXPECT rax, 0, 53
    MEM32 0x410, 2, 54
    SC SYS_HANDLE_CLOSE, rbp, 0, 0, 0, 0, 0
    EXPECT rax, 0, 55
    SC SYS_FILE_READ, r12, 0, DATA+0x1C0, 16, 0, 0
    EXPECT rax, 0, 56
    mov dword [r15 + 0x70C], 0
    SC SYS_HANDLE_WAIT, DATA+0x700, 1, 0, 0, 0, 0
    EXPECT rax, 1, 57
    SC SYS_PIPE_CREATE, 64, DATA+0x610, 0, 0, 0, 0
    EXPECT rax, 0, 58
    mov r12, [r15 + 0x610]
    mov rbp, [r15 + 0x618]
    SC SYS_FILE_WRITE, rbp, 0, DATA+0x800, 100, 0, 0
    EXPECT rax, 64, 59
    SC SYS_FILE_WRITE, rbp, 0, DATA+0x800, 100, 0, 0
    EXPECT rax, -5, 60
    SC SYS_HANDLE_CLOSE, r12, 0, 0, 0, 0, 0
    EXPECT rax, 0, 61
    SC SYS_FILE_WRITE, rbp, 0, DATA+0x800, 1, 0, 0
    EXPECT rax, -6, 62
    SC SYS_FILE_WRITE, rbp, 0, 0xFFFF800000000000, 1, 0, 0
    EXPECT rax, -2, 63
    EXIT 0
ENDBLOB fileio

align 16
BLOB apxchild
    mov r15, DATA
    mov r14, rdi
    cmp dword [r15], 'APXD'
    je .init_ok
    EXIT 1
.init_ok:
    cmp dword [r15 + 8], 0
    je .bss_ok
    EXIT 2
.bss_ok:
    cmp dword [r15 + 0x1000], 0
    je .page_ok
    EXIT 3
.page_ok:
    SC SYS_CHANNEL_SEND, r14, DATA, 4, 0, 0, 0
    EXPECT rax, 0, 4
    EXIT 7
ENDBLOB apxchild

align 16
BLOB apxspin
.again:
    SYSC SYS_YIELD
    jmp .again
ENDBLOB apxspin

align 16
BLOB apxdata
    mov r15, DATA
    cmp byte [r15], 'A'
    je .first
    EXIT 99
.first:
    cmp byte [r15 + 2], 0
    je .rest
    EXIT 98
.rest:
    movzx edi, byte [r15 + 1]
    xor eax, eax
    syscall
    ud2
ENDBLOB apxdata

align 16
BLOB spawner
    mov r15, DATA
    mov r14, rdi
    mov rax, 'child'
    mov [r15 + 0x100], rax
    mov rax, 'spin'
    mov [r15 + 0x110], rax
    mov rax, 'bad'
    mov [r15 + 0x120], rax
    SC SYS_FILE_OPEN, r14, DATA+0x100, 5, 0x201, DATA+0x300, 0
    EXPECT rax, 0, 1
    mov r13, [r15 + 0x300]
    SC SYS_FILE_OPEN, r14, DATA+0x100, 5, 1, DATA+0x308, 0
    EXPECT rax, 0, 2
    mov r12, [r15 + 0x308]
    SC SYS_FILE_OPEN, r14, DATA+0x110, 4, 0x201, DATA+0x310, 0
    EXPECT rax, 0, 3
    mov rbp, [r15 + 0x310]
    SC SYS_FILE_OPEN, r14, DATA+0x120, 3, 0x201, DATA+0x318, 0
    EXPECT rax, 0, 4
    mov rbx, [r15 + 0x318]
    SC SYS_PROCESS_CREATE, r12, 0, DATA+0x330, 0, 0, 0
    EXPECT rax, -7, 5
    SC SYS_PROCESS_CREATE, rbx, 0, DATA+0x330, 0, 0, 0
    EXPECT rax, -14, 6
    SC SYS_PROCESS_CREATE, r14, 0, DATA+0x330, 0, 0, 0
    EXPECT rax, -16, 7
    SC SYS_PROCESS_CREATE, 0x7777777700000099, 0, DATA+0x330, 0, 0, 0
    EXPECT rax, -8, 8
    SC SYS_PROCESS_CREATE, r13, 0x7777777700000099, DATA+0x330, 0, 0, 0
    EXPECT rax, -8, 9
    SC SYS_PROCESS_CREATE, r13, 0, DATA+0x330, 1, 0, 0
    EXPECT rax, -3, 10
    SC SYS_PROCESS_CREATE, r13, 0, 0xFFFF800000000000, 0, 0, 0
    EXPECT rax, -2, 11
    SC SYS_CHANNEL_CREATE, DATA+0x340, 0, 0, 0, 0, 0
    EXPECT rax, 0, 12
    SC SYS_PROCESS_CREATE, r13, [r15 + 0x348], DATA+0x350, 0, 0, 0
    EXPECT rax, 0, 13
    SC SYS_HANDLE_INFO, [r15 + 0x350], 0, 0, 0, 0, 0
    EXPECT rax, INFO_PROC, 14
    SC SYS_HANDLE_INFO, [r15 + 0x348], 0, 0, 0, 0, 0
    EXPECT rax, -8, 15
    SC SYS_CHANNEL_RECV, [r15 + 0x340], DATA+0x400, 64, 0, 0, 1
    EXPECT rax, 4, 16
    MEM32 0x400, 'APXD', 17
    mov rax, [r15 + 0x350]
    mov [r15 + 0x700], rax
    mov dword [r15 + 0x708], 5
    mov dword [r15 + 0x70C], 0
    SC SYS_HANDLE_WAIT, DATA+0x700, 1, -1, 0, 0, 0
    EXPECT rax, 1, 18
    SC SYS_PROCESS_STATUS, [r15 + 0x350], DATA+0x360, 0, 0, 0, 0
    EXPECT rax, 0, 19
    cmp qword [r15 + 0x360], 7
    je .status_ok
    EXIT 20
.status_ok:
    SC SYS_PROCESS_STATUS, [r15 + 0x340], DATA+0x360, 0, 0, 0, 0
    EXPECT rax, -3, 21
    SC SYS_PROCESS_KILL, [r15 + 0x350], 0, 0, 0, 0, 0
    EXPECT rax, -6, 22
    SC SYS_HANDLE_CLOSE, [r15 + 0x350], 0, 0, 0, 0, 0
    EXPECT rax, 0, 23
    SC SYS_PROCESS_CREATE, rbp, 0, DATA+0x358, 0, 0, 0
    EXPECT rax, 0, 24
    SC SYS_PROCESS_STATUS, [r15 + 0x358], DATA+0x360, 0, 0, 0, 0
    EXPECT rax, -5, 25
    SC SYS_HANDLE_NARROW, [r15 + 0x358], 1, DATA+0x368, 0, 0, 0
    EXPECT rax, 0, 26
    SC SYS_PROCESS_KILL, [r15 + 0x368], 0, 0, 0, 0, 0
    EXPECT rax, -7, 27
    SC SYS_PROCESS_KILL, [r15 + 0x358], 0, 0, 0, 0, 0
    EXPECT rax, 0, 28
    mov rax, [r15 + 0x358]
    mov [r15 + 0x700], rax
    mov dword [r15 + 0x708], 5
    mov dword [r15 + 0x70C], 0
    SC SYS_HANDLE_WAIT, DATA+0x700, 1, -1, 0, 0, 0
    EXPECT rax, 1, 29
    SC SYS_PROCESS_STATUS, [r15 + 0x358], DATA+0x360, 0, 0, 0, 0
    EXPECT rax, 0, 30
    mov rax, -512
    cmp [r15 + 0x360], rax
    je .killed_ok
    EXIT 31
.killed_ok:
    SC SYS_PROCESS_KILL, [r15 + 0x358], 0, 0, 0, 0, 0
    EXPECT rax, -6, 32
    SC SYS_PROCESS_CREATE, rbp, 0, DATA+0x370, 0, 0, 0
    EXPECT rax, 0, 33
    SC SYS_PROCESS_CREATE, rbp, 0, DATA+0x378, 0, 0, 0
    EXPECT rax, 0, 34
    EXIT 0
ENDBLOB spawner

section .note.GNU-stack noalloc noexec nowrite
