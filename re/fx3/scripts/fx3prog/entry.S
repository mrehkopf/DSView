/* Entry point, jumped to by the FX3 ROM bootloader. The bootloader has
 * disconnected from USB by then and doesn't expect a return: fx3prog_main
 * resets the chip when it's done. */
    .arm
    .section .text.entry, "ax"
    .global _start
_start:
    mrs r0, cpsr
    orr r0, r0, #0xc0           @ mask IRQ and FIQ
    msr cpsr_c, r0
    ldr sp, =0x40008000
    bl cache_sync
    bl fx3prog_main
1:  b 1b

@ If the D-cache is on, clean and invalidate it, so the image the
@ bootloader wrote is what fx3prog reads.
cache_sync:
    mrc p15, 0, r0, c1, c0, 0
    tst r0, #4
    beq 2f
1:  mrc p15, 0, APSR_nzcv, c7, c14, 3   @ test, clean and invalidate D-cache
    bne 1b
2:  mov r0, #0
    mcr p15, 0, r0, c7, c10, 4          @ drain write buffer
    bx lr
