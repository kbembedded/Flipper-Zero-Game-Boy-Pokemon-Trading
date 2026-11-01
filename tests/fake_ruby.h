#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_pkm.h>

/* Test-only model of Pokemon Ruby as the multiplayer child (slot 1) during the Trade Center
 * linkup, the room and leaving it (docs/gen3/link-protocol.md §A2-A7, §B7), sitting down, the
 * trade menu's relink, its party exchange and the menu itself with the Flipper as the arbiter,
 * the trade's relink, animation and TradeMons, the link save with its way back to the trade
 * menu, and an EEBB's way back to the room with its 0x1111 relink and the re-sit
 * (docs/gen3/link-protocol-trade.md §B1-B7). It models the child's own link stack and does
 * not use src/gen3/gen3_link.c: the per-transfer ISR (DoHandshake, DoRecv, DoSend), its own send
 * and receive queues, LinkMain1/LinkMain2 with gSendCmd/gRecvCmds, gLinkCallback, the
 * cable_club.c linkup tasks, and in the room the lockstep game update (CB1_OverworldLink: player
 * link states, key intercept callbacks) with the exit and seat scripts, then trade.c's own
 * callbacks and tasks. One fake_ruby_frame() is one GBA frame: VBlank (LinkVSync), the frame's
 * transfers, then the main loop. Any error stops it answering (0xFFFF from then on).
 *
 * FRLG mode (docs/gen3/link-protocol-trade.md Part C): a LinkPlayer whose version & 0xFF is 4
 * (FireRed) or 5 (LeafGreen) at fake_ruby_init makes this GBA FireRed/LeafGreen. Every Ruby check
 * stays on; the mode adds what pokefirered does differently at the linkup, each branch citing
 * its function (fr: file: Function):
 * - its own LinkPlayer: lp_field_2 0x8000 and progressFlags in name[8], name[9..10] zero
 *   (fr: link.c: InitLocalLinkPlayer);
 * - every LinkPlayer it receives: name[8..10] zeroed for an R/S sender, else name[10] = name[8]
 *   (fr: link.c: ProcessRecvCmds, ConvertLinkPlayerName);
 * - the 0x1133 linkup gate (fr: trade.c: GetGameProgressForLinkTrade): "not ready" below;
 * - its own trainer card with the version byte at 0x38
 *   (fr: trainer_card.c: TrainerCard_GenerateCardForLinkPlayer), and the check of the version
 *   byte of the Flipper's (fr: cable_club.c: Task_LinkupAwaitTrainerCardData): "bad card" below,
 *   whose stars check holds in both modes;
 * - the trade menu's creation, with its wireless-only state 5 (one frame on a cable) before the
 *   party exchange (fr: trade.c: CB2_CreateTradeMenu);
 * - the party exchange's 11-frame delay before every round's request case, on this child too
 *   (fr: trade.c: BufferTradeParties cases 2/6/10/14/18, §C4);
 * - which of its own Pokemon it may offer (fr: trade.c: CanTradeSelectedMon) and, on YES, the
 *   Flipper's (fr: trade.c: CheckValidityOfTradeMons): fake_ruby_offer, fake_ruby_answer;
 * - word 1 of its 5FFF, gReadyCloseLinkType (fr: link.c: BuildSendCmd): 32 for the trade's close
 *   after CCDD (fr: trade.c: CB_WaitToStartTrade), 12 for the room's after EEBB
 *   (CB_InitExitCanceledTrade), 0 for every other close (SetCloseLinkCallback) and from each
 *   OpenLink, which zeroes it (fr: link.c: OpenLink); Ruby's is always 0 (§C4);
 * - the 0x1144 relink's deadline (fr: trade_scene.c: CB2_LinkTrade, CheckLinkTimeout): "link
 *   timeout" below;
 * - TradeMons without the National Dex from the Flipper's lp_field_2 (fr: trade_scene.c:
 *   TradeMons, TryEnableNationalDexFromLinkPartner has its check commented out).
 * The room, the seats, the walk-in, the 0x1111 re-establish (its fade-in included) and the
 * 300-frame tasks are Ruby's, as FRLG's are the same (§C5; fr: cable_club.c:
 * Task_EnterCableClubSeat, Task_WaitForLinkPlayerConnection, Task_ReestablishLink; fr:
 * overworld.c: ReturnToFieldLink; fr: field_fadetransition.c: Task_ReturnToFieldCableLink,
 * cartridge build). So are the trade's animation, ABCD/DCBA and the child's link save
 * (fr: trade_scene.c: CB2_UpdateLinkTrade, HandleLinkDataReceive, CB2_SaveAndEndTrade: the extra
 * Random() % 30 frames before sync 2 are the master's only, §C6). */

#define FAKE_RUBY_CMD_LEN   8 /* CMD_LENGTH */
#define FAKE_RUBY_QUEUE_LEN 50 /* QUEUE_CAPACITY */
#define FAKE_RUBY_PLAYERS   2 /* Flipper (slot 0) + this GBA (slot 1) */
#define FAKE_RUBY_BLOCK_MAX 256 /* BLOCK_BUFFER_SIZE: one gBlockRecvBuffer slot */
#define FAKE_RUBY_PARTY     6 /* PARTY_SIZE */
#define FAKE_RUBY_MAIL_SIZE 216 /* 6 x sizeof(struct MailStruct), 36 B each (§B2.3) */
#define FAKE_RUBY_RIBBONS   11 /* giftRibbons */

typedef enum {
    FakeRubyOff,
    FakeRubyHandshake,
    FakeRubyLinkup,
    FakeRubyWalkIn,
    FakeRubyRoom,
    FakeRubyLeaving,
    FakeRubyClosed,
    FakeRubySeated, /* seat script: "Please wait..." until the trade starts or the seat is left */
    FakeRubyMenu, /* 0x1122: relink, party exchange, trade menu */
    FakeRubyTrade, /* 0x1144: relink, trade animation, ABCD/DCBA */
    FakeRubySave, /* link save */
    FakeRubyReturn, /* after EEBB: back to the room through the 0x1111 re-establish */
} FakeRubyPhase;

typedef enum {
    FakeRubyMenuNone, /* not in the trade menu */
    FakeRubyMenuLoading, /* relink, exchange, building the menu */
    FakeRubyMenuBrowsing, /* states 0/1: the player can act */
    FakeRubyMenuSummary, /* Task 7 */
    FakeRubyMenuStandby, /* states 5/100: "Link standby...", waiting for the Flipper */
    FakeRubyMenuConfirming, /* states 7/14: after DDDD, before YES/NO */
    FakeRubyMenuPrompt, /* state 3: "Is this trade okay?" YES/NO */
    FakeRubyMenuMessage, /* state 8: canceled / friend wants to trade; needs A */
    FakeRubyMenuClosing, /* states 10-13 or 11-12: fading out to the trade or the room */
} FakeRubyMenuState;

/* Why fake_ruby_offer sent nothing. FRLG mode: fr: trade.c CANT_TRADE_* (CanTradeSelectedMon,
 * the cartridge build, no BUGFIX). Ruby mode gives LastMon only (sub_80499F0
 * [CheckValidityOfTradeMons]: no other battler). */
typedef enum {
    FakeRubyCantTradeNone,
    FakeRubyCantTradeLastMon,
    FakeRubyCantTradeNational,
    FakeRubyCantTradeInvalidMon,
    FakeRubyCantTradeEggYet,
    FakeRubyCantTradePartnerEggYet,
} FakeRubyCantTrade;

/* Private state below; tests use the functions only. */

typedef enum {
    FakeRubyLinkStart0, /* LINK_STATE_START0 */
    FakeRubyLinkStart1, /* LINK_STATE_START1 */
    FakeRubyLinkHandshake, /* LINK_STATE_HANDSHAKE */
    FakeRubyLinkConnected, /* LINK_STATE_CONN_ESTABLISHED */
} FakeRubyLinkState;

typedef enum {
    FakeRubyCbNone,
    FakeRubyCbRequestPlayerData, /* LinkCB_RequestPlayerDataExchange */
    FakeRubyCbBlockSendBegin, /* LinkCB_BlockSendBegin */
    FakeRubyCbBlockSend, /* LinkCB_BlockSend */
    FakeRubyCbBlockSendEnd, /* LinkCB_BlockSendEnd */
    FakeRubyCbReadyCloseLink, /* sub_8008350 [LinkCB_ReadyCloseLink] */
    FakeRubyCbWaitCloseLink, /* sub_800837C [LinkCB_WaitCloseLink] */
    FakeRubyCbStandby, /* sub_80084C8 [LinkCB_Standby] */
    FakeRubyCbStandbyForAll, /* sub_80084F4 [LinkCB_StandbyForAll] */
    FakeRubyCbSendHeldKeys, /* sub_8007B44 [LinkCB_SendHeldKeys] */
} FakeRubyCallback;

typedef enum {
    FakeRubyTaskNone,
    FakeRubyTaskExchange, /* sub_80831F8 [Task_LinkupExchangeDataWithLeader] */
    FakeRubyTaskMakeCard, /* TrainerCard_GenerateCardForPlayer, after the task latency */
    FakeRubyTaskAwaitCards, /* sub_8083314 [Task_LinkupAwaitTrainerCardData] */
    FakeRubyTaskWalkIn, /* script: door animation, warp, map load */
    FakeRubyTaskStandby, /* sub_8080A5C: 0x2FFE standby */
    FakeRubyTaskRoom, /* sending CAFE every frame */
    FakeRubyTaskStop, /* sub_80833C4 [Task_StopLinkup]: closing after "different selections" */
    /* Part B: these run every frame, the link open or not (runs_while_closed in fake_ruby.c) */
    FakeRubyTaskStartTrade, /* sub_8083AAC [Task_StartWiredTrade] */
    FakeRubyTaskStartMenu, /* callback2 sub_8047CD8 [CB2_StartCreateTradeMenu] */
    FakeRubyTaskCreateMenu, /* callback2 sub_8047EC0 [CB2_CreateTradeMenu] */
    FakeRubyTaskLinkTrade, /* callback2 sub_804B41C [CB2_LinkTrade]: 0x1144 relink, pictures */
    FakeRubyTaskTradeAnim, /* callback2 sub_804DB84 [CB2_UpdateLinkTrade]: the animation */
    FakeRubyTaskWaitTrade, /* callback2 sub_804DC18 [CB2_WaitTradeComplete]: waits for DCBA */
    FakeRubyTaskTradeEvolution, /* callback2 sub_804D588 [CB2_TryLinkTradeEvolution] */
    FakeRubyTaskSave, /* callback2 sub_804DC88 [CB2_SaveAndEndTrade] */
    FakeRubyTaskFreeTradeAnim, /* callback2 sub_804E144 [CB2_FreeTradeAnim] */
    /* callback2 sub_805465C [CB2_ReturnToFieldFromMultiplayer], then CB2_ReturnToFieldLink */
    FakeRubyTaskReturnField,
    FakeRubyTaskReestablish, /* sub_8083710 [Task_ReestablishLink] .. sub_80837EC */
} FakeRubyTask;

/* gUnknown_03000584 [sPlayerKeyInterceptCallback]: picks the key code this GBA sends next. */
typedef enum {
    FakeRubyKeySelfIdle, /* sub_8055390 [KeyInterCB_SelfIdle] */
    FakeRubyKeyDeferToEventScript, /* sub_80553E4 [KeyInterCB_DeferToEventScript] */
    FakeRubyKeySendExitRoomKey, /* sub_80554E4 [KeyInterCB_SendExitRoomKey] */
    FakeRubyKeyWaitForPlayersToExit, /* sub_80554BC [KeyInterCB_WaitForPlayersToExit] */
    FakeRubyKeySendNothing, /* sub_80554B8 [KeyInterCB_SendNothing] */
    FakeRubyKeyIdle, /* sub_80553E0 [KeyInterCB_Idle] */
    FakeRubyKeySetReady, /* sub_80554A4 [KeyInterCB_SetReady] */
    FakeRubyKeyReady, /* sub_805546C [KeyInterCB_Ready] */
    FakeRubyKeyExitingSeat, /* sub_8055468 [KeyInterCB_ExitingSeat] */
} FakeRubyKeyCb;

/* Room event scripts and the task that takes the player out (§B7); the seat script (§B1.1). */
typedef enum {
    FakeRubyScriptNone,
    FakeRubyScriptPrompt, /* TradeRoom_PromptToCancelLink: the player answers YES */
    FakeRubyScriptTerminate, /* TradeRoom_TerminateLink: message, then ExitLinkRoom */
    FakeRubyScriptExitStart, /* gUnknown_081A4508 -> sub_8081050 case 0: stop CAFE, fade */
    FakeRubyScriptExitFade, /* sub_8081050 case 1: after the fade, SetCloseLinkCallback */
    FakeRubyScriptExitWait, /* sub_8081050 case 2: wait for CloseLink, then warp */
    FakeRubyScriptSeat, /* TradeCenter_EventScript_1A43F0/1A43FA: special sub_8083B5C */
    FakeRubyScriptSeatWait, /* its waitstate, until the seat task's ScriptContext_Enable */
    FakeRubyScriptSeatEnd, /* its end: the script context shuts down and unlocks the controls */
} FakeRubyScript;

/* sub_80839DC [Task_EnterCableClubSeat] */
typedef enum {
    FakeRubySeatNone,
    FakeRubySeatPrint, /* cases 0-1: "Please wait..." prints */
    FakeRubySeatWait, /* case 2: sub_80554F8 [GetCableClubPartnersReady] every update */
    FakeRubySeatCancel, /* case 3 */
} FakeRubySeatTask;

/* The strictness rule for CCDD (§B3.4): how far the latest blocks on either side went along
 * this GBA's AABB -> the Flipper's DDDD -> this GBA's BBBB. Any other block starts it over. */
typedef enum {
    FakeRubySeqNone,
    FakeRubySeqReady, /* this GBA sent AABB */
    FakeRubySeqSet, /* then the Flipper's DDDD */
    FakeRubySeqConfirmed, /* then this GBA's BBBB */
} FakeRubySeq;

typedef struct FakeRuby FakeRuby;

struct FakeRuby {
    Gen3LinkPlayer me; /* this GBA's LinkPlayer (InitLocalLinkPlayer) */
    bool frlg; /* FRLG mode: me.version & 0xFF was 4 or 5 at fake_ruby_init */
    FakeRubyPhase phase;
    const char* error;
    bool powered;
    uint32_t frame; /* main-loop frames since init */

    /* link.c: gLink and its statics */
    bool serial_on; /* SIOCNT in multiplayer mode (EnableSerial .. DisableSerial) */
    bool link_open; /* gLinkOpen */
    FakeRubyLinkState state;
    uint16_t sio_send; /* REG_SIOMLT_SEND: this GBA's word in the next transfer */
    uint8_t irq_counter; /* serialIntrCounter: s8, never reset on a child -> wraps every 256 */
    uint8_t vblanks_without_irq; /* sNumVBlanksWithoutSerialIntr */
    uint8_t hs_prev_count; /* sHandshakePlayerCount */
    uint8_t player_count;
    uint8_t send_index, recv_index; /* sendCmdIndex, recvCmdIndex */
    uint16_t checksum;
    bool checksum_available; /* sChecksumAvailable */
    bool bad_checksum;
    bool lag; /* LAG_SLAVE */
    bool recv_nonzero; /* sRecvNonzeroCheck */
    bool send_empty; /* sSendBufferEmpty */
    uint16_t send_queue[FAKE_RUBY_QUEUE_LEN][FAKE_RUBY_CMD_LEN];
    uint8_t send_pos, send_count;
    uint16_t recv_queue[FAKE_RUBY_QUEUE_LEN][FAKE_RUBY_PLAYERS][FAKE_RUBY_CMD_LEN];
    uint8_t recv_pos, recv_count;
    uint8_t last_recv_count; /* gLastRecvQueueCount */
    uint16_t send_cmd[FAKE_RUBY_CMD_LEN]; /* gSendCmd */
    uint16_t recv_cmds[FAKE_RUBY_PLAYERS][FAKE_RUBY_CMD_LEN]; /* gRecvCmds */
    bool should_advance; /* gShouldAdvanceLinkState */
    bool trigger_active; /* Task_TriggerHandshake */
    uint8_t trigger_runs;
    FakeRubyCallback callback; /* gLinkCallback */

    /* sBlockSend, gBlockSendBuffer */
    bool block_send_active;
    uint16_t block_send_size, block_send_pos;
    uint8_t block_send_delay; /* sBlockSendDelayCounter */
    uint8_t block_send_buffer[FAKE_RUBY_BLOCK_MAX];

    /* sBlockRecv, gBlockRecvBuffer, per slot */
    uint16_t block_recv_size[FAKE_RUBY_PLAYERS], block_recv_pos[FAKE_RUBY_PLAYERS];
    uint8_t block_recv_buffer[FAKE_RUBY_PLAYERS][FAKE_RUBY_BLOCK_MAX];
    bool link_player_pending[FAKE_RUBY_PLAYERS]; /* gLinkPlayerPending */
    Gen3LinkPlayer saved_players[FAKE_RUBY_PLAYERS]; /* gSavedLinkPlayers (sub_80081C8, Part A) */
    bool received_link_players; /* gReceivedRemoteLinkPlayers */
    uint8_t block_received; /* gBlockReceived, bit i = slot i */
    Gen3LinkPlayer link_players[FAKE_RUBY_PLAYERS]; /* gLinkPlayers */
    bool ready_close[FAKE_RUBY_PLAYERS]; /* gReadyToCloseLink */
    /* FRLG: gReadyCloseLinkType, word 1 of its 5FFF (fr: link.c: BuildSendCmd); 0 in Ruby mode */
    uint16_t close_type;
    bool ready_standby[FAKE_RUBY_PLAYERS]; /* gReadyToExitStandby */
    uint16_t held_keys[FAKE_RUBY_PLAYERS]; /* gLinkPartnersHeldKeys */

    /* cable_club.c tasks and the counter script */
    FakeRubyTask task;
    uint32_t task_frame; /* frame the current task step began */
    bool card_ready; /* own trainer card is in gBlockSendBuffer */
    /* the Flipper's card block (request type 2) as received at this linkup; zeros until then */
    uint8_t partner_card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint32_t standby_frame; /* frame this GBA built its 0x2FFE */

    /* overworld.c in the Trade Center: CB1_OverworldLink and the room scripts */
    bool received_nothing; /* gLink.receivedNothing: the last dequeue found the queue empty */
    uint8_t link_state[FAKE_RUBY_PLAYERS]; /* gUnknown_03000580 [sPlayerLinkStates] */
    FakeRubyKeyCb key_cb;
    uint16_t held_key_to_send; /* word_3004858 [gHeldKeyCodeToSend] */
    bool controls_locked; /* ArePlayerFieldControlsLocked */
    bool facing_exit; /* on its exit tile facing south, DOWN held (the walk is abstracted) */
    FakeRubyScript script;
    uint16_t script_updates; /* game updates left in the current script step */

    bool corrupt_pending; /* fake_ruby_corrupt_next: glitch the next command word */
    bool corrupt_checksum_pending; /* fake_ruby_corrupt_next_checksum: glitch the next checksum */

    /* Part B: the seat (overworld.c, cable_club.c) and the trade menu (trade.c) */
    bool sit_pending; /* fake_ruby_sit: a step onto sit_seat finishes at the next room update */
    uint8_t sit_seat;
    bool resit_pending; /* fake_ruby_resit, seen by the next room update */
    uint8_t resit_updates; /* game updates left holding Up, then Down */
    uint8_t fade_in_updates; /* task_mpl_807DD60: game updates left in the fade-in */
    bool b_press; /* fake_ruby_stand_up: B is new at the next room update */
    bool on_seat; /* the avatar stands on a seat tile */
    uint8_t seat; /* VAR_0x8005 of the seat script */
    FakeRubySeatTask seat_task;
    uint16_t seat_updates; /* game updates left in FakeRubySeatPrint */
    uint8_t start_case; /* sub_8083AAC data[0] */
    uint8_t menu_state; /* sub_8047EC0 gMain.state */
    uint8_t menu_count; /* its unk_00b4 */
    bool wait_task; /* sub_8083C50 [Task_WaitForLinkPlayerConnection] runs */
    uint16_t wait_count; /* its data[0] */
    uint32_t open_frame; /* main-loop frame of the latest OpenLink */
    uint32_t relink_frames; /* open_frame -> gReceivedRemoteLinkPlayers; UINT32_MAX while waiting */
    FakeRubyMenuState menu;

    /* Part B: the parties and sub_8048D44 [BufferTradeParties] (§B2) */
    uint8_t party[FAKE_RUBY_PARTY][GEN3_PARTY_SIZE]; /* gPlayerParty, game form */
    uint8_t saved_party[FAKE_RUBY_PARTY][GEN3_PARTY_SIZE]; /* the party in its save */
    uint8_t enemy[FAKE_RUBY_PARTY][GEN3_PARTY_SIZE]; /* gEnemyParty */
    uint8_t enemy_count; /* gEnemyPartyCount */
    uint8_t partner_mail[FAKE_RUBY_MAIL_SIZE]; /* gUnknown_02029700 [gLinkPartnerMail] */
    uint8_t partner_ribbons[FAKE_RUBY_RIBBONS]; /* unk_00b5 */
    /* Highest species (internal, an egg as 412) in any Flipper party slot stored since init */
    uint16_t max_enemy_species;
    uint8_t xchg_case; /* its unk_0075 (FRLG: bufferPartyState) */
    uint32_t xchg_case_frame; /* frame the current case began */
    uint8_t send_copy; /* the round (1..5) whose case copy gBlockSendBuffer holds; 0 none */
    uint8_t requests; /* blocks begun by CCCC types 0/1/3/4 since OpenLink: the k-th is round k's */
    bool request_block; /* the block being sent began with such a CCCC */
    uint8_t stall; /* fake_ruby_stall: main-loop frames left to skip */

    /* Part B: the trade menu (trade.c, §B3), its callback1 and callback2 */
    uint8_t trade_state; /* unk_007b */
    uint32_t trade_state_frame; /* frame trade_state was set */
    uint16_t link_data[10]; /* linkData: Trade_SendData's words, kept between sends (§B3.1) */
    uint8_t cursor; /* tradeMenuCursorPosition */
    uint8_t partner_pos; /* unk_008a: the Flipper's slot + 6, from its DDDD */
    uint8_t selected[2]; /* gUnknown_020297D8 [gSelectedTradeMonPositions] */
    bool send_queued; /* unk_08dc: one QueueAction(delay, 0), Trade_SendData once it's due */
    uint8_t send_delay;
    FakeRubySeq seq;
    /* Player input for the next trade-menu update */
    bool offer_pending;
    uint8_t offer_slot;
    bool cancel_pending;
    bool press_a_pending;
    bool press_r_pending;
    bool summary_pending; /* fake_ruby_open_summary */
    bool summary_close_pending; /* fake_ruby_close_summary */
    bool in_summary; /* callback2 is the summary screen; callback1 still reads the blocks */
    bool answer_pending; /* kept until the prompt is up */
    bool answer_yes;
    FakeRubyCantTrade cant_trade; /* the latest offer's local refusal; None once one passes */
    bool partner_mon_invalid; /* FRLG: the latest YES found the Flipper's Pokemon unfateful */

    /* Part B: the trade (§B4) and the link save (§B5), callback2 sub_804B41C .. sub_804DC88 */
    uint8_t cb2_state; /* gMain.state of the callback2 running (SetMainCallback2 zeroes it) */
    uint16_t cb2_count; /* its unk_00c0 */
    uint16_t link_timeout; /* FRLG: linkTimeoutTimer (fr: trade_scene.c: CheckLinkTimeout) */
    uint32_t cb2_frame; /* frame its current timed step began */
    bool abcd_sent; /* this trade's ABCD went out (sub_804BB78) */
    bool national_dex; /* EnableNationalPokedex ran (sub_804BA64) */
    uint8_t sector_stall; /* fake_ruby_set_sector_stall */
    uint8_t sectors; /* sub_8125DA8 calls in this save */
    bool save_dirty; /* a sector of the new slot is written, the commit isn't (§B5.2) */
    bool save_corrupt;
    uint32_t saved_trades;
    bool saved_national_dex;
};

/* me.link_type picks the counter; me.version & 0xFF of 4 or 5 picks FRLG mode, whose own
 * LinkPlayer then has lp_field_2 0x8000 and name[8..10] 00 00 00 (no progress) */
void fake_ruby_init(FakeRuby* g, const Gen3LinkPlayer* me);
/* FRLG mode (me.version & 0xFF is 4 or 5 at fake_ruby_init) */
bool fake_ruby_is_frlg(const FakeRuby* g);
/* FRLG mode, before fake_ruby_open_link: the save's National Dex and FLAG_SYS_CAN_LINK_WITH_RS,
 * both false after fake_ruby_init (the strict case). Writes its own name[8], progressFlags
 * (fr: link.c: InitLocalLinkPlayer). No effect in Ruby mode. */
void fake_ruby_set_frlg_progress(FakeRuby* g, bool national_dex, bool can_link_rs);
/* The Flipper's 100-byte card block as received at the latest linkup (both modes); all zeros
 * before one arrives, from fake_ruby_init and from each fake_ruby_open_link */
const uint8_t* fake_ruby_partner_card(const FakeRuby* g);
/* player chose the counter: link opens after 6 frames (§A3 timeline) */
void fake_ruby_open_link(FakeRuby* g);
/* answers 0xFFFF from now on; fake_ruby_init powers it on again */
void fake_ruby_power_off(FakeRuby* g);
/* In the room: the player walks onto its exit, faces south and confirms "Terminate the link?"
 * (§B7). It sends CAFE 0017 and waits, with no timeout, until every player is EXITING_ROOM; then
 * stops sending keys, fades out, sends 5FFF, waits for the Flipper's 5FFF and closes (0xFFFF). */
void fake_ruby_leave_room(FakeRuby* g);
/* Flips one bit of the next command word this GBA puts on the wire, as the Flipper reads it.
 * This GBA sums its own word, so both checksums diverge, as with a glitch at the receiver. */
void fake_ruby_corrupt_next(FakeRuby* g);
/* Flips one bit of the next checked checksum word (transfer #1, not the first burst's stale
 * 0xB9A0) this GBA puts on the wire, as the Flipper reads it. Transfer #1 is not summed (§A2) and
 * this GBA checks the Flipper's word and the one it sent, so only the Flipper sees a mismatch:
 * this GBA stays healthy, as with a glitch at the Flipper's receiver. */
void fake_ruby_corrupt_next_checksum(FakeRuby* g);
/* Room: the player walks onto seat 0 (4,5) or 1 (7,5). It acts on the next game update in the
 * room if this GBA's player is IDLE and not on a seat, else it has no effect: standing on a seat
 * never triggers it again (§B1.1). */
void fake_ruby_sit(FakeRuby* g, uint8_t seat);
/* "Please wait...": the player presses B, seen by the next game update in the room (§B1.2). */
void fake_ruby_stand_up(FakeRuby* g);
/* Room, its player IDLE, seen by the next game update: the player steps off its seat (16 game
 * updates holding Up, 0x13) and back on (16 holding Down, 0x12), with no A press (§B7 step 4).
 * The steps themselves aren't simulated; then the seat event, as for fake_ruby_sit. No effect in
 * another phase or while the player isn't IDLE. */
void fake_ruby_resit(FakeRuby* g);
/* The avatar stands on a seat tile: a fake_ruby_sit step was taken, whether or not it ran the
 * seat script. */
bool fake_ruby_on_seat(const FakeRuby* g);
/* gUnknown_03000580 [sPlayerLinkStates]: 0x80 IDLE, 0x81 BUSY, 0x82 READY, 0x83 EXITING_ROOM */
uint8_t fake_ruby_link_state(const FakeRuby* g, uint8_t slot);
/* Frames from the latest OpenLink to gReceivedRemoteLinkPlayers; UINT32_MAX while waiting. */
uint32_t fake_ruby_relink_frames(const FakeRuby* g);
/* simulates one frame of transfers */
void fake_ruby_frame(FakeRuby* g, const MlinkTx* tx, MlinkRx* rx);
FakeRubyPhase fake_ruby_phase(const FakeRuby* g);
/* NULL, or "checksum", "magic", "stale card", "lag", "standby timeout", "link timeout" (no
 * LinkPlayers 300 frames after a Part B OpenLink), "link type" (the trade menu's or the trade's
 * link types differ; Ruby hangs silently there, §B8), "stale block" (a party-exchange block
 * requested before its round's copy: Ruby sends stale bytes, U3), "exchange stalled" (a
 * sub_8048D44 wait case lasted 600 frames: Ruby's black screen, §B2.2), "bad egg" (a Flipper
 * party slot fails its checksum), "bad offer" (a Flipper party slot holds a species, move or item
 * Ruby can't take, or mail, §B2.5), "bad request" (a CCCC type past sBlockRequestLookupTable),
 * "empty party" (a DDDD for a Flipper slot this GBA didn't count, as with an empty Flipper party:
 * Ruby animates a party icon it never made, R8), "unexpected dddd" (a DDDD unless this GBA waits
 * in Link standby after its own AABB), "bad ccdd" (a CCDD unless this GBA's AABB, the Flipper's
 * DDDD and this GBA's BBBB were the latest blocks, in that order: Ruby trades whatever its cursor
 * holds, §B3.4), "ccdd hang" / "eebb hang" (CCDD's or EEBB's fade ended while gLinkCallback was
 * busy, so its SetCloseLinkCallback was ignored and the link stays open: Ruby hangs in trade-menu
 * state 13 or 12. Only a stray Flipper command that makes this GBA send a block gets here, near
 * the fade's end: a 2222 (it resends its LinkPlayerBlock) or a CCCC. An earlier round-type CCCC
 * trips "stale block" first, 3 frames after it arrives; a card CCCC (type 2) doesn't, since the
 * card stays ready after the counter), "early dcba" (a DCBA before this GBA sent its ABCD: Ruby
 * skips TradeMons and saves the untraded party, §B4.3), "flipper mail" (the Flipper's traded
 * Pokemon has a mail index: Ruby gives it mail data the Flipper never sent, §B4.4), "stray 2ffe"
 * (a Flipper 2FFE that isn't the answer to this GBA's own, sent and back as its echo: the flag
 * would let a sync finish without the Flipper, §B5.3), "sync skipped" (a link-save sync began
 * while gLinkCallback was busy, so SetLinkStandbyCallback ignored it, or a block send replaced
 * the standby callback: either way the sync passes without the Flipper), "close skipped" (the
 * link save's SetCloseLinkCallback ignored while gLinkCallback was busy: Ruby hangs in
 * sub_804DC88 state 9). Like the hangs, these two need a stray Flipper command that makes this
 * GBA send a block (a 2222, or a CCCC). "relink mismatch" (the 0x1111 relink after an EEBB: the
 * Flipper's trainer id, or its name up to the first EOS, differs from the Part A linkup's:
 * sub_800826C closes the link, CB2_LinkError), "bad card" (the Flipper's card has stars above
 * 4: sub_8083314 [Task_LinkupAwaitTrainerCardData] keeps it in gTrainerCards, and its stars index
 * the 5 gTrainerCardPalettes (trainer_card.c TrainerCard_LoadPalettes) and, less one, the 4
 * gTrainerCardColorNames (cable_club.c sub_8083BF4), §A5; FRLG indexes its own 5-entry
 * sKantoTrainerCardPals or sHoennTrainerCardPals, fr: trainer_card.c). FRLG mode adds "not
 * ready" (the 0x1133 linkup gate: an R/S Flipper without this save's FLAG_SYS_CAN_LINK_WITH_RS,
 * or an Emerald or other Flipper unless both progressFlagsCopy have a 0xF0 bit, fr: trade.c:
 * GetGameProgressForLinkTrade. FRLG closes the link with no 5FFF, fr: cable_club.c:
 * Task_LinkupExchangeDataWithLeader; phase Closed), and "bad card" also for an FRLG Flipper's
 * card whose version byte at 0x38 isn't its LinkPlayer's (fr: cable_club.c:
 * Task_LinkupAwaitTrainerCardData copies it whole, and fr: trainer_card.c: GetCardType picks
 * the Kanto or the Hoenn card from that byte), and "link timeout" also for the trade's 0x1144
 * relink (fr: trade_scene.c: CB2_LinkTrade calls CheckLinkTimeout in state 3 on the master and in
 * state 4: on this child, more than 300 frames in state 4, each counted, data or not; it closes
 * the link, CB2_LinkError) */
const char* fake_ruby_error(const FakeRuby* g);
/* the Flipper's block as this GBA parsed it (in FRLG mode, converted as every received
 * LinkPlayer is: fr: link.c: ConvertLinkPlayerName) */
const Gen3LinkPlayer* fake_ruby_partner(const FakeRuby* g);

/* The GBA's party, game form, slots 0..count-1 (also its saved party: it saved at the counter,
 * §A6). Later slots are empty (zeros). Call after fake_ruby_init, which clears it. */
void fake_ruby_set_party(FakeRuby* g, const uint8_t party[][GEN3_PARTY_SIZE], uint8_t count);
/* Party slot 0..5, from its RAM or (saved) its save; NULL for another slot. */
const uint8_t* fake_ruby_party(const FakeRuby* g, uint8_t slot, bool saved);
/* The Flipper's party from the latest exchange, slot 0..5; NULL for another slot. */
const uint8_t* fake_ruby_enemy(const FakeRuby* g, uint8_t slot);
uint8_t fake_ruby_enemy_count(const FakeRuby* g); /* CalculateEnemyPartyCount */
/* The highest internal species (an egg counts as 412, SPECIES_EGG) in any Flipper party block
 * this GBA stored (rounds 1-3) since init: every slot that decrypts with a species. 0 if none. */
uint16_t fake_ruby_max_enemy_species(const FakeRuby* g);
FakeRubyMenuState fake_ruby_menu_state(const FakeRuby* g);
/* The main loop skips the next `frames` frames (a map load or flash write): LinkMain1, LinkMain2
 * and the game don't run, VBlank and the serial IRQs do, so the receive queue backs up. */
void fake_ruby_stall(FakeRuby* g, uint8_t frames);

/* Trade menu input (§B3.2). Each is seen by the next trade-menu update and has no effect unless
 * the menu is running in the state named, except fake_ruby_answer, which waits for its prompt. */
/* Browsing: select own party slot, TRADE (a slot past the party can't be selected). With no other
 * battle-able Pokemon (not an egg, HP > 0) nothing is sent: "That's your only POKéMON for
 * battle." (Message). FRLG mode instead (fr: trade.c: CanTradeSelectedMon, the cartridge build),
 * in this order: without this save's National Dex, a species-or-egg above 151 (an Egg is 412) is
 * National; with a Flipper not R/S whose progressFlagsCopy & 0x0F is 0, an Egg is
 * PartnerEggYet and a species above 151 InvalidMon; its own Mew or Deoxys without the
 * fateful-encounter bit is InvalidMon; no other Pokemon but eggs, HP not tested, is LastMon.
 * Every refusal sends nothing and shows its message (Message; fake_ruby_cant_trade). */
void fake_ruby_offer(FakeRuby* g, uint8_t slot);
/* Why the latest fake_ruby_offer sent nothing; None after one that passed its checks (AABB), and
 * from init. */
FakeRubyCantTrade fake_ruby_cant_trade(const FakeRuby* g);
/* Browsing: CANCEL, "Cancel trade?" YES: EEAA 5 frames later (QueueAction(5, 0)), Standby. */
void fake_ruby_cancel_trade(FakeRuby* g);
/* "Is this trade okay?": YES (BBBB) or NO (BBCC), applied once the prompt is up. Either is sent
 * only while gLinkCallback is NULL, as on Ruby; then Standby. FRLG mode: YES with the Flipper's
 * Pokemon (its DDDD slot) a Mew or Deoxys without the fateful-encounter bit sends no BBBB: "The
 * other TRAINER's POKéMON can't be traded now.", and BBCC 180 frames later, unguarded
 * (fr: trade.c: CommunicateWhetherMonCanBeTraded, CheckValidityOfTradeMons); Standby. */
void fake_ruby_answer(FakeRuby* g, bool yes);
/* FRLG mode: the latest YES found the Flipper's Pokemon a Mew or Deoxys without the
 * fateful-encounter bit (PARTNER_MON_INVALID). False from init, and always in Ruby mode. */
bool fake_ruby_partner_mon_invalid(const FakeRuby* g);
/* Message: A, back to Browsing. */
void fake_ruby_press_a(FakeRuby* g);
/* Browsing: SUMMARY of a Pokemon (ShowPokemonSummaryScreen). callback1 still handles the
 * Flipper's blocks, but a slot-0 EEBB handled while the summary is open is lost: closing it runs
 * sub_80484F4 [CB2_ReturnToTradeMenu], which resets the menu state (§B3.4;
 * link-protocol-trade.md T2). */
void fake_ruby_open_summary(FakeRuby* g);
/* Summary: back to Browsing. */
void fake_ruby_close_summary(FakeRuby* g);
/* Browsing: R, the debug block: linkData[i] = i, sent at once with no IsLinkTaskFinished guard
 * (§B3.2). It stays Browsing. */
void fake_ruby_press_r(FakeRuby* g);

/* The trade (§B4) and the link save (§B5). */
uint32_t fake_ruby_saved_trades(const FakeRuby* g); /* link saves committed */
/* The new save slot was partly written when this GBA stopped: a link error or power-off after the
 * save's first sector (after sync 1) and before the commit (§B5.2). The saved party stays as it
 * was: on Ruby, the boot reports a corrupt save and loads the previous one. */
bool fake_ruby_save_corrupt(const FakeRuby* g);
/* Saved National Dex flag, from the Flipper's 0x1144 lp_field_2 (§B4.5); never in FRLG mode */
bool fake_ruby_national_dex(const FakeRuby* g);
/* Main-loop stall per flash sector written in the link save; default 2 (not measured:
 * link-protocol-trade.md T4). */
void fake_ruby_set_sector_stall(FakeRuby* g, uint8_t frames);
