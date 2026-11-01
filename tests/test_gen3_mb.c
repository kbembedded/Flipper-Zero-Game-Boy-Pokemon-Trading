#include "test.h"
#include "fake_bios.h"
#include <stdbool.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/mlink_types.h>

/* The BIOS slave (fake_bios, child 1) and the sender (gen3_mb) on one simulated stream-mode
 * link, on one wire: each driver frame is fake_bios_frame then gen3_mb_frame, timed by the
 * frame's rx.ticks, as the driver reports it. */

#define BENCH_LOG_MAX 100000u
#define BENCH_SEED 0x2468ACE1u

typedef struct {
    MlinkTx tx; /* what the sender asked for */
    uint32_t us; /* that frame's time */
    uint16_t reply; /* the first word's reply */
    bool replied; /* the first word came back unmarked */
    uint16_t notready; /* the frame's notready_mask (Normal wire) */
    Gen3MbPhase phase; /* the sender's phase after the frame: the one that made the next tx */
} BenchFrame;

static BenchFrame bench_log[BENCH_LOG_MAX];
static uint32_t bench_n;
static MlinkTx bench_pending;
static bool bench_started;
static Gen3Mb mb;
static FakeBios bios;
static uint8_t image[GEN3_MB_MAX_LEN + 0x10];

/* A valid image: the logo's first word, the rest of 0x04..0x9F zero, 0x96, version 1 and the
 * complement; payload bytes from an LCG. */
static void make_image(uint8_t* buf, uint32_t len) {
    memset(buf, 0, GEN3_MB_HEADER_LEN);
    buf[4] = 0x24;
    buf[5] = 0xFF;
    buf[6] = 0xAE;
    buf[7] = 0x51;
    buf[0xB2] = 0x96;
    buf[0xBC] = 1;
    uint8_t chk = 0;
    for(uint32_t i = 0xA0; i <= 0xBC; i++) chk -= buf[i];
    buf[0xBD] = (uint8_t)(chk - 0x19);
    uint32_t x = len;
    for(uint32_t i = GEN3_MB_HEADER_LEN; i < len; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (uint8_t)(x >> 16);
    }
}

static void bench_init_on(MlinkWire wire, uint32_t len, uint8_t cc, uint8_t rr) {
    make_image(image, len);
    gen3_mb_init(&mb, wire, image, len, BENCH_SEED);
    fake_bios_init(&bios, cc, rr);
    fake_bios_set_wire(&bios, wire);
    bench_n = 0;
    bench_started = false;
}

static void bench_init(uint32_t len, uint8_t cc, uint8_t rr) {
    bench_init_on(MlinkWireMulti, len, cc, rr);
}

/* The sender's frame callback, as the driver runs it: the next frame's upper halves zeroed
 * first. */
static void bench_frame(Gen3Mb* m, const MlinkRx* rx, uint32_t us) {
    memset(bench_pending.hi, 0, sizeof(bench_pending.hi));
    gen3_mb_frame(m, rx, &bench_pending, us);
}

static Gen3MbStatus status(void) {
    Gen3MbStatus st;
    gen3_mb_status(&mb, &st);
    return st;
}

/* Runs at most max_frames driver frames, recording each; stops at Done or Failed. A later call
 * continues where the last one stopped. */
static void run(Gen3Mb* m, FakeBios* b, uint32_t max_frames) {
    if(!bench_started) {
        bench_frame(m, NULL, 0);
        bench_started = true;
    }
    for(uint32_t i = 0; i < max_frames; i++) {
        Gen3MbStatus st;
        gen3_mb_status(m, &st);
        if(st.phase == Gen3MbPhaseDone || st.phase == Gen3MbPhaseFailed) return;
        MlinkRx rx;
        const MlinkTx sent = bench_pending;
        fake_bios_frame(b, &sent, &rx);
        const uint32_t us = (uint32_t)rx.ticks * MLINK_TICK_US;
        bench_frame(m, &rx, us);
        gen3_mb_status(m, &st);
        if(bench_n < BENCH_LOG_MAX) {
            bench_log[bench_n++] = (BenchFrame){
                .tx = sent,
                .us = us,
                .reply = rx.word[0],
                .replied = rx.count && !((rx.timeout_mask | rx.framing_mask) & 1u),
                .notready = rx.notready_mask,
                .phase = st.phase};
        }
    }
}

/* Runs until the sender is in phase; false if it ends (Done, Failed) or max_frames pass first. */
static bool run_until(Gen3MbPhase phase, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames && status().phase != phase; i++) {
        if(status().phase == Gen3MbPhaseDone || status().phase == Gen3MbPhaseFailed) break;
        run(&mb, &bios, 1);
    }
    return status().phase == phase;
}

/* The frame in flight never reaches the fake (lost on the wire); its first word replies reply. */
static void inject(uint16_t reply) {
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = bench_pending.count;
    rx.word[0] = reply;
    gen3_mb_frame(&mb, &rx, &bench_pending, mlink_frame_us(MlinkModeStream, rx.count));
}

static bool is_frame(const BenchFrame* f, uint16_t word) {
    return f->tx.count == 1 && f->tx.word[0] == word;
}

/* Time from the start of the first header frame to the end of the logged run. */
static uint64_t us_since_first_header(void) {
    uint64_t us = 0;
    bool started = false;
    for(uint32_t i = 1; i < bench_n; i++) {
        if(bench_log[i - 1].phase == Gen3MbPhaseHeader) started = true;
        if(started) us += bench_log[i].us;
    }
    return us;
}

/* Time from the end of the frame whose failure first started a Wait to the end of the run. */
static uint64_t us_since_first_wait(void) {
    uint64_t us = 0;
    bool started = false;
    for(uint32_t i = 0; i < bench_n; i++) {
        if(started) us += bench_log[i].us;
        if(bench_log[i].phase == Gen3MbPhaseWait) started = true;
    }
    return us;
}

/* After Done or Failed the sender only sends idle ticks. */
static bool idles_forever(void) {
    for(int i = 0; i < 50; i++) {
        MlinkRx rx;
        memset(&rx, 0, sizeof(rx));
        rx.count = bench_pending.count;
        gen3_mb_frame(&mb, &rx, &bench_pending, MLINK_TICK_US);
        if(bench_pending.count != 0) return false;
    }
    return true;
}

static bool image_received(uint32_t len) {
    uint32_t got = 0;
    const uint8_t* img = fake_bios_image(&bios, &got);
    return got == len && memcmp(img, image, len) == 0;
}

TEST(test_mlink_frame_us) {
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 0), 1073);
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 1), 1073);
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 9), 9657);
    ASSERT_EQ(mlink_frame_us(MlinkModeStream, 255), 273615);
    ASSERT_EQ(mlink_frame_us(MlinkModeFrame, 1), 16740);
    ASSERT_EQ(mlink_frame_us(MlinkModeFrame, 9), 16740);
}

TEST(test_mb_crc_step) {
    ASSERT_EQ(gen3_mb_crc_step(0xFFF8, 0, 0xA517), 0x0749);
    ASSERT_EQ(gen3_mb_crc_step(0xFFF8, 0xFFFFFFFFu, 0xA517), 0x8723);
}

/* multiboot-and-save.md §2.3's test vector. */
static const uint16_t doc_first[8] =
    {0x3525, 0x4645, 0x0D28, 0xFD10, 0xCF51, 0x4E63, 0xC51C, 0x2615};
static const uint16_t doc_last[4] = {0xDAF9, 0x5C4D, 0x4DC4, 0xA30A};

static void doc_vector_halfwords(uint16_t out[128], uint16_t* crc) {
    uint8_t payload[0x100];
    for(int i = 0; i < 0x100; i++) payload[i] = (uint8_t)i;
    Gen3MbCrypt k;
    gen3_mb_crypt_init(&k, MlinkWireMulti, GEN3_MB_PALETTE, 0x5A);
    for(uint32_t i = 0; i < 64; i++) {
        const uint8_t* p = payload + 4 * i;
        const uint32_t w = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                           (uint32_t)p[3] << 24;
        const uint32_t e = gen3_mb_crypt_word(&k, 0xC0 + 4 * i, w);
        out[2 * i] = (uint16_t)e;
        out[2 * i + 1] = (uint16_t)(e >> 16);
    }
    *crc = gen3_mb_crypt_final(&k, gen3_mb_hh(0x5A), 0xC3);
}

TEST(test_mb_doc_vector) {
    ASSERT_EQ(gen3_mb_hh(0x5A), 0x69);
    ASSERT_EQ((0x1C0 - 0x190) / 4, 0x000C);
    Gen3MbCrypt k;
    gen3_mb_crypt_init(&k, MlinkWireMulti, 0x93, 0x5A);
    ASSERT_EQ(k.seed, 0xFFFF5A93u);
    ASSERT_EQ(k.crc, 0xFFF8);
    uint16_t hw[128];
    uint16_t crc;
    doc_vector_halfwords(hw, &crc);
    for(int i = 0; i < 8; i++) ASSERT_EQ(hw[i], doc_first[i]);
    for(int i = 0; i < 4; i++) ASSERT_EQ(hw[124 + i], doc_last[i]);
    ASSERT_EQ(crc, 0x2A28);
}

TEST(test_mb_image_check) {
    make_image(image, 0x1C0);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageOk);
    make_image(image, 0x2010);
    ASSERT_EQ(gen3_mb_image_check(image, 0x2010, 1), Gen3MbImageOk);
    make_image(image, 0x3000);
    ASSERT_EQ(gen3_mb_image_check(image, 0x3000, 1), Gen3MbImageOk);
    make_image(image, 0x3010);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1B0, 1), Gen3MbImageBadLen);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C8, 1), Gen3MbImageBadLen);
    ASSERT_EQ(gen3_mb_image_check(image, 0x3010, 1), Gen3MbImageBadLen);
    make_image(image, 0x1C0);
    image[0xB2] = 0;
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageBadHeader);
    make_image(image, 0x1C0);
    image[0xBD]++;
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageBadHeader);
    make_image(image, 0x1C0);
    image[4] ^= 1;
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 1), Gen3MbImageBadHeader);
    make_image(image, 0x1C0);
    ASSERT_EQ(gen3_mb_image_check(image, 0x1C0, 2), Gen3MbImageBadVersion);
}

TEST(test_mb_boot) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 20000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(st.cc, 0x5A);
    ASSERT_EQ(st.rr, 0xC3);
    ASSERT_EQ(st.total, 0x1000 - GEN3_MB_HEADER_LEN);
    ASSERT_EQ(st.sent, st.total);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosBooted);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(image_received(0x1000));
    for(uint32_t i = 0; i < bench_n; i++) ASSERT_TRUE(bench_log[i].tx.count <= 9);
    ASSERT_TRUE(idles_forever());
}

TEST(test_mb_waits_for_gba) {
    bench_init(0x1000, 0x5A, 0xC3);
    run(&mb, &bios, 4000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDetect);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_TRUE(st.waiting_us >= 3000000u);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.waiting_us, 0);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
}

TEST(test_mb_length_delay) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 20000);
    ASSERT_EQ(status().phase, Gen3MbPhaseDone);
    const uint16_t handshake = 0x6400 | gen3_mb_hh(0x5A);
    const uint16_t length = (0x1000 - 0x190) / 4;
    uint32_t i = 0;
    while(i < bench_n && !(bench_log[i].tx.count == 1 && bench_log[i].tx.word[0] == handshake))
        i++;
    ASSERT_TRUE(i < bench_n);
    uint32_t idle_us = 0;
    for(i++; i < bench_n && bench_log[i].tx.count == 0; i++) idle_us += bench_log[i].us;
    ASSERT_TRUE(i < bench_n);
    ASSERT_EQ(bench_log[i].tx.count, 1);
    ASSERT_EQ(bench_log[i].tx.word[0], length);
    ASSERT_TRUE(idle_us >= 100000u);
}

/* Detect and Palette poll back to back, one word per tick, as afska's LinkCableMultiboot does;
 * Final's 0x0065 retries come at most once per GBA frame, with idle ticks between, as afska's
 * do (once per VBlank). */
TEST(test_mb_poll_spacing) {
    ASSERT_EQ(GEN3_MB_POLL_US, MLINK_FRAME_US);
    bench_init(0x1000, 0x5A, 0xC3);
    run(&mb, &bios, 300);
    fake_bios_set_busy(&bios, 50000, 3500000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    ASSERT_EQ(status().phase, Gen3MbPhaseDone);
    uint32_t detect = 0;
    uint32_t palette = 0;
    uint32_t final = 0;
    uint64_t now = 0;
    uint64_t last_final = 0;
    for(uint32_t i = 0; i < bench_n; i++) {
        const Gen3MbPhase from = i ? bench_log[i - 1].phase : Gen3MbPhaseDetect;
        const BenchFrame* f = &bench_log[i];
        if(from == Gen3MbPhaseDetect) {
            ASSERT_TRUE(is_frame(f, 0x6200));
            detect++;
        } else if(from == Gen3MbPhasePalette) {
            ASSERT_TRUE(is_frame(f, 0x6300 | GEN3_MB_PALETTE));
            palette++;
        } else if(from == Gen3MbPhaseFinal && f->tx.count) {
            ASSERT_TRUE(is_frame(f, 0x0065));
            if(final) ASSERT_TRUE(now - last_final >= GEN3_MB_POLL_US);
            last_final = now;
            final++;
        }
        now += f->us;
    }
    /* 300 frames off, then 2 timeouts, 0x0000 and 0x7202; 50 ms busy; 3.5 s busy */
    ASSERT_TRUE(detect >= 300 + 4);
    ASSERT_TRUE(palette >= 40);
    ASSERT_TRUE(final >= 200);
}

/* A slow slave: at least 40 busy 0x7202 replies to the palette word, and 200 busy 0x0074 replies
 * to 0x0065. */
TEST(test_mb_busy_slave) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, 50000, 3500000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(image_received(0x1000));
    uint32_t busy_palette = 0;
    uint32_t busy_final = 0;
    for(uint32_t i = 0; i < bench_n; i++) {
        const BenchFrame* f = &bench_log[i];
        if(is_frame(f, 0x6300 | GEN3_MB_PALETTE) && f->replied && f->reply == 0x7202) {
            busy_palette++;
        }
        if(is_frame(f, 0x0065) && f->replied && f->reply == 0x0074) busy_final++;
    }
    ASSERT_TRUE(busy_palette >= 40);
    ASSERT_TRUE(busy_final >= 200);
}

/* The valid 0x7202 replies to the palette word in the last palette wait of the run logged */
static uint32_t palette_busy_last(void) {
    uint32_t busy = 0;
    for(uint32_t i = 1; i < bench_n; i++) {
        if(bench_log[i - 1].phase != Gen3MbPhasePalette) continue;
        /* a new palette wait: its first poll */
        if(i < 2 || bench_log[i - 2].phase != Gen3MbPhasePalette) busy = 0;
        const BenchFrame* f = &bench_log[i];
        if(is_frame(f, 0x6300 | GEN3_MB_PALETTE) && f->replied && f->reply == 0x7202) busy++;
    }
    return busy;
}

/* What the cart log records for the hardware check of the budgets: the slave's two bytes, the
 * busy replies of the last palette wait, the time from the first 0x0065 to the 0x0075 (the frames
 * in between, idle ones too), and every restart wait, counted or not */
TEST(test_mb_status_detail) {
    bench_init(0x1000, 0x5A, 0xC3);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.cc, 0);
    ASSERT_EQ(st.rr, 0);
    ASSERT_EQ(st.palette_busy, 0);
    ASSERT_EQ(st.final_us, 0);
    ASSERT_EQ(st.all_restarts, 0);
    fake_bios_set_busy(&bios, 50000, 3500000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.cc, 0x5A);
    ASSERT_EQ(st.rr, 0xC3);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 0);
    ASSERT_TRUE(palette_busy_last() >= 40);
    ASSERT_EQ(st.palette_busy, palette_busy_last());
    uint32_t i = 0;
    while(i < bench_n && !is_frame(&bench_log[i], 0x0065)) i++;
    ASSERT_TRUE(i < bench_n);
    uint64_t final_us = 0;
    for(; i < bench_n; i++) {
        final_us += bench_log[i].us;
        const BenchFrame* f = &bench_log[i];
        if(is_frame(f, 0x0065) && f->replied && f->reply == 0x0075) break;
    }
    ASSERT_TRUE(i < bench_n);
    ASSERT_TRUE(final_us >= 3500000u);
    ASSERT_EQ(st.final_us, final_us);

    /* A palette wait whose slave resets in it counts; the next one starts its count afresh */
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, 1000000, 0);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhasePalette, 1000));
    run(&mb, &bios, 300);
    ASSERT_TRUE(status().palette_busy >= 250);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.all_restarts, 1);
    ASSERT_EQ(st.palette_busy, palette_busy_last());
    ASSERT_TRUE(st.palette_busy < 1000000u / MLINK_TICK_US + 2u);
}

/* The palette budget, one 0x63pp per tick: the last poll it allows is the one whose reply comes
 * at or past GEN3_MB_PALETTE_US. A slave whose 0x73cc comes on that poll is taken; one busy for
 * one poll longer times the wait out. That wait saw only valid 0x7202 replies, so it is not
 * counted. This fake is busy exactly that long on every attempt, so each attempt times out the
 * same way, and the attempt budget ends the fourth one's successor. */
TEST(test_mb_palette_timeout) {
    const uint32_t polls = (GEN3_MB_PALETTE_US + MLINK_TICK_US - 1u) / MLINK_TICK_US;
    ASSERT_TRUE(GEN3_MB_PALETTE_US >= 3500000u && GEN3_MB_PALETTE_US <= 4500000u);
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, (polls - 1u) * MLINK_TICK_US, 0);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);

    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, polls * MLINK_TICK_US, 0);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(fake_bios_boots(&bios), 0);
    uint32_t timeouts = 0;
    for(uint32_t i = 0; i < bench_n; i++) {
        const BenchFrame* f = &bench_log[i];
        if(is_frame(f, 0x6300 | GEN3_MB_PALETTE)) ASSERT_EQ(f->reply, 0x7202);
        if(i && bench_log[i - 1].phase == Gen3MbPhasePalette && f->phase == Gen3MbPhaseWait) {
            timeouts++;
        }
    }
    /* none counted, each a restart all the same; the attempt budget ends it */
    ASSERT_EQ(timeouts, 4);
    ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 4);
    ASSERT_EQ(st.last_fail, Gen3MbFailPowerCycle);
}

/* A slave that never stops answering 0x7202 to the palette word. Each palette wait times out
 * uncounted. The restart's first Detect reply is that same 0x7202, loaded before the wait: it is
 * stale and ignored, so it never starts a Hello that fails. The attempt budget ends it with a
 * power-cycle request and nothing counted. */
TEST(test_mb_palette_stuck_busy) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, UINT32_MAX, 0);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailPowerCycle);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
    uint32_t timeouts = 0;
    uint32_t hellos = 0;
    for(uint32_t i = 1; i < bench_n; i++) {
        const Gen3MbPhase from = bench_log[i - 1].phase;
        if(from == Gen3MbPhasePalette && bench_log[i].phase == Gen3MbPhaseWait) timeouts++;
        if(from == Gen3MbPhaseHello) {
            ASSERT_EQ(bench_log[i].phase, Gen3MbPhaseHeader);
            hellos++;
        }
    }
    ASSERT_EQ(timeouts, 4);
    ASSERT_EQ(hellos, 5);
    ASSERT_TRUE(us_since_first_header() >= GEN3_MB_ATTEMPT_US);
}

/* A palette wait that saw anything but valid 0x7202 replies counts: here the GBA resets in the
 * middle of it (two timeouts, then 0x0000). */
TEST(test_mb_palette_reset_counts) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, 1000000, 0);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhasePalette, 1000));
    run(&mb, &bios, 10);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailPalette);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
}

/* No 0x0075 within GEN3_MB_FINAL_US is a counted Final failure. Four of those don't fit in the
 * attempt budget, so the budget ends it. */
TEST(test_mb_final_timeout) {
    bench_init(0x1C0, 0x5A, 0xC3);
    fake_bios_set_busy(&bios, 0, 6000000);
    fake_bios_power_on(&bios);
    for(uint32_t i = 0; i < 100000 && status().restarts == 0; i++) run(&mb, &bios, 1);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailFinal);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 3);
    ASSERT_EQ(st.all_restarts, 3);
    ASSERT_EQ(st.last_fail, Gen3MbFailPowerCycle);
    ASSERT_EQ(st.final_us, 0); /* no 0x0075 came */
    ASSERT_EQ(fake_bios_boots(&bios), 0);
}

/* The attempt budget, from the first header word: a corrupted reply to 0x64hh leaves the slave
 * waiting for its length word, so the restart's 0x6200 becomes a length of 0x18990 bytes and the
 * slave drains it for minutes. The sender gives up within the budget and asks for a power
 * cycle. */
TEST(test_mb_attempt_budget) {
    ASSERT_TRUE(GEN3_MB_ATTEMPT_US >= 15000000u && GEN3_MB_ATTEMPT_US <= 25000000u);
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 105); /* 2 timeouts, 0x0000, 0x7202, Hello, 96 + 2, palette */
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailPowerCycle);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
    bool handshake_failed = false;
    for(uint32_t i = 1; i < bench_n; i++) {
        if(bench_log[i - 1].phase == Gen3MbPhaseHandshake &&
           bench_log[i].phase == Gen3MbPhaseWait) {
            handshake_failed = true;
        }
    }
    ASSERT_TRUE(handshake_failed);
    const uint64_t us = us_since_first_header();
    ASSERT_TRUE(us >= GEN3_MB_ATTEMPT_US);
    ASSERT_TRUE(us < GEN3_MB_ATTEMPT_US + MLINK_BURST_LEN * MLINK_TICK_US);
}

/* The budget is checked after the frame's reply: a CRC that matches in the frame that uses up the
 * budget still ends Done. One that doesn't is a counted CRC failure, and then the budget ends it
 * with PowerCycle instead of a restart. The CRC frame is given exactly what is left of the
 * budget. */
TEST(test_mb_crc_at_budget_end) {
    for(int bad = 0; bad <= 1; bad++) {
        bench_init(0x1C0, 0x5A, 0xC3);
        fake_bios_force_bad_crc(&bios, bad);
        fake_bios_power_on(&bios);
        ASSERT_TRUE(run_until(Gen3MbPhaseCrc, 10000));
        const uint64_t used = us_since_first_header();
        ASSERT_TRUE(used < GEN3_MB_ATTEMPT_US);
        MlinkRx rx;
        fake_bios_frame(&bios, &bench_pending, &rx);
        gen3_mb_frame(&mb, &rx, &bench_pending, (uint32_t)(GEN3_MB_ATTEMPT_US - used));
        Gen3MbStatus st = status();
        if(bad) {
            ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
            ASSERT_EQ(st.restarts, 1);
            ASSERT_EQ(st.last_fail, Gen3MbFailPowerCycle);
            ASSERT_EQ(fake_bios_boots(&bios), 0);
        } else {
            ASSERT_EQ(st.phase, Gen3MbPhaseDone);
            ASSERT_EQ(st.restarts, 0);
            ASSERT_EQ(st.last_fail, Gen3MbFailNone);
            ASSERT_EQ(fake_bios_boots(&bios), 1);
        }
    }
}

/* One early data glitch recovers to Done: within 15 s of the glitch for an 8 KB image, and inside
 * the attempt budget for the largest one. */
TEST(test_mb_glitch_recovers_in_budget) {
    bench_init(0x2000, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 110); /* data halfword 3 */
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailData);
    ASSERT_TRUE(image_received(0x2000));
    ASSERT_TRUE(us_since_first_wait() <= 15000000u);

    bench_init(GEN3_MB_MAX_LEN, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 110);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailData);
    ASSERT_TRUE(image_received(GEN3_MB_MAX_LEN));
    ASSERT_TRUE(us_since_first_header() < GEN3_MB_ATTEMPT_US);
}

/* Waiting for the GBA to be powered on doesn't count against the attempt budget. */
TEST(test_mb_late_power_on) {
    bench_init(0x1000, 0x5A, 0xC3);
    run(&mb, &bios, 30000000u / MLINK_TICK_US);
    ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
}

TEST(test_mb_header_glitch) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 50);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.all_restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailHeader);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1000));
    /* The restart wait (multiboot-and-save.md §2.4): 70 ms plus up to 170 ms of idle ticks, at
     * least GBATEK's 1/16 s, then detection polls again. */
    uint32_t i = 0;
    while(i < bench_n && bench_log[i].phase != Gen3MbPhaseWait) i++;
    ASSERT_TRUE(i < bench_n);
    uint32_t idle_us = 0;
    for(i++; i < bench_n && bench_log[i].tx.count == 0; i++) idle_us += bench_log[i].us;
    ASSERT_TRUE(i < bench_n);
    ASSERT_EQ(bench_log[i].tx.word[0], 0x6200);
    ASSERT_TRUE(idle_us >= 70000u);
    ASSERT_TRUE(idle_us < 70000u + 170000u + MLINK_TICK_US);
}

TEST(test_mb_data_glitch) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 1000);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailData);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1000));
}

TEST(test_mb_bad_crc_gives_up) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_force_bad_crc(&bios, true);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
    ASSERT_EQ(st.restarts, 4);
    ASSERT_EQ(st.last_fail, Gen3MbFailCrc);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosWaiting);
    ASSERT_TRUE(idles_forever());
}

/* A word the driver marks (timeout or framing) is ignored while polling, and fails any other
 * phase. */
TEST(test_mb_masked_word) {
    make_image(image, 0x1C0);
    gen3_mb_init(&mb, MlinkWireMulti, image, 0x1C0, BENCH_SEED);
    MlinkTx tx;
    gen3_mb_frame(&mb, NULL, &tx, 0);
    ASSERT_EQ(tx.count, 1);
    ASSERT_EQ(tx.word[0], 0x6200);
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = 1;
    rx.word[0] = 0x7202;
    rx.framing_mask = 1;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
    ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
    ASSERT_EQ(tx.count, 1);
    ASSERT_EQ(tx.word[0], 0x6200);
    memset(&rx, 0, sizeof(rx));
    rx.count = 1;
    rx.word[0] = 0x7202;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US); /* the stale reply */
    ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
    rx.framing_mask = 1;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US); /* polling: ignored */
    ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
    ASSERT_EQ(tx.word[0], 0x6200);
    rx.framing_mask = 0;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
    ASSERT_EQ(status().phase, Gen3MbPhaseHello);
    ASSERT_EQ(tx.count, 1);
    ASSERT_EQ(tx.word[0], 0x6102);
    rx.word[0] = 0x7202;
    rx.timeout_mask = 1;
    gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailHello);
    ASSERT_EQ(tx.count, 0);
}

/* Replies to the frame in flight with word (unmarked), as the next driver frame would. */
static void reply_with(MlinkTx* tx, uint16_t word) {
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = tx->count;
    rx.word[0] = word;
    gen3_mb_frame(&mb, &rx, tx, mlink_frame_us(MlinkModeStream, rx.count));
}

/* MultiPlay replies run one transfer behind (multiboot-and-save.md §2.2), so the first reply in
 * Detect answers a word sent before Detect began: after gen3_mb_init, and after every wait. It is
 * ignored even when it reads 0x7202; the next one is the slave's answer to 0x6200. */
TEST(test_mb_stale_detect_reply) {
    make_image(image, 0x1C0);
    gen3_mb_init(&mb, MlinkWireMulti, image, 0x1C0, BENCH_SEED);
    MlinkTx tx;
    gen3_mb_frame(&mb, NULL, &tx, 0);
    for(int round = 0; round < 2; round++) {
        ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
        ASSERT_TRUE(tx.count == 1 && tx.word[0] == 0x6200);
        reply_with(&tx, 0x7202); /* stale */
        ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
        ASSERT_TRUE(tx.count == 1 && tx.word[0] == 0x6200);
        reply_with(&tx, 0x7202);
        ASSERT_EQ(status().phase, Gen3MbPhaseHello);
        ASSERT_TRUE(tx.count == 1 && tx.word[0] == 0x6102);
        reply_with(&tx, 0x0000); /* Hello fails: a counted restart, then a wait */
        ASSERT_EQ(status().phase, Gen3MbPhaseWait);
        ASSERT_EQ(status().restarts, round + 1);
        for(int i = 0; i < 1000 && tx.count == 0; i++) reply_with(&tx, 0);
    }
}

/* A marked first reply (timed out, or a framing error) doesn't use up the skip: the slave's SEND
 * still holds the stale word, so the next unmarked reply is the stale one. */
TEST(test_mb_stale_after_marked_reply) {
    for(int framing = 0; framing < 2; framing++) {
        make_image(image, 0x1C0);
        gen3_mb_init(&mb, MlinkWireMulti, image, 0x1C0, BENCH_SEED);
        MlinkTx tx;
        gen3_mb_frame(&mb, NULL, &tx, 0);
        ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
        MlinkRx rx;
        memset(&rx, 0, sizeof(rx));
        rx.count = tx.count;
        rx.word[0] = 0x7202;
        if(framing) {
            rx.framing_mask = 1;
        } else {
            rx.timeout_mask = 1;
        }
        gen3_mb_frame(&mb, &rx, &tx, mlink_frame_us(MlinkModeStream, rx.count));
        ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
        reply_with(&tx, 0x7202); /* the stale one */
        ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
        ASSERT_TRUE(tx.count == 1 && tx.word[0] == 0x6200);
        reply_with(&tx, 0x7202);
        ASSERT_EQ(status().phase, Gen3MbPhaseHello);
    }
}

/* A length gen3_mb_image_check refuses starts Failed: idle ticks only, and the image is never
 * read. */
TEST(test_mb_bad_len) {
    static const uint32_t lens[] = {0, 0x100, 0x1B0, 0x1C8, 0x3010};
    for(size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        gen3_mb_init(&mb, MlinkWireMulti, NULL, lens[i], BENCH_SEED);
        Gen3MbStatus st = status();
        ASSERT_EQ(st.phase, Gen3MbPhaseFailed);
        ASSERT_EQ(st.restarts, 0);
        ASSERT_EQ(st.last_fail, Gen3MbFailNone);
        MlinkTx tx;
        gen3_mb_frame(&mb, NULL, &tx, 0);
        ASSERT_EQ(tx.count, 0);
        MlinkRx rx;
        memset(&rx, 0, sizeof(rx));
        gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
        ASSERT_EQ(tx.count, 0);
        ASSERT_EQ(status().phase, Gen3MbPhaseFailed);
    }
}

/* A valid reply that isn't 0x73xx to 0x64hh is a counted Handshake failure. The fake never saw
 * the word, so the restart finds it in its palette step: it refuses that and resyncs. */
TEST(test_mb_wrong_handshake_reply) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhaseHandshake, 1000));
    ASSERT_TRUE(bench_pending.count == 1 && bench_pending.word[0] == (0x6400 | gen3_mb_hh(0x5A)));
    inject(0x7202);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailHandshake);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
}

/* The same for the length word: a valid reply that isn't 0x73rr is a counted Length failure. The
 * fake never saw the word and still waits for one, so it is power-cycled, as the user would. */
TEST(test_mb_wrong_length_reply) {
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhaseLength, 1000));
    ASSERT_TRUE(bench_pending.count == 1 && bench_pending.word[0] == (0x1000 - 0x190) / 4);
    inject(0x7202);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailLength);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
}

/* Detection: GEN3_MB_JUNK_LIMIT replies that are neither 0x0000, 0xFFFF nor 0x7202 start a
 * restart wait, not counted: 70 ms plus up to 170 ms of idle ticks (multiboot-and-save.md §2.4).
 * waiting_us counts the waits too, and no attempt budget runs before a header word. */
TEST(test_mb_junk_wait) {
    static const uint16_t replies[3] = {0x0000, 0xFFFF, 0x1234};
    make_image(image, 0x1C0);
    gen3_mb_init(&mb, MlinkWireMulti, image, 0x1C0, BENCH_SEED);
    MlinkTx tx;
    MlinkRx rx;
    gen3_mb_frame(&mb, NULL, &tx, 0);
    uint32_t total_us = 0;
    uint32_t min_us = UINT32_MAX;
    uint32_t max_us = 0;
    for(int wait = 0; wait < 200; wait++) {
        uint32_t polls = 0;
        while(status().phase == Gen3MbPhaseDetect) {
            memset(&rx, 0, sizeof(rx));
            rx.count = tx.count;
            if(tx.count) rx.word[0] = replies[polls++ % 3];
            const uint32_t us = mlink_frame_us(MlinkModeStream, rx.count);
            total_us += us;
            gen3_mb_frame(&mb, &rx, &tx, us);
        }
        ASSERT_EQ(status().phase, Gen3MbPhaseWait);
        ASSERT_EQ(polls, 3 * GEN3_MB_JUNK_LIMIT);
        ASSERT_EQ(GEN3_MB_JUNK_LIMIT, 255);
        uint32_t idle_us = 0;
        while(tx.count == 0) {
            memset(&rx, 0, sizeof(rx));
            idle_us += MLINK_TICK_US;
            total_us += MLINK_TICK_US;
            gen3_mb_frame(&mb, &rx, &tx, MLINK_TICK_US);
        }
        ASSERT_EQ(status().phase, Gen3MbPhaseDetect);
        ASSERT_EQ(tx.word[0], 0x6200);
        if(idle_us < min_us) min_us = idle_us;
        if(idle_us > max_us) max_us = idle_us;
    }
    Gen3MbStatus st = status();
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 200); /* none counted, each a restart all the same */
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(st.waiting_us, total_us);
    ASSERT_TRUE(min_us >= 70000u && min_us < 80000u);
    ASSERT_TRUE(max_us > 230000u && max_us < 240000u + MLINK_TICK_US);
}

/* The Normal-mode vector of multiboot-and-save.md §7.2 (plan 6), computed in Python from
 * GBATEK's formulas, apart from this code: Normal's CRC init, polynomial and data key, with the
 * seed, hh and the final CRC's form as on MultiPlay. */
TEST(test_mb_normal_vector) {
    Gen3MbCrypt k;
    gen3_mb_crypt_init(&k, MlinkWireNormal, 0x93, 0x5A);
    ASSERT_EQ(k.seed, 0xFFFF5A93u);
    ASSERT_EQ(k.crc, 0xC387);
    ASSERT_EQ(gen3_mb_crypt_word(&k, 0xC0, 0x12345678u), 0x7036291Du);
    ASSERT_EQ(k.crc, 0x4DD0);
    ASSERT_EQ(gen3_mb_crypt_word(&k, 0xC4, 0x9ABCDEF0u), 0x47EF9D9Cu);
    ASSERT_EQ(k.crc, 0x1271);
    ASSERT_EQ(gen3_mb_hh(0x5A), 0x69);
    ASSERT_EQ(gen3_mb_crypt_final(&k, 0x69, 0x3C), 0xF80B);
    /* each wire's constants */
    ASSERT_EQ(k.poly, 0xC37B);
    ASSERT_EQ(k.key, 0x43202F2Fu);
    gen3_mb_crypt_init(&k, MlinkWireMulti, 0x93, 0x5A);
    ASSERT_EQ(k.seed, 0xFFFF5A93u);
    ASSERT_EQ(k.crc, 0xFFF8);
    ASSERT_EQ(k.poly, 0xA517);
    ASSERT_EQ(k.key, 0x6465646Fu);
}

/* The whole boot on the Normal wire: one 32-bit word per data transfer, an upper half on data
 * words only (the fake refuses one anywhere else), and the image arrives as sent. */
TEST(test_mb_normal_boot) {
    bench_init_on(MlinkWireNormal, 0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 20000);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(st.fail_want, 0);
    ASSERT_EQ(st.fail_got, 0);
    ASSERT_EQ(st.cc, 0x5A);
    ASSERT_EQ(st.rr, 0xC3);
    ASSERT_EQ(st.total, 0x1000 - GEN3_MB_HEADER_LEN);
    ASSERT_EQ(st.sent, st.total);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosBooted);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(image_received(0x1000));
    uint32_t data = 0;
    bool data_hi = false;
    for(uint32_t i = 0; i < bench_n; i++) {
        const BenchFrame* f = &bench_log[i];
        ASSERT_TRUE(f->tx.count <= MLINK_BURST_LEN);
        if(i && bench_log[i - 1].phase == Gen3MbPhaseData) {
            data += f->tx.count;
            for(uint8_t t = 0; t < f->tx.count; t++) data_hi |= f->tx.hi[t] != 0;
        } else {
            for(uint8_t t = 0; t < MLINK_BURST_LEN; t++) ASSERT_EQ(f->tx.hi[t], 0);
        }
    }
    ASSERT_EQ(data, (0x1000 - GEN3_MB_HEADER_LEN) / 4);
    ASSERT_TRUE(data_hi);
    ASSERT_TRUE(idles_forever());
}

/* The GBA is off: every slot is not ready, each MLINK_NORMAL_STALL_TICKS long, and the sender
 * keeps polling. Once it is on, the boot goes through without a counted restart. */
TEST(test_mb_normal_waits_for_gba) {
    bench_init_on(MlinkWireNormal, 0x1000, 0x5A, 0xC3);
    run(&mb, &bios, 200);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDetect);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(bench_n, 200);
    for(uint32_t i = 0; i < bench_n; i++) {
        ASSERT_TRUE(is_frame(&bench_log[i], 0x6200));
        ASSERT_EQ(bench_log[i].notready, 1);
        ASSERT_TRUE(!bench_log[i].replied);
        ASSERT_EQ(bench_log[i].us, MLINK_NORMAL_STALL_TICKS * MLINK_TICK_US);
    }
    ASSERT_EQ(st.waiting_us, 200u * MLINK_NORMAL_STALL_TICKS * MLINK_TICK_US);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.waiting_us, 0);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(image_received(0x1000));
}

/* A data reply that isn't the word's offset is a counted Data failure, and the status keeps what
 * was expected and what came. The third data word's reply should be 0x00C8. */
TEST(test_mb_normal_data_reply_mismatch) {
    bench_init_on(MlinkWireNormal, 0x1000, 0x5A, 0xC3);
    /* 2 not ready, 0x0000, 0x7202, Hello, 96 + 2, palette, handshake, length, data words 0..2 */
    fake_bios_corrupt_reply(&bios, 109);
    fake_bios_power_on(&bios);
    for(uint32_t i = 0; i < 100000 && status().restarts == 0; i++) run(&mb, &bios, 1);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailData);
    ASSERT_EQ(st.fail_want, 0x00C8);
    ASSERT_EQ(st.fail_got, 0x00C8 ^ 0x0100);
    run(&mb, &bios, 100000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailData);
    ASSERT_EQ(st.fail_want, 0x00C8);
    ASSERT_EQ(st.fail_got, 0x00C8 ^ 0x0100);
    ASSERT_EQ(st.sent, st.total);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1000));
}

/* What each counted failure that compares a word records: the word expected (for 0x73xx replies,
 * the 0x7300 they need) and the word that came, MLINK_NO_DATA for a marked one. A failure that
 * compares no word keeps them. */
TEST(test_mb_fail_words) {
    /* Header: the slave counts 0x60 down, so the reply to halfword 44 is 0x3402 */
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_corrupt_reply(&bios, 50); /* 2 timeouts, 0x0000, 0x7202, Hello, header 0..44 */
    fake_bios_power_on(&bios);
    for(uint32_t i = 0; i < 100000 && status().restarts == 0; i++) run(&mb, &bios, 1);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.last_fail, Gen3MbFailHeader);
    ASSERT_EQ(st.fail_want, 0x3402);
    ASSERT_EQ(st.fail_got, 0x3502);
    /* Handshake: a right word, but marked (a framing error) */
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhaseHandshake, 1000));
    MlinkRx rx;
    memset(&rx, 0, sizeof(rx));
    rx.count = bench_pending.count;
    rx.word[0] = 0x735A;
    rx.framing_mask = 1;
    gen3_mb_frame(&mb, &rx, &bench_pending, MLINK_TICK_US);
    st = status();
    ASSERT_EQ(st.last_fail, Gen3MbFailHandshake);
    ASSERT_EQ(st.fail_want, 0x7300);
    ASSERT_EQ(st.fail_got, MLINK_NO_DATA);
    /* Hello compares no word here: they stay */
    for(int i = 0; i < 1000 && status().phase != Gen3MbPhaseDetect; i++) {
        memset(&rx, 0, sizeof(rx));
        gen3_mb_frame(&mb, &rx, &bench_pending, MLINK_TICK_US);
    }
    reply_with(&bench_pending, 0x7202); /* stale */
    reply_with(&bench_pending, 0x7202);
    ASSERT_EQ(status().phase, Gen3MbPhaseHello);
    reply_with(&bench_pending, 0x0000);
    st = status();
    ASSERT_EQ(st.last_fail, Gen3MbFailHello);
    ASSERT_EQ(st.fail_want, 0x7300);
    ASSERT_EQ(st.fail_got, MLINK_NO_DATA);
    /* Length: a 0x7202 */
    bench_init(0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhaseLength, 1000));
    inject(0x7202);
    st = status();
    ASSERT_EQ(st.last_fail, Gen3MbFailLength);
    ASSERT_EQ(st.fail_want, 0x7300);
    ASSERT_EQ(st.fail_got, 0x7202);
    /* Crc: the slave's CRC is ours plus one */
    bench_init(0x1C0, 0x5A, 0xC3);
    fake_bios_force_bad_crc(&bios, true);
    fake_bios_power_on(&bios);
    for(uint32_t i = 0; i < 100000 && status().restarts == 0; i++) run(&mb, &bios, 1);
    st = status();
    ASSERT_EQ(st.last_fail, Gen3MbFailCrc);
    ASSERT_EQ(st.fail_got, (uint16_t)(st.fail_want + 1u));
}

/* The default on the Normal wire: the BIOS phase clocks every slot, ungated (multiboot-and-save.md
 * §7.3). With the GBA off, and while it starts up, each slot is clocked and reads 0xFFFF (SI
 * pulled high), one tick, never not ready; the sender keeps polling. Once the GBA is on, the boot
 * goes through as gated, and the image arrives as sent. */
TEST(test_mb_normal_boot_ungated) {
    bench_init_on(MlinkWireNormal, 0x1000, 0x5A, 0xC3);
    fake_bios_set_gate(&bios, false);
    run(&mb, &bios, 200);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDetect);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 0);
    ASSERT_EQ(bench_n, 200);
    for(uint32_t i = 0; i < bench_n; i++) {
        ASSERT_TRUE(is_frame(&bench_log[i], 0x6200));
        ASSERT_EQ(bench_log[i].notready, 0);
        ASSERT_TRUE(bench_log[i].replied);
        ASSERT_EQ(bench_log[i].reply, 0xFFFF);
        ASSERT_EQ(bench_log[i].us, MLINK_TICK_US);
    }
    ASSERT_EQ(st.notready, 0);
    ASSERT_EQ(st.last_reply, 0xFFFF);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 20000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 0);
    ASSERT_EQ(st.last_fail, Gen3MbFailNone);
    ASSERT_EQ(st.notready, 0);
    ASSERT_EQ(st.sent, st.total);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosBooted);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(image_received(0x1000));
    ASSERT_TRUE(idles_forever());
}

/* For the log: the not-ready slots since init (up to UINT16_MAX) and the latest reply delivered
 * (MLINK_NO_DATA until one). Gated, with the GBA off, every poll is a not-ready slot and nothing
 * is delivered; once it is on, last_reply follows the replies. MultiPlay's timed-out words are
 * neither. */
TEST(test_mb_status_notready_last_reply) {
    bench_init_on(MlinkWireNormal, 0x1000, 0x5A, 0xC3);
    Gen3MbStatus st = status();
    ASSERT_EQ(st.notready, 0);
    ASSERT_EQ(st.last_reply, MLINK_NO_DATA);
    run(&mb, &bios, 50);
    st = status();
    ASSERT_EQ(st.notready, 50);
    ASSERT_EQ(st.last_reply, MLINK_NO_DATA);
    fake_bios_power_on(&bios);
    run(&mb, &bios, 3); /* 2 starting up, then 0x0000 */
    st = status();
    ASSERT_EQ(st.notready, 52);
    ASSERT_EQ(st.last_reply, 0x0000);
    run(&mb, &bios, 1);
    ASSERT_EQ(status().last_reply, 0x7202);
    run(&mb, &bios, 2); /* Hello, then the first header frame */
    ASSERT_EQ(status().last_reply, 0x5802); /* its 9th halfword's: (0x60 - 8) << 8 | 0x02 */
    run(&mb, &bios, 20000);
    st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseDone);
    ASSERT_EQ(st.notready, 52);
    /* A long wait saturates */
    bench_init_on(MlinkWireNormal, 0x1000, 0x5A, 0xC3);
    run(&mb, &bios, UINT16_MAX + 10u);
    ASSERT_EQ(status().notready, UINT16_MAX);
    /* MultiPlay */
    bench_init(0x1000, 0x5A, 0xC3);
    run(&mb, &bios, 10);
    st = status();
    ASSERT_EQ(st.notready, 0);
    ASSERT_EQ(st.last_reply, MLINK_NO_DATA);
}

/* Normal wire, gated: a not-ready reply to the palette word is a busy slave, as a valid 0x7202
 * is. A palette wait that saw only those times out uncounted. */
TEST(test_mb_normal_palette_notready_is_busy) {
    bench_init_on(MlinkWireNormal, 0x1000, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(run_until(Gen3MbPhasePalette, 1000));
    const uint32_t nr_us = MLINK_NORMAL_STALL_TICKS * MLINK_TICK_US;
    for(uint32_t i = 0; i < 1000 && status().phase == Gen3MbPhasePalette; i++) {
        MlinkRx rx;
        memset(&rx, 0, sizeof(rx));
        rx.count = bench_pending.count;
        rx.word[0] = MLINK_NO_DATA;
        rx.timeout_mask = 1;
        rx.notready_mask = 1;
        bench_frame(&mb, &rx, nr_us);
    }
    Gen3MbStatus st = status();
    ASSERT_EQ(st.phase, Gen3MbPhaseWait);
    ASSERT_EQ(st.restarts, 0);
    ASSERT_EQ(st.all_restarts, 1);
    ASSERT_EQ(st.last_fail, Gen3MbFailPalette);
    ASSERT_EQ(st.palette_busy, (GEN3_MB_PALETTE_US + nr_us - 1u) / nr_us);
}

/* D6 (plan 6): MultiPlay stays while SI reads low, or once the GBA answered; SI high in Wait or
 * Detect moves to Normal, which stays. */
TEST(test_mb_wire_next) {
    ASSERT_EQ(gen3_mb_wire_next(MlinkWireMulti, true, Gen3MbPhaseDetect), MlinkWireMulti);
    ASSERT_EQ(gen3_mb_wire_next(MlinkWireMulti, false, Gen3MbPhaseDetect), MlinkWireNormal);
    ASSERT_EQ(gen3_mb_wire_next(MlinkWireMulti, false, Gen3MbPhaseWait), MlinkWireNormal);
    ASSERT_EQ(gen3_mb_wire_next(MlinkWireMulti, false, Gen3MbPhaseHeader), MlinkWireMulti);
    ASSERT_EQ(gen3_mb_wire_next(MlinkWireMulti, false, Gen3MbPhaseDone), MlinkWireMulti);
    ASSERT_EQ(gen3_mb_wire_next(MlinkWireNormal, true, Gen3MbPhaseDetect), MlinkWireNormal);
    ASSERT_EQ(gen3_mb_wire_next(MlinkWireNormal, false, Gen3MbPhaseData), MlinkWireNormal);
}

/* The fake on its own, driven word by word with multiboot-and-save.md §2.2's replies and the
 * payload of the multiboot-and-save.md §2.3 test vector. */
static uint16_t xfer(FakeBios* b, uint16_t word, bool* timed_out) {
    MlinkTx tx = {.count = 1, .word = {word}};
    MlinkRx rx;
    fake_bios_frame(b, &tx, &rx);
    if(timed_out) *timed_out = (rx.timeout_mask & 1u) != 0;
    return rx.word[0];
}

static void idle(FakeBios* b, uint32_t ticks) {
    MlinkTx tx = {.count = 0};
    MlinkRx rx;
    for(uint32_t i = 0; i < ticks; i++) fake_bios_frame(b, &tx, &rx);
}

/* Power-on through the 96 header halfwords; false on the first unexpected reply. */
static bool fake_header(FakeBios* b, const uint8_t* hdr) {
    bool to;
    xfer(b, 0x6200, &to);
    if(!to) return false;
    xfer(b, 0x6200, &to);
    if(!to) return false;
    if(xfer(b, 0x6200, &to) != 0x0000 || to) return false;
    if(xfer(b, 0x6200, NULL) != 0x7202) return false;
    if(xfer(b, 0x6102, NULL) != 0x7202) return false;
    for(uint32_t k = 0; k < 0x60; k++) {
        const uint16_t w = hdr[2 * k] | (uint16_t)(hdr[2 * k + 1] << 8);
        if(xfer(b, w, NULL) != (((0x60 - k) << 8) | 0x02)) return false;
    }
    return fake_bios_error(b) == NULL;
}

/* Power-on through the 0x64hh transfer; false on the first unexpected reply. */
static bool fake_handshake(FakeBios* b, const uint8_t* hdr, uint8_t hh) {
    if(!fake_header(b, hdr)) return false;
    if(xfer(b, 0x6200, NULL) != 0x0002) return false;
    if(xfer(b, 0x6202, NULL) != 0x7202) return false;
    if(xfer(b, 0x6393, NULL) != 0x735A) return false;
    if(xfer(b, 0x6400 | hh, NULL) != 0x735A) return false;
    return fake_bios_error(b) == NULL;
}

/* Power-on through the last payload halfword of the multiboot-and-save.md §2.3 test vector,
 * which it writes into image; false on the first unexpected reply. */
static bool fake_doc_vector_payload(FakeBios* b) {
    make_image(image, 0x1C0);
    for(int i = 0; i < 0x100; i++) image[0xC0 + i] = (uint8_t)i;
    uint16_t hw[128];
    uint16_t crc;
    doc_vector_halfwords(hw, &crc);
    if(!fake_handshake(b, image, 0x69)) return false;
    idle(b, 94);
    if(xfer(b, 0x000C, NULL) != 0x73C3) return false;
    for(uint32_t k = 0; k < 128; k++) {
        if(xfer(b, hw[k], NULL) != 0xC0 + 2 * k) return false;
    }
    return fake_bios_error(b) == NULL;
}

TEST(test_fake_bios_doc_vector) {
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_doc_vector_payload(&bios));
    ASSERT_TRUE(xfer(&bios, 0x0065, NULL) != 0x0075);
    ASSERT_EQ(xfer(&bios, 0x0065, NULL), 0x0075);
    ASSERT_EQ(xfer(&bios, 0x0066, NULL), 0x0075);
    ASSERT_EQ(xfer(&bios, 0x2A28, NULL), 0x2A28);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosBooted);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1C0));
}

/* The fake refuses what multiboot-and-save.md §2.2 forbids, and goes back to detection. */
TEST(test_fake_bios_checks) {
    make_image(image, 0x1C0);
    /* the length word inside the 1/16 s wait */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_handshake(&bios, image, 0x69));
    idle(&bios, 50);
    ASSERT_EQ(xfer(&bios, 0x000C, NULL), 0x73C3);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    /* a wrong handshake byte */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(!fake_handshake(&bios, image, 0x6A));
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    /* a bad header: no 0x96 */
    image[0xB2] = 0;
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(!fake_handshake(&bios, image, 0x69));
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    make_image(image, 0x1C0);
    /* 0x6102 before the slave answered 0x7202 */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    xfer(&bios, 0x6200, NULL);
    xfer(&bios, 0x6200, NULL);
    ASSERT_EQ(xfer(&bios, 0x6102, NULL), 0x0000);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    /* an undocumented palette byte */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_header(&bios, image));
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0002);
    ASSERT_EQ(xfer(&bios, 0x6202, NULL), 0x7202);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    xfer(&bios, 0x6380, NULL);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    /* after the header, 0x6202 where 0x6200 belongs */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_header(&bios, image));
    ASSERT_EQ(xfer(&bios, 0x6202, NULL), 0x0002);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    /* then 0x6200 where 0x6202 belongs */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_header(&bios, image));
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0002);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x7202);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    /* 0x0066 without a 0x0075 reply: right after the first 0x0065 */
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_doc_vector_payload(&bios));
    ASSERT_TRUE(xfer(&bios, 0x0065, NULL) != 0x0075);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(xfer(&bios, 0x0066, NULL), 0x0075);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_EQ(xfer(&bios, 0x6200, NULL), 0x0000);
    ASSERT_EQ(fake_bios_boots(&bios), 0);
}

/* One Normal-wire transfer of the 32-bit word w (its low half in word, its upper half in hi): the
 * reply, the slave's upper half. */
static uint16_t xfer32(FakeBios* b, uint32_t w) {
    MlinkTx tx = {.count = 1, .word = {(uint16_t)w}, .hi = {(uint16_t)(w >> 16)}};
    MlinkRx rx;
    fake_bios_frame(b, &tx, &rx);
    return rx.word[0];
}

static uint32_t le32_at(const uint8_t* p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A 0x100-byte payload led by the Normal-mode vector's two plain words, then bytes i; its 64
 * encrypted words and the final CRC, with rr 0x3C. */
static void normal_vector_words(uint32_t out[64], uint16_t* crc) {
    make_image(image, 0x1C0);
    for(int i = 0; i < 0x100; i++) image[0xC0 + i] = (uint8_t)i;
    static const uint8_t lead[8] = {0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A};
    memcpy(image + 0xC0, lead, sizeof(lead));
    Gen3MbCrypt k;
    gen3_mb_crypt_init(&k, MlinkWireNormal, GEN3_MB_PALETTE, 0x5A);
    for(uint32_t i = 0; i < 64; i++) {
        out[i] = gen3_mb_crypt_word(&k, 0xC0 + 4 * i, le32_at(image + 0xC0 + 4 * i));
    }
    *crc = gen3_mb_crypt_final(&k, gen3_mb_hh(0x5A), 0x3C);
}

/* The fake on the Normal wire, word by word: its own decryption takes the vector's encrypted words
 * (multiboot-and-save.md §7.2) back to the plain ones, each data reply is the word's offset, and
 * the CRC matches. */
TEST(test_fake_bios_normal_vector) {
    uint32_t words[64];
    uint16_t crc;
    normal_vector_words(words, &crc);
    ASSERT_EQ(words[0], 0x7036291Du);
    ASSERT_EQ(words[1], 0x47EF9D9Cu);
    fake_bios_init(&bios, 0x5A, 0x3C);
    fake_bios_set_wire(&bios, MlinkWireNormal);
    fake_bios_power_on(&bios);
    ASSERT_TRUE(fake_handshake(&bios, image, 0x69));
    idle(&bios, 94);
    ASSERT_EQ(xfer(&bios, 0x000C, NULL), 0x733C);
    ASSERT_EQ(xfer32(&bios, 0x7036291Du), 0x00C0);
    ASSERT_EQ(le32_at(fake_bios_image(&bios, NULL) + 0xC0), 0x12345678u);
    ASSERT_EQ(xfer32(&bios, 0x47EF9D9Cu), 0x00C4);
    ASSERT_EQ(le32_at(fake_bios_image(&bios, NULL) + 0xC4), 0x9ABCDEF0u);
    for(uint32_t k = 2; k < 64; k++) ASSERT_EQ(xfer32(&bios, words[k]), 0xC0 + 4 * k);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_TRUE(xfer(&bios, 0x0065, NULL) != 0x0075);
    ASSERT_EQ(xfer(&bios, 0x0065, NULL), 0x0075);
    ASSERT_EQ(xfer(&bios, 0x0066, NULL), 0x0075);
    ASSERT_EQ(xfer(&bios, crc, NULL), crc);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(fake_bios_phase(&bios), FakeBiosBooted);
    ASSERT_EQ(fake_bios_boots(&bios), 1);
    ASSERT_TRUE(image_received(0x1C0));
    /* booted: not ready from now on */
    MlinkTx tx = {.count = 1, .word = {0x6200}};
    MlinkRx rx;
    fake_bios_frame(&bios, &tx, &rx);
    ASSERT_EQ(rx.notready_mask, 1);
    ASSERT_EQ(rx.timeout_mask, 1);
    ASSERT_EQ(rx.ticks, MLINK_NORMAL_STALL_TICKS);
}

/* The Normal wire ungated (the BIOS phase's default): a slot while off or starting up is clocked
 * all the same and reads 0xFFFF, SI pulled high, in one tick, with no mask bit; then 0x0000 as
 * gated. */
TEST(test_fake_bios_normal_ungated) {
    MlinkTx tx = {.count = 2, .word = {0x6200, 0x6200}};
    MlinkRx rx;
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_set_wire(&bios, MlinkWireNormal);
    fake_bios_set_gate(&bios, false);
    fake_bios_frame(&bios, &tx, &rx); /* off */
    ASSERT_EQ(rx.ticks, 2);
    ASSERT_EQ(rx.notready_mask, 0);
    ASSERT_EQ(rx.timeout_mask, 0);
    ASSERT_EQ(rx.word[0], 0xFFFF);
    ASSERT_EQ(rx.word[1], 0xFFFF);
    fake_bios_power_on(&bios);
    tx.count = 3;
    tx.word[2] = 0x6200;
    fake_bios_frame(&bios, &tx, &rx); /* 2 starting up, then 0x0000 */
    ASSERT_EQ(rx.ticks, 3);
    ASSERT_EQ(rx.notready_mask, 0);
    ASSERT_EQ(rx.timeout_mask, 0);
    ASSERT_EQ(rx.word[0], 0xFFFF);
    ASSERT_EQ(rx.word[1], 0xFFFF);
    ASSERT_EQ(rx.word[2], 0x0000);
    ASSERT_EQ(xfer32(&bios, 0x6200), 0x7202);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    /* gated again: starting up is not ready */
    fake_bios_set_gate(&bios, true);
    fake_bios_power_on(&bios);
    tx.count = 1;
    fake_bios_frame(&bios, &tx, &rx);
    ASSERT_EQ(rx.notready_mask, 1);
    ASSERT_EQ(rx.ticks, MLINK_NORMAL_STALL_TICKS);
}

/* The Normal wire as the driver reports it: not-ready slots (off, starting up, booted) take
 * MLINK_NORMAL_STALL_TICKS each and clock nothing; a delivered one takes a tick. An upper half on
 * any word but a data word is refused. MultiPlay frames take max(count, 1) ticks. */
TEST(test_fake_bios_normal_wire) {
    MlinkTx tx = {.count = 2, .word = {0x6200, 0x6200}};
    MlinkRx rx;
    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_frame(&bios, &tx, &rx);
    ASSERT_EQ(rx.ticks, 2);
    ASSERT_EQ(rx.notready_mask, 0);
    ASSERT_EQ(rx.timeout_mask, 3);
    tx.count = 0;
    fake_bios_frame(&bios, &tx, &rx);
    ASSERT_EQ(rx.ticks, 1);

    fake_bios_init(&bios, 0x5A, 0xC3);
    fake_bios_set_wire(&bios, MlinkWireNormal);
    tx.count = 2;
    fake_bios_frame(&bios, &tx, &rx); /* off */
    ASSERT_EQ(rx.ticks, 2 * MLINK_NORMAL_STALL_TICKS);
    ASSERT_EQ(rx.notready_mask, 3);
    ASSERT_EQ(rx.timeout_mask, 3);
    ASSERT_EQ(rx.word[0], MLINK_NO_DATA);
    ASSERT_EQ(rx.word[1], MLINK_NO_DATA);
    tx.count = 0;
    fake_bios_frame(&bios, &tx, &rx);
    ASSERT_EQ(rx.ticks, 1);
    fake_bios_power_on(&bios);
    tx.count = 3;
    tx.word[2] = 0x6200;
    fake_bios_frame(&bios, &tx, &rx); /* 2 starting up, then 0x0000 */
    ASSERT_EQ(rx.ticks, 2 * MLINK_NORMAL_STALL_TICKS + 1);
    ASSERT_EQ(rx.notready_mask, 3);
    ASSERT_EQ(rx.timeout_mask, 3);
    ASSERT_EQ(rx.word[2], 0x0000);
    ASSERT_TRUE(fake_bios_error(&bios) == NULL);
    ASSERT_EQ(xfer32(&bios, 0x00016200u), 0x7202);
    ASSERT_TRUE(fake_bios_error(&bios) != NULL);
    ASSERT_STR_EQ(fake_bios_error(&bios), "upper half not zero");
    ASSERT_EQ(xfer32(&bios, 0x6200), 0x0000); /* back in detection */
}

int main(void) {
    RUN(test_mlink_frame_us);
    RUN(test_mb_crc_step);
    RUN(test_mb_doc_vector);
    RUN(test_mb_image_check);
    RUN(test_mb_boot);
    RUN(test_mb_waits_for_gba);
    RUN(test_mb_length_delay);
    RUN(test_mb_poll_spacing);
    RUN(test_mb_busy_slave);
    RUN(test_mb_status_detail);
    RUN(test_mb_palette_timeout);
    RUN(test_mb_palette_stuck_busy);
    RUN(test_mb_palette_reset_counts);
    RUN(test_mb_final_timeout);
    RUN(test_mb_attempt_budget);
    RUN(test_mb_crc_at_budget_end);
    RUN(test_mb_glitch_recovers_in_budget);
    RUN(test_mb_late_power_on);
    RUN(test_mb_header_glitch);
    RUN(test_mb_data_glitch);
    RUN(test_mb_bad_crc_gives_up);
    RUN(test_mb_masked_word);
    RUN(test_mb_stale_detect_reply);
    RUN(test_mb_stale_after_marked_reply);
    RUN(test_mb_bad_len);
    RUN(test_mb_wrong_handshake_reply);
    RUN(test_mb_wrong_length_reply);
    RUN(test_mb_junk_wait);
    RUN(test_mb_normal_vector);
    RUN(test_mb_normal_boot);
    RUN(test_mb_normal_waits_for_gba);
    RUN(test_mb_normal_data_reply_mismatch);
    RUN(test_mb_fail_words);
    RUN(test_mb_normal_boot_ungated);
    RUN(test_mb_status_notready_last_reply);
    RUN(test_mb_normal_palette_notready_is_busy);
    RUN(test_mb_wire_next);
    RUN(test_fake_bios_doc_vector);
    RUN(test_fake_bios_checks);
    RUN(test_fake_bios_normal_vector);
    RUN(test_fake_bios_normal_wire);
    RUN(test_fake_bios_normal_ungated);
    TEST_EXIT();
}
