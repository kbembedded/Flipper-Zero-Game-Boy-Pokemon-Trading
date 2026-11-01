#pragma once

#include "test.h"
#include <src/gen3/gen3_link.h>

/* Shared link-layer test helpers. */

static MlinkRx make_rx(const MlinkTx* tx, uint16_t w0) {
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = tx->count;
    rx.word[0] = w0;
    return rx;
}

/* One handshake frame: tx, then rx with G answering `g`. */
static Gen3LinkEvent handshake_frame(Gen3Link* l, uint16_t g, uint16_t* sent) {
    MlinkTx tx;
    uint16_t cmd[GEN3_CMD_LEN];
    gen3_link_tx(l, &tx);
    if(sent) *sent = tx.word[0];
    MlinkRx rx = make_rx(&tx, g);
    return gen3_link_rx(l, &rx, cmd);
}

static void connect(Gen3Link* l) {
    uint16_t s;
    handshake_frame(l, 0x0000, &s);
    handshake_frame(l, 0xB9A0, &s);
    ASSERT_EQ(handshake_frame(l, 0xB9A0, &s), Gen3LinkEventConnected);
}

/* One burst frame: tx, then G answers with g[0..8] (NULL = zeros). */
static Gen3LinkEvent
    burst(Gen3Link* l, MlinkTx* tx, const uint16_t* g, uint16_t cmd[GEN3_CMD_LEN]) {
    gen3_link_tx(l, tx);
    MlinkRx rx = make_rx(tx, 0);
    if(g) memcpy(rx.word, g, sizeof(rx.word));
    return gen3_link_rx(l, &rx, cmd);
}
