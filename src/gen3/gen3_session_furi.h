#pragma once

#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/mlink_hw_furi.h>

/* One Gen III link session, for the Link Test screen or a trade: the multiplayer-parent driver,
 * the Cable Club state machine and the SD-card frame log, wired together.
 *
 * The driver's frame callback (TIM2 ISR) runs the club, feeds the log ring and publishes the
 * club status. A "Gen3Log" thread drains the ring to APP_DATA_PATH("gen3_link.log") every 100 ms.
 * Every function here is an app-context call: use them from one thread, never from an ISR. The
 * app never touches the club itself: each hand-off call below runs its club call with every
 * interrupt masked (PRIMASK), copying at most 104 bytes and calling nothing that blocks. */

#define GEN3_TEST_TRAINER_NAME "FLIPPER"
#define GEN3_TEST_TRAINER_TID  22222
#define GEN3_TEST_TRAINER_SID  0

typedef struct Gen3Session Gen3Session;

/* Takes the SC/SD/SO/SI pins from the gblink handle (PIN_CLK, PIN_SD, PIN_SEROUT, PIN_SERIN).
 * The pins must not change while the session exists. */
Gen3Session* gen3_session_alloc(void* gblink_handle);

/* Stops the session first if it is running. */
void gen3_session_free(Gen3Session* s);

MlinkCable gen3_session_check_cable(Gen3Session* s);

/* Starts a fresh Link Test run: club as GEN3_TEST_TRAINER_* with trading off, empty log ring,
 * status reset to Waiting, then the driver; once that is running, opens the log file
 * (FSOM_CREATE_ALWAYS) and starts the log thread. false (nothing started) if the cable is
 * reversed or TIM2 is busy. A missing SD card is not an error: the link runs and log_ok reads
 * false. Already running: true for a Link Test run, false (nothing changed) for a trade session. */
bool gen3_session_start(Gen3Session* s); /* false if cable reversed or TIM2 busy */

/* As gen3_session_start, but the club runs with trading on (GEN3_TEST_TRAINER_* identity, D7) and
 * the National Dex flag set before the driver starts. No offer yet: the app tick publishes one.
 * Already running: true for a trade session, false (nothing changed) for a Link Test run, so a
 * Link Test session never passes for a trade session. */
bool gen3_session_start_trade(Gen3Session* s, bool national_dex);

/* Started and not stopped since. false for a NULL session. */
bool gen3_session_running(Gen3Session* s);
/* Running, and started by gen3_session_start_trade. false for a NULL session. */
bool gen3_session_trading(Gen3Session* s);

/* Stops the driver, lets the log thread drain the ring and exit, then closes the log file.
 * Does nothing if not running. The last status stays readable. */
void gen3_session_stop(Gen3Session* s);

/* Latest status published by the frame callback. log_ok (may be NULL): the log file is open and
 * no write to it has failed. cable_ok (may be NULL): the driver's live cable guard
 * (mlink_hw_cable_ok); while it reads false the driver skips every frame's transfers. */
void gen3_session_status(Gen3Session* s, Gen3ClubStatus* out, bool* log_ok, bool* cable_ok);

/* ---- App <-> club hand-off: each runs its gen3_club_* call inside one PRIMASK section. The
 * status shows the change from the next frame on. ---- */

/* gen3_club_set_offer_traits: offer is the Flipper's one party Pokemon (game form,
 * GEN3_PARTY_SIZE bytes), or NULL for no offer; traits are its gen3_offer_traits (ignored without
 * an offer). The section copies the 100-byte offer, has_offer and the traits byte, and decrypts
 * nothing, so both come from the app thread: publish only a Pokemon that passes
 * gen3_trade_offer_check, with the traits of its decrypted form (gen3_trade_sync_poll does both).
 * The club sits and shows it only while the GBA's rules take those traits. false and nothing
 * changed if trades_seen isn't the club's trades_done (D6), or if the session isn't running (the
 * next start's club wouldn't have it). */
bool gen3_session_set_offer_traits(
    Gen3Session* s,
    const uint8_t* offer,
    uint32_t trades_seen,
    uint8_t traits);
/* gen3_club_get_received: the latest finalized trade's Pokemon (game form; zeros before any) and
 * trades_done (may be NULL). After a stop: the stopped run's; before the first start: zeros, 0. */
void gen3_session_get_received(Gen3Session* s, uint8_t out[GEN3_PARTY_SIZE], uint32_t* trades_done);
/* gen3_club_set_editing (O1): setting it fails (false, nothing changed) while a trade is pending
 * or when trades_seen isn't the club's trades_done; clearing it always succeeds. Not running:
 * true, and nothing done. */
bool gen3_session_set_editing(Gen3Session* s, bool on, uint32_t trades_seen);
/* gen3_club_set_national_dex (D10). Any time: each start sets it again, from
 * gen3_session_start_trade's argument (off for gen3_session_start). */
void gen3_session_set_national_dex(Gen3Session* s, bool on);
/* gen3_club_request_leave (D9): done once status.left. left is sticky, and can come while
 * trade_pending (a 0x1144 relink that never came): it never means "trade resolved". Does nothing
 * unless running. */
void gen3_session_request_leave(Gen3Session* s);
