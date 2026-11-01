#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>

/* Multiplayer link layer, master side (mirrors pokeruby link.c). Pure C, no heap. */

#define GEN3_CMD_LEN          8
#define GEN3_MASTER_HANDSHAKE 0x8FFF
#define GEN3_SLAVE_HANDSHAKE  0xB9A0
#define GEN3_LINK_QUEUE_LEN   24

typedef enum {
    Gen3LinkEventNone,
    Gen3LinkEventConnected,
    Gen3LinkEventCommand,
    Gen3LinkEventChecksumError,
    Gen3LinkEventDisconnected,
} Gen3LinkEvent;

typedef struct {
    uint32_t bursts, checksum_errors, timeouts, framing_errors;
} Gen3LinkStats;

/* Fully defined so callers can embed it. Fields are private: use the API only. */
typedef struct Gen3Link {
    bool connected;
    uint8_t prev_count; /* handshake: previous evaluated player count */
    uint8_t last_count; /* handshake: count of the transfer just evaluated */
    uint16_t hs_sent; /* handshake: word F sent in the last tx */
    bool checksum_valid;
    uint16_t checksum; /* expected word 0 of the next burst */
    uint16_t sent[GEN3_CMD_LEN]; /* command words sent in the last burst tx */
    uint16_t queue[GEN3_LINK_QUEUE_LEN][GEN3_CMD_LEN];
    uint8_t q_head, q_len;
    Gen3LinkStats stats;
} Gen3Link;

void gen3_link_init(Gen3Link* link);
bool gen3_link_connected(const Gen3Link* link);
/* Process the previous frame's result. On Gen3LinkEventCommand, cmd_out holds the GBA's 8 words.
 * On Gen3LinkEventChecksumError it holds them too, all zero if the GBA sent no command: the
 * failed check is about the previous burst, so this burst's command still counts. */
Gen3LinkEvent gen3_link_rx(Gen3Link* link, const MlinkRx* rx, uint16_t cmd_out[GEN3_CMD_LEN]);
/* Build this frame's transfers. Call exactly once per frame, after gen3_link_rx (or right after init). */
void gen3_link_tx(Gen3Link* link, MlinkTx* tx);
bool gen3_link_send(Gen3Link* link, const uint16_t cmd[GEN3_CMD_LEN]); /* false if queue full */
size_t gen3_link_queued(const Gen3Link* link);
void gen3_link_flush(Gen3Link* link); /* drop every queued command */
void gen3_link_stats(const Gen3Link* link, Gen3LinkStats* out);
