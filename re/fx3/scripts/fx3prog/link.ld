ENTRY(_start)

SECTIONS
{
    . = 0x40003000;
    .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "fx3prog must not use .data or .bss")
    ASSERT(. <= 0x40004000, "fx3prog overlaps its parameter block")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
