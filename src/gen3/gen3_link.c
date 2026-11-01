#include <src/gen3/gen3_link.h>

#include <string.h>

static void handshake_reset(Gen3Link* link) {
    link->connected = false;
    link->prev_count = 0;
    link->last_count = 0;
    link->checksum_valid = false;
    link->checksum = 0;
    memset(link->sent, 0, sizeof(link->sent));
}

void gen3_link_init(Gen3Link* link) {
    memset(link, 0, sizeof(*link));
    link->hs_sent = GEN3_SLAVE_HANDSHAKE;
}

bool gen3_link_connected(const Gen3Link* link) {
    return link->connected;
}

static bool is_handshake_word(uint16_t w) {
    return (w & ~3u) == GEN3_SLAVE_HANDSHAKE || w == GEN3_MASTER_HANDSHAKE;
}

static unsigned popcount16(uint16_t v) {
    unsigned n = 0;
    for(; v; v &= (uint16_t)(v - 1)) n++;
    return n;
}

/* pokeruby DoHandshake player counting over slots {F, G, 0xFFFF, 0xFFFF}. */
static uint8_t handshake_count(const uint16_t slots[4]) {
    uint8_t count = 0;
    for(int i = 0; i < 4; i++) {
        if(is_handshake_word(slots[i])) {
            count++;
        } else {
            if(slots[i] != MLINK_NO_DATA) count = 0;
            break;
        }
    }
    return count;
}

static Gen3LinkEvent handshake_rx(Gen3Link* link, const MlinkRx* rx) {
    const uint16_t slots[4] = {link->hs_sent, rx->word[0], MLINK_NO_DATA, MLINK_NO_DATA};
    uint8_t count = handshake_count(slots);
    bool success = count > 1 && count == link->prev_count && slots[0] == GEN3_MASTER_HANDSHAKE;
    link->prev_count = count;
    link->last_count = count;
    if(success) {
        link->connected = true;
        link->checksum_valid = false;
        link->checksum = 0;
        return Gen3LinkEventConnected;
    }
    return Gen3LinkEventNone;
}

static bool all_words(const uint16_t* w, size_t n, uint16_t v) {
    for(size_t i = 0; i < n; i++) {
        if(w[i] != v) return false;
    }
    return true;
}

static Gen3LinkEvent burst_rx(Gen3Link* link, const MlinkRx* rx, uint16_t cmd_out[GEN3_CMD_LEN]) {
    link->stats.bursts++;
    link->stats.timeouts += popcount16(rx->timeout_mask);
    link->stats.framing_errors += popcount16(rx->framing_mask);

    const uint16_t* g = &rx->word[1];

    if(all_words(rx->word, MLINK_BURST_LEN, MLINK_NO_DATA) ||
       all_words(g, GEN3_CMD_LEN, GEN3_SLAVE_HANDSHAKE)) {
        gen3_link_flush(link);
        handshake_reset(link);
        return Gen3LinkEventDisconnected;
    }

    bool bad = link->checksum_valid && rx->word[0] != link->checksum;

    /* Checksum of this burst: both players' words of transfers #2..#9. */
    uint16_t sum = 0;
    for(size_t i = 0; i < GEN3_CMD_LEN; i++) {
        sum = (uint16_t)(sum + link->sent[i] + g[i]);
    }
    link->checksum = sum;
    link->checksum_valid = true;

    /* Transfer #1 checks the previous burst, not this one: hand this burst's words over anyway. */
    if(bad) {
        memcpy(cmd_out, g, GEN3_CMD_LEN * sizeof(uint16_t));
        link->stats.checksum_errors++;
        return Gen3LinkEventChecksumError;
    }
    if(!all_words(g, GEN3_CMD_LEN, 0)) {
        memcpy(cmd_out, g, GEN3_CMD_LEN * sizeof(uint16_t));
        return Gen3LinkEventCommand;
    }
    return Gen3LinkEventNone;
}

Gen3LinkEvent gen3_link_rx(Gen3Link* link, const MlinkRx* rx, uint16_t cmd_out[GEN3_CMD_LEN]) {
    return link->connected ? burst_rx(link, rx, cmd_out) : handshake_rx(link, rx);
}

void gen3_link_tx(Gen3Link* link, MlinkTx* tx) {
    memset(tx, 0, sizeof(*tx));
    if(!link->connected) {
        link->hs_sent = link->last_count >= 2 ? GEN3_MASTER_HANDSHAKE : GEN3_SLAVE_HANDSHAKE;
        tx->count = 1;
        tx->word[0] = link->hs_sent;
        return;
    }
    tx->count = MLINK_BURST_LEN;
    tx->word[0] = link->checksum;
    if(link->q_len > 0) {
        memcpy(link->sent, link->queue[link->q_head], sizeof(link->sent));
        link->q_head = (uint8_t)((link->q_head + 1) % GEN3_LINK_QUEUE_LEN);
        link->q_len--;
    } else {
        memset(link->sent, 0, sizeof(link->sent));
    }
    memcpy(&tx->word[1], link->sent, sizeof(link->sent));
}

bool gen3_link_send(Gen3Link* link, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(link->q_len >= GEN3_LINK_QUEUE_LEN) return false;
    uint8_t tail = (uint8_t)((link->q_head + link->q_len) % GEN3_LINK_QUEUE_LEN);
    memcpy(link->queue[tail], cmd, GEN3_CMD_LEN * sizeof(uint16_t));
    link->q_len++;
    return true;
}

size_t gen3_link_queued(const Gen3Link* link) {
    return link->q_len;
}

void gen3_link_flush(Gen3Link* link) {
    link->q_head = 0;
    link->q_len = 0;
}

void gen3_link_stats(const Gen3Link* link, Gen3LinkStats* out) {
    *out = link->stats;
}
