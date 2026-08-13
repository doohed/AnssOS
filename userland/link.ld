ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data*)
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame)
    }
}
