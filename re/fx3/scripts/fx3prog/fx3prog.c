/* FX3 RAM program: writes the boot flash from an image in RAM, run once
 * through the ROM bootloader's jump (see fx3prog.h). It bit-bangs the flash
 * on GPIO 53-56 like the DSLogic firmware does, with direct register writes.
 * No .data or .bss: the linker script checks.
 *
 * LED (green GPIO 51, red GPIO 52, active high): red while working, green
 * toggling once per erased sector; then green for success or red blinking
 * for an error, about two seconds before the reset. */
#include "fx3prog.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define GCTL_CONTROL 0xe0050000u
#define GCTL_HARD_RESET_N (1u << 31)
#define GCTL_GPIO_SIMPLE_HI 0xe0051010u
#define GCTL_GPIO_FAST_CLK 0xe0052018u
#define LPP_POWER 0xe0007f04u
#define GPIO_POWER 0xe00013f4u
#define GPIO_SIMPLE(n) (0xe0001100u + 4u * (n))
#define GPIO_OUT 0x80000030u /* ENABLE | DRIVE_HI_EN | DRIVE_LO_EN */
#define GPIO_IN 0x80000040u  /* ENABLE | INPUT_EN */
#define GPIO_FAST_CLK_VALUE 0x80000061u /* as the firmware's CyU3PGpioInit */

#define LED_GREEN 51
#define LED_RED 52
#define SCK 53
#define SSN 54
#define MISO 55
#define MOSI 56

#define PAGE 256u
#define SECTOR 4096u
#define POLL_LIMIT 2000000u

static void wait(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++)
        ;
}

static void pin(int n, int level)
{
    REG(GPIO_SIMPLE(n)) = GPIO_OUT | (level ? 1 : 0);
}

static void leds(int red, int green)
{
    pin(LED_RED, red);
    pin(LED_GREEN, green);
}

/* Clock and power the GPIO block if the bootloader left it off (it was on
 * when tested), then take the flash and LED pins over as plain GPIOs. */
static int power_block(uint32_t reg)
{
    if (REG(reg) & 1)
        return 0;
    REG(reg) &= ~0x80000000u;
    wait(10000);
    REG(reg) |= 0x80000000u;
    for (uint32_t i = 0; i < 10000000u; i++)
        if (REG(reg) & 1)
            return 0;
    return -1;
}

static int gpio_setup(void)
{
    if (!(REG(GCTL_GPIO_FAST_CLK) & 0x80000000u)){
        REG(GCTL_GPIO_FAST_CLK) = GPIO_FAST_CLK_VALUE;
        wait(10000);
    }
    if (power_block(LPP_POWER) || power_block(GPIO_POWER))
        return FX3PROG_ERR_GPIO;
    pin(SSN, 1);
    pin(SCK, 0);
    pin(MOSI, 0);
    REG(GPIO_SIMPLE(MISO)) = GPIO_IN;
    leds(1, 0);
    REG(GCTL_GPIO_SIMPLE_HI) |= 1u << (SCK - 32) | 1u << (SSN - 32) |
                                1u << (MISO - 32) | 1u << (MOSI - 32) |
                                1u << (LED_GREEN - 32) | 1u << (LED_RED - 32);
    return 0;
}

static void select(int on, uint32_t d)
{
    pin(SSN, !on);
    wait(d);
}

static void out8(uint32_t b, uint32_t d)
{
    for (int i = 7; i >= 0; i--){
        pin(MOSI, (b >> i) & 1);
        wait(d);
        pin(SCK, 1);
        wait(d);
        pin(SCK, 0);
    }
}

/* Mode 0: the flash shifts on the falling edge; sample with SCK high. */
static uint32_t in8(uint32_t d)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++){
        pin(SCK, 1);
        wait(d);
        v = v << 1 | ((REG(GPIO_SIMPLE(MISO)) >> 1) & 1);
        pin(SCK, 0);
        wait(d);
    }
    return v;
}

static void cmd_addr(uint32_t c, uint32_t addr, uint32_t d)
{
    out8(c, d);
    out8(addr >> 16, d);
    out8(addr >> 8, d);
    out8(addr, d);
}

static uint32_t read_status(uint32_t d)
{
    select(1, d);
    out8(0x05, d);
    uint32_t s = in8(d);
    select(0, d);
    return s;
}

static int wait_ready(uint32_t d)
{
    for (uint32_t i = 0; i < POLL_LIMIT; i++)
        if (!(read_status(d) & 1))
            return 0;
    return FX3PROG_ERR_TIMEOUT;
}

static int write_enable(uint32_t d)
{
    select(1, d);
    out8(0x06, d);
    select(0, d);
    return (read_status(d) & 3) == 2 ? 0 : FX3PROG_ERR_WEL;
}

static int erase(uint32_t addr, uint32_t d)
{
    int err;
    if ((err = write_enable(d)))
        return err;
    select(1, d);
    cmd_addr(0x20, addr, d);
    select(0, d);
    return wait_ready(d);
}

static int program(uint32_t addr, const volatile uint8_t *p, uint32_t d)
{
    int err, blank = 1;
    for (uint32_t i = 0; i < PAGE && blank; i++)
        blank = p[i] == 0xff;
    if (blank)
        return 0;
    if ((err = write_enable(d)))
        return err;
    select(1, d);
    cmd_addr(0x02, addr, d);
    for (uint32_t i = 0; i < PAGE; i++)
        out8(p[i], d);
    select(0, d);
    return wait_ready(d);
}

static int verify(volatile struct fx3prog_param *p, uint32_t addr, uint32_t len, uint32_t d)
{
    const volatile uint8_t *img = (const volatile uint8_t *)p->addr;
    select(1, d);
    cmd_addr(0x03, addr, d);
    for (uint32_t i = 0; i < len; i++)
        if (in8(d) != img[addr + i]){
            select(0, d);
            p->result[0] = addr + i;
            return FX3PROG_ERR_VERIFY;
        }
    select(0, d);
    return 0;
}

static int flash(volatile struct fx3prog_param *p, uint32_t d)
{
    const volatile uint8_t *img = (const volatile uint8_t *)p->addr;
    uint32_t len = p->len;
    int err;

    if (len == 0 || len % SECTOR || len > FX3PROG_IMAGE_MAX)
        return FX3PROG_ERR_ARG;

    select(1, d);
    out8(0x9f, d);
    uint32_t id = in8(d) << 16;
    id |= in8(d) << 8;
    id |= in8(d);
    select(0, d);
    p->result[1] = id;
    p->result[2] = read_status(d);
    if ((id >> 16) == 0x00 || (id >> 16) == 0xff)
        return FX3PROG_ERR_ID;
    if (p->result[2] & 0x1c)
        return FX3PROG_ERR_PROTECTED;

    for (uint32_t a = 0; a < len; a += SECTOR){
        p->result[3] = a;
        pin(LED_GREEN, (a / SECTOR) & 1);
        if ((err = erase(a, d)))
            return err;
    }
    pin(LED_GREEN, 0);
    /* Everything but page 0, verified, before page 0 gets the signature. */
    for (uint32_t a = PAGE; a < len; a += PAGE){
        p->result[3] = a;
        if ((err = program(a, img + a, d)))
            return err;
    }
    if ((err = verify(p, PAGE, len - PAGE, d)))
        return err;
    p->result[3] = 0;
    if ((err = program(0, img, d)))
        return err;
    if ((err = verify(p, 0, PAGE, d)))
        return err;
    p->result[0] = 0xffffffffu;
    return 0;
}

void fx3prog_main(void)
{
    volatile struct fx3prog_param *p = (volatile struct fx3prog_param *)FX3PROG_PARAM;
    int err;

    p->status = FX3PROG_RUNNING;
    if (p->magic != FX3PROG_MAGIC || p->cmd != FX3PROG_FLASH)
        err = FX3PROG_ERR_MAGIC;
    else if (!(err = gpio_setup()))
        err = flash(p, p->delay);
    p->status = err ? FX3PROG_ERR | err : FX3PROG_DONE;

    if (err != FX3PROG_ERR_GPIO){
        for (int i = 0; i < 8; i++){
            if (err)
                leds(i & 1, 0);
            else
                leds(0, 1);
            wait(3000000);
        }
    }
    /* A hard reset reboots from flash: into the new image, or into the
     * bootloader if page 0 is still blank. */
    REG(GCTL_CONTROL) &= ~GCTL_HARD_RESET_N;
    for (;;)
        ;
}
