/* Rewrite a DSLogic U3's FX3 boot flash over USB, without a programmer.
 *
 * Step 1 runs on the DSLogic firmware (2a0e:xxxx, selected by port path):
 *
 *   fx3flash clear-sig PORT --yes
 *       Zeroes the first 16 bytes of flash page 0 (the "CY" signature)
 *       with the firmware's own DSL_CTL_NVM write at offset 0xFD00, which
 *       the firmware maps to flash address 0. After a power cycle the FX3
 *       ROM finds no valid image and falls back to its USB bootloader.
 *       Power cycle the analyzer itself: behind a powered hub, replugging
 *       the hub's upstream cable isn't enough.
 *
 * The rest runs on the FX3 ROM bootloader (04b4:00f3, 00bc or 0053). Its
 * vendor request 0xA0 reads and writes memory and MMIO registers, and jumps
 * to code in RAM. The flash sits on GPIO 53-56, driven as plain GPIOs.
 * DSView's udev rule doesn't cover the bootloader; a normal user needs
 *   SUBSYSTEM=="usb", ATTR{idVendor}=="04b4", ATTR{idProduct}=="00f3", TAG+="uaccess"
 *
 *   fx3flash info                    bootloader revision, block power state
 *   fx3flash id                      flash JEDEC ID and status register
 *   fx3flash read ADDR LEN FILE      read flash into FILE
 *   fx3flash write FILE --yes        erase and program FILE's image, verify
 *   fx3flash verify FILE             compare flash with FILE's image
 *   fx3flash check FILE              check FILE's image, no device needed
 *
 * write loads fx3prog (fx3prog/) and the image into the FX3's RAM and
 * jumps to it; the bootloader disconnects at the jump. fx3prog writes and
 * verifies the flash, then resets the chip, and the analyzer comes back as
 * a DSLogic (success) or as the bootloader (failure: page 0 is written
 * last, so it stays blank, and fx3flash reads fx3prog's error from RAM).
 * That takes seconds. With --slow, and for verify, id and read, the host
 * drives the pins itself, one register access per control transfer (write
 * and verify take about 20 minutes together). --delay N slows fx3prog's SPI
 * clock.
 *
 * FILE is a flash dump or boot image; its AN76405 header and checksum are
 * checked before anything is erased. Only the bytes up to the end of the
 * image (rounded up to a 4 KB sector) are erased and written, the sector
 * holding the signature last.
 *
 * Build (from this directory, needs arm-none-eabi-gcc):
 *   sh fx3prog/build.sh OUTDIR
 * Without fx3prog (slow path only):
 *   gcc -O2 -Wall -o fx3flash fx3flash.c $(pkg-config --cflags --libs libusb-1.0)
 */
#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DS_VID 0x2a0e
#define CY_VID 0x04b4

/* DSLogic firmware vendor requests (libsigrok4DSL/hardware/DSL/command.h) */
#define CMD_CTL_WR 0xb0
#define CMD_CTL_RD_PRE 0xb1
#define CMD_CTL_RD 0xb2
#define DSL_CTL_NVM 12
#define NVM_PAGE0 0xfd00 /* (0xfd00 + 0x300) & 0xffff = page 0 */

/* FX3 registers (TRM 001-76074 Rev. *F) */
#define GCTL_GPIO_SIMPLE_HI 0xe0051010u /* override bits for GPIO 32-60 */
#define GCTL_GPIO_FAST_CLK 0xe0052018u
#define LPP_POWER 0xe0007f04u
#define GPIO_POWER 0xe00013f4u
#define GPIO_SIMPLE(n) (0xe0001100u + 4u * (n))
#define GPIO_ENABLE 0x80000000u
#define GPIO_INPUT_EN 0x40u
#define GPIO_DRIVE 0x30u /* DRIVE_HI_EN | DRIVE_LO_EN */
/* The firmware's CyU3PGpioInit: fast clock = SYS_CLK / 2, simple GPIO
 * clock = fast / 2. */
#define GPIO_FAST_CLK_VALUE 0x80000061u

#define PIN_SCK 53
#define PIN_SSN 54
#define PIN_MISO 55
#define PIN_MOSI 56

#define FLASH_PAGE 256
#define FLASH_SECTOR 4096

#ifdef FX3PROG
#define WRITE_OPTIONS "[--slow] [--delay N]"
#else
#define WRITE_OPTIONS "[--slow]"
#endif

static libusb_device_handle *h;
static int mosi_level = -1;
static unsigned long transfers;

static void die(const char *what, int ret)
{
    fprintf(stderr, "%s: %s\n", what, libusb_error_name(ret));
    exit(1);
}

static uint32_t rd32(uint32_t addr)
{
    uint8_t b[4];
    int ret = libusb_control_transfer(h, 0xc0, 0xa0, addr & 0xffff, addr >> 16, b, 4, 1000);
    transfers++;
    if (ret != 4)
        die("bootloader read", ret < 0 ? ret : LIBUSB_ERROR_IO);
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

static void wr32(uint32_t addr, uint32_t v)
{
    uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 };
    int ret = libusb_control_transfer(h, 0x40, 0xa0, addr & 0xffff, addr >> 16, b, 4, 1000);
    transfers++;
    if (ret != 4)
        die("bootloader write", ret < 0 ? ret : LIBUSB_ERROR_IO);
}

/* SYSMEM transfers, up to 4 KB (the bootloader's buffer size). */
static int mem_write(uint32_t addr, const void *p, int n)
{
    int ret = libusb_control_transfer(h, 0x40, 0xa0, addr & 0xffff, addr >> 16,
                                      (unsigned char *)p, n, 2000);
    transfers++;
    return ret == n ? 0 : -1;
}

static int mem_read(uint32_t addr, void *p, int n)
{
    int ret = libusb_control_transfer(h, 0xc0, 0xa0, addr & 0xffff, addr >> 16, p, n, 2000);
    transfers++;
    return ret == n ? 0 : -1;
}

/* Write and read back, as AN76405's download example does. */
static __attribute__((unused)) int mem_load(uint32_t addr, const uint8_t *p, int n)
{
    uint8_t back[4096];
    for (int done = 0; done < n; done += 4096){
        int len = n - done < 4096 ? n - done : 4096;
        if (mem_write(addr + done, p + done, len) || mem_read(addr + done, back, len)){
            fprintf(stderr, "bootloader transfer at 0x%08x failed\n", addr + done);
            return -1;
        }
        if (memcmp(back, p + done, len)){
            fprintf(stderr, "RAM at 0x%08x reads back different\n", addr + done);
            return -1;
        }
    }
    return 0;
}

static int open_bootloader(libusb_context *ctx);

#ifdef FX3PROG
#include "fx3prog.h"
#include "fx3prog_bin.h"

static uint32_t prog_delay = 2; /* loops per SPI edge, for margin on the SCK pulse width */

static const char *prog_error(uint32_t status)
{
    static const char *names[] = {
        "", "bad parameter block", "bad arguments", "GPIO block didn't power up",
        "no flash answers", "flash is write protected", "write enable failed",
        "flash stayed busy", "verify failed",
    };
    uint32_t e = status & ~FX3PROG_ERR;
    if (status == 0)
        return "fx3prog never ran, or the reset cleared its RAM";
    if (status == FX3PROG_RUNNING)
        return "fx3prog didn't finish";
    if (status == FX3PROG_DONE)
        return "done";
    return e < sizeof(names) / sizeof(names[0]) ? names[e] : "unknown error";
}

static int port_chain(libusb_device *dev, uint8_t *ports)
{
    return libusb_get_port_numbers(dev, ports, 8);
}

/* After fx3prog's reset the analyzer comes back on the same port (maybe on
 * the USB 3 bus): 1 = DSLogic firmware, 0 = bootloader, -1 = nothing yet. */
static int find_again(libusb_context *ctx, const uint8_t *ports, int np,
                      uint8_t old_bus, uint8_t old_addr)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int found = -1;
    for (ssize_t i = 0; i < n && found < 0; i++){
        struct libusb_device_descriptor des;
        uint8_t p[8];
        if (libusb_get_device_descriptor(list[i], &des))
            continue;
        if (libusb_get_bus_number(list[i]) == old_bus && libusb_get_device_address(list[i]) == old_addr)
            continue;
        if (port_chain(list[i], p) != np || memcmp(p, ports, np))
            continue;
        if (des.idVendor == DS_VID)
            found = 1;
        else if (des.idVendor == CY_VID && des.idProduct == 0x00f3)
            found = 0;
    }
    libusb_free_device_list(list, 1);
    return found;
}

static int prog_flash(libusb_context *ctx, const uint8_t *img, long end)
{
    struct fx3prog_param p = { FX3PROG_MAGIC, FX3PROG_FLASH, FX3PROG_IMAGE, end, prog_delay, 0, { 0 } };
    libusb_device *dev = libusb_get_device(h);
    uint8_t ports[8], bus = libusb_get_bus_number(dev), addr = libusb_get_device_address(dev);
    int np = port_chain(dev, ports), r = -1;
    struct timespec t0, t;

    if (end > FX3PROG_IMAGE_MAX){
        fprintf(stderr, "image too large for fx3prog\n");
        return -1;
    }
    if (mem_load(FX3PROG_ENTRY, fx3prog_bin, fx3prog_bin_len) ||
        mem_load(FX3PROG_IMAGE, img, end) ||
        mem_load(FX3PROG_PARAM, (const uint8_t *)&p, sizeof(p)))
        return -1;
    printf("loaded fx3prog (%u bytes) and the image into RAM; GCTL_CONTROL 0x%08x\n",
           fx3prog_bin_len, rd32(0xe0050000));
    /* The bootloader disconnects and hands over the CPU. */
    libusb_control_transfer(h, 0x40, 0xa0, FX3PROG_ENTRY & 0xffff, FX3PROG_ENTRY >> 16, NULL, 0, 2000);
    libusb_close(h);
    h = NULL;
    printf("fx3prog is writing the flash (LED red, green blinking while erasing)...\n");
    fflush(stdout);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        usleep(250000);
        r = find_again(ctx, ports, np, bus, addr);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (r < 0 && t.tv_sec - t0.tv_sec < 90);

    if (r == 1){
        printf("after %ld s the analyzer is back as a DSLogic: it boots the new image\n",
               (long)(t.tv_sec - t0.tv_sec));
        return 0;
    }
    if (r < 0){
        fprintf(stderr, "the analyzer didn't come back within 90 s. The LED shows the result:\n"
                "green = written, blinking red = failed. Replug it: it boots the new image,\n"
                "or comes up as the bootloader again if page 0 wasn't written.\n");
        return -1;
    }
    printf("after %ld s the analyzer is back as the bootloader: page 0 wasn't written\n",
           (long)(t.tv_sec - t0.tv_sec));
    usleep(500000);
    struct fx3prog_param back;
    if (open_bootloader(ctx) || mem_read(FX3PROG_PARAM, &back, sizeof(back))){
        fprintf(stderr, "can't read fx3prog's result\n");
        return -1;
    }
    fprintf(stderr, "fx3prog: %s; JEDEC %06x, status 0x%02x, at flash 0x%05x",
            prog_error(back.status), back.result[1], back.result[2], back.result[3]);
    if (back.status == (FX3PROG_ERR | FX3PROG_ERR_VERIFY))
        fprintf(stderr, ", first mismatch at 0x%05x", back.result[0]);
    fprintf(stderr, "\n");
    return -1;
}
#endif

static int wait_active(uint32_t reg, const char *name)
{
    for (int i = 0; i < 100; i++){
        if (rd32(reg) & 1)
            return 0;
        usleep(1000);
    }
    fprintf(stderr, "%s did not become active\n", name);
    return -1;
}

/* Power and reset a block the way the SDK does: RESETN low, wait, high,
 * poll ACTIVE. Left alone if it is already active. */
static int power_block(uint32_t reg, const char *name)
{
    uint32_t v = rd32(reg);
    if (v & 1)
        return 0;
    wr32(reg, v & ~0x80000000u);
    usleep(1000);
    wr32(reg, v | 0x80000000u);
    return wait_active(reg, name);
}

static int open_bootloader(libusb_context *ctx)
{
    static const uint16_t pids[] = { 0x00f3, 0x00bc, 0x0053 };
    for (unsigned i = 0; i < sizeof(pids) / sizeof(pids[0]) && !h; i++)
        h = libusb_open_device_with_vid_pid(ctx, CY_VID, pids[i]);
    if (!h){
        fprintf(stderr, "no FX3 bootloader (04b4:00f3) found, or no access to it\n");
        return -1;
    }
    return 0;
}

static void info(void)
{
    uint32_t rev = rd32(0xffff0020);
    printf("bootloader revision: %u.%u\n", (rev >> 8) & 0xff, rev & 0xff);
    printf("GCTL_GPIO_FAST_CLK: 0x%08x\n", rd32(GCTL_GPIO_FAST_CLK));
    printf("LPP_POWER:          0x%08x\n", rd32(LPP_POWER));
    printf("GPIO_POWER:         0x%08x\n", rd32(GPIO_POWER));
    printf("GCTL_GPIO_SIMPLE:   0x%08x (GPIO 32-60)\n", rd32(GCTL_GPIO_SIMPLE_HI));
}

static void pin(int n, int level)
{
    wr32(GPIO_SIMPLE(n), GPIO_ENABLE | GPIO_DRIVE | (level ? 1 : 0));
}

static int spi_setup(void)
{
    uint32_t clk = rd32(GCTL_GPIO_FAST_CLK);
    if (!(clk & 0x80000000u)){
        wr32(GCTL_GPIO_FAST_CLK, GPIO_FAST_CLK_VALUE);
        usleep(1000);
    }
    if (power_block(LPP_POWER, "LPP_POWER") || power_block(GPIO_POWER, "GPIO_POWER"))
        return -1;
    /* Configure before taking the pins over, chip select high first. */
    pin(PIN_SSN, 1);
    pin(PIN_SCK, 0);
    pin(PIN_MOSI, 0);
    mosi_level = 0;
    wr32(GPIO_SIMPLE(PIN_MISO), GPIO_ENABLE | GPIO_INPUT_EN);
    wr32(GCTL_GPIO_SIMPLE_HI, rd32(GCTL_GPIO_SIMPLE_HI) |
         1u << (PIN_SCK - 32) | 1u << (PIN_SSN - 32) |
         1u << (PIN_MISO - 32) | 1u << (PIN_MOSI - 32));
    return 0;
}

static void spi_out(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--){
            int bit = (p[i] >> b) & 1;
            if (bit != mosi_level){
                pin(PIN_MOSI, bit);
                mosi_level = bit;
            }
            pin(PIN_SCK, 1);
            pin(PIN_SCK, 0);
        }
}

/* Mode 0: the flash shifts on the falling edge, sample with SCK high. */
static void spi_in(uint8_t *p, int n)
{
    for (int i = 0; i < n; i++){
        uint8_t v = 0;
        for (int b = 0; b < 8; b++){
            pin(PIN_SCK, 1);
            v = v << 1 | ((rd32(GPIO_SIMPLE(PIN_MISO)) >> 1) & 1);
            pin(PIN_SCK, 0);
        }
        p[i] = v;
    }
}

static void cmd(const uint8_t *out, int nout, uint8_t *in, int nin)
{
    pin(PIN_SSN, 0);
    spi_out(out, nout);
    if (nin)
        spi_in(in, nin);
    pin(PIN_SSN, 1);
}

static uint8_t read_status(void)
{
    uint8_t c = 0x05, s;
    cmd(&c, 1, &s, 1);
    return s;
}

static int wait_ready(int timeout_ms)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;){
        uint8_t s = read_status();
        if (!(s & 1))
            return 0;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 > timeout_ms){
            fprintf(stderr, "flash stays busy (status 0x%02x)\n", s);
            return -1;
        }
    }
}

static int write_enable(void)
{
    uint8_t c = 0x06;
    cmd(&c, 1, NULL, 0);
    uint8_t s = read_status();
    if ((s & 3) != 2){
        fprintf(stderr, "write enable failed (status 0x%02x)\n", s);
        return -1;
    }
    return 0;
}

static void flash_read(uint32_t addr, uint8_t *p, int n)
{
    uint8_t c[4] = { 0x03, addr >> 16, addr >> 8, addr };
    cmd(c, 4, p, n);
}

static int sector_erase(uint32_t addr)
{
    uint8_t c[4] = { 0x20, addr >> 16, addr >> 8, addr };
    if (write_enable())
        return -1;
    cmd(c, 4, NULL, 0);
    return wait_ready(2000);
}

static int page_program(uint32_t addr, const uint8_t *p)
{
    uint8_t c[4] = { 0x02, addr >> 16, addr >> 8, addr };
    if (write_enable())
        return -1;
    pin(PIN_SSN, 0);
    spi_out(c, 4);
    spi_out(p, FLASH_PAGE);
    pin(PIN_SSN, 1);
    return wait_ready(100);
}

/* Returns the image length in bytes (end of the checksum word), or -1. */
static long check_image(const uint8_t *d, long size)
{
    long pos = 4;
    uint32_t sum = 0;
    if (size < 12 || d[0] != 'C' || d[1] != 'Y' || d[3] != 0xb0){
        fprintf(stderr, "not an FX3 boot image (no CY header, type 0xB0)\n");
        return -1;
    }
    for (;;){
        uint32_t len, addr;
        if (pos + 8 > size)
            goto trunc;
        memcpy(&len, d + pos, 4);
        memcpy(&addr, d + pos + 4, 4);
        pos += 8;
        if (len == 0)
            break;
        if (pos + 4L * len > size)
            goto trunc;
        for (uint32_t i = 0; i < len; i++){
            uint32_t w;
            memcpy(&w, d + pos + 4 * i, 4);
            sum += w;
        }
        pos += 4L * len;
    }
    if (pos + 4 > size)
        goto trunc;
    uint32_t stored;
    memcpy(&stored, d + pos, 4);
    if (stored != sum){
        fprintf(stderr, "image checksum 0x%08x, computed 0x%08x\n", stored, sum);
        return -1;
    }
    return pos + 4;
trunc:
    fprintf(stderr, "image truncated\n");
    return -1;
}

static uint8_t *load_image(const char *path, long *end)
{
    FILE *f = fopen(path, "rb");
    if (!f){
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    uint8_t *d = malloc(size);
    if (fread(d, 1, size, f) != (size_t)size){
        perror(path);
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    long len = check_image(d, size);
    if (len < 0){
        free(d);
        return NULL;
    }
    /* Round up to whole sectors, and keep FILE's bytes up to there. */
    long e = (len + FLASH_SECTOR - 1) / FLASH_SECTOR * FLASH_SECTOR;
    uint8_t *img = malloc(e);
    memset(img, 0xff, e);
    memcpy(img, d, size < e ? size : e);
    free(d);
    printf("image: %ld bytes, checksum ok; flash range 0x0-0x%lx\n", len, e - 1);
    *end = e;
    return img;
}

static int verify(const uint8_t *img, long end)
{
    uint8_t buf[FLASH_PAGE];
    int bad = 0;
    for (long a = 0; a < end; a += FLASH_PAGE){
        flash_read(a, buf, FLASH_PAGE);
        if (memcmp(buf, img + a, FLASH_PAGE)){
            fprintf(stderr, "mismatch in page 0x%05lx\n", a);
            if (++bad > 8)
                break;
        }
        if ((a / FLASH_PAGE) % 16 == 15){
            printf("\rverified 0x%05lx", a + FLASH_PAGE);
            fflush(stdout);
        }
    }
    printf("\n%s\n", bad ? "VERIFY FAILED" : "verify ok");
    return bad ? -1 : 0;
}

static int flash_id(void)
{
    uint8_t c = 0x9f, id[3];
    cmd(&c, 1, id, 3);
    uint8_t s = read_status();
    printf("JEDEC ID: %02x %02x %02x, status 0x%02x\n", id[0], id[1], id[2], s);
    if (id[0] == 0x00 || id[0] == 0xff){
        fprintf(stderr, "no flash answers on the SPI pins\n");
        return -1;
    }
    return s;
}

/* Step 1, on the DSLogic firmware. */
static int clear_sig(libusb_context *ctx, const char *port)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device *dev = NULL;
    for (ssize_t i = 0; i < n && !dev; i++){
        struct libusb_device_descriptor des;
        uint8_t ports[8];
        char name[64];
        if (libusb_get_device_descriptor(list[i], &des) || des.idVendor != DS_VID)
            continue;
        int np = libusb_get_port_numbers(list[i], ports, sizeof(ports));
        int len = snprintf(name, sizeof(name), "%u-", libusb_get_bus_number(list[i]));
        for (int k = 0; k < np; k++)
            len += snprintf(name + len, sizeof(name) - len, k ? ".%u" : "%u", ports[k]);
        if (np > 0 && strcmp(name, port) == 0)
            dev = list[i];
    }
    if (!dev){
        fprintf(stderr, "no DSLogic at port %s\n", port);
        return 1;
    }
    int ret = libusb_open(dev, &h);
    libusb_free_device_list(list, 1);
    if (ret)
        die("open", ret);

    uint8_t rd[4] = { DSL_CTL_NVM, NVM_PAGE0 & 0xff, NVM_PAGE0 >> 8, 16 }, buf[16];
    ret = libusb_control_transfer(h, 0x40, CMD_CTL_RD_PRE, 0, 0, rd, 4, 3000);
    usleep(10000);
    if (ret == 4)
        ret = libusb_control_transfer(h, 0xc0, CMD_CTL_RD, 0, 0, buf, 16, 3000);
    if (ret != 16)
        die("NVM read", ret < 0 ? ret : LIBUSB_ERROR_IO);
    if (buf[0] != 'C' || buf[1] != 'Y'){
        fprintf(stderr, "page 0 doesn't start with CY (%02x %02x), not touching it\n", buf[0], buf[1]);
        return 1;
    }

    uint8_t wr[4 + 16] = { DSL_CTL_NVM, NVM_PAGE0 & 0xff, NVM_PAGE0 >> 8, 16 };
    ret = libusb_control_transfer(h, 0x40, CMD_CTL_WR, 0, 0, wr, sizeof(wr), 3000);
    if (ret != (int)sizeof(wr))
        die("NVM write", ret < 0 ? ret : LIBUSB_ERROR_IO);
    usleep(100000);

    ret = libusb_control_transfer(h, 0x40, CMD_CTL_RD_PRE, 0, 0, rd, 4, 3000);
    usleep(10000);
    if (ret == 4)
        ret = libusb_control_transfer(h, 0xc0, CMD_CTL_RD, 0, 0, buf, 16, 3000);
    if (ret != 16)
        die("NVM read back", ret < 0 ? ret : LIBUSB_ERROR_IO);
    printf("page 0 now:");
    for (int i = 0; i < 16; i++)
        printf(" %02x", buf[i]);
    printf("\n");
    for (int i = 0; i < 16; i++)
        if (buf[i]){
            fprintf(stderr, "signature not cleared (write protected?)\n");
            return 1;
        }
    printf("signature cleared; unplug the analyzer and plug it back in\n");
    return 0;
}

int main(int argc, char **argv)
{
    libusb_context *ctx;
    int rc = 1, yes = 0, slow = 0;
    struct timespec t0, t1;

    for (int i = 1; i < argc; i++){
        int drop = 1;
        if (strcmp(argv[i], "--yes") == 0)
            yes = 1;
        else if (strcmp(argv[i], "--slow") == 0)
            slow = 1;
#ifdef FX3PROG
        else if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc){
            prog_delay = strtoul(argv[i + 1], NULL, 0);
            drop = 2;
        }
#endif
        else
            continue;
        memmove(argv + i, argv + i + drop, (argc - i - drop + 1) * sizeof(*argv));
        argc -= drop;
        i--;
    }
#ifndef FX3PROG
    slow = 1;
    (void)slow;
#endif
    if (argc < 2){
        fprintf(stderr, "usage: %s clear-sig PORT --yes | info | id | read ADDR LEN FILE |"
                " write FILE --yes " WRITE_OPTIONS " | verify FILE | check FILE\n", argv[0]);
        return 2;
    }
    if (libusb_init(&ctx))
        return 1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    const char *c = argv[1];
    if (strcmp(c, "clear-sig") == 0 && argc == 3){
        if (!yes){
            fprintf(stderr, "clear-sig makes the analyzer boot into the FX3 bootloader "
                    "until its flash is rewritten; add --yes\n");
            goto out;
        }
        rc = clear_sig(ctx, argv[2]);
        goto out;
    }
    if (strcmp(c, "check") == 0 && argc == 3){
        long end;
        uint8_t *img = load_image(argv[2], &end);
        rc = img ? 0 : 1;
        goto out;
    }
    if (open_bootloader(ctx))
        goto out;
    if (strcmp(c, "info") == 0){
        info();
        rc = 0;
    }
    else if (strcmp(c, "id") == 0){
        if (spi_setup() == 0 && flash_id() >= 0)
            rc = 0;
    }
    else if (strcmp(c, "read") == 0 && argc == 5){
        uint32_t addr = strtoul(argv[2], NULL, 0);
        long len = strtol(argv[3], NULL, 0);
        uint8_t *buf = malloc(len);
        FILE *f = fopen(argv[4], "wb");
        if (!f || spi_setup())
            goto out;
        for (long done = 0; done < len; done += FLASH_PAGE){
            int n = len - done < FLASH_PAGE ? len - done : FLASH_PAGE;
            flash_read(addr + done, buf + done, n);
            printf("\rread 0x%05lx", done + n);
            fflush(stdout);
        }
        printf("\n");
        fwrite(buf, 1, len, f);
        fclose(f);
        rc = 0;
    }
#ifdef FX3PROG
    else if (!slow && strcmp(c, "write") == 0 && argc == 3){
        long end;
        uint8_t *img = load_image(argv[2], &end);
        if (!img || spi_setup())
            goto out;
        int s = flash_id();
        if (s < 0)
            goto out;
        if (!yes){
            fprintf(stderr, "write erases flash 0x0-0x%lx; add --yes\n", end - 1);
            goto out;
        }
        if (s & 0x1c){ /* BP0-BP2 */
            fprintf(stderr, "status register 0x%02x has protection bits set, not writing\n", s);
            goto out;
        }
        rc = prog_flash(ctx, img, end) ? 1 : 0;
    }
#endif
    else if ((strcmp(c, "write") == 0 || strcmp(c, "verify") == 0) && argc == 3){
        long end;
        uint8_t *img = load_image(argv[2], &end);
        if (!img || spi_setup())
            goto out;
        int s = flash_id();
        if (s < 0)
            goto out;
        if (strcmp(c, "verify") == 0){
            rc = verify(img, end) ? 1 : 0;
            goto out;
        }
        if (!yes){
            fprintf(stderr, "write erases flash 0x0-0x%lx; add --yes\n", end - 1);
            goto out;
        }
        if (s & 0x1c){ /* BP0-BP2 */
            fprintf(stderr, "status register 0x%02x has protection bits set, not writing\n", s);
            goto out;
        }
        for (long a = 0; a < end; a += FLASH_SECTOR){
            printf("\rerase 0x%05lx", a);
            fflush(stdout);
            if (sector_erase(a))
                goto out;
        }
        printf("\n");
        /* Page 0 (the CY signature) goes last: if the write is cut short,
         * the ROM finds no signature and falls back to USB boot again,
         * rather than running a partial image. */
        for (long a = FLASH_PAGE; a <= end; a += FLASH_PAGE){
            long p = a < end ? a : 0;
            int blank = 1;
            for (int i = 0; i < FLASH_PAGE && blank; i++)
                blank = img[p + i] == 0xff;
            if (!blank && page_program(p, img + p))
                goto out;
            if ((a / FLASH_PAGE) % 16 == 15 || p == 0){
                printf("\rprogram 0x%05lx", p ? p + FLASH_PAGE : 0);
                fflush(stdout);
            }
        }
        printf(" (page 0 last)\n");
        rc = verify(img, end) ? 1 : 0;
    }
    else
        fprintf(stderr, "unknown command\n");

out:
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (transfers){
        double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "%lu bootloader transfers in %.1f s (%.0f/s)\n", transfers, s, transfers / s);
    }
    if (h)
        libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
