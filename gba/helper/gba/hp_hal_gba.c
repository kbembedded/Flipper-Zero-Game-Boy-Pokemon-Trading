#include "hp_hal.h"

/* hp_hal.h on the GBA, with the registers of multiboot-and-save.md §4.7-§4.8 (GBATEK "GBA I/O
 * Map", "GBA Timers"). Interrupts stay off for the helper's whole life, so everything polls. */

#define HP_REG16(a) (*(volatile uint16_t*)(a))
#define HP_TM2CNT_L HP_REG16(0x04000108u)
#define HP_TM2CNT_H HP_REG16(0x0400010Au)
#define HP_TM3CNT_L HP_REG16(0x0400010Cu)
#define HP_TM3CNT_H HP_REG16(0x0400010Eu)
#define HP_BACKDROP HP_REG16(0x05000000u) /* palette entry 0, BGR555 */

#define HP_ROM_BASE 0x08000000u
#define HP_FLASH_BASE 0x0E000000u
#define HP_EWRAM_BASE 0x02000000u /* where the BIOS loaded the image, header first */

/* TM2: prescaler 256 from 65469, overflowing every 67 * 256 cycles = 1.0224 ms; TM3 counts those
 * overflows. */
#define HP_TM2_RELOAD 65469u
#define HP_TM2_START 0x0082u /* enable, prescaler 256 */
#define HP_TM3_START 0x0084u /* enable, count-up */

/* From IWRAM, as the ROM scan's inner loop calls them once per word: ARM to ARM, no veneer. */
HP_IWRAM uint16_t hp_hal_rom16(uint32_t off) {
    return *(volatile const uint16_t*)(HP_ROM_BASE + off);
}

HP_IWRAM uint32_t hp_hal_rom32(uint32_t off) {
    return *(volatile const uint32_t*)(HP_ROM_BASE + off);
}

/* Flash reads, status polls included, must run from RAM (GBATEK "GBA Cart Backup Flash ROM"). */
HP_IWRAM uint8_t hp_hal_flash_read(uint16_t off) {
    return *(volatile const uint8_t*)(HP_FLASH_BASE + off);
}

HP_IWRAM void hp_hal_flash_write(uint16_t off, uint8_t v) {
    *(volatile uint8_t*)(HP_FLASH_BASE + off) = v;
}

void hp_hal_timer_start(void) {
    HP_TM2CNT_H = 0;
    HP_TM3CNT_H = 0;
    HP_TM2CNT_L = HP_TM2_RELOAD;
    HP_TM3CNT_L = 0;
    HP_TM3CNT_H = HP_TM3_START; /* both reload as they start */
    HP_TM2CNT_H = HP_TM2_START;
}

uint16_t hp_hal_timer_units(void) {
    return HP_TM3CNT_L;
}

void hp_hal_id_delay(void) {
    for(volatile uint16_t i = 20000; i != 0; i--) {
    }
}

const uint8_t* hp_hal_own_header(void) {
    return (const uint8_t*)HP_EWRAM_BASE;
}

void hp_hal_status(HpStatus s) {
    switch(s) {
    case HpStatusReady:
        HP_BACKDROP = 0x03E0; /* green */
        break;
    case HpStatusBusy:
        HP_BACKDROP = 0x03FF; /* yellow */
        break;
    case HpStatusWriting:
        HP_BACKDROP = 0x001F; /* red */
        break;
    case HpStatusRefused:
        HP_BACKDROP = 0x7C00; /* blue */
        break;
    }
}
