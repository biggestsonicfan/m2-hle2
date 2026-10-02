/* qtlink.ld — qtlink in the QT960 SRAM at 0x10100000, where NINDY's own downloads go. */
OUTPUT_ARCH(i960:kb)
ENTRY(start)

SECTIONS
{
    . = 0x10100000;
    .text : {
        *(.text.start)
        *(.text) *(.text.*)
        . = ALIGN(16);
        __stubs_start = .;
        *(.stubs)
        . = ALIGN(16);
        __stubs_end = .;
    }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data : { *(.data) *(.data.*) . = ALIGN(4); }
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss) *(.bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
