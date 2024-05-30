[org 0x7c00]
KERNEL_LOCATION equ 0x1000

; Boot Disk
BOOT_DISK: db 0

; Error Messages
disk_error_msg db 'Disk read error', 0
video_error_msg db 'Video mode error', 0

start:
    mov [BOOT_DISK], dl                 ; Save boot disk

    ; Set up stack
    xor ax, ax
    mov es, ax
    mov ds, ax
    mov bp, 0x8000
    mov sp, bp

    ; Load Kernel
    mov bx, KERNEL_LOCATION
    mov dh, 2

    mov ah, 0x02
    mov al, dh 
    mov ch, 0x00
    mov dh, 0x00
    mov cl, 0x02
    mov dl, [BOOT_DISK]
    int 0x13
    jc disk_read_error                 ; Jump if carry flag set (error)

    ; Set video mode
    mov ah, 0x0
    mov al, 0x3
    int 0x10
    jc video_mode_error                ; Jump if carry flag set (error)

    ; Set up GDT and enter protected mode
    CODE_SEG equ GDT_code - GDT_start
    DATA_SEG equ GDT_data - GDT_start

    cli
    lgdt [GDT_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEG:start_protected_mode

    jmp $

disk_read_error:
    mov si, disk_error_msg
    call print_string
    hlt

video_mode_error:
    mov si, video_error_msg
    call print_string
    hlt

print_string:
    .loop:
        lodsb
        or al, al
        jz .done
        mov ah, 0x0E
        int 0x10
        jmp .loop
    .done:
        ret

GDT_start:
    GDT_null:
        dd 0x0
        dd 0x0

    GDT_code:
        dw 0xffff
        dw 0x0
        db 0x0
        db 0b10011010
        db 0b11001111
        db 0x0

    GDT_data:
        dw 0xffff
        dw 0x0
        db 0x0
        db 0b10010010
        db 0b11001111
        db 0x0

GDT_end:

GDT_descriptor:
    dw GDT_end - GDT_start - 1
    dd GDT_start

[bits 32]
start_protected_mode:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    mov ebp, 0x90000        ; 32-bit stack base pointer
    mov esp, ebp

    jmp KERNEL_LOCATION

times 510-($-$$) db 0
dw 0xaa55
