#pragma once

#include <stdint.h>

/* Link words and the packet CRC of the Flipper <-> helper protocol (multiboot-and-save.md
 * §6.1-§6.2). The Flipper is the parent (P), the helper child 1 (H). */

#define HP_SOF 0x55AAu /* w0 of every packet */
#define HP_READY 0xB0A0u /* H is hunting for a request */
#define HP_BUSY 0xB0B0u /* H is executing one: P sends idle words only */
#define HP_RECV 0xB0C0u /* H is receiving a packet */
#define HP_MAX_PAYLOAD 2064u /* payload words in one packet, either way */

/* CRC-32 (IEEE: reflected polynomial 0xEDB88320). The caller starts the register at 0xFFFFFFFF
 * and finishes with ~reg; check("123456789") = 0xCBF43926. A packet's CRC covers w1 .. its last
 * payload word, each word as its low byte, then its high byte. */
uint32_t hp_crc32_word(uint32_t reg, uint16_t w);
uint32_t hp_crc32_bytes(uint32_t reg, const uint8_t* p, uint32_t n);
