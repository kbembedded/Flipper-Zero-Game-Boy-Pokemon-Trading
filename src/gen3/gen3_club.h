#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>
#include <src/gen3/gen3_link.h>
#include <src/gen3/gen3_block.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_trade_block.h>

/* Cable Club (Trade Center) state machine, Flipper as multiplayer master: linkup, the room and
 * leaving it (docs/gen3/link-protocol.md §A6-A8, §B7), and in a trade session sitting down, the
 * relink for the trade menu, its party exchange, the trade menu with the Flipper as the arbiter,
 * the trade and the GBA's link save, the EEBB that sends the GBA back to the room when the
 * Flipper's Pokemon changes (docs/gen3/link-protocol-trade.md §B1-B7), and walking the GBA out of
 * the room when the app asks (D9). Pure C, no heap; gen3_club_frame runs in the link frame ISR. */

#define GEN3_CLUB_CARD_DELAY_BURSTS 10 /* idle bursts between both player blocks and 0xCCCC (§A6, U3 margin >= 5) */
/* Idle bursts before a party-exchange CCCC (§B2.2). Round 1's follows both LinkPlayerBlocks: the
 * GBA must take it 3 frames after them or later, and 10 also absorbs a GBA main-loop stall of up
 * to 8 frames there. Rounds 2-5 follow both blocks of the previous round: this delay is
 * defensive padding (§B2.2's ">= 5" is inferred), not required by pokeruby. The CCCC reaches the
 * GBA's receive queue behind both blocks, so the GBA takes it no earlier than the frame it ends
 * the round, and its copy for the next round (a frame later) precedes the block's first chunk
 * (3 frames after the request). */
#define GEN3_CLUB_ROUND1_DELAY_BURSTS 10
#define GEN3_CLUB_ROUND_DELAY_BURSTS  6
/* The same for an FRLG GBA, which runs an 11-frame delay state before every CCCC, on master and
 * child alike (fr: trade.c: BufferTradeParties cases 2/6/10/14/18): an FRLG master's round 1
 * comes about 14 frames after both LinkPlayerBlocks, and §C4 asks for at least 15 there. The
 * club mirrors that master: one burst over §C4's 15 before round 1, and the 11 frames plus one
 * before each later round. FRLG's trade menu runs one frame more than Ruby's before its case 0
 * (fr: trade.c: CB2_CreateTradeMenu case 5), so it must take round 1's CCCC 4 frames after both
 * LinkPlayerBlocks or later; 16 absorbs a GBA main-loop stall of up to 13 frames there. */
#define GEN3_CLUB_ROUND1_DELAY_BURSTS_FRLG 16
#define GEN3_CLUB_ROUND_DELAY_BURSTS_FRLG  12
#define GEN3_CLUB_GBA_PARTY           6 /* the GBA's party slots */
/* Idle bursts between the last chunk of the GBA's block and the reply (§B3.4: >= 5; DCBA §B4.3:
 * >= 2) */
#define GEN3_CLUB_REPLY_DELAY_BURSTS  6
#define GEN3_CLUB_SAVE_SYNCS          3 /* the GBA's 2FFEs in one link save (§B5.1) */
/* Resend an EEBB without the GBA's 5FFF: ~3 s (§B3.4; inferred, link-protocol-trade.md T2) */
#define GEN3_CLUB_EEBB_RETRY_BURSTS   180
/* Menu: an incomplete GBA block is dropped on the GEN3_CLUB_BLOCK_GAP_BURSTS-th burst in a row
 * with no chunk for it, or that many plus GEN3_CLUB_BLOCK_HEADER_BURSTS after its header (the
 * frames LinkCB_BlockSendBegin waits before the first chunk). Ruby sends a block's chunks on
 * consecutive frames: the margin lets a filed block's chunk come after a few GBA lag frames, and a
 * glitched size header still can't hold the arbiter forever. */
#define GEN3_CLUB_BLOCK_GAP_BURSTS    GEN3_CLUB_REPLY_DELAY_BURSTS
#define GEN3_CLUB_BLOCK_HEADER_BURSTS 3
/* D9: the GBA CAFEs (one per GBA frame) the club sees in a room session before its CAFE 0017.
 * After a 0x1111 relink the GBA fades in with its controls locked for about 23-24 GBA frames from
 * its first CAFE: the FADE_FROM_BLACK palette fade and its 5 finishing calls, then the weather
 * update and task_mpl_807DD60 case 2 (field_weather.c:497-501, field_fadetransition.c:122-128).
 * An exit forced in the fade-in finds the controls unlocked when it ends (sub_80553E4 sends 0x1A),
 * and TerminateLink restarts once: no link error, but a reprinted message. At least 26: the
 * fade-in and a margin. */
#define GEN3_CLUB_LEAVE_DELAY_CAFES   30
/* After the GBA closed a link to open the next one at once (the 5FFF answered in Room for the
 * trade menu, in Menu or in Save), the frames the club waits for it to connect before taking the
 * GBA as gone. Ruby opens the next link within about 20 frames of its CloseLink (§B1.5, §B4.1,
 * §B6.1, §B6.3) and gives a 0x1122 or 0x1111 relink 300 frames (§B8). FireRed and LeafGreen
 * open theirs as fast, with the same 300 frames, and give the trade's 0x1144 relink a deadline of
 * its own: 300 frames, then a link error on the GBA (§C4; fr: trade_scene.c: CB2_LinkTrade,
 * CheckLinkTimeout). */
#define GEN3_CLUB_RELINK_WAIT_FRAMES  300

/* The values are the phase numbers in the link log: append only. */
typedef enum {
    Gen3ClubPhaseWaiting = 0,
    Gen3ClubPhaseLinked = 1,
    Gen3ClubPhasePlayers = 2,
    Gen3ClubPhaseCards = 3,
    Gen3ClubPhaseRoom = 4,
    Gen3ClubPhaseClosing = 5,
    Gen3ClubPhaseError = 6, /* bad player block, other room, unexpected: only 5FFF is answered */
    Gen3ClubPhaseExchange = 7, /* 0x1122 relink: party exchange (trade menu loading) */
    Gen3ClubPhaseMenu = 8, /* trade menu */
    Gen3ClubPhaseTrade = 9, /* 0x1144 relink: trade animation */
    Gen3ClubPhaseSave = 10, /* the GBA's link save */
} Gen3ClubPhase;

typedef enum {
    Gen3ClubErrorNone = 0,
    Gen3ClubErrorChecksum = 1, /* recorded only: the phase goes on (the GBA may be fine) */
    Gen3ClubErrorBadPlayerBlock = 2,
    Gen3ClubErrorOtherRoom = 3,
    Gen3ClubErrorUnexpected = 4, /* a link type the club didn't expect at this point */
} Gen3ClubError;

/* The arbiter's side of the trade menu (§B3.3, D4), per link session. */
typedef enum {
    Gen3ClubMenuIdle, /* no GBA request pending */
    Gen3ClubMenuOffered, /* DDDD sent: the GBA is confirming */
    Gen3ClubMenuStarting, /* CCDD sent */
    Gen3ClubMenuLeaving, /* EEBB sent */
} Gen3ClubMenu;

/* How the latest pending trade ended (D5, §B5.3), by the GBA 2FFEs the club had answered. */
typedef enum {
    Gen3TradeResultNone,
    Gen3TradeResultDone, /* sync 3 seen */
    Gen3TradeResultCanceled, /* lost before the club answered sync 1: nothing written on the GBA */
    Gen3TradeResultRolledBack, /* lost after the sync-1 answer, before the sync-2 answer: the GBA
                                  reports a corrupt save and loads the one from before the trade */
    Gen3TradeResultAssumed, /* lost after the sync-2 answer, before sync 3: taken as done (D5) */
} Gen3TradeResult;

/* name must outlive the club (a string literal): the club keeps a copy of the config. */
typedef struct {
    const char* name; /* up to 7 chars are sent */
    uint16_t tid;
    uint16_t sid;
    uint8_t gender; /* 0 male, anything else is sent as 1 */
    bool trading; /* trade session; false (Link Test) never sits down */
} Gen3ClubConfig;

typedef struct {
    Gen3ClubPhase phase;
    Gen3ClubError last_error; /* last_error survives the reset to Waiting */
    bool partner_valid; /* the GBA's LinkPlayer of this session was accepted */
    Gen3LinkPlayer partner;
    uint32_t frames;
    uint32_t room_frames; /* frames spent in Room since init, over all sessions */
    Gen3LinkStats link;
    uint32_t link_type; /* the GBA's link type this session; 0 until its LinkPlayerBlock arrives */
    bool trainer_valid; /* trainer: the GBA's LinkPlayer from the last 0x1133 linkup, kept across */
    Gen3LinkPlayer trainer; /* disconnects and relinks */
    bool has_offer;
    bool gba_seated; /* this room session: the GBA sent CAFE 0016 and hasn't sent 001D/001A since */
    bool flipper_seated; /* this room session: the Flipper sent CAFE 0016 */
    uint32_t trades_done; /* finalized trades since init (Done or Assumed) */
    uint8_t round; /* Exchange/Menu: the round in progress or done, 1..5; 0 before round 1 */
    Gen3ClubMenu menu; /* per link session: Idle until the trade menu */
    uint8_t gba_slot; /* arg of the GBA's latest AABB that got DDDD: the GBA mon offered */
    /* Reason the latest AABB got DDEE; None after a DDDD, or for a DDEE because the GBA was shown
     * no Flipper Pokemon (R8) */
    Gen3Refuse refused;
    bool trade_pending; /* CCDD sent; until done or lost */
    uint8_t save_syncs; /* GBA 2FFEs answered in this link save, 0..3; zeroed at CCDD */
    bool save_window; /* sync 1 answered, sync 3 not yet seen: don't stop the link (D10) */
    Gen3TradeResult last_result; /* how the latest pending trade ended; None before any */
    /* Menu: the GBA's view of the Flipper's party is out of date (no usable offer, offer
     * changed, or a checksum error in this 0x1122 session), the app is editing, or a leave is
     * asked or done (leaving, left): it gets EEBB. After left, a menu reached by a trade's late
     * relink never starts another trade */
    bool offer_stale;
    /* Room via 0x1111: the GBA's avatar is on its seat; it must step off and back on. Until the
     * GBA's next CAFE 0016 or a 0x1133 linkup */
    bool resit_needed;
    bool editing; /* gen3_club_set_editing (O1) */
    bool leaving; /* requested, not done yet */
    bool left; /* the GBA walked out and closed the link after a request, or no GBA was linked */
    /* The GBA closed its link to open the next one at once (its 5FFF answered in Room for the
     * trade menu, in Menu, or in Save): from that answer until it connects again, or until
     * GEN3_CLUB_RELINK_WAIT_FRAMES of Waiting take it as gone. Waiting with relinking is no
     * "no GBA linked": a stop then is a link error on the GBA, mid-relink. */
    bool relinking;
    /* The rules of the GBA whose LinkPlayerBlock last parsed, of any link type: its game and,
     * for FRLG, whether that save has the National Dex. Kept across disconnects, relinks and bad
     * blocks, so after a cartridge swap they are the old GBA's until the new one's block parses;
     * {Unknown, true} from init until the first block. */
    Gen3PartnerRules partner_rules;
    /* The offer's gen3_offer_traits, as given with it (GEN3_OFFER_TRAITS_UNKNOWN through
     * gen3_club_set_offer), or from the AABB decrypt for a traded Pokemon; 0 without an offer */
    uint8_t offer_traits;
    /* has_offer, and partner_rules don't refuse it (gen3_partner_refuse None). The club sits, and
     * shows the offer at round 1, only while it is; an R/S, Emerald or unknown partner never
     * refuses, so there it is has_offer */
    bool offer_usable;
} Gen3ClubStatus;

typedef struct Gen3Club Gen3Club;

/* Fully defined so callers can embed it. Fields are private: use the API only. */
struct Gen3Club {
    Gen3Link link;
    Gen3BlockRx block_rx; /* the GBA's current block (its LinkPlayerBlock, then its card) */
    Gen3ClubConfig cfg;
    /* The Flipper's blocks for this session, from gen3_club_claim_game when the GBA's
     * LinkPlayerBlock arrives */
    uint8_t own_player_block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    uint8_t own_card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    Gen3ClubPhase phase;
    Gen3ClubError last_error;
    bool partner_valid;
    Gen3LinkPlayer partner;
    uint32_t link_type;
    bool trainer_valid; /* never reset after init */
    Gen3LinkPlayer trainer;
    bool card_requested; /* 0xCCCC and own card queued this session */
    bool close_answered; /* {5FFF} queued this session */
    bool exit_answer_due; /* the GBA sent CAFE 0017: answer after its next command */
    bool gba_exit_seen; /* the GBA sent CAFE 0017 this session */
    bool exit_sent; /* the Flipper's CAFE 0017 queued this session (an answer or D9's) */
    uint8_t room_cafes; /* the GBA's CAFEs this session in Room, up to UINT8_MAX */
    uint8_t card_delay; /* idle bursts to wait before 0xCCCC */
    uint8_t idle_bursts;
    uint32_t frames;
    uint32_t room_frames;
    /* Room seats, per room session (§B1.3, §B1.4) */
    bool gba_seated;
    bool flipper_seated;
    /* Party exchange, per session (§B2) */
    uint8_t round; /* 0 before round 1, then the round in progress or done */
    bool round_gba_done; /* the GBA's block of this round is in */
    uint8_t round1_delay; /* idle bursts before round 1's CCCC */
    uint8_t round_delay; /* before the CCCC of rounds 2..5 */
    /* gen3_club_set_round_delays was called: the GBA's game no longer picks the delays */
    bool delays_fixed;
    uint8_t round_block[GEN3_TRADE_ROUND_MAX]; /* the Flipper's block of this round (R2) */
    /* Trade menu, per session (§B3) */
    Gen3ClubMenu menu;
    bool reply_due; /* the GBA's block below awaits its one reply */
    uint8_t rx_gap; /* bursts since the latest command of the GBA's incoming block */
    uint16_t gba_code, gba_arg; /* that block's words 0 and 1 */
    uint8_t gba_slot;
    Gen3Refuse refused;
    /* R8: round 1 showed the GBA a Flipper Pokemon (party_mon had one); else its partner column
     * is empty */
    bool party_shown;
    bool tainted; /* a checksum error was counted in this link session (D4) */
    uint8_t eebb_bursts; /* Leaving: bursts since the latest EEBB went out, up to the retry */
    Gen3Pkm scratch; /* the GBA mon of the latest AABB, decrypted (R2) */
    /* Trade context: survives relinks (D3) */
    /* gen3_partner_rules of the GBA's latest parsed LinkPlayerBlock, any link type; {Unknown,
     * true} from init. Disconnects and bad blocks keep them */
    Gen3PartnerRules partner_rules;
    uint8_t offer[GEN3_PARTY_SIZE]; /* the Flipper's one party Pokemon, game form; zeros if none */
    bool has_offer;
    uint8_t offer_traits; /* the offer's gen3_offer_traits; 0 without one */
    /* gen3_offer_traits of the GBA's Pokemon its AABB got DDDD for: the offer's once traded */
    uint8_t pending_traits;
    uint32_t trades_done;
    uint8_t party_mon[GEN3_PARTY_SIZE]; /* the Flipper mon the GBA was shown: offer at round 1 */
    uint8_t gba_party[GEN3_CLUB_GBA_PARTY][GEN3_PARTY_SIZE]; /* game form, rounds 1-3 */
    bool trade_pending;
    uint8_t pending_mon[GEN3_PARTY_SIZE]; /* the GBA mon the pending trade takes, game form */
    uint8_t save_syncs; /* GBA 2FFEs answered in this link save; zeroed at CCDD */
    /* The sync-2 answer is queued, and no whole clean burst (all 9 transfers answered, no
     * checksum error) has come back since it left */
    bool sync2_unconfirmed;
    bool save_window;
    Gen3TradeResult last_result;
    uint8_t received[GEN3_PARTY_SIZE]; /* the latest finalized trade's Pokemon, game form */
    bool national_dex; /* lp_field_2 of the Flipper's 0x1144 block (§B4.5); never reset */
    /* The Flipper's EEBB sent the GBA toward the room: its next LinkPlayerBlock is 0x1111 (§B6.3,
     * D2). Consumed by that block, whatever its type */
    bool return_due;
    bool resit_needed;
    bool editing; /* O1: app state, like the offer */
    /* D9: app state, like the offer */
    bool leaving;
    bool left;
    /* The GBA closed the link to open the next one at once (closes_for_relink): from that
     * Disconnected until the next Connected, or GEN3_CLUB_RELINK_WAIT_FRAMES (relink_wait) */
    bool relink_due;
    uint16_t relink_wait;
};

/* The one place that decides which game the Flipper claims to be (D11). Fills the Flipper's
 * LinkPlayer for a session of link_type, and its trainer card, by the game of partner: the GBA's
 * LinkPlayer of this session, whatever its link type. NULL, or zeros after a bad block or a reset,
 * read as an Unknown game. Every claim has name cfg->name (7 chars, EOS-filled to 11), trainer_id
 * sid<<16|tid, gender 0/1, id 0 and language GEN3_LANGUAGE_ENGLISH.
 * - FireRed/LeafGreen: the GBA's own game, version GEN3_LP_VERSION_BASE |
 *   (partner->version & 0xFF), so FR meets FR and LG meets LG. lp_field_2 GEN3_LP_NATIONAL_DEX
 *   in every block (what FRLG itself sends; it never reads it), so national_dex is ignored.
 *   name[8..10] 01 00 01: progress GEN3_FRLG_PROGRESS_NATIONAL_DEX, which lets the FRLG player
 *   offer a Pokemon above #151 (fr: trade.c: CanTradeSelectedMon). Card
 *   gen3_trainer_card_build_frlg. An FRLG claim passes FRLG's linkup gate, and the GBA shows an
 *   FRLG avatar and the Kanto card (§C1, §C2).
 * - Ruby/Sapphire, Emerald and Unknown: Ruby, version GEN3_VERSION_RUBY, name[8..10] EOS, card
 *   gen3_trainer_card_build (§A5); lp_field_2 GEN3_LP_NATIONAL_DEX only for
 *   GEN3_LINKTYPE_TRADE_DISCONNECTED with national_dex set, else 0. An FRLG GBA would refuse
 *   this claim at the counter unless its save has FLAG_SYS_CAN_LINK_WITH_RS
 *   (fr: trade.c: GetGameProgressForLinkTrade).
 * An FRLG GBA in another room gets the FRLG claim too; the link types differ first, so nothing
 * follows from it. Pure: safe in the link ISR. */
void gen3_club_claim_game(
    const Gen3ClubConfig* cfg,
    const Gen3LinkPlayer* partner,
    uint32_t link_type,
    bool national_dex,
    Gen3LinkPlayer* lp,
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE]);

void gen3_club_init(Gen3Club* club, const Gen3ClubConfig* cfg);
/* One call per frame from the driver callback. rx is NULL on the first call. */
void gen3_club_frame(Gen3Club* club, const MlinkRx* rx, MlinkTx* tx);
void gen3_club_status(const Gen3Club* club, Gen3ClubStatus* out);
/* Sets (game != NULL) or clears the offer: the Flipper's one party Pokemon, game form, with its
 * gen3_offer_traits (ignored, and 0, when game is NULL). false and nothing changed if trades_seen
 * != the club's trades_done (D6): an app that hasn't picked up the latest trade can't overwrite
 * the received Pokemon. Unchecked: the app sets it in a PRIMASK section, where nothing may
 * decrypt, so it computes the traits beforehand and offers only a Pokemon that passes
 * gen3_trade_offer_check. Copies the 100 bytes and two scalars. The club sits, and shows the
 * offer at round 1, only while partner_rules don't refuse those traits (status.offer_usable). */
bool gen3_club_set_offer_traits(
    Gen3Club* club,
    const uint8_t* game,
    uint32_t trades_seen,
    uint8_t traits);
/* gen3_club_set_offer_traits with GEN3_OFFER_TRAITS_UNKNOWN: it decrypts nothing either. The offer
 * is then usable for an R/S, Emerald or unknown partner, and never for FRLG. */
bool gen3_club_set_offer(Gen3Club* club, const uint8_t* game, uint32_t trades_seen);
/* O1: the app is editing the offer. While set, offer_stale is true (EEBB to every GBA block in
 * Menu, and an unsolicited EEBB), and the club never sits, nor sends DDDD or CCDD. Setting it
 * fails (false, nothing changed) while trade_pending, or when trades_seen != trades_done (a
 * received Pokemon the app hasn't seen). Clearing it always succeeds (trades_seen unused). */
bool gen3_club_set_editing(Gen3Club* club, bool on, uint32_t trades_seen);
/* Ask the GBA to walk out (D9); safe in any phase. Done when status.left.
 * - No GBA linked (Waiting, and no relink due): left at once.
 * - Room: one CAFE 0017 once the GBA's CAFEs run (GEN3_CLUB_LEAVE_DELAY_CAFES), unless the GBA's
 *   own 0017 has its answer; the GBA's later 0017 gets none. The club no longer sits.
 * - Menu: the offer is stale, so the GBA gets EEBB and comes back to the room through 0x1111.
 * - Elsewhere, a pending trade and its link save included, the club goes on until Room or Menu.
 * Once the GBA's link is lost for good (Waiting with no relink due: its exit's close, a power-off
 * or a pulled cable), left is set and leaving cleared. left is sticky: from then on the club never
 * sits, so a GBA that walks back into the Cable Club can't start a trade. */
void gen3_club_request_leave(Gen3Club* club);
/* National Dex for the GBA (D10, §B4.5): while on, the Flipper's 0x1144 block carries lp_field_2
 * 0x8000. Off after init; the club never changes it. */
void gen3_club_set_national_dex(Gen3Club* club, bool on);
/* The Pokemon of the latest finalized trade (game form; zeros before any) and trades_done
 * (trades_done may be NULL). Copies 100 B and one counter. */
void gen3_club_get_received(
    const Gen3Club* club,
    uint8_t out[GEN3_PARTY_SIZE],
    uint32_t* trades_done);
/* Tests only; production uses GEN3_CLUB_CARD_DELAY_BURSTS. */
void gen3_club_set_card_delay(Gen3Club* club, uint8_t bursts);
/* Tests only. Production takes the delays from the GBA's game at its 0x1122 LinkPlayerBlock:
 * GEN3_CLUB_ROUND1_DELAY_BURSTS_FRLG and GEN3_CLUB_ROUND_DELAY_BURSTS_FRLG for FRLG, else
 * GEN3_CLUB_ROUND1_DELAY_BURSTS and GEN3_CLUB_ROUND_DELAY_BURSTS (also from init). A call fixes
 * both for every GBA until gen3_club_init. */
void gen3_club_set_round_delays(Gen3Club* club, uint8_t first, uint8_t later);
/* The GBA's party slot 0..5 (game form) from the last exchange; zeros for another slot or a slot
 * no exchange has filled. Each of rounds 1-3 refills two slots. */
void gen3_club_gba_mon(const Gen3Club* club, uint8_t slot, uint8_t out[GEN3_PARTY_SIZE]);
