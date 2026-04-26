BITS 32

global _start
extern kernel_main

%define MULTIBOOT2_MAGIC 0xE85250D6
%define MULTIBOOT2_ARCH_I386 0
%define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289
%define IA32_EFER_MSR 0xC0000080
%define IA32_EFER_LME (1 << 8)
%define CR0_PG (1 << 31)
%define CR4_PAE (1 << 5)

section .multiboot
align 8
mb2_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH_I386
    dd mb2_header_end - mb2_header_start
    dd -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    ; Request a linear framebuffer from GRUB.
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32
    dd 0

    dw 0
    dw 0
    dd 8
mb2_header_end:

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
pd_table0:
    resb 4096
pd_table1:
    resb 4096
pd_table2:
    resb 4096
pd_table3:
    resb 4096

section .rodata
align 16
gdt64:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

section .text
serial_init:
    mov dx, 0x3F8 + 1
    mov al, 0x00
    out dx, al

    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al

    mov dx, 0x3F8 + 0
    mov al, 0x03
    out dx, al

    mov dx, 0x3F8 + 1
    mov al, 0x00
    out dx, al

    mov dx, 0x3F8 + 3
    mov al, 0x03
    out dx, al

    mov dx, 0x3F8 + 2
    mov al, 0xC7
    out dx, al

    mov dx, 0x3F8 + 4
    mov al, 0x0B
    out dx, al
    ret

serial_write_char:
    push eax
.wait:
    mov dx, 0x3F8 + 5
    in al, dx
    test al, 0x20
    jz .wait
    pop eax
    mov dx, 0x3F8
    out dx, al
    ret

serial_write_str:
    push esi
.next:
    lodsb
    test al, al
    jz .done
    call serial_write_char
    jmp .next
.done:
    pop esi
    ret

_start:
    cli
    mov esp, stack_top
    mov edi, eax
    mov ebp, ebx
    call serial_init
    mov esi, msg_stage_1
    call serial_write_str

    cmp edi, MULTIBOOT2_BOOTLOADER_MAGIC
    jne boot_hang

    call setup_paging
    mov esi, msg_stage_2
    call serial_write_str
    lgdt [gdt64_ptr]

    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    mov eax, pml4_table
    mov cr3, eax

    mov ecx, IA32_EFER_MSR
    rdmsr
    or eax, IA32_EFER_LME
    wrmsr

    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax
    mov esi, msg_stage_3
    call serial_write_str

    jmp 0x08:long_mode_start

setup_paging:
    ; PML4[0] -> PDPT
    mov eax, pdpt_table
    or eax, 0x03
    mov [pml4_table], eax
    mov dword [pml4_table + 4], 0

    ; PDPT[0..3] -> PD tables (maps first 4 GiB via 2 MiB pages)
    mov eax, pd_table0
    or eax, 0x03
    mov [pdpt_table], eax
    mov dword [pdpt_table + 4], 0

    mov eax, pd_table1
    or eax, 0x03
    mov [pdpt_table + 8], eax
    mov dword [pdpt_table + 12], 0

    mov eax, pd_table2
    or eax, 0x03
    mov [pdpt_table + 16], eax
    mov dword [pdpt_table + 20], 0

    mov eax, pd_table3
    or eax, 0x03
    mov [pdpt_table + 24], eax
    mov dword [pdpt_table + 28], 0

    ; Identity-map first 4GiB with 2MiB pages.
    xor ecx, ecx
.map_loop:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    cmp ecx, 512
    jb .map_pd0
    cmp ecx, 1024
    jb .map_pd1
    cmp ecx, 1536
    jb .map_pd2
    jmp .map_pd3

.map_pd0:
    mov edx, ecx
    mov [pd_table0 + edx * 8], eax
    mov dword [pd_table0 + edx * 8 + 4], 0
    jmp .next

.map_pd1:
    mov edx, ecx
    sub edx, 512
    mov [pd_table1 + edx * 8], eax
    mov dword [pd_table1 + edx * 8 + 4], 0
    jmp .next

.map_pd2:
    mov edx, ecx
    sub edx, 1024
    mov [pd_table2 + edx * 8], eax
    mov dword [pd_table2 + edx * 8 + 4], 0
    jmp .next

.map_pd3:
    mov edx, ecx
    sub edx, 1536
    mov [pd_table3 + edx * 8], eax
    mov dword [pd_table3 + edx * 8 + 4], 0
.next:
    inc ecx
    cmp ecx, 2048
    jne .map_loop
    ret

BITS 64
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Enable SSE/SSE2 for compiler-generated XMM instructions.
    ; CR0: clear EM/TS, set MP. CR4: set OSFXSR/OSXMMEXCPT.
    mov rax, cr0
    and rax, ~(1 << 2)        ; EM = 0
    and rax, ~(1 << 3)        ; TS = 0
    or  rax, (1 << 1)         ; MP = 1
    mov cr0, rax

    mov rax, cr4
    or  rax, (1 << 9)         ; OSFXSR
    or  rax, (1 << 10)        ; OSXMMEXCPT
    mov cr4, rax

    ; SysV ABI: before a CALL, RSP must be 16-byte aligned so the callee
    ; sees RSP%16==8 at function entry (after the return address is pushed).
    mov rsp, stack_top
    mov edi, ebp
    mov al, 'L'
    call serial_write_char
    mov al, 'M'
    call serial_write_char
    mov al, 10
    call serial_write_char
    call kernel_main

.hang:
boot_hang:
    cli
    hlt
    jmp boot_hang

section .note.GNU-stack noalloc noexec nowrite

section .rodata
msg_stage_1: db "stage1: entered _start", 10, 0
msg_stage_2: db "stage2: paging tables ready", 10, 0
msg_stage_3: db "stage3: long mode enabled", 10, 0
