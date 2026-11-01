#include <src/gen3/gen3_hlink.h>
#include <string.h>

#define HLINK_CRC_POLY 0xEDB88320u /* IEEE, reflected */
#define HLINK_CRC_INIT 0xFFFFFFFFu

/* Packet words around the payload: SOF, w1, n before it, the CRC's two halves after it */
#define HLINK_HEAD_WORDS 3u
#define HLINK_FRAME_WORDS(n) (HLINK_HEAD_WORDS + (n) + 2u)

uint32_t gen3_crc32_update(uint32_t reg, const uint8_t* p, size_t n) {
    for(size_t i = 0; i < n; i++) {
        reg ^= p[i];
        for(uint8_t bit = 0; bit < 8; bit++) {
            reg = (reg & 1u) ? (reg >> 1) ^ HLINK_CRC_POLY : reg >> 1;
        }
    }
    return reg;
}

uint32_t gen3_crc32(const uint8_t* p, size_t n) {
    return ~gen3_crc32_update(HLINK_CRC_INIT, p, n);
}

/* One packet word into the CRC: its low byte, then its high byte. */
static uint32_t hlink_crc_word(uint32_t reg, uint16_t w) {
    const uint8_t le[2] = {(uint8_t)w, (uint8_t)(w >> 8)};
    return gen3_crc32_update(reg, le, sizeof(le));
}

static uint32_t hlink_add(uint32_t a, uint32_t b) {
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

static bool hlink_settled(Gen3HlinkState s) {
    return s == Gen3HlinkIdle || s == Gen3HlinkDone || s == Gen3HlinkFailed;
}

/* Everything but the request: counters, flags and results. */
static void hlink_reset(Gen3Hlink* h) {
    h->busy_us = 0;
    h->sync_us = 0;
    h->crc = HLINK_CRC_INIT;
    h->pos = 0;
    h->other = 0;
    h->resp_words = 0;
    h->notready = 0;
    h->status = 0;
    h->skip = 0;
    h->run = 0;
    h->nr_run = 0;
    h->ready = false;
    h->busy = false;
    h->wire = false;
    h->too_long = false;
    h->fail = Gen3HlinkFailNone;
}

/* App side: the new state goes out only after everything it covers. */
static void hlink_publish(Gen3Hlink* h, Gen3HlinkState s) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    h->state = s;
}

/* ISR side: Done or Failed, the results first. The pump is finished with req's buffers. */
static void hlink_finish(Gen3Hlink* h, Gen3HlinkState s, Gen3HlinkFail fail) {
    h->fail = fail;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    h->state = s;
}

void gen3_hlink_init(Gen3Hlink* h) {
    memset(h, 0, sizeof(*h));
    h->state = Gen3HlinkIdle;
}

void gen3_hlink_submit(Gen3Hlink* h, const Gen3HlinkReq* req) {
    if(!hlink_settled(h->state)) return;
    h->req = *req;
    hlink_reset(h);
    h->busy_max = req->busy_us ? req->busy_us : GEN3_HLINK_BUSY_US;
    const uint32_t n = (uint32_t)req->nargs + req->data_words;
    if(req->nargs > GEN3_HLINK_MAX_ARGS || n > GEN3_HLINK_MAX_PAYLOAD) {
        h->fail = Gen3HlinkFailTooLong;
        hlink_publish(h, Gen3HlinkFailed);
        return;
    }
    h->n = (uint16_t)n;
    hlink_publish(h, Gen3HlinkSending);
}

void gen3_hlink_sync(Gen3Hlink* h, uint8_t need, uint32_t timeout_us) {
    if(!hlink_settled(h->state)) return;
    hlink_reset(h);
    h->need = need; /* 0 acts as 1: the run is checked after a READY */
    h->sync_max = timeout_us;
    hlink_publish(h, Gen3HlinkSync);
}

/* Request word k: SOF, w1, n, the args, the data words, then the CRC over w1 .. the last data
 * word, which every word before it has gone into, one update each. */
static uint16_t hlink_req_word(Gen3Hlink* h, uint16_t k) {
    const Gen3HlinkReq* r = &h->req;
    const uint16_t crc_at = (uint16_t)(HLINK_HEAD_WORDS + h->n);
    uint16_t w;
    if(k == 0) return GEN3_HLINK_SOF;
    if(k == crc_at) return (uint16_t)~h->crc;
    if(k > crc_at) return (uint16_t)(~h->crc >> 16);
    if(k == 1) {
        w = (uint16_t)(r->seq << 8 | r->code);
    } else if(k == 2) {
        w = h->n;
    } else if(k < HLINK_HEAD_WORDS + r->nargs) {
        w = r->args[k - HLINK_HEAD_WORDS];
    } else {
        const uint8_t* d = r->data + 2u * (k - HLINK_HEAD_WORDS - r->nargs);
        w = (uint16_t)(d[0] | d[1] << 8);
    }
    h->crc = hlink_crc_word(h->crc, w);
    return w;
}

/* A word, while waiting for READY before a request or for the SOF after it, that isn't the one
 * awaited: a valid BUSY, or a not-ready word (the Normal wire's busy helper), starts the count of
 * other words again; `limit` of them in a row fail with `why`. */
static bool hlink_wait_other(
    Gen3Hlink* h,
    uint16_t w,
    bool valid,
    bool notready,
    uint16_t limit,
    Gen3HlinkFail why) {
    if(notready || (valid && w == GEN3_HLINK_BUSY)) {
        h->busy = true;
        h->other = 0;
        return true;
    }
    if(++h->other >= limit) {
        hlink_finish(h, Gen3HlinkFailed, why);
        return false;
    }
    return true;
}

/* A word inside a packet: a reply to a request word after READY, or a response word after its
 * SOF. A delivered word ends the run of not-ready ones; the GEN3_HLINK_NOTREADY_RUN-th in a row
 * fails the packet as Wire at once. False when it did. */
static bool hlink_packet_slot(Gen3Hlink* h, bool notready) {
    if(!notready) {
        h->nr_run = 0;
        return true;
    }
    if(++h->nr_run < GEN3_HLINK_NOTREADY_RUN) return true;
    hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailWire);
    return false;
}

/* Response word h->pos (1 = w1): one CRC update per word, the payload straight into resp, unless
 * it is longer than resp_max_words: then it is read out to its end without being stored. */
static bool hlink_recv(Gen3Hlink* h, uint16_t w, bool valid, bool notready) {
    if(!hlink_packet_slot(h, notready)) return false;
    const uint16_t k = h->pos++;
    if(!valid) {
        h->wire = true;
        if(k == 2) {
            hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailWire); /* no length to read to */
            return false;
        }
    }
    const uint16_t crc_at = (uint16_t)(HLINK_HEAD_WORDS + h->rn);
    if(k == 1) {
        h->rw1 = w;
    } else if(k == 2) {
        if(w > GEN3_HLINK_MAX_PAYLOAD) {
            /* No sound packet has it, so a word went wrong on the wire (multiboot-and-save.md
             * §6.2: drop the packet and hunt again); there is no length to read out to. */
            hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailWire);
            return false;
        }
        h->rn = w;
        h->too_long = w > h->req.resp_max_words;
    } else if(k < crc_at) {
        if(!h->too_long) {
            uint8_t* d = h->req.resp + 2u * (k - HLINK_HEAD_WORDS);
            d[0] = (uint8_t)w;
            d[1] = (uint8_t)(w >> 8);
        }
    } else if(k == crc_at) {
        h->crc_lo = w;
        return true;
    } else {
        /* The last word. A timed-out word reads 0xFFFF, which is also erased flash, so a wire
         * fault wins over a CRC that matches; the seq and the length count only in a packet that
         * is sound, and a sound one for another seq is a stale response, not a long one. */
        if(h->wire) {
            hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailWire);
        } else if((h->crc_lo | (uint32_t)w << 16) != ~h->crc) {
            hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailBadCrc);
        } else if((uint8_t)(h->rw1 >> 8) != h->req.seq) {
            hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailSeq);
        } else if(h->too_long) {
            hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailTooLong);
        } else {
            h->status = (uint8_t)h->rw1;
            h->resp_words = h->rn;
            hlink_finish(h, Gen3HlinkDone, Gen3HlinkFailNone);
        }
        return false;
    }
    h->crc = hlink_crc_word(h->crc, w);
    return true;
}

/* One reply word, in order; false when the rest of the frame doesn't matter. A not-ready word is
 * never valid: BUSY while waiting, a broken run in Sync, a wire error in a packet. */
static bool hlink_word(Gen3Hlink* h, uint16_t w, bool valid, bool notready) {
    switch(h->state) {
    case Gen3HlinkSync:
        if(!valid || w != GEN3_HLINK_READY) {
            h->run = 0;
        } else if(++h->run >= h->need) {
            hlink_finish(h, Gen3HlinkDone, Gen3HlinkFailNone);
            return false;
        }
        return true;
    case Gen3HlinkSending:
        /* Replies to request words: only a run of not-ready ones matters */
        if(h->ready) return hlink_packet_slot(h, notready);
        if(valid && w == GEN3_HLINK_READY) {
            h->ready = true; /* the request goes out from the next frame */
            return false;
        }
        return hlink_wait_other(
            h, w, valid, notready, GEN3_HLINK_NOT_READY_XFERS, Gen3HlinkFailNoReady);
    case Gen3HlinkWaiting:
        if(valid && w == GEN3_HLINK_SOF) {
            h->state = Gen3HlinkReceiving;
            h->pos = 1;
            h->rn = 0;
            h->too_long = false;
            h->nr_run = 0;
            h->crc = HLINK_CRC_INIT;
            return true;
        }
        return hlink_wait_other(
            h, w, valid, notready, GEN3_HLINK_NO_RESPONSE_XFERS, Gen3HlinkFailNoResponse);
    case Gen3HlinkReceiving:
        return hlink_recv(h, w, valid, notready);
    case Gen3HlinkIdle:
    case Gen3HlinkDone:
    case Gen3HlinkFailed:
        break;
    }
    return false;
}

/* After the frame's words, so that a response or a run that completes in the last frame wins. */
static void hlink_time(Gen3Hlink* h, uint32_t elapsed_us) {
    switch(h->state) {
    case Gen3HlinkSync:
        h->sync_us = hlink_add(h->sync_us, elapsed_us);
        if(h->sync_us >= h->sync_max) hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailNoReady);
        break;
    case Gen3HlinkSending:
    case Gen3HlinkWaiting:
        if(h->ready || !h->busy) break;
        h->busy_us = hlink_add(h->busy_us, elapsed_us);
        if(h->busy_us > h->busy_max) {
            hlink_finish(h, Gen3HlinkFailed, Gen3HlinkFailBusyTimeout);
        }
        break;
    case Gen3HlinkIdle:
    case Gen3HlinkReceiving:
    case Gen3HlinkDone:
    case Gen3HlinkFailed:
        break;
    }
}

static void hlink_send(Gen3Hlink* h, MlinkTx* tx) {
    if(hlink_settled(h->state)) {
        tx->count = 0; /* idle ticks */
        return;
    }
    uint8_t k = 0;
    if(h->state == Gen3HlinkSending && h->ready) {
        const uint16_t total = (uint16_t)HLINK_FRAME_WORDS(h->n);
        while(k < MLINK_BURST_LEN && h->pos < total) {
            tx->word[k++] = hlink_req_word(h, h->pos++);
        }
        if(h->pos == total) {
            /* The replies to this frame's request words are skipped; the response logic takes
             * the rest. */
            h->skip = k;
            h->ready = false;
            h->busy = false;
            h->busy_us = 0;
            h->other = 0;
            h->state = Gen3HlinkWaiting;
        }
    }
    while(k < MLINK_BURST_LEN) tx->word[k++] = GEN3_HLINK_IDLE;
    tx->count = MLINK_BURST_LEN;
}

void gen3_hlink_frame(Gen3Hlink* h, const MlinkRx* rx, MlinkTx* tx, uint32_t elapsed_us) {
    if(hlink_settled(h->state)) {
        tx->count = 0;
        return;
    }
    const uint8_t count = !rx ? 0 : rx->count < MLINK_BURST_LEN ? rx->count : MLINK_BURST_LEN;
    const uint16_t notready = rx ? (uint16_t)(rx->notready_mask & ((1u << count) - 1u)) : 0;
    const uint16_t bad = rx ? (uint16_t)(rx->timeout_mask | rx->framing_mask | notready) : 0;
    for(uint16_t m = notready; m && h->notready < UINT16_MAX; m &= (uint16_t)(m - 1u)) {
        h->notready++;
    }
    for(uint8_t t = 0; t < count; t++) {
        const bool nr = (notready >> t) & 1u;
        if(t < h->skip) {
            /* a reply to one of the request's last words: still inside the packet */
            if(!hlink_packet_slot(h, nr)) break;
            continue;
        }
        if(!hlink_word(h, rx->word[t], !((bad >> t) & 1u), nr)) break;
    }
    h->skip = 0;
    hlink_time(h, elapsed_us);
    hlink_send(h, tx);
}

Gen3HlinkState gen3_hlink_state(const Gen3Hlink* h) {
    const Gen3HlinkState s = h->state;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return s;
}

void gen3_hlink_result(
    const Gen3Hlink* h,
    uint8_t* status,
    uint16_t* resp_words,
    Gen3HlinkFail* fail) {
    *status = h->status;
    *resp_words = h->resp_words;
    *fail = h->fail;
}

uint16_t gen3_hlink_notready(const Gen3Hlink* h) {
    return h->notready;
}
