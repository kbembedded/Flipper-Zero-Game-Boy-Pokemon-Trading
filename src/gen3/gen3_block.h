#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/gen3_link.h>

/* pokeruby-compatible block transfer (0xBBBB header + 0x8888 chunks). Pure C, no heap. */

#define GEN3_CMD_INIT_BLOCK    0xBBBB
#define GEN3_CMD_CONT_BLOCK    0x8888
#define GEN3_BLOCK_MAX         256
#define GEN3_BLOCK_CHUNK_BYTES 14

/* Queues the header {BBBB, size, 0x80+sender_id, 0...} plus ceil(size/14) chunks {8888, w0..w6},
 * little-endian. All or nothing: returns false and queues nothing if size==0,
 * size>GEN3_BLOCK_MAX, or the queue lacks room. */
bool gen3_block_send(Gen3Link* link, const uint8_t* data, uint16_t size, uint8_t sender_id);

typedef struct {
    uint8_t data[GEN3_BLOCK_MAX];
    uint16_t size;
    uint16_t pos;
    bool active;
} Gen3BlockRx;

typedef enum {
    Gen3BlockRxIgnored,
    Gen3BlockRxStarted,
    Gen3BlockRxProgress,
    Gen3BlockRxDone,
    Gen3BlockRxBad,
} Gen3BlockRxResult;

void gen3_block_rx_reset(Gen3BlockRx* rx);
/* Feed every received command. Non-block commands -> Ignored. Done when pos >= size. */
Gen3BlockRxResult gen3_block_rx_feed(Gen3BlockRx* rx, const uint16_t cmd[GEN3_CMD_LEN]);
