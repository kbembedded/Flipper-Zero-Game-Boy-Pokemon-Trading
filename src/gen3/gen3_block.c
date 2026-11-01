#include <src/gen3/gen3_block.h>

#include <string.h>

bool gen3_block_send(Gen3Link* link, const uint8_t* data, uint16_t size, uint8_t sender_id) {
    if(!data || size == 0 || size > GEN3_BLOCK_MAX) return false;
    size_t chunks = (size + GEN3_BLOCK_CHUNK_BYTES - 1) / GEN3_BLOCK_CHUNK_BYTES;
    size_t room = GEN3_LINK_QUEUE_LEN - gen3_link_queued(link);
    if(chunks + 1 > room) return false;

    uint16_t cmd[GEN3_CMD_LEN] = {0};
    cmd[0] = GEN3_CMD_INIT_BLOCK;
    cmd[1] = size;
    cmd[2] = (uint16_t)(0x80 + sender_id);
    if(!gen3_link_send(link, cmd)) return false;

    for(size_t c = 0; c < chunks; c++) {
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = GEN3_CMD_CONT_BLOCK;
        for(size_t i = 0; i < 7; i++) {
            size_t off = c * GEN3_BLOCK_CHUNK_BYTES + 2 * i;
            uint16_t lo = off < size ? data[off] : 0;
            uint16_t hi = (off + 1) < size ? data[off + 1] : 0;
            cmd[1 + i] = (uint16_t)(lo | (hi << 8));
        }
        if(!gen3_link_send(link, cmd)) return false; /* unreachable: room was checked */
    }
    return true;
}

void gen3_block_rx_reset(Gen3BlockRx* rx) {
    memset(rx, 0, sizeof(*rx));
}

Gen3BlockRxResult gen3_block_rx_feed(Gen3BlockRx* rx, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(cmd[0] == GEN3_CMD_INIT_BLOCK) {
        if(cmd[1] == 0 || cmd[1] > GEN3_BLOCK_MAX) {
            rx->active = false;
            return Gen3BlockRxBad;
        }
        rx->size = cmd[1];
        rx->pos = 0;
        rx->active = true;
        return Gen3BlockRxStarted;
    }
    if(cmd[0] == GEN3_CMD_CONT_BLOCK) {
        if(!rx->active) return Gen3BlockRxIgnored;
        for(size_t i = 0; i < 7; i++) {
            size_t off = (size_t)rx->pos + 2 * i;
            if(off < GEN3_BLOCK_MAX) rx->data[off] = (uint8_t)(cmd[1 + i] & 0xFF);
            if(off + 1 < GEN3_BLOCK_MAX) rx->data[off + 1] = (uint8_t)(cmd[1 + i] >> 8);
        }
        rx->pos += GEN3_BLOCK_CHUNK_BYTES;
        if(rx->pos >= rx->size) {
            rx->active = false;
            return Gen3BlockRxDone;
        }
        return Gen3BlockRxProgress;
    }
    return Gen3BlockRxIgnored;
}
