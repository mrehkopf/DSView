/* Shared between fx3prog (runs from FX3 RAM) and fx3flash (the host).
 *
 * The ROM bootloader's jump is one-way: it disconnects from USB and hands
 * the CPU over. So the host loads fx3prog, the whole flash image and a
 * parameter block into RAM, then jumps. fx3prog erases, programs and
 * verifies the flash (page 0, with the signature, last and only after
 * everything else verified), shows the result on the LED, writes it to the
 * parameter block and resets the chip. The analyzer then boots the new
 * image, or comes back as the bootloader if page 0 wasn't written; the host
 * reads the parameter block back in that case. */
#ifndef FX3PROG_H
#define FX3PROG_H

#include <stdint.h>

#define FX3PROG_ENTRY 0x40003000u /* code, up to 4 KB */
#define FX3PROG_PARAM 0x40004000u /* struct fx3prog_param */
#define FX3PROG_STACK_TOP 0x40008000u
#define FX3PROG_IMAGE 0x40010000u /* the flash image */
#define FX3PROG_IMAGE_MAX 0x30000u /* stays below the NVM area at 0x30000 */
#define FX3PROG_MAGIC 0x50533346u /* "F3SP" */
#define FX3PROG_FLASH 1u

#define FX3PROG_RUNNING 1u
#define FX3PROG_DONE 2u
#define FX3PROG_ERR 0x100u /* | one of the codes below */

enum {
    FX3PROG_ERR_MAGIC = 1,
    FX3PROG_ERR_ARG,
    FX3PROG_ERR_GPIO,      /* GPIO block didn't power up */
    FX3PROG_ERR_ID,        /* no flash answers */
    FX3PROG_ERR_PROTECTED, /* status register block-protect bits set */
    FX3PROG_ERR_WEL,       /* write enable didn't set WEL */
    FX3PROG_ERR_TIMEOUT,   /* flash stayed busy */
    FX3PROG_ERR_VERIFY,    /* result[0]: first mismatching flash address */
};

struct fx3prog_param {
    uint32_t magic;
    uint32_t cmd;
    uint32_t addr;   /* image in RAM */
    uint32_t len;    /* bytes, whole 4 KB sectors */
    uint32_t delay;  /* extra delay loops per SPI clock edge */
    uint32_t status; /* host clears it; fx3prog sets RUNNING, then the result */
    uint32_t result[4]; /* mismatch address, JEDEC ID, status register,
                           last flash address reached */
};

#endif
