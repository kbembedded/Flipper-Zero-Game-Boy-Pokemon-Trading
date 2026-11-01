#include "fake_ruby.h"

#include <string.h>

/* Citations are pokeruby names, as in docs/gen3/link-protocol.md. FRLG mode cites pokefirered as
 * fr: file: Function (docs/gen3/link-protocol-trade.md Part C). */

#define CMD_SEND_LINK_TYPE     0x2222
#define CMD_READY_EXIT_STANDBY 0x2FFE
#define CMD_READY_CLOSE_LINK   0x5FFF
#define CMD_CONT_BLOCK         0x8888
#define CMD_INIT_BLOCK         0xBBBB
#define CMD_SEND_HELD_KEYS     0xCAFE
#define CMD_SEND_BLOCK_REQ     0xCCCC
#define CMD_NONE               0xEFFF

#define MASTER_HANDSHAKE 0x8FFF
#define SLAVE_HANDSHAKE  0xB9A0

#define LOCAL_ID 1 /* multiplayer id of the child */

/* Room key codes (§A7 "Room command format") */
#define KEY_CODE_EMPTY      0x11
#define KEY_CODE_DPAD_DOWN  0x12
#define KEY_CODE_DPAD_UP    0x13
#define KEY_CODE_READY      0x16
#define KEY_CODE_EXIT_ROOM  0x17
#define KEY_CODE_START      0x18
#define KEY_CODE_IDLE       0x1A
#define KEY_CODE_RECV_QUEUE 0x1B
#define KEY_CODE_SEND_QUEUE 0x1C
#define KEY_CODE_EXIT_SEAT  0x1D
#define KEY_CODE_END        0x1E /* sub_8055280 keeps codes 0x11..0x1D, else sends 0x11 */

/* Player link states (gUnknown_03000580) */
#define PLAYER_IDLE    0x80
#define PLAYER_BUSY    0x81
#define PLAYER_READY   0x82
#define PLAYER_EXITING 0x83

#define TRIGGER_HANDSHAKE_RUNS 5 /* Task_TriggerHandshake */
#define LAG_VBLANKS            10 /* LinkVSync: > 10 VBlanks without a serial IRQ */
#define CARD_READY_BURSTS      3 /* own card ready this many bursts after both player blocks */
#define WALK_IN_FRAMES         30 /* zero-command frames between the cards and 0x2FFE */
#define STANDBY_TIMEOUT_FRAMES 600 /* test limit; Ruby itself waits forever */
/* Script pacing in game updates; the player's reading time, not protocol values. */
#define PROMPT_UPDATES     60 /* "Terminate the link?" until the player picks YES */
#define TERMINATE_UPDATES  60 /* "Terminating the link..." until ExitLinkRoom */
#define EXIT_FADE_UPDATES  30 /* FadeScreen and music fade-out before SetCloseLinkCallback */
#define SEAT_PRINT_UPDATES 30 /* "Please wait..." prints (sub_80839DC cases 0-1) */
#define CORRUPT_BIT        0x0001

/* Part B (docs/gen3/link-protocol-trade.md §B1) */
#define START_FADE_FRAMES 17 /* sub_8083AAC case 1: FadeScreen(1, 0) */
#define WAIT_LINK_FRAMES  300 /* sub_8083C50 [Task_WaitForLinkPlayerConnection] */
#define SAVED_PLAYERS     2 /* sub_800820C [GetSavedPlayerCount], saved at the Part A linkup */
/* sub_8047EC0 [CB2_CreateTradeMenu] gMain.state values */
#define MENU_SETUP     0 /* graphics and windows, ResetTasks */
#define MENU_OPEN      1 /* gLinkType = 0x1122, OpenLink, the 300-frame task */
#define MENU_DELAY     2 /* unk_00b4 counts 12 frames */
#define MENU_PLAYERS   3 /* child: GetLinkPlayerCount_2() >= sub_800820C() */
#define MENU_EXCHANGE  4 /* gReceivedRemoteLinkPlayers && IsLinkPlayerDataExchangeComplete */
#define MENU_PARTIES   5 /* sub_8048D44 [BufferTradeParties] (§B2) */
#define MENU_ENEMIES   6 /* CalculateEnemyPartyCount, the party icons */
#define MENU_BUILD     7 /* states 7-21: held items, names, HP bars, the fade-in */
#define MENU_RUNNING   22 /* gMain.callback1 = sub_80494D8 [CB1_UpdateLink] */
/* FRLG mode only, no Ruby state: fr: trade.c: CB2_CreateTradeMenu case 5, between its case 4
 * (MENU_EXCHANGE) and BufferTradeParties, waits for a wireless link; on a cable it passes in one
 * frame */
#define MENU_FRLG_WIRELESS 23
#define MENU_DELAY_MAX 11 /* state 2 leaves once ++unk_00b4 > 11 */
#define MENU_BUILD_FRAMES 32 /* state 6 up to callback1 (states 6-21 and the fade-in) */
#define XCHG_STALL_FRAMES 600 /* test limit: a sub_8048D44 wait case has no timeout on Ruby */
#define MAIL_BLOCK_SIZE   220 /* round 4: mail[0..5] and 4 more bytes (§B2.3) */
/* What Ruby can hold (§B2.5 rules 1-4): internal species 1..251 and 277..411, moves below
 * NUM_MOVES, items below ITEMS_COUNT, never the ENIGMA BERRY, and no mail (a mail item or mail
 * index needs mail data in round 4) */
#define SPECIES_JOHTO_LAST  251
#define SPECIES_HOENN_FIRST 277
#define SPECIES_LAST        411
#define MOVES_COUNT         355
#define ITEMS_COUNT         349
#define ITEM_ENIGMA_BERRY   175
#define ITEM_MAIL_FIRST     121 /* ORANGE MAIL */
#define ITEM_MAIL_LAST      132 /* RETRO MAIL */
#define MAIL_NONE           0xFF

/* The trade menu (§B3): trade.c unk_007b states */
#define TRADE_BROWSING      0 /* sub_8049680, and the submenus of states 1, 2 and 4 */
#define TRADE_PROMPT        3 /* sub_8049AC0: "Is this trade okay?" YES/NO */
#define TRADE_STANDBY_READY 5 /* "Link standby..." after this GBA's AABB, no input */
#define TRADE_SLIDE_IN      7 /* sub_8049CC4: both Pokemon slide in */
#define TRADE_MESSAGE       8 /* sub_8049D44: a message until A */
#define TRADE_FADE_TRADE    10 /* sub_8048A50 */
#define TRADE_FADE_ROOM     11 /* sub_8049D9C */
#define TRADE_CLOSE_ROOM    12 /* sub_8049DC4 */
#define TRADE_CLOSE_TRADE   13 /* sub_8048A90 */
#define TRADE_CONFIRM_WAIT  14 /* DisplayMessageAndContinueTask */
#define TRADE_STANDBY       100 /* "Link standby..." after EEAA, BBBB or BBCC, no input */
#define SLIDE_IN_FRAMES        20 /* sub_8049ED4 until both slots' unk_0080 reach 5 */
#define CONFIRM_MESSAGE_FRAMES 121 /* DisplayMessageAndContinueTask: ++unk_00b4 > 120 */
#define MENU_FADE_FRAMES       17 /* BeginNormalPaletteFade(0xFFFFFFFF, 0, 0, 16, 0) */
#define CANCEL_SEND_DELAY      5 /* sub_8049BC0: QueueAction(5, 0) */
#define CURSOR_CANCEL          12 /* tradeMenuCursorPosition on CANCEL */
#define PARTNER_POS_FIRST      6 /* unk_008a: the partner's slots are cursor positions 6..11 */
#define LINK_DATA_WORDS        10
#define LINK_DATA_LEFTOVER     0x5A5A /* the trade struct lives in ewram_2010000, never cleared */
/* Trade-menu codes, word 0 of a 20-byte block (§B3.2, §B3.3; Emerald LINKCMD_ names) */
#define TRADE_BLOCK_SIZE    20
#define CODE_READY          0xAABB /* READY_TO_TRADE */
#define CODE_CANCEL         0xEEAA /* REQUEST_CANCEL */
#define CODE_CONFIRM        0xBBBB /* INIT_BLOCK */
#define CODE_CONFIRM_NO     0xBBCC /* READY_CANCEL_TRADE */
#define CODE_SET_MONS       0xDDDD /* SET_MONS_TO_TRADE */
#define CODE_PARTNER_CANCEL 0xEECC /* PARTNER_CANCEL_TRADE */
#define CODE_PLAYER_CANCEL  0xDDEE /* PLAYER_CANCEL_TRADE */
#define CODE_BOTH_CANCEL    0xEEBB /* BOTH_CANCEL_TRADE */
#define CODE_START          0xCCDD /* START_TRADE */
#define CODE_ANIM_DONE      0xABCD /* this GBA: TradeMons done (§B4.3) */
#define CODE_ANIM_ACK       0xDCBA /* the Flipper's answer */

/* The trade (§B4): sub_804B41C [CB2_LinkTrade] and sub_804DB84 [CB2_UpdateLinkTrade] */
#define LINK_TRADE_DELAY_MAX 60 /* state 2 leaves once ++unk_00c0 > 60 */
#define TRADE_PICS_FRAMES    25 /* states 5-12: both pictures, then the fade-in */
/* sub_804C29C's length isn't computed (link-protocol-trade.md T16); no traffic, no deadline */
#define TRADE_ANIM_FRAMES    600
#define TRADE_FRIENDSHIP     70 /* sub_804BA94 [TradeMons]: the received Pokemon's, if no egg */
#define LP_NATIONAL_DEX      0x8000 /* sub_804BA64: the partner's lp_field_2 */

/* Back to the room after EEBB (§B6.3) and the re-sit (§B7) */
/* Frames from the one sub_8049DC4 sees the close (t) to the 0x1111 OpenLink: SetMainCallback2
 * takes effect the next frame, so sub_805465C runs at t+1 (CB2_ReturnToField: SetMainCallback2
 * (CB2_ReturnToFieldLink)), sub_8054A9C's states 0-13 at t+2..t+15 (12: RunFieldCallback,
 * sub_8080A3C creates task_mpl_807DD60; 13 returns 1: SetMainCallback2(CB2_Overworld)), and
 * CB2_Overworld's RunTasks at t+16 runs task_mpl_807DD60 case 0 (priority 10), whose sub_8083664
 * creates sub_8083710 (priority 80, after it in the list), which OpenLinks in the same RunTasks */
#define RETURN_FIELD_FRAMES 16
/* sub_8083710: data[0] 0 at OpenLink (frame o), 1..9 at o+1..o+9; at o+10 data[0] >= 10 hands
 * over to sub_8083760, which runs from o+11 and, with 2 players, hands over to sub_80837EC from
 * the next frame: its check comes at o+12 at the earliest */
#define REESTABLISH_FRAMES  12
#define FADE_IN_UPDATES     17 /* task_mpl_807DD60: the fade-in, controls locked */
#define RESIT_STEP_UPDATES  16 /* game updates of one held D-pad key (the step isn't modelled) */
#define NAME_EOS            0xFF

/* The link save (§B5.1): sub_804DC88 [CB2_SaveAndEndTrade] gMain.state values, in run order */
#define SAVE_STANDBY_TEXT  0 /* "Link standby..." */
#define SAVE_SYNC1         1 /* sub_80084A4 [SetLinkStandbyCallback] */
#define SAVE_WAIT_SYNC1    100 /* until IsLinkTaskFinished; 101 after 180 frames, same wait */
#define SAVE_WAIT_SYNC1_2  101
#define SAVE_SAVING_TEXT   2 /* "SAVING... DON'T TURN OFF THE POWER." */
#define SAVE_INIT          50 /* sub_8125D80: serialise, rotate (RAM only) */
#define SAVE_WAIT_SECTOR   51 /* 5 frames */
#define SAVE_SECTOR        52 /* sub_8125DA8: one chunk into the other slot */
#define SAVE_LAST_SECTOR   4 /* sub_8125DDC: chunk 13 but its signature's low byte */
#define SAVE_WAIT_SYNC2    40 /* 51 frames */
#define SAVE_SYNC2         41
#define SAVE_COMMIT        42 /* sync 2 done: sub_8125E04 writes the signature byte */
#define SAVE_WAIT_SYNC3    5 /* 61 frames, then sync 3 */
#define SAVE_FADE          6 /* sync 3 done: BeginNormalPaletteFade */
#define SAVE_BGM           7 /* the fade done: FadeOutBGM(3) */
#define SAVE_CLOSE         8 /* the BGM stopped: SetCloseLinkCallback */
#define SAVE_CLOSED        9 /* !gReceivedRemoteLinkPlayers: sub_804E144 */
#define SAVE_SYNC1_SLOW    180
#define SAVE_SECTOR_FRAMES 5 /* state 51 leaves once ++unk_00c0 == 5 */
#define SAVE_CHUNKS        14 /* sSaveBlockChunks: 13 written by sub_8125DA8, the 14th call ends */
#define SAVE_SYNC2_DELAY   50 /* state 40 leaves once ++unk_00c0 > 50 */
#define SAVE_SYNC3_DELAY   60 /* state 5 leaves once ++unk_00c0 > 60 */
#define SAVE_FADE_FRAMES   17 /* BeginNormalPaletteFade(0xFFFFFFFF, 0, 0, 16, 0) */
#define SAVE_BGM_FRAMES    20 /* FadeOutBGM(3) until IsBGMStopped (not computed) */
/* Default main-loop stall per sector (not measured: link-protocol-trade.md T4) */
#define SECTOR_STALL       2

/* FRLG mode (Part C). LinkPlayer versions are 0x4000 + these (fr: include/constants/global.h);
 * every receiver reads the low byte only. */
#define VERSION_SAPPHIRE   1
#define VERSION_RUBY       2
#define VERSION_FIRE_RED   4
#define VERSION_LEAF_GREEN 5
#define FRLG_LP_FIELD_2    0x8000 /* fr: link.c: InitLocalLinkPlayer */
/* fr: include/link.h: struct LinkPlayer 0x10..0x12, right after its 8-byte name: name[8..10] of
 * a Gen3LinkPlayer, LinkPlayerBlock 0x20..0x22 */
#define LP_PROGRESS_FLAGS      8 /* progressFlags */
#define LP_NEVER_READ          9 /* neverRead */
#define LP_PROGRESS_FLAGS_COPY 10 /* progressFlagsCopy */
#define PROGRESS_NATIONAL_DEX  0x01 /* IsNationalPokedexEnabled() */
#define PROGRESS_CAN_LINK_RS   0x10 /* FLAG_SYS_CAN_LINK_WITH_RS */
#define PROGRESS_CLEARED_GAME  0xF0 /* the bits fr: trade.c: GetGameProgressForLinkTrade tests */
/* the bits of a partner's progressFlagsCopy fr: trade.c: CanTradeSelectedMon tests */
#define PROGRESS_PARTNER_NATIONAL_DEX 0x0F
/* fr: include/trainer_card.h: struct TrainerCard.version, after its TrainerCardRSE (0x00..0x37) */
#define CARD_VERSION 0x38
/* fr: include/constants/species.h, internal numbers (Ruby's are the same) */
#define SPECIES_MEW    151 /* KANTO_SPECIES_END */
#define SPECIES_DEOXYS 410
#define SPECIES_EGG    412 /* MON_DATA_SPECIES_OR_EGG of an egg */
/* fr: trade.c: BufferTradeParties delay cases 2/6/10/14/18 leave once ++timer > 10 */
#define FRLG_XCHG_DELAY_MAX 10
/* fr: trade.c: CommunicateWhetherMonCanBeTraded, PARTNER_MON_INVALID:
 * QueueAction(180, QUEUE_SEND_DATA) */
#define FRLG_REFUSE_SEND_DELAY 180
/* gReadyCloseLinkType from fr: link.c: SetCloseLinkCallbackAndType: fr: trade.c:
 * CB_WaitToStartTrade (the trade) and CB_InitExitCanceledTrade (back to the room) */
#define FRLG_CLOSE_TYPE_TRADE  32
#define FRLG_CLOSE_TYPE_CANCEL 12
/* fr: trade_scene.c: CheckLinkTimeout: CloseLink and CB2_LinkError once linkTimeoutTimer > 300 */
#define FRLG_LINK_TIMEOUT_FRAMES 300

/* sBlockRequestLookupTable sizes, request types 0..4 */
static const uint16_t block_request_size[] = {200, 200, 100, 220, 40};
#define BLOCK_REQ_CARD 2
/* The card's stars (§A5) index 5-entry tables: gTrainerCardPalettes (trainer_card.c
 * TrainerCard_LoadPalettes) and, less one, the 4 gTrainerCardColorNames (cable_club.c
 * sub_8083BF4); in FRLG sKantoTrainerCardPals or sHoennTrainerCardPals (fr: trainer_card.c) */
#define CARD_STARS     0x01
#define CARD_STARS_MAX 4

/* sMagic: an ASCII C string, compared with strcmp (15 bytes including the NUL) */
static const char link_magic[] = "GameFreak inc.";
#define LP_OFF_MAGIC2 0x2C

static bool is_rs_version(uint16_t version) {
    const uint8_t v = (uint8_t)(version & 0xFF);
    return v == VERSION_RUBY || v == VERSION_SAPPHIRE;
}

static bool is_frlg_version(uint16_t version) {
    const uint8_t v = (uint8_t)(version & 0xFF);
    return v == VERSION_FIRE_RED || v == VERSION_LEAF_GREEN;
}

static void set_error(FakeRuby* g, const char* reason) {
    if(!g->error) g->error = reason;
    if(g->save_dirty) g->save_corrupt = true; /* the link save stops between sectors (§B5.2) */
    g->serial_on = false;
    g->link_open = false;
    g->state = FakeRubyLinkStart1;
}

/* gLink and the link.c statics, as zeroed by EnableSerial/DisableSerial. */
static void link_reset(FakeRuby* g) {
    g->state = FakeRubyLinkStart0;
    g->sio_send = 0;
    g->irq_counter = 0;
    g->vblanks_without_irq = 0;
    g->hs_prev_count = 0;
    g->player_count = 0;
    g->send_index = 0;
    g->recv_index = 0;
    g->checksum = 0;
    g->checksum_available = false;
    g->bad_checksum = false;
    g->lag = false;
    g->recv_nonzero = false;
    g->send_empty = false;
    g->received_nothing = false;
    g->send_pos = 0;
    g->send_count = 0;
    g->recv_pos = 0;
    g->recv_count = 0;
    memset(g->send_queue, 0, sizeof(g->send_queue));
    memset(g->recv_queue, 0, sizeof(g->recv_queue));
}

static void enable_serial(FakeRuby* g) {
    link_reset(g);
    g->serial_on = true;
}

static void disable_serial(FakeRuby* g) {
    link_reset(g);
    g->serial_on = false;
}

/* ---- serial ISR (SerialCB), one call per transfer ---- */

static bool is_handshake_word(uint16_t w) {
    return (w & ~3u) == SLAVE_HANDSHAKE || w == MASTER_HANDSHAKE;
}

/* DoHandshake: write the next word, count players over slots 0..3, success rule. */
static bool do_handshake(FakeRuby* g, const uint16_t slots[4]) {
    g->sio_send = SLAVE_HANDSHAKE; /* handshakeAsMaster is never set on a child */
    uint8_t count = 0;
    for(int i = 0; i < 4; i++) {
        if(is_handshake_word(slots[i])) {
            count++;
        } else {
            if(slots[i] != MLINK_NO_DATA) count = 0;
            break;
        }
    }
    g->player_count = count;
    if(count > 1 && count == g->hs_prev_count && slots[0] == MASTER_HANDSHAKE) return true;
    g->hs_prev_count = count;
    return false;
}

/* DoRecv: transfer #1 checks the previous burst's checksum, #2..#9 fill the receive queue. */
static void do_recv(FakeRuby* g, const uint16_t slots[4]) {
    if(g->send_index == 0) {
        for(uint8_t i = 0; i < g->player_count; i++) {
            if(g->checksum != slots[i] && g->checksum_available) g->bad_checksum = true;
        }
        g->checksum = 0;
        g->checksum_available = true;
        return;
    }
    uint8_t index = (uint8_t)((g->recv_pos + g->recv_count) % FAKE_RUBY_QUEUE_LEN);
    if(g->recv_count < FAKE_RUBY_QUEUE_LEN) {
        /* A full queue is not summed, so it surfaces as a checksum error (§A2). */
        for(uint8_t i = 0; i < g->player_count && i < FAKE_RUBY_PLAYERS; i++) {
            g->checksum = (uint16_t)(g->checksum + slots[i]);
            if(slots[i] != 0) g->recv_nonzero = true;
            g->recv_queue[index][i][g->recv_index] = slots[i];
        }
    }
    g->recv_index++;
    if(g->recv_index == FAKE_RUBY_CMD_LEN && g->recv_nonzero) {
        g->recv_count++;
        g->recv_nonzero = false;
    }
}

/* DoSend: the word for the next transfer; after #9 the checksum, and dequeue. */
static void do_send(FakeRuby* g) {
    if(g->send_index == FAKE_RUBY_CMD_LEN) {
        g->sio_send = g->checksum;
        if(!g->send_empty) {
            g->send_count--;
            g->send_pos = (uint8_t)((g->send_pos + 1) % FAKE_RUBY_QUEUE_LEN);
        } else {
            g->send_empty = false;
        }
        return;
    }
    if(!g->send_empty && g->send_count == 0) g->send_empty = true;
    g->sio_send = g->send_empty ? 0 : g->send_queue[g->send_pos][g->send_index];
    g->send_index++;
}

static void serial_irq(FakeRuby* g, uint16_t master_word, uint16_t own_word) {
    const uint16_t slots[4] = {master_word, own_word, MLINK_NO_DATA, MLINK_NO_DATA};
    if(g->state == FakeRubyLinkConnected) {
        do_recv(g, slots);
        do_send(g);
        if(g->recv_index == FAKE_RUBY_CMD_LEN) { /* SendRecvDone */
            g->send_index = 0;
            g->recv_index = 0;
        }
    } else if(g->state == FakeRubyLinkHandshake) {
        if(do_handshake(g, slots)) {
            /* Child: straight to CONN_ESTABLISHED; send/recv indexes are still 0, so the
             * next transfer is burst #1's checksum slot. Only the counter's link
             * (fake_ruby_open_link) is a linkup phase; Part B relinks keep theirs. */
            g->state = FakeRubyLinkConnected;
            if(g->phase == FakeRubyHandshake) g->phase = FakeRubyLinkup;
        }
    }
    g->irq_counter++;
    g->vblanks_without_irq = 0;
    if(g->irq_counter == 8) g->last_recv_count = g->recv_count;
}

/* LinkVSync, child branch. */
static void link_vsync(FakeRuby* g) {
    if(g->state != FakeRubyLinkHandshake && g->state != FakeRubyLinkConnected) return;
    if(g->vblanks_without_irq < UINT8_MAX) g->vblanks_without_irq++;
    if(g->vblanks_without_irq > LAG_VBLANKS) {
        if(g->state == FakeRubyLinkConnected) {
            g->lag = true;
        } else {
            g->player_count = 0;
        }
    }
}

/* ---- main loop: LinkMain1, LinkMain2 (ProcessRecvCmds, gLinkCallback), tasks ---- */

static void close_link(FakeRuby* g) {
    g->received_link_players = false;
    g->link_open = false;
    disable_serial(g);
}

/* SetCloseLinkCallback: ignored while gLinkCallback is busy. In FRLG mode its 5FFF's word 1 is
 * then 0 (fr: link.c: SetCloseLinkCallback), or `type` for fr: link.c:
 * SetCloseLinkCallbackAndType. Ruby's is always 0, as every Ruby call passes 0. */
static bool set_close_link_callback_type(FakeRuby* g, uint16_t type) {
    if(g->callback != FakeRubyCbNone) return false;
    g->callback = FakeRubyCbReadyCloseLink;
    g->close_type = type;
    return true;
}

static bool set_close_link_callback(FakeRuby* g) {
    return set_close_link_callback_type(g, 0);
}

static void enqueue_send_cmd(FakeRuby* g) {
    bool nonzero = false;
    /* A full queue would be error 0x4000; unreachable here: one enqueue per frame against one
     * dequeue per burst, and a stalled master trips the lag or checksum check first. */
    if(g->send_count < FAKE_RUBY_QUEUE_LEN) {
        uint8_t off = (uint8_t)((g->send_pos + g->send_count) % FAKE_RUBY_QUEUE_LEN);
        for(int i = 0; i < FAKE_RUBY_CMD_LEN; i++) {
            if(g->send_cmd[i] != 0) nonzero = true;
            g->send_queue[off][i] = g->send_cmd[i];
        }
    }
    memset(g->send_cmd, 0, sizeof(g->send_cmd));
    if(nonzero) g->send_count++;
}

static void dequeue_recv_cmds(FakeRuby* g) {
    g->received_nothing = g->recv_count == 0;
    if(g->recv_count == 0) {
        memset(g->recv_cmds, 0, sizeof(g->recv_cmds));
        return;
    }
    memcpy(g->recv_cmds, g->recv_queue[g->recv_pos], sizeof(g->recv_cmds));
    g->recv_count--;
    g->recv_pos = (uint8_t)((g->recv_pos + 1) % FAKE_RUBY_QUEUE_LEN);
}

static void link_main1(FakeRuby* g) {
    switch(g->state) {
    case FakeRubyLinkStart0:
        disable_serial(g);
        g->state = FakeRubyLinkStart1;
        break;
    case FakeRubyLinkStart1:
        if(g->should_advance) {
            enable_serial(g);
            g->state = FakeRubyLinkHandshake;
        }
        break;
    case FakeRubyLinkHandshake:
        break;
    case FakeRubyLinkConnected:
        enqueue_send_cmd(g);
        dequeue_recv_cmds(g);
        break;
    }
    g->should_advance = false;
}

/* InitBlockSend: refused only while a block is active. Otherwise it replaces gLinkCallback,
 * whatever was running (standby, close, held keys...). The gLinkCallback != NULL check belongs
 * to the master's SendBlockRequest, not to the receiver of 0x2222/0xCCCC (§A4). */
static bool init_block_send(FakeRuby* g, const uint8_t* src, uint16_t size) {
    if(g->block_send_active) return false;
    if(g->phase == FakeRubySave &&
       (g->callback == FakeRubyCbStandby || g->callback == FakeRubyCbStandbyForAll)) {
        /* In the link save the replaced standby ends with the block, so its sync passes without
         * the Flipper, as with an ignored SetLinkStandbyCallback */
        set_error(g, "sync skipped");
        return false;
    }
    g->block_send_active = true;
    g->block_send_size = size;
    g->block_send_pos = 0;
    g->request_block = false;
    if(src != g->block_send_buffer) {
        memcpy(g->block_send_buffer, src, size);
        g->send_copy = 0; /* a round's copy is gone */
    }
    g->send_cmd[0] = CMD_INIT_BLOCK;
    g->send_cmd[1] = size;
    g->send_cmd[2] = 0x80 + LOCAL_ID;
    g->callback = FakeRubyCbBlockSendBegin;
    g->block_send_delay = 0;
    return true;
}

static void send_link_player_block(FakeRuby* g) {
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&g->me, block); /* InitLocalLinkPlayer */
    init_block_send(g, block, sizeof(block));
}

/* HandleReceiveRemoteLinkPlayer */
static void receive_link_player(FakeRuby* g, uint8_t slot) {
    g->link_player_pending[slot] = false;
    int pending = 0;
    for(uint8_t i = 0; i < g->player_count && i < FAKE_RUBY_PLAYERS; i++) {
        pending += g->link_player_pending[i];
    }
    if(pending == 0 && !g->received_link_players) {
        g->received_link_players = true;
        g->relink_frames = g->frame - g->open_frame;
    }
}

/* FRLG mode: fr: link.c: ProcessRecvCmds, then ConvertLinkPlayerName, on every LinkPlayer this
 * GBA receives, its own echo included. An R/S sender's three progress bytes are zeroed; then
 * progressFlagsCopy becomes progressFlags, whatever the sender wrote there. */
static void frlg_convert_link_player(Gen3LinkPlayer* lp) {
    if(is_rs_version(lp->version)) {
        lp->name[LP_PROGRESS_FLAGS] = 0;
        lp->name[LP_NEVER_READ] = 0;
        lp->name[LP_PROGRESS_FLAGS_COPY] = 0;
    }
    lp->name[LP_PROGRESS_FLAGS_COPY] = lp->name[LP_PROGRESS_FLAGS];
}

/* IsLinkPlayerDataExchangeComplete: every linkType equals slot 0's. */
static bool link_types_match(const FakeRuby* g) {
    for(int i = 1; i < FAKE_RUBY_PLAYERS; i++) {
        if(g->link_players[i].link_type != g->link_players[0].link_type) return false;
    }
    return true;
}

static void cont_block(FakeRuby* g, uint8_t slot, const uint16_t* cmd) {
    uint8_t* buf = g->block_recv_buffer[slot];
    uint16_t pos = g->block_recv_pos[slot];
    for(int j = 0; j < 7; j++) {
        size_t off = (size_t)pos + 2 * j;
        if(off + 1 < FAKE_RUBY_BLOCK_MAX) {
            buf[off] = (uint8_t)(cmd[1 + j] & 0xFF);
            buf[off + 1] = (uint8_t)(cmd[1 + j] >> 8);
        }
    }
    g->block_recv_pos[slot] = (uint16_t)(pos + 14);
    if(g->block_recv_pos[slot] < g->block_recv_size[slot]) return;

    if(!g->link_player_pending[slot]) {
        g->block_received |= (uint8_t)(1u << slot); /* SetBlockReceivedFlag */
        return;
    }
    /* First block from this slot after OpenLink: always a LinkPlayerBlock. */
    if(memcmp(buf, link_magic, sizeof(link_magic)) != 0 ||
       memcmp(buf + LP_OFF_MAGIC2, link_magic, sizeof(link_magic)) != 0) {
        set_error(g, "magic"); /* CB2_LinkError */
        return;
    }
    gen3_link_player_block_parse(buf, &g->link_players[slot]);
    if(g->frlg) frlg_convert_link_player(&g->link_players[slot]); /* fr: link.c: ProcessRecvCmds */
    receive_link_player(g, slot);
}

/* 0xCCCC: SendBlock(gBlockSendBuffer, sBlockRequestLookupTable[type].size). */
static void block_request(FakeRuby* g, uint16_t type) {
    if(type == BLOCK_REQ_CARD) {
        if(!g->card_ready) {
            set_error(g, "stale card"); /* would send stale gBlockSendBuffer bytes (U3) */
            return;
        }
        init_block_send(g, g->block_send_buffer, block_request_size[type]);
        return;
    }
    /* Ruby indexes the table without a bounds check: it would send whatever lies past it. */
    if(type >= sizeof(block_request_size) / sizeof(block_request_size[0])) {
        set_error(g, "bad request");
        return;
    }
    /* A party-exchange round (§B2.1), refused while a block is active. block_send_chunk checks
     * its first chunk against the round's copy. */
    if(init_block_send(g, g->block_send_buffer, block_request_size[type])) {
        g->request_block = true;
        g->requests++;
    }
}

static void process_recv_cmds(FakeRuby* g) {
    for(uint8_t i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        const uint16_t* cmd = g->recv_cmds[i];
        g->held_keys[i] = 0; /* a key code lasts one game update */
        switch(cmd[0]) {
        case CMD_SEND_LINK_TYPE:
            send_link_player_block(g);
            break;
        case CMD_READY_EXIT_STANDBY:
            /* The flag persists until a sync completes, so a Flipper 2FFE that isn't the answer to
             * this GBA's own (sent, and back as its echo) would let a sync finish without the
             * Flipper (§B5.3). Ruby keeps it silently. */
            if(i != LOCAL_ID &&
               (g->callback != FakeRubyCbStandbyForAll || !g->ready_standby[LOCAL_ID])) {
                set_error(g, "stray 2ffe");
                return;
            }
            g->ready_standby[i] = true;
            break;
        case CMD_READY_CLOSE_LINK:
            g->ready_close[i] = true;
            break;
        case CMD_INIT_BLOCK:
            g->block_recv_pos[i] = 0;
            g->block_recv_size[i] = cmd[1];
            break;
        case CMD_CONT_BLOCK:
            cont_block(g, i, cmd);
            break;
        case CMD_SEND_BLOCK_REQ:
            block_request(g, cmd[1]);
            break;
        case CMD_SEND_HELD_KEYS:
            g->held_keys[i] = cmd[1];
            break;
        default:
            break;
        }
        if(g->error) return;
    }
}

/* LinkCB_BlockSend reads gBlockSendBuffer as it builds each chunk: a copy made after the request
 * but before chunk 1 still goes out. */
static void block_send_chunk(FakeRuby* g) {
    const uint8_t* src = g->block_send_buffer;
    if(g->block_send_pos == 0 && g->request_block && g->send_copy != g->requests) {
        set_error(g, "stale block"); /* not this round's copy: Ruby sends stale bytes (U3) */
        return;
    }
    g->send_cmd[0] = CMD_CONT_BLOCK;
    for(int i = 0; i < 7; i++) {
        size_t off = (size_t)g->block_send_pos + 2 * i;
        uint16_t lo = off < FAKE_RUBY_BLOCK_MAX ? src[off] : 0;
        uint16_t hi = off + 1 < FAKE_RUBY_BLOCK_MAX ? src[off + 1] : 0;
        g->send_cmd[1 + i] = (uint16_t)(lo | (hi << 8));
    }
    g->block_send_pos += 14;
    if(g->block_send_size <= g->block_send_pos) {
        g->block_send_active = false;
        g->callback = FakeRubyCbBlockSendEnd;
    }
}

static void run_callback(FakeRuby* g) {
    switch(g->callback) {
    case FakeRubyCbNone:
        break;
    case FakeRubyCbRequestPlayerData:
        g->callback = FakeRubyCbNone; /* only a master builds 0x2222 */
        break;
    case FakeRubyCbBlockSendBegin:
        if(++g->block_send_delay > 2) g->callback = FakeRubyCbBlockSend;
        break;
    case FakeRubyCbBlockSend:
        block_send_chunk(g);
        break;
    case FakeRubyCbBlockSendEnd:
        g->callback = FakeRubyCbNone;
        break;
    case FakeRubyCbReadyCloseLink:
        if(g->last_recv_count == 0) {
            g->send_cmd[0] = CMD_READY_CLOSE_LINK;
            g->send_cmd[1] = g->close_type; /* FRLG: fr: link.c: BuildSendCmd */
            g->callback = FakeRubyCbWaitCloseLink;
        }
        break;
    case FakeRubyCbWaitCloseLink:
        if(g->ready_close[0] && g->ready_close[1]) {
            close_link(g);
            g->callback = FakeRubyCbNone;
            g->phase = FakeRubyClosed;
        }
        break;
    case FakeRubyCbStandby:
        if(g->last_recv_count == 0) {
            g->send_cmd[0] = CMD_READY_EXIT_STANDBY;
            g->callback = FakeRubyCbStandbyForAll;
            g->standby_frame = g->frame;
        }
        break;
    case FakeRubyCbStandbyForAll:
        if(g->ready_standby[0] && g->ready_standby[1]) {
            memset(g->ready_standby, 0, sizeof(g->ready_standby));
            g->callback = FakeRubyCbNone;
        }
        break;
    case FakeRubyCbSendHeldKeys:
        /* BuildSendCmd(0xCAFE), every frame, even when the game update is skipped */
        if(g->received_link_players && g->held_key_to_send != 0) {
            g->send_cmd[0] = CMD_SEND_HELD_KEYS;
            g->send_cmd[1] = g->held_key_to_send;
        }
        break;
    }
}

static void link_main2(FakeRuby* g) {
    if(!g->link_open) return;
    memset(g->send_cmd, 0, sizeof(g->send_cmd));
    if(g->state != FakeRubyLinkConnected) return;
    process_recv_cmds(g);
    if(g->error) return;
    run_callback(g);
    if(!g->link_open) return; /* closed by WaitCloseLink this frame */
    /* CheckErrorStatus */
    if(g->bad_checksum) {
        set_error(g, "checksum");
    } else if(g->lag) {
        set_error(g, "lag");
    }
}

static void next_task(FakeRuby* g, FakeRubyTask task) {
    g->task = task;
    g->task_frame = g->frame;
}

/* ---- Trade Center room: overworld.c, field_fadetransition.c (§A7, §B7) ---- */

static bool any_player_in(const FakeRuby* g, uint8_t state) { /* sub_8054FC0 */
    for(int i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        if(g->link_state[i] == state) return true;
    }
    return false;
}

static bool all_players_in(const FakeRuby* g, uint8_t state) { /* sub_8054F88 */
    for(int i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        if(g->link_state[i] != state) return false;
    }
    return true;
}

/* sub_8055824 (exit prompt) / sub_805585C (forced exit): the script takes over the local player.
 * Neither script unlocks the controls before ExitLinkRoom replaces the key intercept. */
static void start_leave_script(FakeRuby* g, FakeRubyScript script, uint16_t updates) {
    g->key_cb = FakeRubyKeyDeferToEventScript;
    g->controls_locked = true;
    g->script = script;
    g->script_updates = updates;
    g->phase = FakeRubyLeaving;
}

/* sub_8055808: the seat's coord-event script takes over the local player (§B1.1). Its
 * `special sub_8083B5C` starts the seat task when the script runs, later in this update. */
static void start_seat_script(FakeRuby* g, uint8_t seat) {
    g->key_cb = FakeRubyKeyDeferToEventScript;
    g->controls_locked = true;
    g->script = FakeRubyScriptSeat;
    g->seat = seat;
    g->on_seat = true;
    g->phase = FakeRubySeated;
}

/* sub_8054FF8 [HandleLinkPlayerKeyInput], per slot per game update. While anyone is
 * EXITING_ROOM, an IDLE player is made BUSY and that update's key is dropped (early return), so
 * only a player that is already not IDLE takes 0x17. A partner's 0x17 processed in the update
 * right after our own echoed 0x17 is lost, and we then wait forever for all EXITING_ROOM.
 * seat_step: this GBA's player finished a step onto a seat in this update (movement mode 2). */
static void handle_link_player_key(FakeRuby* g, uint8_t slot, uint16_t key, bool seat_step) {
    bool local = slot == LOCAL_ID;
    uint8_t* state = &g->link_state[slot];
    if(*state == PLAYER_IDLE) {
        /* sub_8055648 [TryGetTileEventScript]: only the local avatar ever steps in this model,
         * and only onto a seat. Every GBA makes the slot BUSY; the local one runs the script. */
        if(local && seat_step) {
            *state = PLAYER_BUSY;
            start_seat_script(g, g->sit_seat);
            return;
        }
        if(any_player_in(g, PLAYER_EXITING)) {
            *state = PLAYER_BUSY;
            if(local) start_leave_script(g, FakeRubyScriptTerminate, TERMINATE_UPDATES);
            return;
        }
        switch(key) {
        case KEY_CODE_START:
        case KEY_CODE_RECV_QUEUE:
        case KEY_CODE_SEND_QUEUE:
            *state = PLAYER_BUSY; /* the local player never sends these in this model */
            break;
        case KEY_CODE_DPAD_DOWN: /* the exit prompt needs the south exit, facing south */
            if(local && g->facing_exit) {
                *state = PLAYER_BUSY;
                start_leave_script(g, FakeRubyScriptPrompt, PROMPT_UPDATES);
            }
            break;
        default: /* 0x19 (A) needs map data and is not modelled */
            break;
        }
    }
    switch(key) {
    case KEY_CODE_EXIT_ROOM:
        *state = PLAYER_EXITING;
        break;
    case KEY_CODE_READY:
        *state = PLAYER_READY;
        break;
    case KEY_CODE_IDLE:
        *state = PLAYER_IDLE;
        if(local) g->key_cb = FakeRubyKeySelfIdle;
        break;
    case KEY_CODE_EXIT_SEAT:
        if(*state == PLAYER_READY) *state = PLAYER_BUSY;
        break;
    default:
        break;
    }
}

/* gUnknown_03000584: this GBA's next key code, after every player's key was handled. b_new:
 * JOY_NEW(B_BUTTON) in this update. */
static uint16_t key_intercept(FakeRuby* g, bool b_new) {
    switch(g->key_cb) {
    case FakeRubyKeySelfIdle:
        /* 0x1B/0x1C flow control needs a queue above 4; one burst per frame keeps both <= 1. */
        if(g->controls_locked) return KEY_CODE_EMPTY;
        if(g->resit_updates > 0) { /* sub_80552B0: JOY_HELD(DPAD_UP), then DPAD_DOWN */
            return g->resit_updates > RESIT_STEP_UPDATES ? KEY_CODE_DPAD_UP : KEY_CODE_DPAD_DOWN;
        }
        return g->facing_exit ? KEY_CODE_DPAD_DOWN : KEY_CODE_EMPTY;
    case FakeRubyKeySendExitRoomKey:
        g->key_cb = FakeRubyKeyWaitForPlayersToExit;
        return KEY_CODE_EXIT_ROOM;
    case FakeRubyKeyWaitForPlayersToExit:
        if(all_players_in(g, PLAYER_EXITING)) {
            g->script = FakeRubyScriptExitStart; /* ScriptContext_SetupScript(gUnknown_081A4508) */
            g->key_cb = FakeRubyKeySendNothing;
        }
        return KEY_CODE_EMPTY;
    case FakeRubyKeyDeferToEventScript:
        /* The exit scripts never unlock before ExitLinkRoom replaces this (start_leave_script);
         * the seat script does when it ends after a cancel. */
        if(g->controls_locked) return KEY_CODE_EMPTY;
        g->key_cb = FakeRubyKeyIdle;
        return KEY_CODE_IDLE;
    case FakeRubyKeySetReady:
        g->key_cb = FakeRubyKeyReady;
        return KEY_CODE_READY;
    case FakeRubyKeyReady:
        if(g->link_state[LOCAL_ID] == PLAYER_READY && b_new) {
            g->key_cb = FakeRubyKeyExitingSeat;
            return KEY_CODE_EXIT_SEAT;
        }
        return KEY_CODE_EMPTY;
    case FakeRubyKeyExitingSeat:
    case FakeRubyKeyIdle:
    case FakeRubyKeySendNothing:
        return KEY_CODE_EMPTY;
    }
    return KEY_CODE_EMPTY;
}

/* Callback2: the running script, then sub_8081050 [Task_ReturnFromLinkRoom]. */
static void run_room_script(FakeRuby* g) {
    switch(g->script) {
    case FakeRubyScriptNone:
    case FakeRubyScriptExitWait: /* WaitCloseLink closes the link; then the warp to PC 2F */
        break;
    case FakeRubyScriptPrompt:
        if(--g->script_updates == 0) { /* YES: goto TradeRoom_TerminateLink */
            g->script = FakeRubyScriptTerminate;
            g->script_updates = TERMINATE_UPDATES;
        }
        break;
    case FakeRubyScriptTerminate:
        if(--g->script_updates == 0) {
            g->key_cb = FakeRubyKeySendExitRoomKey; /* special ExitLinkRoom -> sub_805559C */
            g->script = FakeRubyScriptNone;
        }
        break;
    case FakeRubyScriptExitStart:
        g->callback = FakeRubyCbNone; /* ClearLinkCallback_2: no more CAFE */
        g->script = FakeRubyScriptExitFade;
        g->script_updates = EXIT_FADE_UPDATES;
        break;
    case FakeRubyScriptExitFade:
        if(--g->script_updates == 0) {
            set_close_link_callback(g);
            g->script = FakeRubyScriptExitWait;
        }
        break;
    case FakeRubyScriptSeat: /* special sub_8083B5C -> sub_8083A84: the seat task, then waitstate */
        g->seat_task = FakeRubySeatPrint;
        g->seat_updates = SEAT_PRINT_UPDATES;
        g->script = FakeRubyScriptSeatWait;
        break;
    case FakeRubyScriptSeatWait:
        break;
    case FakeRubyScriptSeatEnd: /* ScriptContext_RunScript: the script ended */
        g->controls_locked = false;
        g->script = FakeRubyScriptNone;
        break;
    }
}

/* sub_80554F8 [GetCableClubPartnersReady] (§B1.3): 0 wait, 1 everyone ready, 2 cancel. */
static int partners_ready(const FakeRuby* g) {
    uint8_t own = g->link_state[LOCAL_ID];
    if(any_player_in(g, PLAYER_EXITING)) return 2;
    if(g->key_cb == FakeRubyKeyReady && own != PLAYER_READY) return 0;
    if(g->key_cb == FakeRubyKeyExitingSeat && own == PLAYER_BUSY) return 2;
    return all_players_in(g, PLAYER_READY) ? 1 : 0;
}

/* sub_80839DC [Task_EnterCableClubSeat], in callback2's RunTasks after the script (§B1.1). */
static void run_seat_task(FakeRuby* g) {
    switch(g->seat_task) {
    case FakeRubySeatNone:
        break;
    case FakeRubySeatPrint: /* case 0 ShowFieldMessage; case 1 once IsFieldMessageBoxHidden */
        if(--g->seat_updates > 0) break;
        g->key_cb = FakeRubyKeySetReady; /* sub_8055574 [SetInCableClubSeat] */
        g->me.id = g->seat; /* sub_8007270: localLinkPlayer.id, sent from the next relink on */
        g->seat_task = FakeRubySeatWait;
        break;
    case FakeRubySeatWait: /* case 2, no timeout */
        switch(partners_ready(g)) {
        case 1: /* HideFieldMessageBox; follow-up sub_8083B44 starts sub_8083AAC */
            g->seat_task = FakeRubySeatNone;
            g->start_case = 0;
            next_task(g, FakeRubyTaskStartTrade);
            break;
        case 2:
            g->seat_task = FakeRubySeatCancel;
            break;
        default:
            break;
        }
        break;
    case FakeRubySeatCancel: /* case 3: sub_8055588 [SetLinkWaitingForScript], ScriptContext_Enable */
        g->key_cb = FakeRubyKeyDeferToEventScript;
        g->script = FakeRubyScriptSeatEnd;
        g->seat_task = FakeRubySeatNone;
        g->phase = FakeRubyRoom;
        break;
    }
}

/* fake_ruby_resit (§B7 step 4), after this update's key: one held D-pad key per game update. An
 * IDLE player moves without changing state (sub_8054FF8, sub_8055BFC). The step off the seat ends
 * with the Up updates, the step back on with the Down ones; its coord event then runs at the next
 * update, as for fake_ruby_sit. It stops if the player leaves SelfIdle or IDLE meanwhile. */
static void resit_step(FakeRuby* g) {
    if(g->resit_updates == 0) return;
    if(g->link_state[LOCAL_ID] != PLAYER_IDLE || g->key_cb != FakeRubyKeySelfIdle) {
        g->resit_updates = 0;
        return;
    }
    if(--g->resit_updates == RESIT_STEP_UPDATES) g->on_seat = false;
    if(g->resit_updates == 0) {
        g->sit_pending = true;
        g->sit_seat = g->seat;
    }
}

/* task_mpl_807DD60 cases 1-2 after the 0x1111 relink: the fade-in, then UnlockPlayerFieldControls.
 * It runs with the room's tasks, so it counts game updates. */
static void fade_in_step(FakeRuby* g) {
    if(g->fade_in_updates == 0 || --g->fade_in_updates > 0) return;
    g->controls_locked = false;
    g->phase = FakeRubyRoom;
}

/* One game update in the room: callback1 (sub_8055354 [CB1_OverworldLink]: every player's key
 * in lockstep, own echo included, then sub_8055280 keeps 0x11..0x1D), then callback2 (the script,
 * then the tasks). fake_ruby_sit, fake_ruby_stand_up and fake_ruby_resit take effect in this
 * update. */
static void room_update(FakeRuby* g) {
    const bool seat_step = g->sit_pending && !g->on_seat;
    const bool b_new = g->b_press;
    const bool resit = g->resit_pending;
    g->sit_pending = false;
    g->b_press = false;
    g->resit_pending = false;
    for(uint8_t i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        handle_link_player_key(g, i, g->held_keys[i], seat_step);
    }
    /* The step ends on the seat tile even when its coord event didn't run (the player wasn't
     * IDLE), and standing there never runs it again. */
    if(seat_step) g->on_seat = true;
    if(resit && g->phase == FakeRubyRoom && g->link_state[LOCAL_ID] == PLAYER_IDLE &&
       g->key_cb == FakeRubyKeySelfIdle && !g->controls_locked) {
        g->resit_updates = 2 * RESIT_STEP_UPDATES;
    }
    uint16_t key = key_intercept(g, b_new);
    g->held_key_to_send = (key >= KEY_CODE_EMPTY && key < KEY_CODE_END) ? key : KEY_CODE_EMPTY;
    resit_step(g);
    run_room_script(g);
    run_seat_task(g);
    fade_in_step(g);
}

/* OpenLink (link.c): ResetSerial, InitLink (EnableSerial, gSendCmd = 0xEFFF filler),
 * gLinkCallback = LinkCB_RequestPlayerDataExchange, the handshake trigger task. */
static void open_link(FakeRuby* g) {
    enable_serial(g);
    for(int i = 0; i < FAKE_RUBY_CMD_LEN; i++) g->send_cmd[i] = CMD_NONE;
    g->link_open = true;
    g->callback = FakeRubyCbRequestPlayerData;
    g->block_send_active = false;
    g->block_received = 0;
    g->received_link_players = false;
    memset(g->link_players, 0, sizeof(g->link_players));
    memset(g->block_recv_size, 0, sizeof(g->block_recv_size));
    memset(g->block_recv_pos, 0, sizeof(g->block_recv_pos));
    memset(g->ready_close, 0, sizeof(g->ready_close));
    g->close_type = 0; /* FRLG: fr: link.c: OpenLink */
    memset(g->ready_standby, 0, sizeof(g->ready_standby));
    for(int i = 0; i < FAKE_RUBY_PLAYERS; i++) g->link_player_pending[i] = true;
    g->trigger_active = true; /* CreateTask(Task_TriggerHandshake) */
    g->trigger_runs = 0;
    g->open_frame = g->frame;
    g->relink_frames = UINT32_MAX;
    g->requests = 0;
    g->request_block = false;
}

/* sub_8083AAC [Task_StartWiredTrade] (§B1.5): from the seat to the trade menu. */
static void run_start_trade(FakeRuby* g) {
    switch(g->start_case) {
    case 0: /* LockPlayerFieldControls, FadeScreen(1, 0), ClearLinkCallback_2: no more CAFE and
               no more lockstep skipping */
        g->controls_locked = true;
        g->callback = FakeRubyCbNone;
        g->task_frame = g->frame;
        g->start_case = 1;
        break;
    case 1:
        if(g->frame - g->task_frame >= START_FADE_FRAMES) g->start_case = 2;
        break;
    case 2: /* m4aMPlayAllStop, SetCloseLinkCallback: it acts only while gLinkCallback is NULL */
        set_close_link_callback(g);
        g->start_case = 3;
        break;
    case 3: /* CloseLink cleared gReceivedRemoteLinkPlayers */
        if(!g->received_link_players) next_task(g, FakeRubyTaskStartMenu); /* SetMainCallback2 */
        break;
    default:
        break;
    }
}

static void xchg_next(FakeRuby* g) {
    g->xchg_case++;
    g->xchg_case_frame = g->frame;
}

/* sub_8048D44 cases 0/4/7/10/13: round r's bytes into gBlockSendBuffer (§B2.1). */
static void xchg_copy(FakeRuby* g, uint8_t round) {
    if(round <= 3) {
        memcpy(g->block_send_buffer, g->party[2 * (round - 1)], 2 * GEN3_PARTY_SIZE);
    } else if(round == 4) {
        memset(g->block_send_buffer, 0, MAIL_BLOCK_SIZE); /* gSaveBlock1.mail: this GBA has none */
    } else {
        memset(g->block_send_buffer, 0, FAKE_RUBY_RIBBONS); /* giftRibbons; bytes 11.. stale */
    }
    g->send_copy = round;
}

/* max_enemy_species, for one stored Flipper slot: one that doesn't decrypt, or has species 0,
 * doesn't count. */
static void note_enemy_species(FakeRuby* g, const uint8_t* mon) {
    Gen3Pkm p;
    if(!gen3_pkm_decrypt(mon, GEN3_PARTY_SIZE, &p)) return;
    uint16_t species = (uint16_t)gen3_pkm_get(&p, Gen3FieldSpecies);
    if(species == 0) return;
    if(gen3_pkm_get(&p, Gen3FieldIsEgg) != 0) species = SPECIES_EGG;
    if(species > g->max_enemy_species) g->max_enemy_species = species;
}

/* Cases 3/6/9/12/15: the Flipper's block (slot mpId ^ 1 = 0) into round r's place. */
static void xchg_store(FakeRuby* g, uint8_t round) {
    const uint8_t* src = g->block_recv_buffer[0];
    if(round <= 3) {
        memcpy(g->enemy[2 * (round - 1)], src, 2 * GEN3_PARTY_SIZE);
        note_enemy_species(g, g->enemy[2 * (round - 1)]);
        note_enemy_species(g, g->enemy[2 * (round - 1) + 1]);
    } else if(round == 4) {
        memcpy(g->partner_mail, src, sizeof(g->partner_mail));
    } else {
        memcpy(g->partner_ribbons, src, sizeof(g->partner_ribbons));
    }
}

/* sub_8048D44 [BufferTradeParties], one case per frame; true at case 16 (§B2.1). A wait case
 * that lasts XCHG_STALL_FRAMES is the §B2.2 deadlock: Ruby's black screen. */
static bool buffer_trade_parties(FakeRuby* g) {
    const uint8_t c = g->xchg_case;
    switch(c) {
    case 0: /* round r = c / 3 + 1 */
    case 4:
    case 7:
    case 10:
    case 13:
        xchg_copy(g, (uint8_t)(c / 3 + 1));
        xchg_next(g);
        return false;
    case 1: /* IsLinkTaskFinished, then ResetBlockReceivedFlags if any flag is set */
        if(g->callback != FakeRubyCbNone) break;
        g->block_received = 0;
        xchg_next(g);
        return false;
    case 2: /* sub_8007E9C [SendBlockRequest]: the master's; the child just advances */
    case 5:
    case 8:
    case 11:
    case 14:
        xchg_next(g);
        return false;
    case 3: /* round r = c / 3: the Flipper's block and our own echo */
    case 6:
    case 9:
    case 12:
    case 15:
        if(g->block_received != 0x3) break;
        xchg_store(g, (uint8_t)(c / 3));
        g->block_received = 0;
        xchg_next(g);
        return false;
    default: /* 16: the Shedinja nickname fix (not modelled) */
        return true;
    }
    if(g->frame - g->xchg_case_frame >= XCHG_STALL_FRAMES) set_error(g, "exchange stalled");
    return false;
}

/* FRLG mode: fr: trade.c: BufferTradeParties (bufferPartyState), one case per frame; true at case
 * 21. Ruby's copy, wait and reset cases renumbered, and before each round's request case a delay
 * case that holds this child too: ++timer > 10, so 11 frames, with no id == 0 test (§C4). The
 * request cases are the master's (SendBlockRequest), so this child just advances. The wait cases
 * keep Ruby's rules: case 1's reset makes the round-1 deadlock, and one that lasts
 * XCHG_STALL_FRAMES is the black screen ("exchange stalled"). */
static bool frlg_buffer_trade_parties(FakeRuby* g) {
    const uint8_t c = g->xchg_case;
    switch(c) {
    case 0: /* round r = c / 4 + 1 into gBlockSendBuffer */
    case 5:
    case 9:
    case 13:
    case 17:
        xchg_copy(g, (uint8_t)(c / 4 + 1));
        xchg_next(g);
        return false;
    case 1: /* IsLinkTaskFinished, then ResetBlockReceivedFlags if any flag is set */
        if(g->callback != FakeRubyCbNone) break;
        g->block_received = 0;
        xchg_next(g);
        return false;
    case 2: /* the delay before round c / 4 + 1's request */
    case 6:
    case 10:
    case 14:
    case 18:
        if(g->frame - g->xchg_case_frame > FRLG_XCHG_DELAY_MAX) xchg_next(g);
        return false;
    case 3: /* if (id == 0) SendBlockRequest: the master's; this child just advances */
    case 7:
    case 11:
    case 15:
    case 19:
        xchg_next(g);
        return false;
    case 4: /* round r = c / 4: the Flipper's block and our own echo */
    case 8:
    case 12:
    case 16:
    case 20:
        if(g->block_received != 0x3) break;
        xchg_store(g, (uint8_t)(c / 4));
        g->block_received = 0;
        xchg_next(g);
        return false;
    default: /* 21: the Shedinja nickname fix (not modelled) */
        return true;
    }
    if(g->frame - g->xchg_case_frame >= XCHG_STALL_FRAMES) set_error(g, "exchange stalled");
    return false;
}

static bool all_zero(const uint8_t* p, size_t len) {
    for(size_t i = 0; i < len; i++) {
        if(p[i] != 0) return false;
    }
    return true;
}

/* §B2.5 rules 1-4, for one decrypted slot of species != 0. */
static bool ruby_can_hold(const Gen3Pkm* p) {
    const uint32_t species = gen3_pkm_get(p, Gen3FieldSpecies);
    const uint32_t item = gen3_pkm_get(p, Gen3FieldHeldItem);
    if(species > SPECIES_LAST || (species > SPECIES_JOHTO_LAST && species < SPECIES_HOENN_FIRST)) {
        return false;
    }
    for(int m = 0; m < 4; m++) {
        if(gen3_pkm_get(p, (Gen3Field)(Gen3FieldMove1 + m)) >= MOVES_COUNT) return false;
    }
    if(item >= ITEMS_COUNT || item == ITEM_ENIGMA_BERRY) return false;
    if(item >= ITEM_MAIL_FIRST && item <= ITEM_MAIL_LAST) return false;
    return gen3_pkm_get(p, Gen3FieldMail) == MAIL_NONE;
}

/* State 6: CalculateEnemyPartyCount counts slots up to the first species 0 (§B2.5); a count of
 * 0 is legal (R8). Stricter than Ruby, which shows whatever it gets: a slot that fails its
 * checksum (a Bad Egg on Ruby) is "bad egg", and one Ruby can't hold is "bad offer". */
static void calculate_enemy_party_count(FakeRuby* g) {
    g->enemy_count = 0;
    for(uint8_t i = 0; i < FAKE_RUBY_PARTY; i++) {
        Gen3Pkm p;
        if(all_zero(g->enemy[i], GEN3_PARTY_SIZE)) return;
        if(!gen3_pkm_decrypt(g->enemy[i], GEN3_PARTY_SIZE, &p)) {
            set_error(g, "bad egg");
            return;
        }
        if(gen3_pkm_get(&p, Gen3FieldSpecies) == 0) return;
        if(!ruby_can_hold(&p)) {
            set_error(g, "bad offer");
            return;
        }
        g->enemy_count++;
    }
}

/* ---- The trade menu (trade.c, §B3) ---- */

/* unk_007b, and the state the tests see */
static void set_trade_state(FakeRuby* g, uint8_t state) {
    g->trade_state = state;
    g->trade_state_frame = g->frame;
    switch(state) {
    case TRADE_BROWSING:
        g->menu = FakeRubyMenuBrowsing;
        break;
    case TRADE_STANDBY_READY:
    case TRADE_STANDBY:
        g->menu = FakeRubyMenuStandby;
        break;
    case TRADE_SLIDE_IN:
    case TRADE_CONFIRM_WAIT:
        g->menu = FakeRubyMenuConfirming;
        break;
    case TRADE_PROMPT:
        g->menu = FakeRubyMenuPrompt;
        break;
    case TRADE_MESSAGE:
        g->menu = FakeRubyMenuMessage;
        break;
    default: /* 10-13 */
        g->menu = FakeRubyMenuClosing;
        break;
    }
    /* fake_ruby_answer waits for a prompt of this confirm sequence only */
    if(g->menu != FakeRubyMenuStandby && g->menu != FakeRubyMenuConfirming &&
       g->menu != FakeRubyMenuPrompt) {
        g->answer_pending = false;
    }
}

/* Trade_SendData: SendBlock(linkData, 20), refused while a block is active. linkData keeps its
 * words between sends (§B3.1). */
static void trade_send_data(FakeRuby* g) {
    uint8_t block[TRADE_BLOCK_SIZE];
    for(int i = 0; i < LINK_DATA_WORDS; i++) {
        block[2 * i] = (uint8_t)(g->link_data[i] & 0xFF);
        block[2 * i + 1] = (uint8_t)(g->link_data[i] >> 8);
    }
    if(!init_block_send(g, block, sizeof(block))) return;
    switch(g->link_data[0]) {
    case CODE_READY:
        g->seq = FakeRubySeqReady;
        break;
    case CODE_CONFIRM:
        g->seq = g->seq == FakeRubySeqSet ? FakeRubySeqConfirmed : FakeRubySeqNone;
        break;
    default:
        g->seq = FakeRubySeqNone;
        break;
    }
}

/* Own party slots up to the first empty one (gPlayerPartyCount) */
static uint8_t own_party_count(const FakeRuby* g) {
    uint8_t count = 0;
    while(count < FAKE_RUBY_PARTY && !all_zero(g->party[count], GEN3_PARTY_SIZE)) count++;
    return count;
}

/* sub_804ACF4 [ComputePartyTradeableFlags], own side: neither an egg (a Bad Egg reads as one) nor
 * fainted. */
static bool can_battle(const uint8_t* mon) {
    Gen3Pkm p;
    if(!gen3_pkm_decrypt(mon, GEN3_PARTY_SIZE, &p)) return false;
    return gen3_pkm_get(&p, Gen3FieldIsEgg) == 0 && gen3_pkm_get(&p, Gen3FieldHp) != 0;
}

/* FRLG mode: fr: pokemon.c: GetMonData MON_DATA_SPECIES_OR_EGG, SPECIES_EGG for an egg, else the
 * species. A slot that fails its checksum reads as an egg: FRLG flags a Bad Egg as one. */
static uint16_t species_or_egg(const uint8_t* mon) {
    Gen3Pkm p;
    if(!gen3_pkm_decrypt(mon, GEN3_PARTY_SIZE, &p)) return SPECIES_EGG;
    const uint16_t species = (uint16_t)gen3_pkm_get(&p, Gen3FieldSpecies);
    return species != 0 && gen3_pkm_get(&p, Gen3FieldIsEgg) != 0 ? SPECIES_EGG : species;
}

/* FRLG mode: a Mew or Deoxys (internal species) whose fateful-encounter bit is 0
 * (MON_DATA_MODERN_FATEFUL_ENCOUNTER, fr: trade.c: CanTradeSelectedMon,
 * CheckValidityOfTradeMons). */
static bool unfateful(const uint8_t* mon) {
    Gen3Pkm p;
    if(!gen3_pkm_decrypt(mon, GEN3_PARTY_SIZE, &p)) return false;
    const uint32_t species = gen3_pkm_get(&p, Gen3FieldSpecies);
    return (species == SPECIES_MEW || species == SPECIES_DEOXYS) &&
           gen3_pkm_get(&p, Gen3FieldFateful) == 0;
}

/* FRLG mode: fr: trade.c: CanTradeSelectedMon on own slot `slot` of `count`, the cartridge build
 * (no BUGFIX). This save's National Dex is its own progressFlags bit 0
 * (fake_ruby_set_frlg_progress); the Flipper's is its progressFlagsCopy, read raw from its
 * LinkPlayer as this GBA converted it (frlg_convert_link_player). */
static FakeRubyCantTrade frlg_can_trade(const FakeRuby* g, uint8_t slot, uint8_t count) {
    const uint16_t selected = species_or_egg(g->party[slot]);
    if((g->me.name[LP_PROGRESS_FLAGS] & PROGRESS_NATIONAL_DEX) == 0) {
        if(selected > SPECIES_MEW) return FakeRubyCantTradeNational; /* an Egg (412) too */
        if(selected == 0) return FakeRubyCantTradeEggYet; /* meant for SPECIES_EGG */
    }
    const Gen3LinkPlayer* partner = &g->link_players[0]; /* gLinkPlayers[GetMultiplayerId() ^ 1] */
    if(!is_rs_version(partner->version) &&
       (partner->name[LP_PROGRESS_FLAGS_COPY] & PROGRESS_PARTNER_NATIONAL_DEX) == 0) {
        if(selected == SPECIES_EGG) return FakeRubyCantTradePartnerEggYet;
        if(selected > SPECIES_MEW) return FakeRubyCantTradeInvalidMon;
    }
    if(unfateful(g->party[slot])) return FakeRubyCantTradeInvalidMon;
    /* numMonsLeft: the other slots' species, an egg as none. No HP test, unlike Ruby's */
    for(uint8_t i = 0; i < count; i++) {
        const uint16_t species = species_or_egg(g->party[i]);
        if(i != slot && species != 0 && species != SPECIES_EGG) return FakeRubyCantTradeNone;
    }
    return FakeRubyCantTradeLastMon;
}

/* sub_8049860 [CB_ProcessSelectedMonInput] TRADE on own slot: sub_80499F0
 * [CheckValidityOfTradeMons] wants another battler, then sub_8049620 [SetReadyToTrade]. FRLG
 * mode: frlg_can_trade instead (fr: trade.c: CB_ProcessSelectedMonInput); a refusal queues its
 * message and waits for A (CB_HandleTradeCanceled), as Ruby's does. */
static void trade_select(FakeRuby* g, uint8_t slot) {
    const uint8_t count = own_party_count(g);
    if(slot >= count) return; /* sub_8048C70: the cursor can't reach an empty slot */
    g->cursor = slot;
    if(g->frlg) {
        g->cant_trade = frlg_can_trade(g, slot, count);
    } else {
        bool other = false;
        for(uint8_t i = 0; i < count; i++) {
            if(i != slot && can_battle(g->party[i])) other = true;
        }
        /* "That's your only POKéMON for battle." */
        g->cant_trade = other ? FakeRubyCantTradeNone : FakeRubyCantTradeLastMon;
    }
    if(g->cant_trade != FakeRubyCantTradeNone) {
        set_trade_state(g, TRADE_MESSAGE);
        return;
    }
    set_trade_state(g, TRADE_STANDBY_READY);
    g->link_data[0] = CODE_READY;
    g->link_data[1] = slot;
    trade_send_data(g); /* no IsLinkTaskFinished guard: dropped while a block is active */
}

/* CANCEL, then sub_8049BC0 [CB_ProcessCancelTradeInput] "Cancel trade?" YES */
static void trade_request_cancel(FakeRuby* g) {
    g->cursor = CURSOR_CANCEL;
    g->link_data[0] = CODE_CANCEL;
    g->link_data[1] = 0;
    g->send_queued = true; /* sub_804AADC [QueueAction] */
    g->send_delay = CANCEL_SEND_DELAY;
    set_trade_state(g, TRADE_STANDBY);
}

/* sub_8049AC0 [CB_ProcessConfirmTradeInput]: YES (sub_8049A20) or NO, each sent only while
 * IsLinkTaskFinished, else this GBA waits in Link standby forever (§B3.4). sub_8049A20's own BBCC
 * (no battler left) can't happen: trade_select already checked this GBA's other slots.
 * FRLG mode, YES: fr: trade.c: CommunicateWhetherMonCanBeTraded. CheckValidityOfTradeMons tests
 * the Flipper's Pokemon first (PARTNER_MON_INVALID): then no BBBB, but its message and BBCC 180
 * frames later, queued with no IsLinkTaskFinished guard, and CB_IDLE (Standby). Its own-side
 * PLAYER_MON_INVALID (no other live Pokemon, by HP: frlg_can_trade counts by species) isn't
 * modelled: it too sends BBCC 180 frames later, which the Flipper answers with the same DDEE. */
static void trade_answer(FakeRuby* g, bool yes) {
    const bool link_idle = g->callback == FakeRubyCbNone;
    if(yes && g->frlg) {
        g->partner_mon_invalid = unfateful(g->enemy[g->partner_pos % FAKE_RUBY_PARTY]);
        if(g->partner_mon_invalid) { /* "The other TRAINER's POKéMON can't be traded now." */
            g->link_data[0] = CODE_CONFIRM_NO;
            g->send_queued = true;
            g->send_delay = FRLG_REFUSE_SEND_DELAY;
            set_trade_state(g, TRADE_STANDBY);
            return;
        }
    }
    if(yes) {
        g->link_data[0] = CODE_CONFIRM;
        if(link_idle) trade_send_data(g);
    } else if(link_idle) {
        g->link_data[0] = CODE_CONFIRM_NO;
        trade_send_data(g);
    }
    set_trade_state(g, TRADE_STANDBY);
}

/* sub_80491E4 [Follower_ReadLinkBuffer]: the Flipper's block, in any state and with no check
 * that it was expected (§B3.3). Ruby checks nothing; the fake adds the DDDD and CCDD rules of
 * §B3.4. */
static void trade_flipper_block(FakeRuby* g) {
    const uint8_t* buf = g->block_recv_buffer[0];
    const uint16_t code = (uint16_t)(buf[0] | (buf[1] << 8));
    const uint16_t arg = (uint16_t)(buf[2] | (buf[3] << 8));
    const FakeRubySeq seq = g->seq;
    g->seq = FakeRubySeqNone; /* every Flipper block ends the sequence, but a DDDD it awaits */
    switch(code) {
    case CODE_BOTH_CANCEL: /* fade out, back to the room (§B6.3) */
        set_trade_state(g, TRADE_FADE_ROOM);
        break;
    case CODE_PARTNER_CANCEL: /* "Your friend wants to trade POKéMON." */
    case CODE_PLAYER_CANCEL: /* "The trade has been canceled." */
        set_trade_state(g, TRADE_MESSAGE);
        break;
    case CODE_SET_MONS:
        /* sub_8049ED4 would animate partyIcons[1][arg], made only for the partner's count */
        if(arg >= g->enemy_count) {
            set_error(g, "empty party");
            return;
        }
        if(seq != FakeRubySeqReady || g->trade_state != TRADE_STANDBY_READY) {
            set_error(g, "unexpected dddd"); /* a second one restarts the confirm sequence */
            return;
        }
        g->partner_pos = (uint8_t)(arg + PARTNER_POS_FIRST);
        g->seq = FakeRubySeqSet;
        set_trade_state(g, TRADE_SLIDE_IN);
        break;
    case CODE_START:
        if(seq != FakeRubySeqConfirmed) {
            set_error(g, "bad ccdd"); /* Ruby trades its cursor and unk_008a, whatever they hold */
            return;
        }
        set_trade_state(g, TRADE_FADE_TRADE);
        break;
    default: /* ignored */
        break;
    }
}

/* sub_805465C [CB2_ReturnToFieldFromMultiplayer] (§B6.3): sub_8054F70 makes every player IDLE,
 * the key intercept is the room's (SelfIdle), and the field callback sub_8080A3C locks the
 * controls until task_mpl_807DD60's fade-in ends. The room is redrawn without reloading object
 * events: the avatar stays on its seat (on_seat). */
static void return_to_field(FakeRuby* g) {
    memset(g->link_state, PLAYER_IDLE, sizeof(g->link_state));
    g->key_cb = FakeRubyKeySelfIdle;
    g->held_key_to_send = KEY_CODE_EMPTY;
    g->controls_locked = true;
    g->script = FakeRubyScriptNone;
    g->seat_task = FakeRubySeatNone;
    g->resit_updates = 0;
    g->phase = FakeRubyReturn;
    next_task(g, FakeRubyTaskReturnField);
}

/* sub_800826C [CheckLinkPlayersMatchSaved]: slots 0-1 (the Part A count) against the Part A
 * copies. trainerId equal, and StringCompareWithoutExtCtrlCodes(name) 0: the same bytes up to and
 * including the saved name's first EOS (it skips extended control codes, which these names never
 * hold; stricter not to). */
static bool link_players_match_saved(const FakeRuby* g) {
    for(int i = 0; i < SAVED_PLAYERS; i++) {
        const Gen3LinkPlayer* saved = &g->saved_players[i];
        const Gen3LinkPlayer* now = &g->link_players[i];
        if(saved->trainer_id != now->trainer_id) return false;
        for(size_t c = 0; c < sizeof(saved->name); c++) {
            if(saved->name[c] != now->name[c]) return false;
            if(saved->name[c] == NAME_EOS) break;
        }
    }
    return true;
}

/* sub_804AB30: a queued action counts its delay down once per update, then runs. Action 0 is
 * Trade_SendData. */
static void run_queued_send(FakeRuby* g) {
    if(!g->send_queued) return;
    if(g->send_delay != 0) {
        g->send_delay--;
        return;
    }
    g->send_queued = false;
    trade_send_data(g);
}

/* callback2 sub_8048AB4: sub_8049DE0 runs the state's handler with this update's input, then
 * sub_804AB30 the queued action. */
static void trade_menu_update(FakeRuby* g) {
    const bool offer = g->offer_pending;
    const bool cancel = g->cancel_pending;
    const bool press_a = g->press_a_pending;
    const bool press_r = g->press_r_pending;
    const bool summary = g->summary_pending;
    const uint32_t elapsed = g->frame - g->trade_state_frame;
    g->offer_pending = false;
    g->cancel_pending = false;
    g->press_a_pending = false;
    g->press_r_pending = false;
    g->summary_pending = false;
    switch(g->trade_state) {
    case TRADE_BROWSING:
        if(offer) {
            trade_select(g, g->offer_slot);
        } else if(cancel) {
            trade_request_cancel(g);
        } else if(press_r) { /* sub_8049680: the debug block, no IsLinkTaskFinished guard */
            for(int i = 0; i < LINK_DATA_WORDS; i++) g->link_data[i] = (uint16_t)i;
            trade_send_data(g);
        } else if(summary) { /* sub_804997C: ShowPokemonSummaryScreen(..., sub_80484F4) */
            g->in_summary = true;
        }
        break;
    case TRADE_PROMPT:
        if(g->answer_pending) {
            g->answer_pending = false;
            trade_answer(g, g->answer_yes);
        }
        break;
    case TRADE_SLIDE_IN:
        if(elapsed >= SLIDE_IN_FRAMES) set_trade_state(g, TRADE_CONFIRM_WAIT);
        break;
    case TRADE_MESSAGE:
        if(press_a) set_trade_state(g, TRADE_BROWSING);
        break;
    case TRADE_FADE_TRADE: /* once the fade is done: (own cursor, partner) for TradeMons */
        if(elapsed < MENU_FADE_FRAMES) break;
        g->selected[0] = g->cursor;
        g->selected[1] = g->partner_pos;
        /* FRLG mode: fr: trade.c: CB_WaitToStartTrade, SetCloseLinkCallbackAndType(32) */
        if(!set_close_link_callback_type(g, g->frlg ? FRLG_CLOSE_TYPE_TRADE : 0)) {
            set_error(g, "ccdd hang"); /* the link stays open: state 13 never ends */
            return;
        }
        set_trade_state(g, TRADE_CLOSE_TRADE);
        break;
    case TRADE_FADE_ROOM: /* sub_8049D9C, once the fade is done */
        if(elapsed < MENU_FADE_FRAMES) break;
        /* FRLG mode: fr: trade.c: CB_InitExitCanceledTrade, SetCloseLinkCallbackAndType(12) */
        if(!set_close_link_callback_type(g, g->frlg ? FRLG_CLOSE_TYPE_CANCEL : 0)) {
            set_error(g, "eebb hang"); /* the link stays open: state 12 never ends */
            return;
        }
        set_trade_state(g, TRADE_CLOSE_ROOM);
        break;
    case TRADE_CLOSE_ROOM: /* sub_8049DC4: SetMainCallback2(sub_805465C), back to the room */
        if(g->received_link_players) break;
        return_to_field(g);
        return;
    case TRADE_CLOSE_TRADE: /* sub_8048A90: callback1 = NULL, SetMainCallback2(sub_804B41C) */
        if(g->received_link_players) break;
        g->phase = FakeRubyTrade;
        g->cb2_state = 0;
        next_task(g, FakeRubyTaskLinkTrade);
        return;
    case TRADE_CONFIRM_WAIT:
        if(elapsed >= CONFIRM_MESSAGE_FRAMES) set_trade_state(g, TRADE_PROMPT);
        break;
    default: /* Link standby: no input */
        break;
    }
    run_queued_send(g);
}

/* callback2 while the summary screen shows (ShowPokemonSummaryScreen leaves callback1 alone, so
 * run_trade_menu still handles the Flipper's blocks). Closing it runs sub_80484F4
 * [CB2_ReturnToTradeMenu], whose state 0 sets unk_007b = 0: whatever a block set meanwhile is
 * lost, an EEBB's fade included (§B3.4; link-protocol-trade.md T2). The menu's rebuild isn't
 * timed. */
static void run_summary(FakeRuby* g) {
    if(!g->summary_close_pending) return;
    g->summary_close_pending = false;
    g->in_summary = false;
    set_trade_state(g, TRADE_BROWSING);
}

/* The running trade menu, once a frame: callback1 sub_80494D8 [CB1_UpdateLink] reads and resets
 * the block flags (the Flipper's block is handled, this GBA's own echo only reset; a flag set
 * before callback1 started waits for it), then callback2. */
static void run_trade_menu(FakeRuby* g) {
    if(g->block_received != 0) {
        if(g->block_received & 1) trade_flipper_block(g);
        g->block_received = 0; /* ResetBlockReceivedFlags */
        if(g->error) return;
    }
    if(g->in_summary) {
        run_summary(g);
        return;
    }
    trade_menu_update(g);
}

/* sub_8047EC0 [CB2_CreateTradeMenu] (§B1.6, §B2). */
static void run_create_menu(FakeRuby* g) {
    switch(g->menu_state) {
    case MENU_SETUP:
        g->xchg_case = 0; /* unk_0075 */
        g->send_queued = false; /* sub_804AA88 */
        g->in_summary = false;
        g->summary_pending = false;
        g->summary_close_pending = false;
        g->seq = FakeRubySeqNone;
        g->cursor = 0;
        g->menu_state = MENU_OPEN;
        break;
    case MENU_OPEN:
        g->me.link_type = GEN3_LINKTYPE_TRADE_CONNECTING; /* gLinkType; InitLocalLinkPlayer */
        open_link(g);
        memset(g->enemy, 0, sizeof(g->enemy)); /* CreateMon(&gEnemyParty[i], species 0) */
        g->wait_task = true; /* CreateTask(sub_8083C50): it runs this frame */
        g->wait_count = 0;
        g->menu_count = 0;
        g->menu_state = MENU_DELAY;
        g->menu = FakeRubyMenuLoading;
        g->phase = FakeRubyMenu;
        break;
    case MENU_DELAY:
        if(++g->menu_count > MENU_DELAY_MAX) {
            g->menu_count = 0;
            g->menu_state = MENU_PLAYERS;
        }
        break;
    case MENU_PLAYERS: /* the child's branch */
        if(g->player_count >= SAVED_PLAYERS) g->menu_state = MENU_EXCHANGE;
        break;
    case MENU_EXCHANGE:
        if(!g->received_link_players) break;
        if(!link_types_match(g)) {
            set_error(g, "link type"); /* Ruby: state 4 never passes, a silent hang (§B8) */
            break;
        }
        /* CalculatePlayerPartyCount; FRLG mode: fr: trade.c: CB2_CreateTradeMenu case 5 first */
        g->menu_state = g->frlg ? MENU_FRLG_WIRELESS : MENU_PARTIES;
        break;
    case MENU_FRLG_WIRELESS: /* fr: trade.c: CB2_CreateTradeMenu case 5, !gWirelessCommType */
        g->menu_state = MENU_PARTIES;
        break;
    case MENU_PARTIES:
        /* then sub_804AF84 [SaveTradeGiftRibbons], not modelled */
        if(g->frlg ? frlg_buffer_trade_parties(g) : buffer_trade_parties(g)) {
            g->menu_state = MENU_ENEMIES;
        }
        break;
    case MENU_ENEMIES:
        calculate_enemy_party_count(g);
        g->menu_count = 1; /* this frame is the build's first */
        g->menu_state = MENU_BUILD;
        break;
    case MENU_BUILD:
        if(++g->menu_count < MENU_BUILD_FRAMES) break;
        set_trade_state(g, TRADE_BROWSING); /* callback1 sub_80494D8, callback2 sub_8048AB4 */
        g->menu_state = MENU_RUNNING;
        break;
    default: /* MENU_RUNNING */
        run_trade_menu(g);
        break;
    }
}

/* ---- The trade (trade.c, §B4) ---- */

/* sub_804BA94 [TradeMons] (§B4.4) on own cursor i and partner j = gSelectedTradeMonPositions[1]
 * % 6. This GBA holds no mail (its step 1 clears nothing here). Pokedex flags (step 5) aren't
 * modelled. FRLG mode: fr: trade_scene.c: TradeMons, the same swap, Friendship 70 and mail, but
 * no National Dex from the Flipper's LinkPlayer (§C4). */
static void trade_mons(FakeRuby* g) {
    const uint8_t i = g->selected[0];
    const uint8_t j = g->selected[1] % FAKE_RUBY_PARTY;
    uint8_t swap[GEN3_PARTY_SIZE];
    Gen3Pkm p;
    /* Step 4: GiveMailToMon2 with the Flipper's round-4 entry, which carries no mail: the
     * Pokemon's held item would become that empty entry's item */
    if(gen3_pkm_decrypt(g->enemy[j], GEN3_PARTY_SIZE, &p) &&
       gen3_pkm_get(&p, Gen3FieldMail) != MAIL_NONE) {
        set_error(g, "flipper mail");
        return;
    }
    memcpy(swap, g->party[i], GEN3_PARTY_SIZE);
    memcpy(g->party[i], g->enemy[j], GEN3_PARTY_SIZE);
    memcpy(g->enemy[j], swap, GEN3_PARTY_SIZE);
    /* SetMonData(FRIENDSHIP) re-encrypts with a new checksum */
    if(gen3_pkm_decrypt(g->party[i], GEN3_PARTY_SIZE, &p) &&
       gen3_pkm_get(&p, Gen3FieldIsEgg) == 0) {
        gen3_pkm_set(&p, Gen3FieldFriendship, TRADE_FRIENDSHIP);
        gen3_pkm_encrypt(&p, g->party[i]);
    }
    /* sub_804BA64 [TryEnableNationalDexFromLinkPartner]: gLinkPlayers[mpId ^ 1]. FRLG mode:
     * fr: trade_scene.c: TryEnableNationalDexFromLinkPartner has this check commented out, so
     * the 0x8000 every FRLG LinkPlayer carries does nothing */
    if(!g->frlg && g->received_link_players && g->link_players[0].lp_field_2 == LP_NATIONAL_DEX) {
        g->national_dex = true;
    }
}

/* sub_804D63C [HandleLinkDataReceive]: slot 0's DCBA ends the wait; its ABCD only marks unk_009c
 * (a master's); this GBA's own echo is only reset. */
static void trade_link_receive(FakeRuby* g) {
    if(g->block_received & 1) {
        const uint8_t* buf = g->block_recv_buffer[0];
        if((uint16_t)(buf[0] | (buf[1] << 8)) == CODE_ANIM_ACK) {
            if(!g->abcd_sent) {
                set_error(g, "early dcba"); /* Ruby skips TradeMons, saves the untraded party */
                return;
            }
            g->cb2_state = 0;
            next_task(g, FakeRubyTaskTradeEvolution); /* SetMainCallback2(sub_804D588) */
        }
    }
    g->block_received = 0; /* ResetBlockReceivedFlag(0), (1) */
}

/* sub_804B41C [CB2_LinkTrade] (§B4.1). It reads no block flags: a block that comes in waits for
 * sub_804DB84. FRLG mode: fr: trade_scene.c: CB2_LinkTrade, the same states on a cable, and on
 * this child CheckLinkTimeout in state 4 (§C4). */
static void run_link_trade(FakeRuby* g) {
    switch(g->cb2_state) {
    case 0: /* ResetTasks, CloseLink, gLinkType = 0x1144; FRLG: ClearLinkTimeoutTimer */
        close_link(g);
        g->me.link_type = GEN3_LINKTYPE_TRADE_DISCONNECTED;
        g->link_timeout = 0;
        g->cb2_state = 1;
        break;
    case 1: /* OpenLink, with no Task_WaitForLinkPlayerConnection */
        open_link(g);
        g->cb2_count = 0;
        g->cb2_state = 2;
        break;
    case 2:
        if(++g->cb2_count > LINK_TRADE_DELAY_MAX) {
            g->cb2_count = 0;
            g->cb2_state = 3;
        }
        break;
    case 3: /* the child's branch (FRLG: CheckLinkTimeout runs here on the master only) */
        g->cb2_state = 4;
        break;
    case 4:
        /* Ruby: sub_804B24C is a debug counter, no timeout. FRLG mode: fr: trade_scene.c:
         * CheckLinkTimeout first, every frame of state 4. Its linkTimeoutCheck1 is never written,
         * so the timer counts whether or not data flows; past 300: CloseLink, CB2_LinkError */
        if(g->frlg && ++g->link_timeout > FRLG_LINK_TIMEOUT_FRAMES) {
            close_link(g);
            set_error(g, "link timeout");
            break;
        }
        if(!g->received_link_players) break;
        if(!link_types_match(g)) {
            set_error(g, "link type"); /* Ruby: state 4 never passes, a silent hang (§B8) */
            break;
        }
        g->abcd_sent = false; /* state 5: unk_009c, unk_009d, unk_00bd = 0 */
        g->cb2_frame = g->frame;
        g->cb2_state = 5;
        break;
    default: /* 5-12: sub_804B2D0 loads both pictures, then the fade-in */
        if(g->frame - g->cb2_frame < TRADE_PICS_FRAMES) break;
        next_task(g, FakeRubyTaskTradeAnim); /* SetMainCallback2(sub_804DB84) */
        break;
    }
}

/* sub_804DB84 [CB2_UpdateLinkTrade]: the animation, then TradeMons and ABCD (§B4.3), then
 * sub_804BB78 [HandleLinkDataSend] and sub_804D63C every frame. */
static void run_trade_anim(FakeRuby* g) {
    if(g->frame - g->task_frame >= TRADE_ANIM_FRAMES) { /* sub_804C29C returned TRUE */
        trade_mons(g);
        if(g->error) return;
        g->link_data[0] = CODE_ANIM_DONE;
        next_task(g, FakeRubyTaskWaitTrade); /* SetMainCallback2(sub_804DC18) */
        /* sub_804BB78 with unk_00bd = 1: Trade_SendData once, only if IsLinkTaskFinished (else
         * never: Ruby waits for DCBA forever) */
        if(g->callback == FakeRubyCbNone) {
            trade_send_data(g);
            g->abcd_sent = true;
        }
    }
    trade_link_receive(g);
}

/* sub_804D588 [CB2_TryLinkTradeEvolution]: state 0 disables soft reset; state 4's trade
 * evolution (GetEvolutionTargetSpecies) isn't modelled, so straight to the save. */
static void run_trade_evolution(FakeRuby* g) {
    if(g->cb2_state == 0) {
        g->cb2_state = 4;
        return;
    }
    g->cb2_state = 0;
    g->phase = FakeRubySave;
    next_task(g, FakeRubyTaskSave); /* SetMainCallback2(sub_804DC88) */
}

/* ---- The link save (trade.c sub_804DC88, save.c, §B5) ---- */

/* sub_80084A4 [SetLinkStandbyCallback]: ignored while gLinkCallback is busy. The save's wait
 * (IsLinkTaskFinished) then ends with the busy callback, and the sync passes without the
 * Flipper: an error here. */
static void set_link_standby_callback(FakeRuby* g) {
    if(g->callback != FakeRubyCbNone) {
        set_error(g, "sync skipped");
        return;
    }
    g->callback = FakeRubyCbStandby;
}

/* One flash write: IRQs stay on, the main loop stalls (§B5.2; link-protocol-trade.md T4). */
static void write_sector(FakeRuby* g) {
    g->save_dirty = true;
    g->stall = g->sector_stall;
}

/* sub_8125E04: the signature byte of chunk 13 makes the new slot the valid save. */
static void commit_save(FakeRuby* g) {
    memcpy(g->saved_party, g->party, sizeof(g->saved_party));
    g->saved_national_dex = g->national_dex;
    g->saved_trades++;
    g->save_dirty = false;
}

/* sub_804DC88 [CB2_SaveAndEndTrade] (§B5.1). FRLG mode: fr: trade_scene.c: CB2_SaveAndEndTrade,
 * the cartridge build, runs the same states on this child (§C6): state 40's 51 frames, then
 * Random() % 30 more on the master only (GetMultiplayerId() == 0), none here; the commit in state
 * 42. Its soft reset stays off until state 9, which nothing on the link sees. */
static void run_save(FakeRuby* g) {
    switch(g->cb2_state) {
    case SAVE_STANDBY_TEXT:
        g->cb2_state = SAVE_SYNC1;
        break;
    case SAVE_SYNC1: /* sync 1 */
        set_link_standby_callback(g);
        g->cb2_count = 0;
        g->cb2_state = SAVE_WAIT_SYNC1;
        break;
    case SAVE_WAIT_SYNC1:
        if(++g->cb2_count > SAVE_SYNC1_SLOW) {
            g->cb2_count = 0;
            g->cb2_state = SAVE_WAIT_SYNC1_2;
        }
        if(g->callback == FakeRubyCbNone) g->cb2_state = SAVE_SAVING_TEXT;
        break;
    case SAVE_WAIT_SYNC1_2:
        if(g->callback == FakeRubyCbNone) g->cb2_state = SAVE_SAVING_TEXT;
        break;
    case SAVE_SAVING_TEXT:
        g->cb2_state = SAVE_INIT;
        break;
    case SAVE_INIT: /* IncrementGameStat, sub_8125D80 */
        g->sectors = 0;
        g->cb2_count = 0;
        g->cb2_state = SAVE_WAIT_SECTOR;
        break;
    case SAVE_WAIT_SECTOR:
        if(++g->cb2_count == SAVE_SECTOR_FRAMES) g->cb2_state = SAVE_SECTOR;
        break;
    case SAVE_SECTOR: /* sub_8125DA8 -> sub_812550C: chunks 0..12, then done */
        if(++g->sectors < SAVE_CHUNKS) {
            write_sector(g);
            g->cb2_count = 0;
            g->cb2_state = SAVE_WAIT_SECTOR;
        } else {
            g->cb2_state = SAVE_LAST_SECTOR;
        }
        break;
    case SAVE_LAST_SECTOR: /* sub_8125DDC */
        write_sector(g);
        g->cb2_count = 0;
        g->cb2_state = SAVE_WAIT_SYNC2;
        break;
    case SAVE_WAIT_SYNC2:
        if(++g->cb2_count > SAVE_SYNC2_DELAY) {
            g->cb2_count = 0;
            g->cb2_state = SAVE_SYNC2;
        }
        break;
    case SAVE_SYNC2: /* sync 2 */
        set_link_standby_callback(g);
        g->cb2_state = SAVE_COMMIT;
        break;
    case SAVE_COMMIT:
        if(g->callback != FakeRubyCbNone) break;
        commit_save(g); /* and gSoftResetDisabled = FALSE */
        g->cb2_state = SAVE_WAIT_SYNC3;
        break;
    case SAVE_WAIT_SYNC3:
        if(++g->cb2_count > SAVE_SYNC3_DELAY) { /* sync 3 */
            set_link_standby_callback(g);
            g->cb2_state = SAVE_FADE;
        }
        break;
    case SAVE_FADE:
        if(g->callback != FakeRubyCbNone) break;
        g->cb2_frame = g->frame;
        g->cb2_state = SAVE_BGM;
        break;
    case SAVE_BGM:
        if(g->frame - g->cb2_frame < SAVE_FADE_FRAMES) break;
        g->cb2_frame = g->frame;
        g->cb2_state = SAVE_CLOSE;
        break;
    case SAVE_CLOSE:
        if(g->frame - g->cb2_frame < SAVE_BGM_FRAMES) break;
        if(!set_close_link_callback(g)) {
            set_error(g, "close skipped"); /* the link stays open: state 9 never ends */
            return;
        }
        g->cb2_state = SAVE_CLOSED;
        break;
    case SAVE_CLOSED:
        if(g->received_link_players) break;
        g->phase = FakeRubySave; /* not Closed: the trade menu reopens the link (§B6.1) */
        next_task(g, FakeRubyTaskFreeTradeAnim); /* SetMainCallback2(sub_804E144) */
        break;
    default:
        break;
    }
}

/* sub_8083C50 [Task_WaitForLinkPlayerConnection]: CloseLink and CB2_LinkError unless both
 * LinkPlayers are in within 300 frames of OpenLink. */
static void run_wait_task(FakeRuby* g) {
    if(!g->wait_task) return;
    if(++g->wait_count > WAIT_LINK_FRAMES) {
        g->wait_task = false;
        close_link(g);
        set_error(g, "link timeout");
        return;
    }
    if(g->received_link_players) g->wait_task = false;
}

/* FRLG mode, the 0x1133 linkup gate: fr: trade.c: GetGameProgressForLinkTrade as this GBA
 * (slot 1) sees the Flipper (slot 0), on the converted LinkPlayers (frlg_convert_link_player).
 * An FRLG partner is ready. An R/S partner needs this GBA's own progressFlagsCopy & 0xF0
 * (FLAG_SYS_CAN_LINK_WITH_RS: the Sevii Islands are done); any other partner needs that, and
 * the partner's own & 0xF0 too (an Emerald champion). */
static bool frlg_partner_ready(const FakeRuby* g) {
    const Gen3LinkPlayer* partner = &g->link_players[0];
    if(is_frlg_version(partner->version)) return true;
    if((g->link_players[LOCAL_ID].name[LP_PROGRESS_FLAGS_COPY] & PROGRESS_CLEARED_GAME) == 0) {
        return false; /* TRADE_PLAYER_NOT_READY */
    }
    if(is_rs_version(partner->version)) return true;
    return (partner->name[LP_PROGRESS_FLAGS_COPY] & PROGRESS_CLEARED_GAME) != 0;
}

/* The Flipper's card as this GBA keeps and draws it. sub_8083314 [Task_LinkupAwaitTrainerCardData]
 * copies it into gTrainerCards, whose stars index gTrainerCardPalettes and gTrainerCardColorNames
 * (CARD_STARS_MAX). FRLG mode: fr: cable_club.c: Task_LinkupAwaitTrainerCardData copies it whole
 * when the Flipper's LinkPlayer version & 0xFF is 4 or 5 (else the first 0x38 bytes, with that
 * version), and fr: trainer_card.c draws it: rse.stars index its palette tables, and GetCardType
 * picks the Kanto or the Hoenn card from the version byte. */
static bool card_ok(const FakeRuby* g) {
    if(g->partner_card[CARD_STARS] > CARD_STARS_MAX) return false;
    if(!g->frlg) return true;
    const uint16_t version = g->link_players[0].version;
    return !is_frlg_version(version) || g->partner_card[CARD_VERSION] == (version & 0xFF);
}

/* cable_club.c (child side of the Trade Center linkup) and the counter script. */
static void run_linkup_task(FakeRuby* g) {
    switch(g->task) {
    case FakeRubyTaskNone:
        break;
    case FakeRubyTaskRoom:
        room_update(g);
        break;
    case FakeRubyTaskExchange:
        if(!g->received_link_players) break;
        /* GetLinkPlayerDataExchangeStatusTimed: every linkType must equal slot 0's. */
        if(!link_types_match(g)) {
            set_close_link_callback(g);
            next_task(g, FakeRubyTaskStop);
            break;
        }
        if(g->frlg && g->link_players[0].link_type == GEN3_LINKTYPE_TRADE_SETUP &&
           !frlg_partner_ready(g)) {
            /* fr: cable_club.c: Task_LinkupExchangeDataWithLeader: PLAYER_NOT_READY and
             * PARTNER_NOT_READY close the link at once, with no 5FFF, and the player is back at
             * the counter */
            close_link(g);
            g->phase = FakeRubyClosed;
            set_error(g, "not ready");
            break;
        }
        next_task(g, FakeRubyTaskMakeCard);
        break;
    case FakeRubyTaskMakeCard:
        /* Generated in the main loop of frame P+2 (P = blocks complete), after that frame's
         * ProcessRecvCmds: a 0xCCCC processed in burst P+3 or later gets the real card. */
        if(g->frame - g->task_frame + 1 < CARD_READY_BURSTS) break;
        /* sub_80831F8 saves the LinkPlayers (sub_80081C8) with the card: the 0x1111 relink's
         * sub_800826C checks against these */
        memcpy(g->saved_players, g->link_players, sizeof(g->saved_players));
        gen3_trainer_card_build(&g->me, g->block_send_buffer);
        if(g->frlg) {
            /* fr: trainer_card.c: TrainerCard_GenerateCardForLinkPlayer: version = GAME_VERSION
             * after the Ruby layout; the rest of the FRLG card stays zero here */
            g->block_send_buffer[CARD_VERSION] = (uint8_t)(g->me.version & 0xFF);
        }
        g->send_copy = 0;
        g->card_ready = true;
        next_task(g, FakeRubyTaskAwaitCards);
        break;
    case FakeRubyTaskAwaitCards:
        if(g->block_received != 0x3) break; /* Flipper's card and our own echo */
        g->block_received = 0; /* ResetBlockReceivedFlags */
        memcpy(g->partner_card, g->block_recv_buffer[0], sizeof(g->partner_card));
        if(!card_ok(g)) {
            set_error(g, "bad card");
            break;
        }
        g->phase = FakeRubyWalkIn;
        next_task(g, FakeRubyTaskWalkIn);
        break;
    case FakeRubyTaskWalkIn:
        if(g->frame - g->task_frame < WALK_IN_FRAMES) break;
        g->callback = FakeRubyCbStandby; /* SetLinkStandbyCallback */
        next_task(g, FakeRubyTaskStandby);
        break;
    case FakeRubyTaskStandby:
        if(g->callback == FakeRubyCbStandbyForAll && !g->ready_standby[0] &&
           g->frame - g->standby_frame >= STANDBY_TIMEOUT_FRAMES) {
            set_error(g, "standby timeout");
            break;
        }
        /* IsLinkTaskFinished: gLinkCallback == NULL. A stray block that replaced the standby
         * callback also ends this wait once it is sent, as on Ruby. */
        if(g->callback != FakeRubyCbNone) break;
        g->callback = FakeRubyCbSendHeldKeys; /* StartSendingKeysToLink */
        /* sub_8054F70 at the map load; CB1 ran through the walk-in with the controls locked
         * (0x11), and sub_8080A5C unlocks them now. */
        memset(g->link_state, PLAYER_IDLE, sizeof(g->link_state));
        g->key_cb = FakeRubyKeySelfIdle;
        g->held_key_to_send = KEY_CODE_EMPTY;
        g->controls_locked = false;
        g->phase = FakeRubyRoom;
        next_task(g, FakeRubyTaskRoom);
        break;
    case FakeRubyTaskStop:
        break;
    case FakeRubyTaskStartTrade:
        run_start_trade(g);
        break;
    case FakeRubyTaskStartMenu: /* sub_8047CD8: SetMainCallback2(sub_8047EC0) */
        g->menu_state = MENU_SETUP;
        next_task(g, FakeRubyTaskCreateMenu);
        break;
    case FakeRubyTaskCreateMenu:
        run_create_menu(g);
        break;
    case FakeRubyTaskLinkTrade:
        run_link_trade(g);
        break;
    case FakeRubyTaskTradeAnim:
        run_trade_anim(g);
        break;
    case FakeRubyTaskWaitTrade: /* sub_804DC18: the child only reads */
        trade_link_receive(g);
        break;
    case FakeRubyTaskTradeEvolution:
        run_trade_evolution(g);
        break;
    case FakeRubyTaskSave:
        run_save(g);
        break;
    case FakeRubyTaskFreeTradeAnim: /* the fade is done: SetMainCallback2(sub_8047CD8) */
        next_task(g, FakeRubyTaskStartMenu);
        break;
    case FakeRubyTaskReturnField:
        /* CB2_ReturnToFieldLink's 14 states (the link is closed, so sub_8055870 never waits),
         * then CB2_Overworld: task_mpl_807DD60 case 0, sub_8083664 (gSpecialVar_0x8004 == 3:
         * gLinkType = 0x1111) and sub_8083710's first run: OpenLink, ResetLinkPlayers and the
         * 300-frame sub_8083C50 */
        if(g->frame - g->task_frame < RETURN_FIELD_FRAMES) break;
        g->me.link_type = GEN3_LINKTYPE_TRADE;
        open_link(g);
        g->wait_task = true; /* it runs this frame */
        g->wait_count = 0;
        next_task(g, FakeRubyTaskReestablish);
        break;
    case FakeRubyTaskReestablish:
        /* sub_8083710, then the child's sub_8083760 (2 players) and, from the next frame,
         * sub_80837EC: both LinkPlayers in with every link type equal (else a silent hang, §B8),
         * sub_800826C, then sub_8007B14 [StartSendingKeysToLink]: CAFE with no walk-in and no
         * 2FFE. The player count sub_8083760 waits for comes with the handshake, before the
         * LinkPlayers that sub_80837EC waits for, so one check here is exact */
        if(g->frame - g->task_frame < REESTABLISH_FRAMES || g->player_count < SAVED_PLAYERS) {
            break;
        }
        if(!g->received_link_players) break;
        if(!link_types_match(g)) {
            set_error(g, "link type");
            break;
        }
        if(!link_players_match_saved(g)) {
            close_link(g);
            set_error(g, "relink mismatch"); /* CB2_LinkError */
            break;
        }
        g->callback = FakeRubyCbSendHeldKeys;
        g->fade_in_updates = FADE_IN_UPDATES;
        next_task(g, FakeRubyTaskRoom);
        break;
    }
}

/* Part B's callbacks and tasks count frames between CloseLink and OpenLink too. */
static bool runs_while_closed(FakeRubyTask task) {
    return task == FakeRubyTaskStartTrade || task == FakeRubyTaskStartMenu ||
           task == FakeRubyTaskCreateMenu || task == FakeRubyTaskLinkTrade ||
           task == FakeRubyTaskTradeAnim || task == FakeRubyTaskWaitTrade ||
           task == FakeRubyTaskTradeEvolution || task == FakeRubyTaskSave ||
           task == FakeRubyTaskFreeTradeAnim || task == FakeRubyTaskReturnField;
}

/* Callback2's own step (it may OpenLink), then RunTasks: the timeout and the handshake trigger,
 * so a trigger created this frame runs this frame (§B1.6: OpenLink at c+3, HANDSHAKE at c+8). */
static void run_tasks(FakeRuby* g) {
    if(g->link_open || runs_while_closed(g->task)) run_linkup_task(g);
    if(g->error) return;
    run_wait_task(g);
    if(g->trigger_active && ++g->trigger_runs == TRIGGER_HANDSHAKE_RUNS) {
        g->should_advance = true;
        g->trigger_active = false;
    }
}

static void main_loop(FakeRuby* g) {
    g->frame++;
    link_main1(g);
    link_main2(g);
    if(g->error) return;
    /* UpdateLinkAndCallCallbacks: while sending keys, a frame whose dequeue found nothing skips
     * the whole game update (lockstep, §A7). LinkMain2 above still built this frame's CAFE. */
    if(g->received_nothing && g->callback == FakeRubyCbSendHeldKeys) return;
    run_tasks(g);
}

/* ---- API ---- */

void fake_ruby_init(FakeRuby* g, const Gen3LinkPlayer* me) {
    memset(g, 0, sizeof(*g));
    g->me = *me;
    g->powered = true;
    g->phase = FakeRubyOff;
    g->state = FakeRubyLinkStart1; /* link closed: serial off */
    g->relink_frames = UINT32_MAX;
    g->sector_stall = SECTOR_STALL;
    for(int i = 0; i < LINK_DATA_WORDS; i++) g->link_data[i] = LINK_DATA_LEFTOVER;
    g->frlg = is_frlg_version(me->version);
    if(g->frlg) {
        /* fr: link.c: InitLocalLinkPlayer: lp_field_2 0x8000 and progressFlags from the save
         * (none until fake_ruby_set_frlg_progress). neverRead and progressFlagsCopy are never
         * written: only a receiver fills the copy (ConvertLinkPlayerName) */
        g->me.lp_field_2 = FRLG_LP_FIELD_2;
        g->me.name[LP_PROGRESS_FLAGS] = 0;
        g->me.name[LP_NEVER_READ] = 0;
        g->me.name[LP_PROGRESS_FLAGS_COPY] = 0;
    }
}

bool fake_ruby_is_frlg(const FakeRuby* g) {
    return g->frlg;
}

void fake_ruby_set_frlg_progress(FakeRuby* g, bool national_dex, bool can_link_rs) {
    if(!g->frlg) return;
    /* fr: link.c: InitLocalLinkPlayer: progressFlags = IsNationalPokedexEnabled(), | 0x10 with
     * FLAG_SYS_CAN_LINK_WITH_RS */
    g->me.name[LP_PROGRESS_FLAGS] = (uint8_t)((national_dex ? PROGRESS_NATIONAL_DEX : 0) |
                                              (can_link_rs ? PROGRESS_CAN_LINK_RS : 0));
}

const uint8_t* fake_ruby_partner_card(const FakeRuby* g) {
    return g->partner_card;
}

void fake_ruby_open_link(FakeRuby* g) {
    if(!g->powered || g->error) return;
    open_link(g);
    g->card_ready = false;
    memset(g->partner_card, 0, sizeof(g->partner_card));
    next_task(g, FakeRubyTaskExchange);
    g->phase = FakeRubyHandshake;
}

void fake_ruby_power_off(FakeRuby* g) {
    if(g->save_dirty) g->save_corrupt = true; /* §B5.2 */
    g->powered = false;
    g->serial_on = false;
    g->link_open = false;
    g->state = FakeRubyLinkStart1;
    g->phase = FakeRubyOff;
}

void fake_ruby_leave_room(FakeRuby* g) {
    if(g->phase != FakeRubyRoom || g->error) return;
    g->facing_exit = true; /* SelfIdle now sends 0x12 until the prompt takes over */
    g->phase = FakeRubyLeaving;
}

void fake_ruby_sit(FakeRuby* g, uint8_t seat) {
    g->sit_pending = true;
    g->sit_seat = seat;
}

void fake_ruby_stand_up(FakeRuby* g) {
    g->b_press = true;
}

void fake_ruby_resit(FakeRuby* g) {
    if(g->phase == FakeRubyRoom && !g->error) g->resit_pending = true;
}

bool fake_ruby_on_seat(const FakeRuby* g) {
    return g->on_seat;
}

uint8_t fake_ruby_link_state(const FakeRuby* g, uint8_t slot) {
    return slot < FAKE_RUBY_PLAYERS ? g->link_state[slot] : 0;
}

uint32_t fake_ruby_relink_frames(const FakeRuby* g) {
    return g->relink_frames;
}

void fake_ruby_corrupt_next(FakeRuby* g) {
    g->corrupt_pending = true;
}

void fake_ruby_corrupt_next_checksum(FakeRuby* g) {
    g->corrupt_checksum_pending = true;
}

void fake_ruby_frame(FakeRuby* g, const MlinkTx* tx, MlinkRx* rx) {
    memset(rx, 0, sizeof(*rx));
    rx->count = tx->count;
    if(g->powered && !g->error) link_vsync(g);
    for(uint8_t t = 0; t < tx->count && t < MLINK_BURST_LEN; t++) {
        if(g->powered && g->serial_on) {
            uint16_t own = g->sio_send;
            rx->word[t] = own;
            /* The Flipper may read a glitched word, while this GBA's ISR below sums and checks
             * the word it sent. send_index 1..8: transfers #2..#9 carry command words; 0 is the
             * checksum slot, checked by the Flipper once checksum_available. */
            const bool connected = g->state == FakeRubyLinkConnected;
            if(g->corrupt_pending && connected && g->send_index != 0) {
                rx->word[t] ^= CORRUPT_BIT;
                g->corrupt_pending = false;
            } else if(
                g->corrupt_checksum_pending && connected && g->send_index == 0 &&
                g->checksum_available) {
                rx->word[t] ^= CORRUPT_BIT;
                g->corrupt_checksum_pending = false;
            }
            serial_irq(g, tx->word[t], own);
        } else {
            rx->word[t] = MLINK_NO_DATA;
            rx->timeout_mask |= (uint16_t)(1u << t);
        }
    }
    if(!g->powered || g->error) return;
    if(g->stall > 0) {
        g->stall--; /* fake_ruby_stall: no main loop this frame */
        return;
    }
    main_loop(g);
}

FakeRubyPhase fake_ruby_phase(const FakeRuby* g) {
    return g->phase;
}

const char* fake_ruby_error(const FakeRuby* g) {
    return g->error;
}

const Gen3LinkPlayer* fake_ruby_partner(const FakeRuby* g) {
    return &g->link_players[0];
}

void fake_ruby_set_party(FakeRuby* g, const uint8_t party[][GEN3_PARTY_SIZE], uint8_t count) {
    memset(g->party, 0, sizeof(g->party));
    for(uint8_t i = 0; i < count && i < FAKE_RUBY_PARTY; i++) {
        memcpy(g->party[i], party[i], GEN3_PARTY_SIZE);
    }
    memcpy(g->saved_party, g->party, sizeof(g->saved_party));
}

const uint8_t* fake_ruby_party(const FakeRuby* g, uint8_t slot, bool saved) {
    if(slot >= FAKE_RUBY_PARTY) return NULL;
    return saved ? g->saved_party[slot] : g->party[slot];
}

const uint8_t* fake_ruby_enemy(const FakeRuby* g, uint8_t slot) {
    return slot < FAKE_RUBY_PARTY ? g->enemy[slot] : NULL;
}

uint8_t fake_ruby_enemy_count(const FakeRuby* g) {
    return g->enemy_count;
}

uint16_t fake_ruby_max_enemy_species(const FakeRuby* g) {
    return g->max_enemy_species;
}

FakeRubyMenuState fake_ruby_menu_state(const FakeRuby* g) {
    if(g->phase != FakeRubyMenu) return FakeRubyMenuNone;
    return g->in_summary ? FakeRubyMenuSummary : g->menu;
}

void fake_ruby_stall(FakeRuby* g, uint8_t frames) {
    g->stall = frames;
}

/* Player input reaches the trade menu only while it runs. */
static bool trade_menu_running(const FakeRuby* g) {
    return g->phase == FakeRubyMenu && g->menu_state == MENU_RUNNING && !g->error;
}

void fake_ruby_offer(FakeRuby* g, uint8_t slot) {
    if(!trade_menu_running(g)) return;
    g->offer_pending = true;
    g->offer_slot = slot;
}

FakeRubyCantTrade fake_ruby_cant_trade(const FakeRuby* g) {
    return g->cant_trade;
}

void fake_ruby_cancel_trade(FakeRuby* g) {
    if(trade_menu_running(g)) g->cancel_pending = true;
}

void fake_ruby_answer(FakeRuby* g, bool yes) {
    if(!trade_menu_running(g)) return;
    g->answer_pending = true;
    g->answer_yes = yes;
}

bool fake_ruby_partner_mon_invalid(const FakeRuby* g) {
    return g->partner_mon_invalid;
}

void fake_ruby_press_a(FakeRuby* g) {
    if(trade_menu_running(g)) g->press_a_pending = true;
}

void fake_ruby_press_r(FakeRuby* g) {
    if(trade_menu_running(g)) g->press_r_pending = true;
}

void fake_ruby_open_summary(FakeRuby* g) {
    if(trade_menu_running(g)) g->summary_pending = true;
}

void fake_ruby_close_summary(FakeRuby* g) {
    if(trade_menu_running(g) && g->in_summary) g->summary_close_pending = true;
}

uint32_t fake_ruby_saved_trades(const FakeRuby* g) {
    return g->saved_trades;
}

bool fake_ruby_save_corrupt(const FakeRuby* g) {
    return g->save_corrupt;
}

bool fake_ruby_national_dex(const FakeRuby* g) {
    return g->saved_national_dex;
}

void fake_ruby_set_sector_stall(FakeRuby* g, uint8_t frames) {
    g->sector_stall = frames;
}
