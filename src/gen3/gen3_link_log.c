#include <src/gen3/gen3_link_log.h>

#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define LOG_BARRIER() __asm__ volatile("" ::: "memory")
#else
#define LOG_BARRIER() \
    do {              \
    } while(0)
#endif

void gen3_log_init(Gen3LogRing* ring) {
    memset((void*)ring, 0, sizeof(*ring));
}

static bool frame_is_idle(const MlinkTx* tx, const MlinkRx* rx) {
    if(tx->count == 1) {
        return rx->word[0] == MLINK_NO_DATA;
    }
    if(tx->count == MLINK_BURST_LEN) {
        if(rx->timeout_mask != 0 || rx->framing_mask != 0) return false;
        for(uint8_t i = 1; i < MLINK_BURST_LEN; i++) {
            if(tx->word[i] != 0 || rx->word[i] != 0) return false;
        }
        return true;
    }
    return false;
}

void gen3_log_frame(
    Gen3LogRing* ring,
    uint32_t frame,
    uint8_t phase,
    const MlinkTx* tx,
    const MlinkRx* rx) {
    const bool phase_same = ring->have_phase && ring->last_phase == phase;
    ring->have_phase = true;
    ring->last_phase = phase;

    if(phase_same && frame_is_idle(tx, rx)) {
        ring->idle_pending++;
        return;
    }

    const uint32_t head = ring->head;
    if(head - ring->tail >= GEN3_LOG_RING_LEN) {
        ring->dropped++;
        return;
    }

    Gen3LogRec* rec = &ring->rec[head % GEN3_LOG_RING_LEN];
    memset(rec, 0, sizeof(*rec));
    rec->frame = frame;
    rec->idle_before = ring->idle_pending;
    rec->phase = phase;
    rec->count = tx->count > MLINK_BURST_LEN ? MLINK_BURST_LEN : tx->count;
    rec->timeout_mask = rx->timeout_mask;
    rec->framing_mask = rx->framing_mask;
    for(uint8_t i = 0; i < rec->count; i++) {
        rec->tx[i] = tx->word[i];
        rec->rx[i] = rx->word[i];
    }
    ring->idle_pending = 0;

    LOG_BARRIER(); /* record contents must be visible before head is published */
    ring->head = head + 1;
}

bool gen3_log_pop(Gen3LogRing* ring, Gen3LogRec* out) {
    const uint32_t tail = ring->tail;
    if(ring->head == tail) return false;
    LOG_BARRIER(); /* read head before the record */
    *out = ring->rec[tail % GEN3_LOG_RING_LEN];
    LOG_BARRIER(); /* finish copying before the slot is released to the producer */
    ring->tail = tail + 1;
    return true;
}

uint32_t gen3_log_dropped(const Gen3LogRing* ring) {
    return ring->dropped;
}

size_t gen3_log_format(const Gen3LogRec* rec, char* buf, size_t len) {
    size_t pos = 0;
    int n = snprintf(
        buf,
        len,
        "%08lu p%u n%u i%lu T:",
        (unsigned long)rec->frame,
        (unsigned)rec->phase,
        (unsigned)rec->count,
        (unsigned long)rec->idle_before);
    if(n < 0 || (size_t)n >= len) return 0;
    pos = (size_t)n;

    for(uint8_t i = 0; i < rec->count; i++) {
        n = snprintf(buf + pos, len - pos, "%04X ", (unsigned)rec->tx[i]);
        if(n < 0 || (size_t)n >= len - pos) return 0;
        pos += (size_t)n;
    }
    n = snprintf(buf + pos, len - pos, "R:");
    if(n < 0 || (size_t)n >= len - pos) return 0;
    pos += (size_t)n;
    for(uint8_t i = 0; i < rec->count; i++) {
        n = snprintf(buf + pos, len - pos, "%04X ", (unsigned)rec->rx[i]);
        if(n < 0 || (size_t)n >= len - pos) return 0;
        pos += (size_t)n;
    }
    n = snprintf(
        buf + pos,
        len - pos,
        "E:%04X/%04X\n",
        (unsigned)rec->timeout_mask,
        (unsigned)rec->framing_mask);
    if(n < 0 || (size_t)n >= len - pos) return 0;
    return pos + (size_t)n;
}

void gen3_log_drain(Gen3LogRing* ring, Gen3LogWriter writer, void* ctx) {
    Gen3LogRec rec;
    char line[160];
    while(gen3_log_pop(ring, &rec)) {
        if(ring->failed) continue;
        size_t n = gen3_log_format(&rec, line, sizeof(line));
        if(n == 0 || !writer(ctx, line, n)) ring->failed = true;
    }
}

bool gen3_log_failed(const Gen3LogRing* ring) {
    return ring->failed;
}
