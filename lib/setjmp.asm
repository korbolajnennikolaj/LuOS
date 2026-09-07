[BITS 64]
section .text

global setjmp
global longjmp

setjmp:
    mov [rdi + 0*8], rbx
    mov [rdi + 1*8], rbp
    mov [rdi + 2*8], r12
    mov [rdi + 3*8], r13
    mov [rdi + 4*8], r14
    mov [rdi + 5*8], r15
    lea rax, [rsp + 8]
    mov [rdi + 6*8], rax
    mov rax, [rsp]
    mov [rdi + 7*8], rax
    xor rax, rax
    ret

longjmp:
    mov rbx, [rdi + 0*8]
    mov rbp, [rdi + 1*8]
    mov r12, [rdi + 2*8]
    mov r13, [rdi + 3*8]
    mov r14, [rdi + 4*8]
    mov r15, [rdi + 5*8]
    mov rsp, [rdi + 6*8]

    mov rax, rsi
    test eax, eax
    jnz .ok
    mov eax, 1
.ok:
    jmp [rdi + 7*8]