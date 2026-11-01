#include "hp_link.h"
#include "hp_hal.h"

/* From IWRAM on the GBA: it is the inner loop of every packet, sector and ROM CRC. */
HP_IWRAM uint32_t hp_crc32_bytes(uint32_t reg, const uint8_t* p, uint32_t n) {
    while(n--) {
        reg ^= *p++;
        for(uint8_t i = 0; i < 8; i++) reg = (reg >> 1) ^ (0xEDB88320u & (0u - (reg & 1u)));
    }
    return reg;
}

uint32_t hp_crc32_word(uint32_t reg, uint16_t w) {
    const uint8_t le[2] = {(uint8_t)w, (uint8_t)(w >> 8)};
    return hp_crc32_bytes(reg, le, 2);
}
