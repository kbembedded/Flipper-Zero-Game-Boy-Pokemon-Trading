#include <src/gen3/gen3_club.h>

#include <src/gen3/gen3_text.h>

#include <string.h>

#define CMD_SEND_LINK_TYPE     0x2222
#define CMD_READY_EXIT_STANDBY 0x2FFE
#define CMD_READY_CLOSE_LINK   0x5FFF
#define CMD_SEND_HELD_KEYS     0xCAFE
#define CMD_SEND_BLOCK_REQ     0xCCCC

/* Room key codes (§A7, §B1.2) */
#define KEY_CODE_READY     0x16
#define KEY_CODE_EXIT_ROOM 0x17
#define KEY_CODE_IDLE      0x1A
#define KEY_CODE_EXIT_SEAT 0x1D

#define BLOCK_REQ_TRAINER_CARD 2
#define FLIPPER_MULTIPLAYER_ID 0 /* slot 0: block headers carry 0x80 */
#define NAME_MAX_CHARS         7

/* Byte 0x13 of a Pokemon, game form or not (the header is never encrypted): bit 1 is hasSpecies
 * (pokeruby BoxPokemon, PKHeX PK3; Gen3FieldHasSpecies in gen3_pkm.c) */
#define PKM_FLAGS_OFF   0x13
#define PKM_HAS_SPECIES 0x02

_Static_assert(GEN3_CLUB_EEBB_RETRY_BURSTS <= UINT8_MAX, "eebb_bursts is a uint8_t");
_Static_assert(GEN3_CLUB_LEAVE_DELAY_CAFES <= UINT8_MAX, "room_cafes is a uint8_t");
_Static_assert(GEN3_CLUB_RELINK_WAIT_FRAMES <= UINT16_MAX, "relink_wait is a uint16_t");
_Static_assert(
    GEN3_CLUB_BLOCK_GAP_BURSTS + GEN3_CLUB_BLOCK_HEADER_BURSTS <= UINT8_MAX,
    "rx_gap is a uint8_t");

static bool send_cmd(Gen3Club* club, uint16_t w0, uint16_t w1) {
    uint16_t cmd[GEN3_CMD_LEN] = {0};
    cmd[0] = w0;
    cmd[1] = w1;
    return gen3_link_send(&club->link, cmd);
}

/* Any non-zero word, as for Gen3LinkEventCommand (an all-zero burst is no command, §A2). */
static bool is_command(const uint16_t cmd[GEN3_CMD_LEN]) {
    for(size_t i = 0; i < GEN3_CMD_LEN; i++) {
        if(cmd[i] != 0) return true;
    }
    return false;
}

/* Bad player block or other room. A checksum error never stops the club: see gen3_club_frame. */
static void set_error(Gen3Club* club, Gen3ClubError error) {
    club->phase = Gen3ClubPhaseError;
    club->last_error = error;
}

/* Per link session, so the seats are per room session. The trainer, the config and the trade
 * context (D3) outlive it. */
static void reset_session(Gen3Club* club) {
    gen3_block_rx_reset(&club->block_rx);
    club->partner_valid = false;
    memset(&club->partner, 0, sizeof(club->partner));
    club->link_type = 0;
    club->card_requested = false;
    club->close_answered = false;
    club->exit_answer_due = false;
    club->gba_exit_seen = false;
    club->exit_sent = false;
    club->room_cafes = 0;
    club->idle_bursts = 0;
    club->gba_seated = false;
    club->flipper_seated = false;
    club->round = 0;
    club->round_gba_done = false;
    club->menu = Gen3ClubMenuIdle;
    club->reply_due = false;
    club->rx_gap = 0;
    club->gba_slot = 0;
    club->refused = Gen3RefuseNone;
    club->party_shown = false;
    club->tainted = false;
    club->eebb_bursts = 0;
}

void gen3_club_claim_game(
    const Gen3ClubConfig* cfg,
    const Gen3LinkPlayer* partner,
    uint32_t link_type,
    bool national_dex,
    Gen3LinkPlayer* lp,
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE]) {
    memset(lp, 0, sizeof(*lp));
    lp->trainer_id = ((uint32_t)cfg->sid << 16) | cfg->tid;
    gen3_text_encode(cfg->name ? cfg->name : "", lp->name, sizeof(lp->name), NAME_MAX_CHARS);
    lp->gender = cfg->gender ? 1 : 0; /* indexes the GBA's avatar table: 0 or 1 only (§A5) */
    lp->link_type = link_type;
    lp->id = 0;
    lp->language = GEN3_LANGUAGE_ENGLISH;
    switch(gen3_link_game(partner ? partner->version : 0)) {
    case Gen3LinkGameFRLG:
        /* §C1: the GBA's own game, and what FRLG's InitLocalLinkPlayer sends. A Ruby claim
         * would fail FRLG's linkup gate (fr: trade.c: GetGameProgressForLinkTrade) */
        lp->version = (uint16_t)(GEN3_LP_VERSION_BASE | (partner->version & 0xFF));
        lp->lp_field_2 = GEN3_LP_NATIONAL_DEX; /* in every block, as FRLG; it never reads it */
        gen3_link_player_set_progress(lp, GEN3_FRLG_PROGRESS_NATIONAL_DEX);
        gen3_trainer_card_build_frlg(lp, card); /* §C2: the Kanto card */
        break;
    case Gen3LinkGameRS:
    case Gen3LinkGameE:
    case Gen3LinkGameUnknown:
        lp->version = GEN3_VERSION_RUBY;
        /* §B4.5: Ruby reads it only from the 0x1144 block */
        lp->lp_field_2 = (link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED && national_dex) ?
                             GEN3_LP_NATIONAL_DEX :
                             0;
        gen3_trainer_card_build(lp, card);
        break;
    }
}

void gen3_club_init(Gen3Club* club, const Gen3ClubConfig* cfg) {
    memset(club, 0, sizeof(*club));
    gen3_link_init(&club->link);
    club->cfg = *cfg;
    club->card_delay = GEN3_CLUB_CARD_DELAY_BURSTS;
    club->round1_delay = GEN3_CLUB_ROUND1_DELAY_BURSTS;
    club->round_delay = GEN3_CLUB_ROUND_DELAY_BURSTS;
    club->phase = Gen3ClubPhaseWaiting;
    club->last_error = Gen3ClubErrorNone;
    gen3_partner_rules(NULL, &club->partner_rules); /* {Unknown, true}: a memset alone isn't */
}

/* §A6 B1: {2222,1133}. Ruby doesn't read word 1 (§A1), and answers with its LinkPlayerBlock. */
static void on_connected(Gen3Club* club) {
    reset_session(club);
    club->relink_due = false; /* the GBA is back, whichever link this is */
    club->phase = Gen3ClubPhaseLinked;
    send_cmd(club, CMD_SEND_LINK_TYPE, GEN3_LINKTYPE_TRADE_SETUP);
}

/* D2: the Flipper's block carries the GBA's link type only where the club follows the GBA there
 * (§B1.6, §B4.1, §B6.3: a mismatch hangs the GBA silently, before anything is written).
 * Otherwise 0x1133: a Link Test never trades, 0x1144 is followed only for a pending trade (CCDD
 * sent), and 0x1111 only after the Flipper's EEBB. */
static uint32_t own_link_type(const Gen3Club* club, uint32_t gba_link_type) {
    if(!club->cfg.trading) return GEN3_LINKTYPE_TRADE_SETUP;
    if(gba_link_type == GEN3_LINKTYPE_TRADE_CONNECTING) return gba_link_type;
    if(gba_link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED && club->trade_pending) {
        return gba_link_type;
    }
    if(gba_link_type == GEN3_LINKTYPE_TRADE && club->return_due) return gba_link_type;
    return GEN3_LINKTYPE_TRADE_SETUP;
}

static bool is_trade_link_type(uint32_t link_type) {
    return link_type == GEN3_LINKTYPE_TRADE || link_type == GEN3_LINKTYPE_TRADE_CONNECTING ||
           link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED;
}

/* §A6 "Optional" (D2): the Flipper's LinkPlayerBlock follows the GBA's. Six commands; nothing
 * else is queued while the GBA's block comes in. */
static void send_own_player_block(Gen3Club* club, uint32_t link_type) {
    Gen3LinkPlayer lp;
    gen3_club_claim_game(
        &club->cfg, &club->partner, link_type, club->national_dex, &lp, club->own_card);
    gen3_link_player_block_build(&lp, club->own_player_block);
    gen3_block_send(
        &club->link,
        club->own_player_block,
        sizeof(club->own_player_block),
        FLIPPER_MULTIPLAYER_ID);
}

/* The pending trade is over (D5): Done (sync 3) and Assumed (lost after the sync-2 answer) take
 * the GBA's Pokemon, which is then also the offer, with the traits its AABB decrypt gave, and the
 * Pokemon the GBA is shown next (§B6.1). Field to field, no local buffer (R2): 300 B. */
static void end_trade(Gen3Club* club, Gen3TradeResult result) {
    if(result == Gen3TradeResultDone || result == Gen3TradeResultAssumed) {
        memcpy(club->received, club->pending_mon, sizeof(club->received));
        memcpy(club->party_mon, club->pending_mon, sizeof(club->party_mon));
        memcpy(club->offer, club->pending_mon, sizeof(club->offer));
        club->has_offer = true;
        club->offer_traits = club->pending_traits;
        club->trades_done++;
    }
    club->trade_pending = false;
    club->save_window = false;
    club->last_result = result;
}

/* §B2.2, §C4: the party exchange's delays for the GBA of a 0x1122 LinkPlayerBlock, by its game,
 * unless a test fixed them (delays_fixed). An FRLG GBA gets an FRLG master's. */
static void set_round_delays_for(Gen3Club* club, uint16_t version) {
    if(club->delays_fixed) return;
    switch(gen3_link_game(version)) {
    case Gen3LinkGameFRLG:
        club->round1_delay = GEN3_CLUB_ROUND1_DELAY_BURSTS_FRLG;
        club->round_delay = GEN3_CLUB_ROUND_DELAY_BURSTS_FRLG;
        break;
    case Gen3LinkGameRS:
    case Gen3LinkGameE:
    case Gen3LinkGameUnknown:
        club->round1_delay = GEN3_CLUB_ROUND1_DELAY_BURSTS;
        club->round_delay = GEN3_CLUB_ROUND_DELAY_BURSTS;
        break;
    }
}

/* The GBA's first block of a session is complete. The Flipper's own block goes out for every
 * completed block, whatever it holds, so the GBA never waits forever for one. A bad block header
 * (Gen3BlockRxBad) sends no block. */
static void on_player_block(Gen3Club* club) {
    Gen3LinkPlayer lp;
    uint32_t own_type = GEN3_LINKTYPE_TRADE_SETUP;
    if(club->block_rx.size != GEN3_LINK_PLAYER_BLOCK_SIZE ||
       !gen3_link_player_block_parse(club->block_rx.data, &lp)) {
        set_error(club, Gen3ClubErrorBadPlayerBlock);
    } else {
        /* Every parsed block, any link type, before the claim: the GBA linked now, so after a
         * cartridge swap its rules replace the old GBA's at its first block */
        gen3_partner_rules(&lp, &club->partner_rules);
        if(lp.link_type == GEN3_LINKTYPE_TRADE_CONNECTING) set_round_delays_for(club, lp.version);
        if(club->trade_pending && lp.link_type != GEN3_LINKTYPE_TRADE_DISCONNECTED) {
            /* D3: the GBA restarted or came back from the counter: the trade never began on it */
            end_trade(club, Gen3TradeResultCanceled);
        }
        club->partner = lp;
        club->link_type = lp.link_type;
        own_type = own_link_type(club, lp.link_type);
        club->return_due = false; /* this is the block it announced, or the GBA went elsewhere */
        if(lp.link_type == GEN3_LINKTYPE_TRADE_SETUP) {
            club->partner_valid = true;
            club->trainer = lp; /* the GBA's player, kept across relinks */
            club->trainer_valid = true;
            club->phase = Gen3ClubPhasePlayers;
            club->idle_bursts = 0;
            club->resit_needed = false; /* it walks in from the counter */
        } else if(own_type == GEN3_LINKTYPE_TRADE) {
            /* §B6.3: back in the room after the Flipper's EEBB. No walk-in and no 2FFE: the GBA
             * starts CAFE at once, its avatar still on its seat, so it must step off and back on
             * to sit again (§B7) */
            club->partner_valid = true;
            club->phase = Gen3ClubPhaseRoom;
            club->resit_needed = true;
        } else if(own_type == GEN3_LINKTYPE_TRADE_CONNECTING) {
            /* §B1.6: the trade menu. The party exchange starts after both blocks. */
            club->partner_valid = true;
            club->phase = Gen3ClubPhaseExchange;
            club->idle_bursts = 0;
        } else if(own_type == GEN3_LINKTYPE_TRADE_DISCONNECTED) {
            /* §B4.1: the pending trade's animation, then the GBA's ABCD and its link save */
            club->partner_valid = true;
            club->phase = Gen3ClubPhaseTrade;
        } else if(is_trade_link_type(lp.link_type)) {
            /* A Cable Club trade type the club doesn't follow: the 0x1133 block stalls the GBA. */
            set_error(club, Gen3ClubErrorUnexpected);
        } else {
            /* At a 0x1133 linkup the GBA reports "different selections" and sends 5FFF (§A6). */
            set_error(club, Gen3ClubErrorOtherRoom);
        }
    }
    gen3_block_rx_reset(&club->block_rx);
    send_own_player_block(club, own_type);
}

/* §A7 "What the Flipper must react to while idling": one answer per request; idle bursts stay
 * all zero (U6). G's CAFE 0017 is answered one G command later. In the game update right after
 * G's own 0x17 comes back, Ruby turns our IDLE avatar BUSY and drops that update's key
 * (pokeruby overworld.c sub_8054FF8), so an answer in the very next burst is lost and G waits
 * forever. A later G command is a receive-queue entry of its own, whose update comes between.
 * The Flipper's one 0017 of a session may be D9's (leave_step): then G's 0017 gets no answer. */
static void on_room_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(club->exit_answer_due) {
        send_cmd(club, CMD_SEND_HELD_KEYS, KEY_CODE_EXIT_ROOM);
        club->exit_answer_due = false;
        club->exit_sent = true;
    }
    if(cmd[0] == CMD_READY_EXIT_STANDBY) {
        send_cmd(club, CMD_READY_EXIT_STANDBY, 0);
    } else if(cmd[0] == CMD_SEND_HELD_KEYS) {
        if(club->room_cafes < UINT8_MAX) club->room_cafes++;
        switch(cmd[1]) {
        case KEY_CODE_EXIT_ROOM:
            club->gba_exit_seen = true;
            if(!club->exit_sent) club->exit_answer_due = true;
            break;
        case KEY_CODE_READY: /* §B1.2: the GBA sat down */
            club->gba_seated = true;
            club->resit_needed = false;
            break;
        case KEY_CODE_EXIT_SEAT: /* B on "Please wait..." */
        case KEY_CODE_IDLE: /* its seat script ended */
            club->gba_seated = false;
            break;
        default:
            break;
        }
    }
}

/* The offer may be shown to this GBA: there is one, and the rules of the GBA whose block last
 * parsed don't refuse its traits (§C3). Otherwise the club treats it as no offer: it doesn't sit,
 * round 1 shows an empty party (R8), and the menu's view of it is stale. So no party block ever
 * carries a Pokemon the partner's own game would refuse. R/S, Emerald and unknown games never
 * refuse: there it is has_offer. Flash only, no decrypt: safe in the link ISR. */
static bool offer_usable(const Gen3Club* club) {
    return club->has_offer &&
           gen3_partner_refuse(&club->partner_rules, club->offer_traits) == Gen3RefuseNone;
}

/* §B1.4 with R5: in a trade session with a usable offer (offer_usable), the Flipper sits as soon
 * as the GBA sits, or later once its offer is usable and no edit is open (O1; the GBA polls
 * GetCableClubPartnersReady with no timeout, as FRLG does: fr: cable_club.c:
 * Task_EnterCableClubSeat). It never sits first, nor while leaving (D9: a 0x16 after its 0x17
 * would turn its EXITING slot READY and start the trade menu), nor once left: left is sticky,
 * and a GBA that walks back in after End trade must not start a new trade in a session that is
 * ending. One CAFE 0016, then zero bursts only, so G's 5FFF isn't delayed (§B1.5). It stays READY
 * if the GBA stands up (§B1.4: legal). */
static void try_sit(Gen3Club* club) {
    if(club->phase != Gen3ClubPhaseRoom || !club->cfg.trading || !offer_usable(club) ||
       !club->gba_seated || club->flipper_seated || club->editing || club->leaving || club->left) {
        return;
    }
    /* A full queue sends nothing: try again next frame */
    if(send_cmd(club, CMD_SEND_HELD_KEYS, KEY_CODE_READY)) club->flipper_seated = true;
}

/* D9 (§B7 "Flipper-initiated leave"): one CAFE 0017 in the room. It makes our slot EXITING_ROOM;
 * an IDLE GBA is then forced BUSY into TradeRoom_TerminateLink, and a seated one cancels its
 * seat first (§B1.3 row 1) and is forced out once its 0x1A makes it IDLE. Either way the GBA
 * sends 0017 itself (not answered: exit_sent), then 5FFF, and closes the link. It waits:
 * - for GEN3_CLUB_LEAVE_DELAY_CAFES of the GBA's CAFEs, so its lockstep runs and a 0x1111
 *   room's fade-in is over;
 * - while both sit: the GBA is starting the trade menu (§B1.3 row 4), whose EEBB follows;
 * - while the answer to the GBA's own 0017 is due: that answer is the one 0017;
 * - for an empty queue, so it goes out in this burst. The GBA's own 0017 can then come in this
 *   burst at the latest, never in the next, where it would make the GBA drop ours (sub_8054FF8,
 *   as for a hasty answer). */
static void leave_step(Gen3Club* club) {
    if(!club->leaving || club->phase != Gen3ClubPhaseRoom) return;
    if(club->exit_sent || club->exit_answer_due) return;
    if(club->room_cafes < GEN3_CLUB_LEAVE_DELAY_CAFES) return;
    if(club->gba_seated && club->flipper_seated) return;
    if(gen3_link_queued(&club->link) != 0) return;
    club->exit_sent = send_cmd(club, CMD_SEND_HELD_KEYS, KEY_CODE_EXIT_ROOM);
}

/* D9: the GBA's link is over for good, by its exit, a power-off or a pulled cable. Any of them
 * ends the leave: there is no GBA left to walk out. */
static void finish_leave(Gen3Club* club) {
    if(!club->leaving) return;
    club->leaving = false;
    club->left = true;
}

/* §B2.1: the GBA's block of the round in progress. Rounds 1-3 carry its party, two slots each;
 * rounds 4 (mail) and 5 (gift ribbons) are not kept. A block the club didn't ask for, or of
 * another size, leaves the GBA and the club out of step. */
static void on_exchange_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    Gen3BlockRxResult r = gen3_block_rx_feed(&club->block_rx, cmd);
    if(r == Gen3BlockRxBad) {
        set_error(club, Gen3ClubErrorUnexpected);
        return;
    }
    if(r != Gen3BlockRxDone) return;
    if(club->round == 0 || club->round_gba_done ||
       club->block_rx.size != gen3_trade_round_size(club->round)) {
        set_error(club, Gen3ClubErrorUnexpected);
        return;
    }
    if(club->round <= 3) {
        memcpy(club->gba_party[2 * (club->round - 1)], club->block_rx.data, 2 * GEN3_PARTY_SIZE);
    }
    club->round_gba_done = true;
}

/* Round r: {CCCC, type} and the Flipper's block, 17 or 18 commands into an empty queue. Round 1
 * fixes the Pokemon the GBA is shown: the offer now, or zeros without a usable one (R8,
 * offer_usable). An empty partner column is legal for FRLG as for Ruby (fr: trade.c:
 * CB2_CreateTradeMenu, SetActiveMenuOptions). */
static void start_round(Gen3Club* club, uint8_t round) {
    if(round == 1) {
        const bool usable = offer_usable(club);
        if(usable) {
            memcpy(club->party_mon, club->offer, sizeof(club->party_mon));
        } else {
            memset(club->party_mon, 0, sizeof(club->party_mon));
        }
        /* R8: Ruby counts the Flipper's party up to the first species 0. The offer has a species
         * (it passed gen3_trade_offer_check); without one the GBA's partner column is empty. The
         * hasSpecies bit is required too: conservative, Ruby counts by the species alone. */
        club->party_shown = usable && (club->party_mon[PKM_FLAGS_OFF] & PKM_HAS_SPECIES) != 0;
    }
    gen3_trade_round_block(round, round == 1 ? club->party_mon : NULL, club->round_block);
    send_cmd(club, CMD_SEND_BLOCK_REQ, gen3_trade_round_request(round));
    gen3_block_send(
        &club->link, club->round_block, gen3_trade_round_size(round), FLIPPER_MULTIPLAYER_ID);
    club->round = round;
    club->round_gba_done = false;
    club->idle_bursts = 0;
    /* Drops a partial block, if any: no full gen3_block_rx_reset (a 262-B memset) in the ISR.
     * Chunks are ignored until the next header, which sets size and pos = 0, and Done comes only
     * once that block's chunks have rewritten every byte below size. */
    club->block_rx.active = false;
}

/* §B2.2. A step is complete once our block has left (the queue is empty) and the GBA's is in:
 * both LinkPlayerBlocks before round 1, both blocks of round r before round r + 1. Then
 * round1_delay idle bursts (the GBA's case 1 must have run) or round_delay (padding, see
 * gen3_club.h). After round 5: the trade menu. */
static void exchange_step(Gen3Club* club) {
    if(club->phase != Gen3ClubPhaseExchange) return;
    if(club->round > 0 && !club->round_gba_done) return;
    if(gen3_link_queued(&club->link) != 0) return;
    if(club->round == GEN3_TRADE_ROUNDS) {
        club->phase = Gen3ClubPhaseMenu;
        club->idle_bursts = 0; /* the arbiter's delay counts from the end of round 5 */
        return;
    }
    if(club->idle_bursts < (club->round == 0 ? club->round1_delay : club->round_delay)) {
        club->idle_bursts++; /* the burst built below is idle */
        return;
    }
    start_round(club, (uint8_t)(club->round + 1));
}

/* §B3.2: the GBA's trade-menu codes, each answered once (§B3.4). Every other code gets no reply,
 * the R-button debug block (word 0 = 0) among them. menu_code is the one list of them:
 * on_menu_command files a reply only for these, and menu_reply covers each (-Wswitch). */
typedef enum {
    MenuCodeNone,
    MenuCodeReady, /* AABB */
    MenuCodeConfirm, /* BBBB */
    MenuCodeConfirmNo, /* BBCC */
    MenuCodeCancel, /* EEAA */
} MenuCode;

static MenuCode menu_code(uint16_t code) {
    switch(code) {
    case GEN3_TRADE_READY:
        return MenuCodeReady;
    case GEN3_TRADE_CONFIRM:
        return MenuCodeConfirm;
    case GEN3_TRADE_CONFIRM_NO:
        return MenuCodeConfirmNo;
    case GEN3_TRADE_CANCEL:
        return MenuCodeCancel;
    default:
        return MenuCodeNone;
    }
}

/* The GBA's block in block_rx is one the arbiter files once it is complete, as far as its bytes
 * so far tell: 20 bytes, a code of §B3.2's once the first chunk is in, and the GBA not on its way
 * out (Starting, Leaving). */
static bool block_filed(const Gen3Club* club) {
    if(club->block_rx.size != GEN3_TRADE_BLOCK_SIZE) return false;
    if(club->menu == Gen3ClubMenuStarting || club->menu == Gen3ClubMenuLeaving) return false;
    if(club->block_rx.pos == 0) return true;
    return menu_code(gen3_trade_block_code(club->block_rx.data, NULL)) != MenuCodeNone;
}

/* §B3.3: the GBA's 20-byte blocks in the trade menu. menu_step answers each once. Only a block it
 * files, or a chunk of one, restarts the arbiter's delay: the R-button debug block 0000
 * doesn't, nor a block in Starting or Leaving, where the GBA is on its way out. */
static void on_menu_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    const Gen3BlockRxResult r = gen3_block_rx_feed(&club->block_rx, cmd);
    if(r == Gen3BlockRxIgnored) return;
    club->rx_gap = 0;
    if(r != Gen3BlockRxProgress && r != Gen3BlockRxDone) return; /* a header, or a bad one */
    if(!block_filed(club)) return;
    club->idle_bursts = 0;
    if(r != Gen3BlockRxDone) return;
    uint16_t arg;
    club->gba_code = gen3_trade_block_code(club->block_rx.data, &arg);
    club->gba_arg = arg;
    club->reply_due = true;
}

/* Ruby sends a block's chunks on consecutive frames, the first one GEN3_CLUB_BLOCK_HEADER_BURSTS
 * after the header (LinkCB_BlockSendBegin). A block that stops short, as with a glitched size, is
 * dropped on the GEN3_CLUB_BLOCK_GAP_BURSTS-th burst in a row without a chunk for it
 * (GEN3_CLUB_BLOCK_HEADER_BURSTS more before the first chunk), so it can't hold the arbiter; a
 * chunk held back by up to GEN3_CLUB_BLOCK_GAP_BURSTS - 1 GBA lag frames still counts. No full
 * gen3_block_rx_reset (a 262-B memset) in the ISR: a later header restarts it. */
static void drop_stopped_block(Gen3Club* club) {
    if(!club->block_rx.active) return;
    const uint8_t limit =
        GEN3_CLUB_BLOCK_GAP_BURSTS + (club->block_rx.pos == 0 ? GEN3_CLUB_BLOCK_HEADER_BURSTS : 0);
    if(club->rx_gap >= limit) {
        club->block_rx.active = false;
    } else {
        club->rx_gap++; /* on_menu_command zeroes it for a command of this block */
    }
}

/* D4 (O1, D9): the GBA's view of the Flipper's party is out of date. No usable offer
 * (offer_usable), an offer other than the one round 1 showed (party_mon), a checksum error
 * counted in this 0x1122 session (a block of the GBA's may be wrong in the club's copy), the app
 * is editing, or a leave is asked or done (leaving, left). After left, a menu can still come from
 * a trade pending when the GBA was taken as gone, finished by a late relink: its post-save menu
 * must not start another trade. Then every GBA block gets EEBB, never DDDD or CCDD (save safety,
 * §B3.4), and with nothing pending the GBA gets an unsolicited one. A 100-byte compare, no copy
 * (R2). */
static bool offer_stale(const Gen3Club* club) {
    return !offer_usable(club) || club->tainted || club->editing || club->leaving || club->left ||
           memcmp(club->offer, club->party_mon, sizeof(club->offer)) != 0;
}

/* EEBB (§B3.3, §B6.3): the GBA fades out, closes the link and goes back to the room through a
 * 0x1111 relink. Resent after GEN3_CLUB_EEBB_RETRY_BURSTS without its 5FFF (menu_step). */
static uint16_t leave_menu(Gen3Club* club) {
    club->menu = Gen3ClubMenuLeaving;
    club->eebb_bursts = 0;
    club->return_due = true;
    return GEN3_TRADE_BOTH_CANCEL;
}

/* §B3.4 and D4, decided as the reply goes out: the code to send for the GBA's block, and the
 * arbiter's new state. Every reply's arg is 0 (the Flipper offers its slot 0). AABB is the one
 * decrypt of the frame (R2); an AABB while Offered is the GBA's new pick and is decided afresh. */
static uint16_t menu_reply(Gen3Club* club) {
    const uint16_t slot = club->gba_arg;
    if(offer_stale(club)) return leave_menu(club);
    switch(menu_code(club->gba_code)) {
    case MenuCodeReady:
        club->refused = slot < GEN3_CLUB_GBA_PARTY ?
                            gen3_trade_check_incoming(club->gba_party[slot], &club->scratch) :
                            Gen3RefuseEmpty;
        /* R8: never a DDDD to an empty partner column */
        if(club->refused != Gen3RefuseNone || !club->party_shown) {
            club->menu = Gen3ClubMenuIdle;
            return GEN3_TRADE_PLAYER_CANCEL;
        }
        /* The traits it will have as the offer once traded, from the scratch just decrypted */
        club->pending_traits = gen3_offer_traits(&club->scratch);
        club->gba_slot = (uint8_t)slot;
        club->menu = Gen3ClubMenuOffered;
        return GEN3_TRADE_SET_MONS;
    case MenuCodeConfirm:
        /* Save safety: Offered means the GBA's AABB got our DDDD, and no block of ours since */
        if(club->menu != Gen3ClubMenuOffered) break;
        memcpy(club->pending_mon, club->gba_party[club->gba_slot], sizeof(club->pending_mon));
        club->trade_pending = true;
        club->save_syncs = 0; /* no earlier trade's syncs through this one */
        club->sync2_unconfirmed = false;
        club->menu = Gen3ClubMenuStarting;
        return GEN3_TRADE_START;
    case MenuCodeCancel:
        return leave_menu(club);
    case MenuCodeConfirmNo:
    case MenuCodeNone: /* never filed: on_menu_command */
        break;
    }
    club->menu = Gen3ClubMenuIdle;
    return GEN3_TRADE_PLAYER_CANCEL;
}

/* §B4.3: true once the reply to the GBA's block may go out (the caller then sends it): an empty
 * queue, and GEN3_CLUB_REPLY_DELAY_BURSTS idle bursts after the one that carried the block's last
 * chunk. */
static bool reply_ready(Gen3Club* club) {
    if(!club->reply_due || gen3_link_queued(&club->link) != 0) return false;
    if(club->idle_bursts < GEN3_CLUB_REPLY_DELAY_BURSTS) {
        club->idle_bursts++; /* the burst built below is idle */
        return false;
    }
    club->reply_due = false;
    return true;
}

/* The one reply: a 20-byte block {code, 0}, 3 commands into an empty queue. */
static void send_reply(Gen3Club* club, uint16_t code) {
    uint8_t block[GEN3_TRADE_BLOCK_SIZE];
    gen3_trade_block_build(code, 0, block);
    gen3_block_send(&club->link, block, sizeof(block), FLIPPER_MULTIPLAYER_ID);
}

/* §B3.4, D4: at most one arbiter block, once the queue is empty, no GBA block it would file is
 * coming in, and GEN3_CLUB_REPLY_DELAY_BURSTS idle bursts have passed since the last chunk of the
 * GBA's latest filed block, or the end of round 5. It is the reply to the GBA's block; else, with
 * nothing pending (Idle: not while the GBA confirms after a DDDD) and the offer stale, an
 * unsolicited EEBB; else, in Leaving, the EEBB again once GEN3_CLUB_EEBB_RETRY_BURSTS have passed
 * without the GBA's 5FFF (lost in its summary screen, link-protocol-trade.md T2). The GBA's 5FFF
 * ends Menu (on_command). A block it doesn't file may cross ours: Ruby reads the Flipper's blocks
 * in any menu state (§B3.3), and its own block is out long before an EEBB's fade ends (§B3.4). */
static void menu_step(Gen3Club* club) {
    if(club->phase != Gen3ClubPhaseMenu) return;
    const bool leaving = club->menu == Gen3ClubMenuLeaving;
    if(leaving && club->eebb_bursts < GEN3_CLUB_EEBB_RETRY_BURSTS) club->eebb_bursts++;
    drop_stopped_block(club);
    if(club->block_rx.active && block_filed(club)) return;
    if(gen3_link_queued(&club->link) != 0) return;
    if(club->idle_bursts < GEN3_CLUB_REPLY_DELAY_BURSTS) {
        club->idle_bursts++; /* the burst built below is idle */
        return;
    }
    if(club->reply_due) {
        club->reply_due = false;
        send_reply(club, menu_reply(club));
    } else if(club->menu == Gen3ClubMenuIdle && offer_stale(club)) {
        send_reply(club, leave_menu(club));
    } else if(leaving && club->eebb_bursts >= GEN3_CLUB_EEBB_RETRY_BURSTS) {
        send_reply(club, leave_menu(club));
    }
}

/* §B4.2-B4.3: the animation runs without link traffic, then the GBA sends ABCD once. Any other
 * block of the GBA's gets no reply. */
static void on_trade_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(gen3_block_rx_feed(&club->block_rx, cmd) != Gen3BlockRxDone) return;
    if(club->block_rx.size != GEN3_TRADE_BLOCK_SIZE) return;
    if(gen3_trade_block_code(club->block_rx.data, NULL) != GEN3_TRADE_ANIM_DONE) return;
    club->reply_due = true;
    club->idle_bursts = 0;
}

/* §B4.3: one DCBA, only after the GBA's ABCD (an early one makes it save the untraded party);
 * then its link save. */
static void trade_step(Gen3Club* club) {
    if(club->phase != Gen3ClubPhaseTrade || !reply_ready(club)) return;
    send_reply(club, GEN3_TRADE_ANIM_ACK);
    club->phase = Gen3ClubPhaseSave;
}

/* §B5.1, §B5.3: the GBA's link save. Each of its 2FFEs gets exactly one 2FFE, at once (it commits
 * 1-2 frames after our sync-2 answer); an extra one would let a later sync finish without us.
 * From our sync-1 answer the GBA may hold a half-written save (save_window, D10). Sync 3 comes
 * only after its commit: the trade is done. */
static void on_save_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(cmd[0] != CMD_READY_EXIT_STANDBY) return;
    /* The queue is empty here; a full one would drop the answer, so the sync isn't counted */
    if(!send_cmd(club, CMD_READY_EXIT_STANDBY, 0)) return;
    if(club->save_syncs >= GEN3_CLUB_SAVE_SYNCS) return;
    club->save_syncs++;
    if(club->save_syncs == 1) club->save_window = true;
    if(club->save_syncs == 2) club->sync2_unconfirmed = true; /* until a burst carries it */
    if(club->save_syncs == GEN3_CLUB_SAVE_SYNCS) end_trade(club, Gen3TradeResultDone);
}

/* D5, §B5.3: the link is lost with a trade pending.
 * - In the trade's own 0x1144 session, by the 2FFEs that reached the GBA: none, Canceled; sync 1
 *   only, RolledBack; sync 2 too, Assumed (it commits 1-2 frames after our answer). A sync-2
 *   answer still unconfirmed counts as not received.
 * - In the 0x1122 session of its CCDD, before its 5FFF close was answered: the GBA can't have
 *   left the trade menu (it closes only after our 5FFF), so the trade never began: Canceled.
 * - Elsewhere (the normal relink between the two) it waits for the GBA's next LinkPlayerBlock
 *   (D3). */
static void on_link_lost(Gen3Club* club) {
    if(!club->trade_pending) return;
    if(club->link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED) {
        if(club->save_syncs == 0) {
            end_trade(club, Gen3TradeResultCanceled);
        } else if(club->save_syncs == 1 || club->sync2_unconfirmed) {
            end_trade(club, Gen3TradeResultRolledBack);
        } else {
            end_trade(club, Gen3TradeResultAssumed);
        }
    } else if(club->link_type == GEN3_LINKTYPE_TRADE_CONNECTING && !club->close_answered) {
        end_trade(club, Gen3TradeResultCanceled);
    }
}

/* The GBA's 5FFF in this phase closes the link to open the next one at once (relink_due): from
 * the room for the trade menu (§B1.5; a GBA walking out sends its CAFE 0017 first, §B7), from
 * the menu for the room or the trade (§B6.3, §B4.1), from its link save for the menu (§B6.1).
 * Any other close ends its link play: the walk out, a linkup that failed (§A6). */
static bool closes_for_relink(const Gen3Club* club) {
    switch(club->phase) {
    case Gen3ClubPhaseRoom:
        return !club->gba_exit_seen;
    case Gen3ClubPhaseMenu:
    case Gen3ClubPhaseSave:
        return true;
    default:
        return false;
    }
}

static void on_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(cmd[0] == CMD_READY_CLOSE_LINK) {
        /* §A7: always answer with exactly one 5FFF, whatever the phase. */
        if(!club->close_answered) {
            send_cmd(club, CMD_READY_CLOSE_LINK, 0);
            club->close_answered = true;
            club->relink_due = closes_for_relink(club);
        }
        club->phase = Gen3ClubPhaseClosing;
        return;
    }

    Gen3BlockRxResult r;
    switch(club->phase) {
    case Gen3ClubPhaseLinked:
        r = gen3_block_rx_feed(&club->block_rx, cmd);
        if(r == Gen3BlockRxDone) {
            on_player_block(club);
        } else if(r == Gen3BlockRxBad) {
            set_error(club, Gen3ClubErrorBadPlayerBlock);
        }
        break;
    case Gen3ClubPhasePlayers:
        if(!club->card_requested) break;
        r = gen3_block_rx_feed(&club->block_rx, cmd);
        if(r == Gen3BlockRxDone) {
            club->phase = Gen3ClubPhaseCards; /* the GBA's card stays in block_rx */
        } else if(r == Gen3BlockRxBad) {
            set_error(club, Gen3ClubErrorBadPlayerBlock);
        }
        break;
    case Gen3ClubPhaseCards:
        /* §A7 entering: zero bursts through the walk-in, then the GBA's standby 2FFE. */
        if(cmd[0] == CMD_READY_EXIT_STANDBY) {
            send_cmd(club, CMD_READY_EXIT_STANDBY, 0);
            club->phase = Gen3ClubPhaseRoom;
        }
        break;
    case Gen3ClubPhaseRoom:
        on_room_command(club, cmd);
        break;
    case Gen3ClubPhaseExchange:
        on_exchange_command(club, cmd);
        break;
    case Gen3ClubPhaseMenu:
        on_menu_command(club, cmd);
        break;
    case Gen3ClubPhaseTrade:
        on_trade_command(club, cmd);
        break;
    case Gen3ClubPhaseSave:
        on_save_command(club, cmd);
        break;
    default: /* Closing: keep bursting until the GBA closes (Disconnected). */
        break;
    }
}

/* §A6 "idle" row: once both player blocks are on the wire (ours goes last), card_delay all-zero
 * bursts, then 0xCCCC type 2 and our trainer card (10 commands into an empty queue). */
static void request_cards(Gen3Club* club) {
    if(club->phase != Gen3ClubPhasePlayers || club->card_requested) return;
    if(gen3_link_queued(&club->link) != 0) return; /* our own block is still going out */
    if(club->idle_bursts < club->card_delay) {
        club->idle_bursts++; /* the burst built below is idle */
        return;
    }
    send_cmd(club, CMD_SEND_BLOCK_REQ, BLOCK_REQ_TRAINER_CARD);
    gen3_block_send(&club->link, club->own_card, sizeof(club->own_card), FLIPPER_MULTIPLAYER_ID);
    club->card_requested = true;
    gen3_block_rx_reset(&club->block_rx);
}

void gen3_club_frame(Gen3Club* club, const MlinkRx* rx, MlinkTx* tx) {
    club->frames++;
    if(rx) {
        uint16_t cmd[GEN3_CMD_LEN];
        const Gen3LinkEvent event = gen3_link_rx(&club->link, rx, cmd);
        /* Confirms the sync-2 answer once it has left (nothing queued) and a whole burst came
         * back without a checksum error. Every transfer answered: pokeruby's DoRecv queues a
         * command only after all 8, so a partial burst means the GBA never got our word. No
         * checksum error: a mismatch both sides see makes the GBA's CheckErrorStatus link-error
         * before its commit. A stop-bit error alone on the GBA's word doesn't hold it back: when
         * unsure, Assumed is the safer misreport. If the GBA in fact kept its old save, the
         * Flipper holds a clone of the GBA's Pokemon, which the GBA still has, and has lost its
         * own offer, which can be made again. RolledBack when the GBA did save loses the GBA's
         * traded Pokemon on both sides. */
        if((event == Gen3LinkEventNone || event == Gen3LinkEventCommand) &&
           rx->timeout_mask == 0 && gen3_link_queued(&club->link) == 0) {
            club->sync2_unconfirmed = false;
        }
        switch(event) {
        case Gen3LinkEventNone:
            break;
        case Gen3LinkEventConnected:
            on_connected(club);
            break;
        case Gen3LinkEventCommand:
            on_command(club, cmd);
            break;
        case Gen3LinkEventChecksumError:
            /* Transfer #1 carries each side's checksum and is not summed (§A2). A glitch on G's
             * word fails only our check: G is healthy and still expects every answer, so keep
             * the phase and the queue. If G's check failed too, G closes the link and its slot
             * reads 0xFFFF: Disconnected resets us. An Error phase keeps the error it shows. */
            if(club->phase != Gen3ClubPhaseError) club->last_error = Gen3ClubErrorChecksum;
            club->tainted = true; /* D4: this session's blocks may be wrong in our copy */
            if(is_command(cmd)) on_command(club, cmd); /* e.g. G's one 2FFE or CAFE 0017 */
            break;
        case Gen3LinkEventDisconnected:
            on_link_lost(club);
            reset_session(club);
            club->phase = Gen3ClubPhaseWaiting; /* last_error is kept */
            /* D9: unless the GBA is opening its next link (relink_due), its link is over: the
             * leave is done */
            club->relink_wait = 0;
            if(!club->relink_due) finish_leave(club);
            break;
        }
    }
    /* A relink that doesn't come: the GBA is gone (powered off or unplugged in the gap) */
    if(club->phase == Gen3ClubPhaseWaiting && club->relink_due &&
       ++club->relink_wait >= GEN3_CLUB_RELINK_WAIT_FRAMES) {
        club->relink_due = false;
        finish_leave(club);
    }
    if(club->phase == Gen3ClubPhaseRoom) club->room_frames++;
    request_cards(club);
    try_sit(club);
    leave_step(club);
    exchange_step(club);
    menu_step(club);
    trade_step(club);
    gen3_link_tx(&club->link, tx);
}

void gen3_club_status(const Gen3Club* club, Gen3ClubStatus* out) {
    memset(out, 0, sizeof(*out));
    out->phase = club->phase;
    out->last_error = club->last_error;
    out->partner_valid = club->partner_valid;
    out->partner = club->partner;
    out->frames = club->frames;
    out->room_frames = club->room_frames;
    gen3_link_stats(&club->link, &out->link);
    out->link_type = club->link_type;
    out->trainer_valid = club->trainer_valid;
    out->trainer = club->trainer;
    out->has_offer = club->has_offer;
    out->gba_seated = club->gba_seated;
    out->flipper_seated = club->flipper_seated;
    out->trades_done = club->trades_done;
    out->round = club->round;
    out->menu = club->menu;
    out->gba_slot = club->gba_slot;
    out->refused = club->refused;
    out->trade_pending = club->trade_pending;
    out->save_syncs = club->save_syncs;
    out->save_window = club->save_window;
    out->last_result = club->last_result;
    out->offer_stale = club->phase == Gen3ClubPhaseMenu && offer_stale(club);
    out->resit_needed = club->resit_needed;
    out->editing = club->editing;
    out->leaving = club->leaving;
    out->left = club->left;
    out->relinking = club->relink_due;
    out->partner_rules = club->partner_rules;
    out->offer_traits = club->offer_traits;
    out->offer_usable = offer_usable(club);
}

bool gen3_club_set_offer_traits(
    Gen3Club* club,
    const uint8_t* game,
    uint32_t trades_seen,
    uint8_t traits) {
    if(trades_seen != club->trades_done) return false;
    if(game) {
        memcpy(club->offer, game, sizeof(club->offer));
    } else {
        memset(club->offer, 0, sizeof(club->offer));
    }
    club->has_offer = game != NULL;
    club->offer_traits = game ? traits : 0;
    return true;
}

bool gen3_club_set_offer(Gen3Club* club, const uint8_t* game, uint32_t trades_seen) {
    return gen3_club_set_offer_traits(club, game, trades_seen, GEN3_OFFER_TRAITS_UNKNOWN);
}

bool gen3_club_set_editing(Gen3Club* club, bool on, uint32_t trades_seen) {
    if(on && (club->trade_pending || trades_seen != club->trades_done)) return false;
    club->editing = on;
    return true;
}

void gen3_club_request_leave(Gen3Club* club) {
    /* Waiting between the close of one link and the next isn't "no GBA linked" (relink_due) */
    if(club->phase == Gen3ClubPhaseWaiting && !club->relink_due) {
        club->leaving = false;
        club->left = true;
        return;
    }
    club->leaving = true;
    club->left = false;
}

void gen3_club_set_card_delay(Gen3Club* club, uint8_t bursts) {
    club->card_delay = bursts;
}

void gen3_club_set_round_delays(Gen3Club* club, uint8_t first, uint8_t later) {
    club->round1_delay = first;
    club->round_delay = later;
    club->delays_fixed = true;
}

void gen3_club_gba_mon(const Gen3Club* club, uint8_t slot, uint8_t out[GEN3_PARTY_SIZE]) {
    if(slot < GEN3_CLUB_GBA_PARTY) {
        memcpy(out, club->gba_party[slot], GEN3_PARTY_SIZE);
    } else {
        memset(out, 0, GEN3_PARTY_SIZE);
    }
}

void gen3_club_set_national_dex(Gen3Club* club, bool on) {
    club->national_dex = on;
}

void gen3_club_get_received(
    const Gen3Club* club,
    uint8_t out[GEN3_PARTY_SIZE],
    uint32_t* trades_done) {
    memcpy(out, club->received, GEN3_PARTY_SIZE);
    if(trades_done) *trades_done = club->trades_done;
}
