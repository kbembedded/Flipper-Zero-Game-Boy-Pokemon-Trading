# Gen III link protocol: Ruby/Sapphire Cable Club, Flipper as SIO master

Reference for the Flipper side of a Pokemon Ruby/Sapphire link-cable session.
The Flipper is the multiplayer **parent** (slot 0, "master"); the GBA running
Ruby is the **child** (slot 1, "slave"). Part A (cable plugged in → both
players idling in the Trade Center) is specified in full; Part B (trade) is an
outline; Part C lists FireRed differences.

## Sources and conventions

| Repo (read-only clone) | Commit |
|---|---|
| pret/pokeruby | `5784633c` (2026-09-28) |
| pret/pokeemerald | `731ad5bf` (2026-10-01) |
| pret/pokefirered | `ce6406d8` (2026-10-20) |

- Citations are `file: Function` in **pokeruby** unless prefixed `em:`
  (pokeemerald) or `fr:` (pokefirered). Ruby's `sub_XXXXXXX` names are given
  with the pokeemerald name of the same code in `[brackets]`; the full map is in
  the appendix.
- Facts not taken from pret code are marked **(GBATEK)**. Inferences are marked
  **(inferred)** and collected under [Uncertain](#uncertain).
- Word = `u16`. Slot *i* = `SIOMULTIi` = player *i*'s word for one transfer.
  F = Flipper (slot 0), G = GBA (slot 1). A **burst** = the 9 transfers a
  connected master runs once per frame. "Frame" = one GBA video frame.
- English Ruby only. The `#if GERMAN` branches in `link.c`/`cable_club.c`
  differ (0xCCCC handling, 0x2255 count check) and are out of scope.

---

# Part A: cable plugged in → idling in the Trade Center

## A1. Constants

### Link layer (`include/link.h`)

| Name | Value | Meaning |
|---|---|---|
| `MAX_LINK_PLAYERS` | 4 | |
| `CMD_LENGTH` | 8 | words per command (one command per burst) |
| `QUEUE_CAPACITY` | 50 | commands in the send queue and in the receive queue |
| `BLOCK_BUFFER_SIZE` | 0x100 | bytes; a received block larger than this goes to `gSharedMem` instead of `gBlockRecvBuffer` (`link.c: ProcessRecvCmds` case 0x8888) |
| `MASTER_HANDSHAKE` | 0x8FFF | |
| `SLAVE_HANDSHAKE` | 0xB9A0 | matched as `(w & ~3) == 0xB9A0` (`link.c: DoHandshake`) |
| link states | `START0`=0, `START1`=1, `HANDSHAKE`=2, `INIT_TIMER`=3, `CONN_ESTABLISHED`=4 | |

### Serial and timer setup (`src/link.c`)

| Item | Value | Where |
|---|---|---|
| `REG_RCNT` | 0 | `EnableSerial` |
| `REG_SIOCNT` | `SIO_MULTI_MODE` 0x2000 \| `SIO_INTR_ENABLE` 0x4000 \| `SIO_115200_BPS` 3 = **0x6003** | `EnableSerial` |
| `REG_SIOMLT_SEND` after enable | 0x0000 | `EnableSerial` |
| Serial off | `REG_SIOCNT = 0`, serial and timer 3 IRQs disabled | `DisableSerial` |
| Timer 3 reload | 65339 (= 0x10000 − 197) | `InitTimer`, `StopTimer` |
| Timer 3 control | `TIMER_INTR_ENABLE` 0x40 \| `TIMER_64CLK` 0x01; `TIMER_ENABLE` 0x80 set per transfer in `SendRecvDone` | `InitTimer` |
| Gap between transfers in a burst | 197 × 64 = 12 608 cycles = **751.5 µs** at 2^24 Hz | derived from the above |

pokefirered and pokeemerald use the same values (`fr:/em: src/link.c: InitTimer`, `REG_TM3CNT_L = -197`).

### Link command words (word 0 of a command)

Receiver action is `link.c: ProcessRecvCmds`; builder is `link.c: BuildSendCmd`.
A command whose word 0 is 0 is ignored by the receiver.

| Word | [Emerald name] | Sent by | Words 1..7 | Receiver action | Part A |
|---|---|---|---|---|---|
| 0x2222 | `LINKCMD_SEND_LINK_TYPE` | master only (`LinkCB_RequestPlayerDataExchange`) | [1] = `gLinkType` (not read by receiver) | every player builds its `LinkPlayerBlock` and starts a 60-byte block send (`InitLocalLinkPlayer`, `InitBlockSend`) | **send once** |
| 0x2FFE | `LINKCMD_READY_EXIT_STANDBY` | any (`sub_80084C8` [`LinkCB_Standby`]) | — | `u8_array_3002B70[i] = 1` [`gReadyToExitStandby`] | **answer** |
| 0x4444 | `LINKCMD_BLENDER_SEND_KEYS` | Berry Blender only (`sub_8007E04`) | [1] = `gMain.heldKeys` | `word_3002910[i] = [1]` | not used in Trade Center |
| 0x5555, 0x5566 | `LINKCMD_DUMMY_1/2` | — | — | `byte_3002A68 = 1` (never read) | no |
| 0x5FFF | `LINKCMD_READY_CLOSE_LINK` | any (`sub_8008350` [`LinkCB_ReadyCloseLink`]) | — | `u8_array_3002B78[i] = 1` [`gReadyToCloseLink`] | **always answer** |
| 0x6666 | `LINKCMD_SEND_EMPTY` | — | [1] = 0 | none | no |
| 0x7777 | `LINKCMD_SEND_0xEE` | — | [1..5] = 0xEE | none | no |
| 0x8888 | `LINKCMD_CONT_BLOCK` | block sender | 7 payload words | append to block ([A4](#a4-block-transfer)) | yes |
| 0xAAAA | `LINKCMD_BLENDER_NO_PBLOCK_SPACE` | — | — | `sub_8007E24`: receiver's `gLinkCallback` becomes Berry Blender key sending | **never send** |
| 0xAAAB | `LINKCMD_SEND_ITEM` | — | [1] = item | `Blender_SetBankBerryData` | never send |
| 0xBBBB | `LINKCMD_INIT_BLOCK` | block sender | [1] = size in bytes, [2] = 0x80 + sender's multiplayer id | start block receive | yes |
| 0xCAFE | `LINKCMD_SEND_HELD_KEYS` | every frame in link rooms (`sub_8007B44` [`LinkCB_SendHeldKeys`]) | [1] = key code 0x11..0x1D ([A7](#a7-in-the-trade-center)) | `word_3002910[i] = [1]` [`gLinkPartnersHeldKeys`] | yes |
| 0xCCCC | `LINKCMD_SEND_BLOCK_REQ` | master (`sub_8007E9C` [`SendBlockRequest`]) | [1] = request type 0..4 | every player calls `SendBlock(gBlockSendBuffer, size[type])` | **send once** |
| 0xEFFF | `LINKCMD_NONE` | — | — | filler in `InitLink`/`ResetSendBuffer`; never reaches the wire because `LinkMain2` zeroes `gSendCmd` each frame | no |

Block request sizes (`link.c: sBlockRequestLookupTable`, all from `gBlockSendBuffer`):
type 0 → 200, 1 → 200, 2 → 100, 3 → 220, 4 → 40 bytes. Never send a type ≥ 5
(the receiver indexes the table without a bounds check).

Words 0xAABB, 0xBBBB, 0xBBCC, 0xEEAA, 0xDDDD, 0xEEBB, 0xEECC, 0xCCDD, 0xDDEE,
0xABCD, 0xDCBA are **not** link commands: they are word 0 of 20-byte trade
blocks (Part B).

### Link types (`gLinkType`)

| Value | [Emerald] | Set by (Ruby) | When |
|---|---|---|---|
| **0x1133** | `LINKTYPE_TRADE_SETUP` | `cable_club.c: sub_80834E4` [`TryTradeLinkup`] | Trade Center counter linkup → room (**Part A**) |
| 0x1111 | `LINKTYPE_TRADE` | `cable_club.c: sub_8083664` [`CreateTask_ReestablishCableClubLink`] with `gSpecialVar_0x8004 == 3` | re-entering the room after the trade menu |
| 0x1122 | `LINKTYPE_TRADE_CONNECTING` | `trade.c: sub_8047EC0` state 1 | trade menu |
| 0x1144 | `LINKTYPE_TRADE_DISCONNECTED` | `trade.c: sub_804B41C` state 0 | trade animation and link save |

Not Trade Center (for rejecting other selections): 0x2211 battle; 0x2233 /
0x2244 / 0x2255 single / double / multi (`sub_808347C`); 0x3311 record corner
(`sub_808350C`); 0x3322 (`sub_8083664` case 4); 0x4411 blender
(`sub_8083614`); 0x6601 contest (`sub_808363C`); 0x1111 also used by
`LinkTestScreen`.

### `gLinkStatus` bits (`include/link.h`, built in `link.c: LinkMain1`)

| Bits | Mask | Meaning |
|---|---|---|
| 0–1 | 0x3 | local multiplayer id (`gLink.localId`, from `SIOCNT` id bits at each serial IRQ) |
| 2–4 | 0x1C | player count (`gLink.playerCount`, set by `DoHandshake`) |
| 5 | 0x20 | this GBA is master (`gLink.isMaster == 8`) |
| 6 | 0x40 | connection established (state == `CONN_ESTABLISHED`) |
| 8 | 0x100 | received nothing this frame (receive queue was empty) |
| 9–11 | 0xE00 | `link_field_F` = `(minRecv & 3) + 1` from the handshake; no effect |
| 12 | 0x1000 | **error**: hardware (`SIOCNT` bit 6 at the latest serial IRQ) |
| 13 | 0x2000 | **error**: bad checksum (sticky) |
| 14 | 0x4000 | **error**: send queue full (sticky) |
| 15 | 0x8000 | **error**: receive queue full (sticky) |
| 16 | 0x10000 | **error**: `LAG_MASTER` (GBA master only; sticky) |
| 17 | 0x20000 | **error**: `localId > 3` (cannot happen) |
| 18 | 0x40000 | **error**: `LAG_SLAVE` (sticky) |

`LINK_STAT_ERRORS` = 0x7F000 (bits 12–18). Sticky flags are only cleared by
`EnableSerial`/`DisableSerial` (`CpuFill32(0, &gLink, …)`).

## A2. Per-frame transfer schedule

### Who starts transfers

- GBA master: `main.c: VBlankIntr` → `link.c: LinkVSync`. In `HANDSHAKE`: one
  `StartTransfer` per VBlank. In `CONN_ESTABLISHED`: `StartTransfer` for
  transfer #1; each completion IRQ (`SerialCB` → `SendRecvDone`) starts timer 3;
  its overflow (`Timer3Intr` → `StopTimer`, `StartTransfer`) starts the next.
  After transfer #9, `SendRecvDone` resets the indexes and does not restart the
  timer, so the burst ends until the next VBlank.
- GBA child: never starts transfers. Its `SerialCB` runs once per transfer.
- **Flipper (master):** replaces VBlank with its own frame tick. Run exactly 9
  transfers per tick, ≈751.5 µs between the end of one transfer and the start
  of the next (match the GBA; see Uncertain U2).

### Burst layout (both sides symmetric)

The word on the wire in transfer *t* is what the ISR wrote to `SIOMLT_SEND`
after transfer *t−1* (`link.c: DoSend`).

| Transfer | Each side sends | Child ISR (`SerialCB`: `DoRecv`, `DoSend`, `SendRecvDone`) |
|---|---|---|
| #1 | checksum of the **previous** burst | `DoRecv` with `sendCmdIndex == 0`: if `sChecksumAvailable`, compare slot *i* to own checksum for every *i* < `playerCount`; then `checksum = 0`, `sChecksumAvailable = TRUE`. `DoSend`: load word 0 of the head of the send queue, or 0 if the queue is empty for this whole burst |
| #2…#9 | command words 0…7 | `DoRecv`: store slot words into the receive queue entry, `checksum += word` per slot. After #9 the entry is committed **only if any word of any slot was non-zero** (`sRecvNonzeroCheck`) |
| after #9 | | `DoSend` writes `checksum` (sent in next #1) and dequeues the command it just sent; `SendRecvDone` resets `sendCmdIndex`/`recvCmdIndex` to 0 |

- One command per burst per player. A side with nothing queued sends 8 zeros.
- The send queue only stores commands with at least one non-zero word
  (`link.c: EnqueueSendCmd`, `sSendNonzeroCheck`).
- A command built in frame *N*'s `LinkMain2` is enqueued by frame *N+1*'s
  `LinkMain1` (`main.c: UpdateLinkAndCallCallbacks` calls `LinkMain1` then
  `LinkMain2`). The child therefore answers a command 1–2 bursts after it
  arrives.

### Checksum (the 9th word)

`checksum = Σ_{transfer #2..#9} Σ_{slot i < playerCount} word  (mod 0x10000)`
(`link.c: DoRecv`, `gLink.checksum` is `u16`). With F + G it is the sum of F's
8 words and G's 8 words of that burst. Both sides compute the same value from
the same shared data. Words of slots 2–3 are not included.

- No check on the first burst after the handshake (`sChecksumAvailable` is
  FALSE after `EnableSerial`). G's word in that transfer #1 is the stale 0xB9A0.
- Example: F = `2222 1133 0000×6`, G = `0000×8` → next transfer #1 carries 0x3355.
- If G's receive queue is full the words are not summed, so a full queue also
  shows up as a checksum mismatch.

### Timing numbers

| Quantity | Value | Source |
|---|---|---|
| GBA frame | 280 896 cycles = 16.7427 ms = 59.7275 Hz | (GBATEK) |
| Gap inside a burst | 751.5 µs + ISR latency | `link.c: InitTimer`, `SendRecvDone` |
| One 2-player transfer | 2 × 18 bits (start, 16 data, stop) at 115 200 bps ≈ 0.31 ms + hand-over delay + timeout | (GBATEK) |
| Burst length | ≈ 8 × 0.75 + 9 × ≈0.35 ≈ 9–10 ms | derived |
| Child's silence limit | error when 11 consecutive VBlanks pass without a serial IRQ (≈ 167–184 ms) | `link.c: LinkVSync` |

### What the child checks (all `src/link.c`)

| Check | Code | Effect |
|---|---|---|
| Framing = serial IRQ count mod 9 | `sendCmdIndex`/`recvCmdIndex` in `DoRecv`/`DoSend`/`SendRecvDone` | no direct check; a lost or extra transfer shifts the framing for good, so the next transfer #1 fails the checksum |
| VBlanks without a serial IRQ | `LinkVSync`: `++sNumVBlanksWithoutSerialIntr` per VBlank, reset to 0 in `SerialCB`; `> 10` → | in `CONN_ESTABLISHED`: `gLink.lag = LAG_SLAVE` (0x40000, sticky). In `HANDSHAKE`: `playerCount = 0` |
| Checksum | `DoRecv` | `badChecksum` (0x2000, sticky) |
| Hardware error bit | `SerialCB`: `hardwareError = SIO_MULTI_CNT->error` every IRQ | 0x1000 if set when `LinkMain1` samples it |
| Receive queue (50) | `DoRecv` | 0x8000 |
| Send queue (50) | `EnqueueSendCmd` | 0x4000 |

The child does **not** check: transfers per frame, burst phase relative to its
own VBlank, or the gap between transfers. (The GBA master's own check,
`LinkVSync`: fewer than 9 IRQs since the last VBlank → `LAG_MASTER`, does not
exist on the child.)

### When an error becomes a link-error screen

`link.c: LinkMain2` → `CheckErrorStatus`, evaluated only while
`CONN_ESTABLISHED` and `gLinkOpen`: if `gLinkStatus & 0x7F000`, then
- if `!gSuppressLinkErrorMessage`: `SetMainCallback2(CB2_LinkError)` (error
  screen; only a reset clears it);
- always: `gLinkErrorOccurred = TRUE`, `CloseLink()`.

`gSuppressLinkErrorMessage` during the counter linkup: FALSE at `OpenLink`;
TRUE from the first frame `sub_8082F68` [`Task_LinkupAwaitConnection`] sees a
connection (`sub_8082E6C` [`CheckLinkCanceled`]); FALSE again in
`sub_8083314` [`Task_LinkupAwaitTrainerCardData`] after the trainer cards.
While suppressed, an error closes the link and the script prints "Sorry, we
have a transmission error..." (result 6) instead of the error screen. **Errors
in the first frames after the handshake, and anywhere after the trainer
cards, go straight to the error screen.**

Other hard errors:

| Trigger | Where |
|---|---|
| `LinkPlayerBlock` magic mismatch | `link.c: ProcessRecvCmds` case 0x8888 → `CB2_LinkError` (not suppressible) |
| 300 frames after `OpenLink` without `gReceivedRemoteLinkPlayers` | `cable_club.c: sub_8083C50` [`Task_WaitForLinkPlayerConnection`]; created only by the room re-establish (`sub_8083710`) and the trade menu (`trade.c: sub_8047EC0` state 1) — Part B |
| Partner's `trainerId` or `name` differ from the saved copy on re-establish | `link.c: sub_800826C` [`CheckLinkPlayersMatchSaved`] — Part B |

### Wire level (GBATEK, not pret)

- Idle: SC high, the line arriving at the GBA's **SI** high, SD pulled high
  once every unit is in multiplayer mode.
- Transfer: master pulls SC low; master sends start bit (0), 16 data bits LSB
  first, stop bit (1) on SD at 115 200 bps; master pulls its SO low (→ GBA SI),
  GBA sends start/16/stop on SD; master releases SC high and SO high. Every
  unit latches all slots into `SIOMULTI0–3` and raises its serial IRQ.
- Slots of absent or non-responding units read 0xFFFF. The Flipper's driver
  must report 0xFFFF for slot 1 when G does not answer.
- GBA error bit (`SIOCNT` bit 6): set when a child saw SC low but never got SI
  low, or when a stop bit was not high.
- **SI must idle high.** `link.c: CheckMasterOrSlave` (every frame in
  `HANDSHAKE`) makes the GBA master if `SI` is low, `SD` high and
  `localId == 0`. If the Flipper holds the GBA's SI low when idle, Ruby will
  try to act as master.

## A3. Handshake

### Algorithm (`link.c: DoHandshake`, run in the ISR of every transfer while in `HANDSHAKE`)

1. Write `SIOMLT_SEND` for the **next** transfer: `MASTER_HANDSHAKE` if
   `handshakeAsMaster`, else `SLAVE_HANDSHAKE`; then clear `handshakeAsMaster`.
2. Count players over slots 0..3 of this transfer: if `(w & ~3) == 0xB9A0` or
   `w == 0x8FFF` → count++; otherwise, if `w != 0xFFFF` set count = 0; stop at
   the first slot that is not a handshake word.
3. Success iff `count > 1` **and** `count ==` previous count
   (`sHandshakePlayerCount`) **and** slot 0 == 0x8FFF.
4. Save count as the previous count; `gLink.playerCount = count`.

On success (`link.c: SerialCB`): the GBA master goes to `INIT_TIMER` (bursts
start at the next VBlank); the child goes to `CONN_ESTABLISHED` immediately,
with `sendCmdIndex = recvCmdIndex = 0`, so the very next transfer is burst #1's
checksum slot.

### Child timeline after `OpenLink` (`link.c: OpenLink`, `LinkMain1`, `Task_TriggerHandshake`)

| Child frame | Event |
|---|---|
| 0 | `OpenLink`: `ResetSerial`, `InitLink` (`EnableSerial`), `gLinkCallback = LinkCB_RequestPlayerDataExchange`, `gLinkPlayerPending[0..3] = TRUE`, creates `Task_TriggerHandshake` |
| 1 | `LinkMain1` `START0`: `DisableSerial` (`SIOCNT = 0`; child deaf) → `START1` |
| ≈5–6 | `Task_TriggerHandshake` 5th run sets `gShouldAdvanceLinkState = 1` → `EnableSerial` (`SIOMLT_SEND = 0`) → `HANDSHAKE` |
| then | `CheckMasterOrSlave` each frame; `DoHandshake` each transfer |

The child needs no `gShouldAdvanceLinkState` to complete the handshake; only
the master's flag matters (`LinkMain1` `HANDSHAKE` case 1).

### Word by word (recommended Flipper sequence)

| Transfer | F sends | G sends | Count seen by both | Result |
|---|---|---|---|---|
| … (G link closed) | 0xB9A0 | — (0xFFFF) | 1 | Flipper idles here, **1 transfer per frame** |
| *n* | 0xB9A0 | 0x0000 | 0 | G's first transfer after `EnableSerial` always sends 0 |
| *n*+1 | 0xB9A0 | 0xB9A0 | 2 | previous 0 → no; Flipper now switches to 0x8FFF |
| *n*+2 | 0x8FFF | 0xB9A0 | 2 | previous 2 and slot 0 = 0x8FFF → **success on both** |
| next frame | burst #1, transfer #1 (any word; not checked) | 0xB9A0 (stale) | — | connected |

- This mirrors a GBA master, which sends 0xB9A0 until its player confirms and
  then 0x8FFF (`LinkMain1` case 1 requires `isMaster == 8 && playerCount > 1`).
  Sending 0x8FFF from the start also succeeds after three transfers
  (inferred, same algorithm).
- The Flipper must run steps 2–4 on the same four words (its own slot 0, G's
  slot 1, 0xFFFF for slots 2–3) to know the exact transfer at which G switches
  to `CONN_ESTABLISHED`, and start bursting on the next frame.
- G's 0x0000 on its first transfer resets both counts to 0, which keeps the
  Flipper's "previous count" in step with G's even though G was deaf earlier.
- Player count = consecutive handshake slots ending at the first 0xFFFF;
  F + G = 2. It is exported in status bits 2–4 (`GetLinkPlayerCount[_2]`) and
  is what the Cable Club tasks compare against.
- If the handshake stops (no transfer for > 10 frames in `HANDSHAKE`), G sets
  `playerCount = 0` (`LinkVSync`).

## A4. Block transfer

Sender: `link.c: InitBlockSend`, `LinkCB_BlockSendBegin`, `LinkCB_BlockSend`,
`LinkCB_BlockSendEnd`. Receiver: `link.c: ProcessRecvCmds` cases 0xBBBB, 0x8888.

| Command | Word 0 | Word 1 | Word 2 | Words 3–7 |
|---|---|---|---|---|
| header | 0xBBBB | size in bytes | 0x80 + sender multiplayer id (F: 0x80, G: 0x81) | 0 |
| chunk | 0x8888 | payload words w0…w6 | | |

- Chunk payload: 7 words = 14 bytes, little-endian
  (`word = buf[off] | buf[off+1] << 8`), `off = pos + 2*i`; `pos += 14`
  per chunk. Chunks = ceil(size / 14); bytes past `size` in the last chunk are
  don't-care.
- Receiver: 0xBBBB sets `pos = 0`, `size = [1]`, `multiplayerId = [2]` (stored,
  never used). Each 0x8888 writes 7 words at `pos/2` and adds 14. Complete when
  `pos >= size`. There is **no acknowledgement**; completion is only visible
  to the sender through the receiver's later behaviour.
- **First block from each slot after `OpenLink` is always parsed as a
  `LinkPlayerBlock`** (`gLinkPlayerPending[i] == TRUE`). Later blocks only set
  `gBlockReceived[i]` (`SetBlockReceivedFlag`), read with
  `GetBlockReceivedStatus` (bit *i* = slot *i*).
- Each slot is independent; both sides may send blocks at the same time. Every
  player also receives its **own** block back through its own slot, and Ruby
  waits for that echo (`HandleReceiveRemoteLinkPlayer`, `GetBlockReceivedStatus`).
- A new `InitBlockSend` is refused while `sBlockSend.active` (the previous
  block's last chunk not yet built). `sub_8007E9C` (0xCCCC) is refused while
  `gLinkCallback != NULL`.
- G's pacing: header built in frame *N*; `LinkCB_BlockSendBegin` waits until
  its counter exceeds 2, so chunk 1 is built in frame *N*+3, then one chunk per
  frame; `gLinkCallback` returns to NULL one frame after the last chunk. On the
  wire: header, two idle bursts, chunks. The receiver does not need the idle
  bursts; the Flipper may send header and chunks back to back (inferred).
- **Send each 0x2222 / 0xCCCC / block exactly once.** A duplicate 0x2222 makes
  every player send a second `LinkPlayerBlock`, which arrives as an ordinary
  block and sets a stray `gBlockReceived` flag; the trainer-card step then
  copies the wrong buffer.

| Block | Size | Chunks | Part |
|---|---|---|---|
| `LinkPlayerBlock` | 60 (0x3C) | 5 | A |
| trainer card (request type 2) | 100 (0x64) | 8 | A |
| party pair (type 1) | 200 | 15 | B |
| mail (type 3) | 220 | 16 | B |
| gift ribbons (type 4) | 40 | 3 | B |
| trade menu / trade end message | 20 | 2 | B |

## A5. `struct LinkPlayer` and `struct LinkPlayerBlock`

### Layout (`include/link.h`; filled by `link.c: InitLocalLinkPlayer`)

`LinkPlayerBlock` is 60 bytes: `magic1[16]` @0x00, `LinkPlayer` (28 bytes)
@0x10, `magic2[16]` @0x2C.

| Block offset | LP offset | Field | Size | Ruby fills with | Flipper sends |
|---|---|---|---|---|---|
| 0x00 | — | `magic1` | 16 | `memcpy(…, sMagic, 15)` = ASCII `"GameFreak inc."` + 0x00; byte 15 untouched (0, BSS) | `47 61 6D 65 46 72 65 61 6B 20 69 6E 63 2E 00 00` |
| 0x10 | 0x00 | `version` | u16 | `gGameVersion + 0x4000`: Sapphire 0x4001, Ruby 0x4002 (`constants/global.h`) | 0x4002 (Ruby never reads it; FRLG/Emerald do) |
| 0x12 | 0x02 | `lp_field_2` | u16 | 0 | 0x0000; 0x8000 only if the user opts into National Dex unlock (`trade.c: sub_804BA64`, Part B) |
| 0x14 | 0x04 | `trainerId` | u32 | `id[0] \| id[1]<<8 \| id[2]<<16 \| id[3]<<24`: low 16 = TID, high 16 = SID | Flipper TID/SID; **constant for the whole session** |
| 0x18 | 0x08 | `name` | 11 | 11 bytes copied from `gSaveBlock2.playerName[8]`, reading past it (bytes 8–10 = `playerGender`, `specialSaveWarp`, `playerTrainerId[0]`) | ≤ 7 Gen III chars (e.g. A=0xBB, a=0xD5, 0=0xA1, space=0x00), then 0xFF (EOS) to fill; constant for the session |
| 0x23 | 0x13 | `gender` | u8 | `playerGender`: 0 male, 1 female | 0 or 1 only |
| 0x24 | 0x14 | `linkType` | u32 | `gLinkType` zero-extended | **0x00001133** for Part A |
| 0x28 | 0x18 | `id` | u16 | seat / battler id (`sub_8007270`); 0 at first | 0 |
| 0x2A | 0x1A | `language` | u16 | `gGameLanguage`: JP 1, EN 2, FR 3, IT 4, DE 5, ES 7 (`constants/global.h`) | 2 (never 1) |
| 0x2C | — | `magic2` | 16 | same as `magic1` | same as `magic1` |

Note `sMagic` is a plain C string (`const u8 sMagic[] = "GameFreak inc.";`
in `link.c`), so it is **ASCII**, not the game charset. pokeemerald calls it
`sASCIIGameFreakInc`.

### What Ruby does with the partner's `LinkPlayer`

| # | Check | Code | Fails → |
|---|---|---|---|
| 1 | `strcmp(magic1, "GameFreak inc.") == 0` and same for `magic2` | `link.c: ProcessRecvCmds` 0x8888 | `CB2_LinkError` immediately |
| 2 | all players' `linkType` equal slot 0's (slot 0 = Flipper) | `link.c: GetLinkPlayerDataExchangeStatusTimed`, `IsLinkPlayerDataExchangeComplete` | status 3 → "The link partners appear to have made different selections." + 0x5FFF close (`cable_club.c: sub_80831F8`) |
| 3 | player count within [min, max] = [2, 2] | `cable_club.c: sub_8082D9C` [`ExchangeDataAndGetLinkupStatus`] | result 4 "The number of participants is incorrect." |
| 4 | `trainerId` and `name` equal the copy saved at linkup | `link.c: sub_800826C` (only on room re-establish, Part B) | `CB2_LinkError` |
| — | `name`: `ConvertInternationalString(name, language)` adds control codes when language == 1 | `text.c: ConvertInternationalString` | (rewrites the name) |
| — | `language == LANGUAGE_JAPANESE` refused only in the Record Corner | `cable_club.c: sub_808353C` | n/a here |
| — | `gender` indexes the avatar graphics table | `overworld.c: CreateLinkPlayerSprite` → `GetRivalAvatarGraphicsIdByStateIdAndGender` | out-of-range read if not 0/1 |
| — | `version`, `id` | never read in Ruby (grep of `src/`) | — |
| — | `lp_field_2 == 0x8000` → `EnableNationalPokedex()` after a trade | `trade.c: sub_804BA64` | — |

`gReceivedRemoteLinkPlayers` becomes TRUE when blocks from **all** slots
0..count−1 (including G's own echo) passed check 1
(`link.c: HandleReceiveRemoteLinkPlayer`).

### The Flipper's trainer card (100-byte block for request type 2)

The child copies the first `sizeof(struct TrainerCard)` = 0x38 bytes into
`gTrainerCards[0]` (`cable_club.c: sub_8083314`); it is shown when the GBA
player talks to the Flipper's avatar (`overworld.c: sub_805568C` →
`TradeRoom_ReadTrainerCard1/2`). Layout `include/trainer_card.h`:

| Off | Field | Flipper value | Why |
|---|---|---|---|
| 0x00 | `gender` u8 | 0/1 | `trainer_card.c`: `LoadTrainerGfx_TrainerCard(gender, …)` |
| 0x01 | `stars` u8 | 0 (≤ 4) | indexes `gTrainerCardPalettes[5]`; `cable_club.c: sub_8083BF4` indexes `gTrainerCardColorNames[stars−1]` |
| 0x02 | `hasPokedex` | 1 | display only |
| 0x03–0x05 | `var_3`, `var_4`, `var_5` | 0 | |
| 0x06–0x0B | `firstHallOfFameA/B/C` u16 | 0 | |
| 0x0C | `pokedexSeen` u16 | any ≤ 999 | |
| 0x0E | `trainerId` u16 | Flipper TID | |
| 0x10–0x12 | `playTimeHours`, `playTimeMinutes` u16 | any | |
| 0x14–0x22 | battle / tower / contest / pokeblock / trade counters, `var_22` | 0 | |
| 0x24 | `money` u32 | 0 | |
| 0x28 | `var_28[4]` u16 easy-chat words | 0xFFFF × 4 (empty) | `easy_chat_2.c: EasyChat_GetWordText` |
| 0x30 | `playerName[8]` | same name as `LinkPlayer.name`, EOS-padded | |
| 0x38–0x63 | — | 0 | not read |

## A6. Cable Club flow (choosing the Trade Center)

### Script (`data/scripts/cable_club.inc`, Trade Center attendant, script `…_EventScript_1A40FA` ff.)

1. `goto_if_unset FLAG_SYS_POKEDEX_GET` → attendant refuses (needs the Pokedex).
2. "Welcome to the POKéMON CABLE CLUB TRADE CENTER..." YES/NO.
3. `…_EventScript_1A41BB`: `CalculatePlayerPartyCount` ≥ 2, else "For trading,
   you must have at least two POKéMON with you."; `GetNameOfEnigmaBerryInPlayerParty`
   → refuses if a Pokemon holds the Enigma Berry.
4. `call Common_EventScript_SaveGame` → `special SaveGame` (normal full save,
   player may decline → abort). **This is the only save in Part A.**
5. "Please wait. ... B Button: Cancel" → `special sub_80834E4`
   [`TryTradeLinkup`]: `gLinkType = 0x1133`, `gBattleTypeFlags = 0`,
   `sub_8082CD4(2, 2)` [`CreateLinkupTask`] → `waitstate`.
6. `VAR_RESULT`: 1 OK; 2 "Someone is not ready to link."; 3 "different
   selections"; 4 "number of participants is incorrect"; 5 cancelled; 6
   "transmission error". Every non-1 result runs `special CloseLink`.
7. Result 1 → `…_EventScript_1A4166`: `VAR_0x8004 = 3`,
   `VAR_CABLE_CLUB_STATE = 3`, "Please enter.", counter/door animation
   (`delay 60`, several `applymovement`, door open/close), then
   `…_EventScript_1A4325`: `special SetCableClubWarp`
   (`field_control_avatar.c`: warp from the door tile's warp event) +
   `special DoCableClubWarp` (`field_fadetransition.c`). Door warp at PC 2F
   (8,1) → `MAP_TRADE_CENTER` warp 0 (`data/maps/*_PokemonCenter_2F/map.json`).

### Linkup task (`src/cable_club.c`)

| Task (Ruby) [Emerald] | Side | Does | Leaves when |
|---|---|---|---|
| `sub_8082F20` [`Task_LinkupStart`] | both | frame 0: `OpenLinkTimed` (`sPlayerDataExchangeStatus = 0`, `gLinkTimeOutCounter = 0`, `OpenLink`), `sub_80082EC`, `ResetLinkPlayers` | after 10 frames |
| `sub_8082F68` [`Task_LinkupAwaitConnection`] | both | B before connection → cancel (5); once connected sets error suppression; B → cancel (5) | `playerCount ≥ 2` |
| ↳ master | GBA master | "When all players are ready... A Button: Confirm" (`sub_8082FEC`), A → "Start link with 2 players." (`sub_808303C`, saves link players), A held → `sub_8007F4C` (handshake as master) (`sub_80830E4`); `sub_8083188` waits for the exchange with a 600-frame timeout (`sub_8083444`); `sub_8083288` sends **0xCCCC type 2** | |
| ↳ child | **G** | `PlaySE(SE_BOO)`, "Awaiting linkup... B Button: Cancel" | immediately → `sub_80831F8` |
| `sub_80831F8` [`Task_LinkupExchangeDataWithLeader`] | **G** | polls `sub_8082D9C(2,2)`; 0 (not started / timed out) → keep polling; 3 → `SetCloseLinkCallback` (0x5FFF) and stop; 1 (or 4, which then closes after the cards) → `gFieldLinkPlayerCount = 2`, `gUnknown_03004860` [`gLocalLinkPlayerId`] = 1, `sub_80081C8` [`SaveLinkPlayers`], `TrainerCard_GenerateCardForPlayer(gBlockSendBuffer)` | exchange complete |
| `sub_8083314` [`Task_LinkupAwaitTrainerCardData`] | **G** | waits until `GetBlockReceivedStatus() == 0b11` (Flipper's card **and** its own echo), copies `gTrainerCards[0..1]`, error suppression off, `ScriptContext_Enable` (script continues at step 7) | both cards |

- Confirmation prompt: **master only**. As master, the Flipper confirms by
  sending 0x8FFF; G shows no prompt and has no timeout while waiting
  (`sub_80831F8` treats "timed out" as "keep polling"; `sub_8083314` has none).
  G can only leave with B before the connection is up, or via a link error.
- G checks no version or progress of the partner here (only checks 2 and 3 of
  [A5](#what-ruby-does-with-the-partners-linkplayer)).

### What the Flipper sends, burst by burst

| Step | Flipper slot 0 | G slot 1 (expected) | Wait for before next step |
|---|---|---|---|
| H | handshake (A3), 1 transfer/frame | 0x0000, then 0xB9A0 | success transfer |
| B1 | `2222 1133 0 0 0 0 0 0` | zeros | — |
| B2 | `BBBB 003C 0080 0 …` | (≈ B3–B4) `BBBB 003C 0081` | — |
| B3–B7 | 5 × `8888 w0..w6` (Flipper `LinkPlayerBlock`) | 2 idle bursts, then 5 × `8888` | both blocks fully on the wire |
| check | parse G's block: magic, `linkType == 0x1133` (else see below), store name, TID/SID, gender, language, version | | — |
| idle | zeros, **≥ 5 bursts** (inferred margin; see U3) | zeros | G has generated its card |
| Bk | `CCCC 0002 0 …` | — | — |
| Bk+1 | `BBBB 0064 0080` | `BBBB 0064 0081` (1–3 bursts later) | — |
| Bk+2…Bk+9 | 8 × `8888` (Flipper trainer card) | 2 idle, then 8 × `8888` (G's card; keep it) | both cards on the wire |
| walk-in | zero bursts every frame (G runs the door animation, warps, loads the room; sends nothing) | zeros | G sends `2FFE` (A7) |

If G's `linkType` is not 0x1133 (the player chose another counter), keep
0x1133: G reports "different selections", sends 0x5FFF, and waits for the
Flipper's 0x5FFF before it can close (`sub_80831F8` → `sub_80833C4`). **Always
answer 0x5FFF** (A7).

Optional (inferred): wait until G's `LinkPlayerBlock` has arrived, then send
the Flipper's own block with `linkType` copied from G's when it is one of
0x1133/0x1111/0x1122/0x1144. The receiver does not need the blocks to overlap,
and this keeps the Flipper correct in every Part B phase without tracking it.

## A7. In the Trade Center

### Entering (both players warp independently; one sync point)

Each GBA warps by itself; there is no link traffic during the door animation
(`gLinkCallback == NULL`). After `DoCableClubWarp`:

1. `field_fadetransition.c: WaitCableClubWarp` → `WarpIntoMap`,
   `SetMainCallback2(sub_8054588)` [`CB2_ReturnToFieldCableClub`]:
   `gFieldCallback = sub_8080AC4`.
2. `overworld.c: c2_80567AC` [`CB2_LoadMapOnReturnToFieldCableClub`] runs
   `sub_805483C` [`LoadMapInStepsLink`] (14 frames: link players spawned by
   `sub_8054EC8` [`SpawnLinkPlayers`]), then
   `callback1 = sub_8055354` [`CB1_OverworldLink`], key intercept
   `sub_8055390` [`KeyInterCB_SelfIdle`].
3. `field_fadetransition.c: sub_8080A5C` [`Task_ReturnToFieldWirelessLink`;
   the name is misleading, cable uses it too]:
   - `sub_80084A4` [`SetLinkStandbyCallback`]: when G's `gLastRecvQueueCount`
     is 0, G sends **0x2FFE** once, then waits until slots 0..1 both sent
     0x2FFE (`sub_80084F4` [`LinkCB_StandbyForAll`]).
   - **Flipper: answer with one `2FFE` command.** (The flag persists until
     consumed, so an early 0x2FFE also works; answering is simplest.)
   - fade in; then `sub_8007B14` [`StartSendingKeysToLink`]:
     `gLinkCallback = sub_8007B44`, G now sends `CAFE <key>` every frame.

`gLastRecvQueueCount` on the child is refreshed only when `serialIntrCounter`
hits 8, and the child never resets that `s8` counter (only the master does, in
`LinkVSync`), so it refreshes every 256 serial IRQs (≈ 28 bursts). G may take
up to ≈ 0.5 s to send its 0x2FFE (inferred from `SerialCB`).

### Spawn and avatars (`overworld.c`)

- Player *i* spawns at (warp x + *i*, warp y), facing north
  (`sub_8054E98`, `sub_8054EC8`, `SpawnLinkPlayerObjectEvent`:
  `range.as_byte = 2`). `MAP_TRADE_CENTER` warp 0 is (5,8)
  (`data/maps/TradeCenter/map.json`), so **F stands at (5,8), G at (6,8)**;
  both are exit-warp tiles. Seats are coord events at (4,5) (seat 0,
  `TradeCenter_EventScript_1A43F0`) and (7,5) (seat 1, `…_1A43FA`).
- Avatar sprite from `gLinkPlayers[i].gender` (`CreateLinkPlayerSprite`).
- **No positions are ever sent.** Every GBA replays every player's key codes
  in lockstep (`sub_8055218` [`UpdateAllLinkPlayers`] processes slots 0..3,
  including its own echo) and moves all avatars itself. G is the only simulator
  here; the Flipper's avatar moves only if the Flipper sends direction codes.

### Room command format

One command per burst: `CAFE <code> 0 0 0 0 0 0` (`link.c: BuildSendCmd`
0xCAFE: `[1] = word_3004858` [`gHeldKeyCodeToSend`]; skipped when that is 0 or
`gLinkTransferringData`). Key codes (`overworld.c: sub_80552B0`,
`sub_8054FF8`; names from `em: include/overworld.h`):

| Code | [Emerald] | Meaning | Effect on that player (`sub_8054FF8` [`HandleLinkPlayerKeyInput`]) |
|---|---|---|---|
| 0x00 | `NULL` | (slot all zero) | same as 0x11 |
| 0x11 | `EMPTY` | no input | none |
| 0x12 / 0x13 / 0x14 / 0x15 | `DPAD_DOWN/UP/LEFT/RIGHT` | held direction | if state `IDLE`: turn/step (16-frame step, collision checked); 0x12 on the south exit facing south starts "end link?" prompt for the local player (`sub_8055660`) |
| 0x16 | `READY` | sat down | state → `READY` (0x82), regardless of position |
| 0x17 | `EXIT_ROOM` | leaving | state → `EXITING_ROOM` (0x83). While any player is `EXITING_ROOM`, every remote `IDLE` player is forced `BUSY` (0x81) and its key code that update is dropped (`sub_8054FF8` first block); the second `switch` applies 0x17 only on a later update, so a remote 0x17 is honoured one command after the first update that sees an `EXITING_ROOM` player |
| 0x18 | `START_BUTTON` | start menu | busy |
| 0x19 | `A_BUTTON` | interact | busy; talking to another avatar shows its trainer card |
| 0x1A | `IDLE` | back to idle | state → `IDLE` (0x80) |
| 0x1B | `HANDLE_RECV_QUEUE` | flow control: own receive queue > 4 | busy until queue ≤ 2 |
| 0x1C | `HANDLE_SEND_QUEUE` | flow control: own send queue > 4 | busy until queue ≤ 2 |
| 0x1D | `EXIT_SEAT` | stood up from seat | `READY` → busy |

Player states (`em: overworld.c`): 0x80 `IDLE`, 0x81 `BUSY`, 0x82 `READY`,
0x83 `EXITING_ROOM` (Ruby: `gUnknown_03000580[4]`, reset to 0x80 by
`sub_8054F70`). Movement and the 0x12 (exit prompt), 0x18, 0x19, 0x1B, 0x1C
cases only apply in `IDLE` (`sub_8054FF8` first block); a remote player that
went `BUSY` that way (or by stepping onto a coord event) stays `BUSY` until
**that player** sends 0x1A. 0x16, 0x17 and 0x1A apply in any state, except for an
`IDLE` slot while anyone is `EXITING_ROOM`: that slot is forced `BUSY` and its
code dropped for that update (`overworld.c: sub_8054FF8`, pokeruby 5784633c,
lines 1961-1970); 0x1D only in `READY` (second `switch`). If the Flipper ever sends 0x18 or
0x19, it must follow with 0x1A.

**Idle player standing still:** a real GBA sends `CAFE 0011` every frame
(`sub_8055390` returns 0x11 with no input or when field controls are locked).
For G, a Flipper slot of all zeros is equivalent (`word_3002910[0] = 0`,
`sub_805530C(0) = 0`, no case in `sub_8054FF8`).

### Lockstep and rate (`main.c`, `overworld.c`)

- While `gLinkCallback == sub_8007B44`, G **skips its whole game frame**
  (callback1 and callback2) when its receive queue was empty that frame
  (`main.c: UpdateLinkAndCallCallbacks`: `RECEIVED_NOTHING` and
  `sub_8055940() == 1`). G's own `CAFE` makes each burst non-empty, so G
  advances one game frame per burst that carries its command.
- G enqueues one `CAFE` per frame even while frozen (`LinkMain2` always runs
  `gLinkCallback`). Mismatched rates are absorbed by the game:
  - Flipper faster → G's receive queue grows → at > 4 G sends 0x1B, locks its
    controls and runs a second update per frame (`main.c` loop,
    `sub_80558AC`) until ≤ 2, then 0x1A.
  - Flipper slower → G's send queue grows → at > 4 G sends 0x1C and skips
    building `CAFE` (`gLinkTransferringData`) until ≤ 2.
  - Either way a short "busy" stall, not an error, as long as no queue reaches 50.
- Recommendation (inferred, U6): burst at 59.7275 Hz or up to ≈ 0.1 % faster,
  and send **all-zero commands when idle**, `CAFE <code>` only for real codes.
  Then G's receive queue gains exactly one entry per G frame (its own `CAFE`),
  the Flipper's spare bursts are all-zero and leave no queue entry, and G's
  send queue cannot back up. Optionally steer the rate from G's own 0x1B
  (slow down) / 0x1C (speed up).

### What the Flipper must react to while idling (from slot 1)

| G sends | Flipper sends | Otherwise |
|---|---|---|
| `2FFE` (any time) | one `2FFE` | G waits forever (`sub_80084F4`) |
| `5FFF` (any time) | one `5FFF`, then keep normal bursts until slot 1 reads 0xFFFF for a whole burst, then return to handshake mode (A3) | G waits forever in `sub_800837C` [`LinkCB_WaitCloseLink`] |
| `CAFE 0017` | one `CAFE 0017` (Part B: leaving), sent **after the GBA's next command**, not in the very next burst (`sub_8054FF8` drops the key of an `IDLE` slot while G is `EXITING_ROOM`) | G frozen waiting for all 0x83 (`sub_80554BC`) |
| `CAFE 0016` | nothing for Part A (G shows "Please wait... B: Cancel" with no timeout); `CAFE 0016` starts the trade menu (Part B) | — |
| other codes | nothing | — |

Do not switch back to single handshake transfers while G is still connected:
G would take the next stray transfer as a checksum slot and raise 0x2000. The
0xFFFF test above avoids that (inferred from `DoRecv`; G only escapes because
`CloseLink` clears `gLinkOpen` before `CheckErrorStatus` in the same
`LinkMain2`).

### What breaks the idle room

| Cause | Result |
|---|---|
| A gap of ≥ 11 frames with no transfer | `LAG_SLAVE` → error screen |
| Lost, extra or split transfer (burst ≠ 9) | checksum error → error screen |
| Wrong checksum word | error screen |
| Either queue reaching 50 (sustained rate mismatch with flow control defeated) | error screen |
| Flipper drives G's SI low when idle / bad stop bits | hardware error bit → error screen (GBATEK) |
| Flipper sends direction codes and tracks its own position | Flipper's model diverges if G's avatar blocks a step (`LinkPlayerDetectCollision`); G has no desync check, nothing errors |
| Flipper sends 0xAAAA, 0xCCCC, 0x2222 or a block | G changes link callback / sends blocks; breaks later steps |

## A8. Flipper master checklist (Part A)

1. Before the GBA links: SC high, GBA-SI high; one transfer per frame of
   0xB9A0; treat no answer as 0xFFFF.
2. Run `DoHandshake` on every transfer; switch to 0x8FFF once count ≥ 2; on
   success start 9-transfer bursts at the next frame tick.
3. Every burst: transfer #1 = checksum of the previous burst (anything for
   the first); #2–#9 = the current command or zeros; 751.5 µs gaps;
   59.7275 Hz (≤ +0.1 %); never pause ≥ 11 frames.
4. Burst 1: `2222 1133`. Then own `LinkPlayerBlock` (60 B, header `BBBB 003C 0080`).
5. Receive and validate G's `LinkPlayerBlock`.
6. ≥ 5 idle bursts, then `CCCC 0002`, then own trainer card (100 B).
7. Zero bursts through G's walk-in; answer `2FFE` once.
8. Idle in the room with zero bursts (or `CAFE 0011`); answer `2FFE`,
   `5FFF`, `CAFE 0017` as in A7 (`CAFE 0017` only after the GBA's next
   command; the first burst after it is dropped by `sub_8054FF8`).

---

# Part B: outline (trade, save, leave)

### B1. Sitting down → trade menu
- G steps on its seat → `TradeCenter_EventScript_1A43F0/1A43FA`
  (`VAR_0x8005` = seat) → `sub_8083B5C` [`PlayerEnteredTradeSeat`] →
  `sub_80839DC` [`Task_EnterCableClubSeat`]: "Please wait..." →
  `sub_8055574` [`SetInCableClubSeat`] (G sends `CAFE 0016` once),
  `sub_8007270` (local `LinkPlayer.id` = seat) → polls `sub_80554F8`
  [`GetCableClubPartnersReady`] until both are 0x82; B → `CAFE 001D`.
- Flipper sends `CAFE 0016` once (its avatar need not move).
- Both ready → `sub_8083AAC` [`Task_StartWiredTrade`]: `ClearLinkCallback_2`
  (no more `CAFE`), fade, `SetCloseLinkCallback` → `5FFF` exchange →
  `CloseLink` → `SetMainCallback2(sub_8047CD8)` [`CB2_StartCreateTradeMenu`].

### B2. Trade menu link (`trade.c: sub_8047EC0` [`CB2_CreateTradeMenu`])
- State 1: `gLinkType = 0x1122`, `OpenLink`, `sub_8083C50` (300-frame timeout
  to `gReceivedRemoteLinkPlayers`). Master side advances after count ≥ saved
  count and 30 frames. State 4 waits for `IsLinkPlayerDataExchangeComplete`.
- Flipper: detect G gone (0xFFFF), re-handshake, `2222 1122`,
  `LinkPlayerBlock` with `linkType` 0x1122 and the same name/TID.

### B3. Party exchange (`trade.c: sub_8048D44` [`BufferTradeParties`])
Five rounds. Each round both sides copy data into `gBlockSendBuffer`; master
sends `CCCC <type>`; both send blocks; both wait for 0b11.

| Round | Type | Size | Content |
|---|---|---|---|
| 1–3 | 1 | 200 | `gPlayerParty[0..1]`, `[2..3]`, `[4..5]` (2 × 100-byte party `struct Pokemon`) |
| 4 | 3 | 220 | `gSaveBlock1.mail[0..5]`: 6 × 34-byte `MailStruct` + 4 = 208 bytes copied (`include/global.h`), rest of the 220 is stale buffer |
| 5 | 4 | 40 | `gSaveBlock1.giftRibbons[11]` |

- Child copies the next round's data one state **after** the previous
  round's blocks arrive: leave a few bursts before each `CCCC` (same race as A6).
- Received ribbons are written into G's save where G's are 0
  (`sub_804AF84`): send zeros. Received Shedinja nicknames are fixed
  (case 16). Empty party slots: species 0 (`CalculateEnemyPartyCount`).

### B4. Selecting and confirming (`trade.c: sub_80494D8` [`CB1_UpdateLink`])
- The master is the arbiter. The child sends 20-byte blocks
  (`Trade_SendData` = `SendBlock(linkData, 20)`, `[0]` = code, `[1]` = arg):
  `AABB <party index 0–5>` ready to trade, `EEAA` cancel, then `BBBB` yes /
  `BBCC` no. The master's own choice stays local (`sub_8049620`).
- Master resolves (`sub_80492D8` [`Leader_HandleCommunication`]; reads blocks in
  `sub_80490BC` [`Leader_ReadLinkBuffer`]) and sends a 20-byte block:

| Master choice | Child choice | Master sends | Child (`sub_80491E4` [`Follower_ReadLinkBuffer`]) |
|---|---|---|---|
| ready | `AABB i` | `DDDD <master index>` | shows the pair (uses `[1] + 6`), asks to confirm |
| ready | `EEAA` | `EECC` | message, back to menu |
| cancel | `AABB i` | `DDEE` | message, back to menu |
| cancel | `EEAA` | `EEBB` | both leave the menu (B7) |
| yes | `BBBB` | `CCDD` | start the trade |
| any no | `BBCC` / no | `DDEE` | back to menu |

### B5. The trade (`trade.c`)
- `CCDD` → `sub_8048A50`: remember both indexes (`gUnknown_020297D8`),
  `SetCloseLinkCallback` (`5FFF`) → `sub_804B41C` [`CB2_LinkTrade`]:
  `CloseLink`, `gLinkType = 0x1144`, `OpenLink`, handshake, `2222` exchange
  with `linkType` 0x1144.
- End of animation (`sub_804DB84`): `sub_804BA94` swaps the two mons in each
  GBA's own copy (friendship 70, mail, `sub_804BA64` National Dex if partner
  `lp_field_2 == 0x8000`) → child sends 20-byte `ABCD` → master
  (`sub_804DC18`) sends 20-byte `DCBA` → child `sub_804D588` (trade evolution).
  The Flipper only needs to send `DCBA` after G's `ABCD`.

### B6. Link save (`trade.c: sub_804DC88` [`CB2_SaveAndEndTrade`])
- "Link standby" → `0x2FFE` sync #1 → "Saving..." → `sub_8125D80` /
  `sub_8125DA8` (sector writes, `save.c`) → `sub_8125DDC` → 50 frames →
  `0x2FFE` sync #2 → `sub_8125E04` (final sector) → 60 frames → `0x2FFE` sync
  #3 → fade → `SetCloseLinkCallback` (`5FFF`) → `sub_804E144` → back to
  `sub_8047CD8` (trade menu, link reopened with 0x1122, parties exchanged again
  with the post-trade parties). Soft reset is disabled throughout.
- The Flipper must answer all three `2FFE` and the `5FFF`, and keep bursting
  through the flash writes.

### B7. Cancelling and leaving
- Both cancel in the menu (`EEBB`) → `sub_8049D9C` (`5FFF`) →
  `sub_805465C` [`CB2_ReturnToFieldFromMultiplayer`] → room reloads with
  `sub_8080A3C` [`FieldCB_ReturnToFieldCableLink`] → `sub_8083664`
  [`CreateTask_ReestablishCableClubLink`]: `gLinkType = 0x1111`, `OpenLink`,
  300-frame timeout, exchange, `sub_800826C` name/TID check, then `CAFE`.
- Leaving the room: G walks onto (5,8)/(6,8) facing south → 0x12 →
  `TradeRoom_PromptToCancelLink` → YES → `sub_80839D0` [`ExitLinkRoom`] →
  `CAFE 0017`. Flipper answers `CAFE 0017` after the GBA's next command, not
  in the very next burst: while G is `EXITING_ROOM`, `sub_8054FF8` forces an
  `IDLE` remote slot `BUSY` and drops its key that update (`overworld.c:
  sub_8054FF8`, pokeruby 5784633c), else G waits forever. Then `gUnknown_081A4508`:
  `sub_80839A4` [`CleanupLinkRoomState`] and `sub_80810DC` → `sub_8081050`:
  stop `CAFE`, fade, `5FFF` exchange, `CloseLink`, warp to PC 2F. The PC map
  script for `VAR_CABLE_CLUB_STATE == 3` runs `special CloseLink`, resets the
  var and prints "The TRAINER CARD data will be rewritten." No save.
- Flipper-initiated leave: send `CAFE 0017`; an idle G is forced into
  `TradeRoom_TerminateLink` (`sub_8054FF8` → `sub_805585C`), sends 0x17, then
  the same close sequence.

---

# Part C: FireRed/LeafGreen differences (pokefirered)

- Link layer identical: same `CMD_LENGTH`, queue sizes, handshake words,
  checksum, timer reload (−197), 10-VBlank slave limit
  (`fr: src/link.c: DoHandshake`, `LinkVSync`, `InitTimer`).
- `struct LinkPlayer`: `name[8]` then `progressFlags` @0x10 (low nibble =
  National Dex, 0x10 = `FLAG_SYS_CAN_LINK_WITH_RS`), `neverRead` @0x11,
  `progressFlagsCopy` @0x12 (`fr: include/link.h`). FRLG always sends
  `lp_field_2 = 0x8000` (`fr: link.c: InitLocalLinkPlayer`), and zeroes the
  progress bytes of an R/S partner (`ProcessRecvCmds`).
- `GetLinkPlayerDataExchangeStatusTimed(min, max)` returns distinct statuses;
  for `LINKTYPE_TRADE_SETUP` it calls `fr: trade.c: GetGameProgressForLinkTrade`:
  if the partner's version is R/S/E, the FRLG player must have
  `FLAG_SYS_CAN_LINK_WITH_RS` (else "player not ready"); an Emerald partner
  must also have cleared the game. Claiming an FRLG version (0x4004/0x4005)
  avoids the gate.
- Trainer card format depends on the partner's version: FRLG `TrainerCard` vs
  `TrainerCardRSE` (`fr: cable_club.c: Task_LinkupAwaitTrainerCardData`).
- Trade restrictions (`fr: trade.c`, `CANT_TRADE_*`): species > 151 refused
  if the FRLG player lacks the National Dex; if the partner is not R/S and its
  `progressFlagsCopy & 0xF` is 0, the GBA refuses to send non-Kanto species;
  Mew/Deoxys need the fateful-encounter bit.
- Avatar sprite chosen from `gLinkPlayers[i].version`
  (`fr: overworld.c: CreateLinkPlayerSprite(i, version)`), so `version` matters.
- Trade menu command words and block rounds are the same
  (`fr: trade.c`: `SendBlockRequest(BLOCK_REQ_SIZE_200)` ×3, `_220`, `_40`;
  `LINKCMD_READY_TO_TRADE` etc.). Not checked byte for byte.

---

# Uncertain

- **U1 Wire protocol.** Bit order, the master→child hand-over via SO/SI, 0xFFFF
  for absent units, error-bit conditions and the "SI idles high" requirement
  come from GBATEK, not pret. Exact inter-slot delay and the master's timeout
  for an absent child are unknown. Capture a real GBA↔GBA Ruby link with a
  logic analyzer before tuning. Which Flipper pin reaches the GBA's SI depends
  on the cable and is unverified.
- **U2 Minimum gap.** The child only needs its `SerialCB` to run between
  transfers; the safe minimum depends on its worst-case IRQ latency (IME=0
  sections, VRAM DMA during map loads, BIOS calls). Matching the GBA's
  751.5 µs is the only gap known to work. Not measured.
- **U3 0xCCCC timing.** The child must have generated its trainer card
  (`sub_80831F8`) before 0xCCCC arrives, or it sends stale `gBlockSendBuffer`
  bytes as its card (likely a crash when viewed: `stars` would index past the
  palette table). Inferred from task order; the GBA master leaves ≈ 2–3
  frames. "≥ 5 bursts" is a margin, not a measured value. Same race for each
  Part B party round.
- **U4 Name bytes 8–10.** Ruby sends `playerGender`, `specialSaveWarp` and
  `playerTrainerId[0]` there; Ruby receivers stop at EOS. 0xFF fill is assumed
  safe for Ruby; Emerald/FRLG read 0x10–0x12 as progress flags.
- **U5 Child timeout.** `sub_80831F8` keeps polling after
  `EXCHANGE_TIMED_OUT` (returns 0 → `return`). Read from code, not tested.
- **U6 Rate strategy.** "Slightly fast + zero idle commands" and steering by
  0x1B/0x1C are deductions from `main.c`, `overworld.c` and `link.c` queue
  code. The game's own flow control should make nominal 59.7275 Hz with
  `CAFE 0011` work too, with occasional "busy" stalls.
- **U7 Handshake edge cases.** If G misses a serial IRQ during the handshake,
  its previous count can differ from the Flipper's and the two may disagree
  on the success transfer. Suggested recovery: if slot 1 shows 0xB9A0 in
  transfers #2–#9 of a burst, G is still handshaking, so fall back to A3.
- **U8 `gLastRecvQueueCount` latency.** The 256-IRQ refresh on the child is
  read from `SerialCB`; the delay before G sends 0x2FFE/0x5FFF is inferred.
- **U9 Hardware error flag.** GBATEK is unsure whether the bit clears by
  itself each transfer; Ruby overwrites its copy every IRQ, so a single bad
  transfer only counts if it is the latest when `LinkMain1` samples.
- **U10 Emerald names.** Bracketed names are the matching pokeemerald
  functions by structure; a few (key-intercept callbacks) are best matches.

---

# Appendix: Ruby → Emerald names

| Ruby | Emerald |
|---|---|
| `link.c: sub_8007B14` / `sub_8007B24` / `sub_8007B44` | `StartSendingKeysToLink` / `IsSendingKeysToLink` / `LinkCB_SendHeldKeys` |
| `sub_8007E24` / `sub_8007E9C` / `sub_8007F4C` | `SetBerryBlenderLinkCallback` / `SendBlockRequest` / `CheckShouldAdvanceLinkState` |
| `sub_80081C8` / `sub_8008198` / `sub_800820C` | `SaveLinkPlayers` / `GetSavedLinkPlayerCountAsBitFlags` / `GetSavedPlayerCount` |
| `sub_800826C` / `sub_80082EC` | `CheckLinkPlayersMatchSaved` / `ResetLinkPlayerCount` |
| `sub_8008350` / `sub_800837C` | `LinkCB_ReadyCloseLink` / `LinkCB_WaitCloseLink` |
| `sub_80084A4` / `sub_80084C8` / `sub_80084F4` | `SetLinkStandbyCallback` / `LinkCB_Standby` / `LinkCB_StandbyForAll` |
| `word_3002910` / `word_3004858` | `gLinkPartnersHeldKeys` / `gHeldKeyCodeToSend` |
| `u8_array_3002B70` / `u8_array_3002B78` | `gReadyToExitStandby` / `gReadyToCloseLink` |
| `gLinkPlayerPending` | `gRemoteLinkPlayersNotReceived` |
| `cable_club.c: sub_8082CD4` / `sub_8082D9C` | `CreateLinkupTask` / `ExchangeDataAndGetLinkupStatus` |
| `sub_8082DF4` / `sub_8082E28` / `sub_8082E6C` / `sub_8082EB8` | `CheckLinkErrored` / `CheckLinkCanceledBeforeConnection` / `CheckLinkCanceled` / `CheckSioErrored` |
| `sub_8082F20` / `sub_8082F68` / `sub_8082FEC` / `sub_808303C` / `sub_80830E4` | `Task_LinkupStart` / `Task_LinkupAwaitConnection` / `Task_LinkupConfirmWhenReady` / `Task_LinkupAwaitConfirmation` / `Task_LinkupTryConfirmation` |
| `sub_8083188` / `sub_80831F8` / `sub_8083288` / `sub_8083314` | `Task_LinkupConfirm` / `Task_LinkupExchangeDataWithLeader` / `Task_LinkupCheckStatusAfterConfirm` / `Task_LinkupAwaitTrainerCardData` |
| `sub_80833C4` / `sub_80833EC` / `sub_8083418` / `sub_8083444` | `Task_StopLinkup` / `Task_LinkupFailed` / `Task_LinkupConnectionError` / `TryLinkTimeout` |
| `sub_80834E4` | `TryTradeLinkup` |
| `sub_8083664` / `sub_8083710` / `sub_8083760` / `sub_80837B4` / `sub_80837EC` | `CreateTask_ReestablishCableClubLink` / `Task_ReestablishLink` / `Task_ReestablishLinkAwaitConnection` / `Task_ReestablishLinkLeader` / `Task_ReestablishLinkAwaitConfirmation` |
| `sub_80839A4` / `sub_80839D0` / `sub_80839DC` / `sub_8083A84` | `CleanupLinkRoomState` / `ExitLinkRoom` / `Task_EnterCableClubSeat` / `CreateTask_EnterCableClubSeat` |
| `sub_8083AAC` / `sub_8083B5C` / `sub_8083BF4` / `sub_8083C50` | `Task_StartWiredTrade` / `PlayerEnteredTradeSeat` / `GetLinkTrainerCardColor` / `Task_WaitForLinkPlayerConnection` |
| `overworld.c: sub_8054588` / `c2_80567AC` / `sub_805483C` | `CB2_ReturnToFieldCableClub` / `CB2_LoadMapOnReturnToFieldCableClub` / `LoadMapInStepsLink` |
| `sub_805465C` / `sub_8054EC8` / `sub_8055354` | `CB2_ReturnToFieldFromMultiplayer` / `SpawnLinkPlayers` / `CB1_OverworldLink` |
| `sub_8055218` / `sub_8054FF8` / `sub_8055280` / `sub_80543DC` | `UpdateAllLinkPlayers` / `HandleLinkPlayerKeyInput` / `UpdateHeldKeyCode` / `SetKeyInterceptCallback` |
| `sub_8055390` / `sub_80552B0` / `sub_8055408` / `sub_8055438` / `sub_80553E4` | `KeyInterCB_SelfIdle` / `KeyInterCB_ReadButtons` / `KeyInterCB_DeferToRecvQueue` / `KeyInterCB_DeferToSendQueue` / `KeyInterCB_DeferToEventScript` |
| `sub_80554F8` / `sub_8055574` / `sub_8055588` / `sub_805559C` | `GetCableClubPartnersReady` / `SetInCableClubSeat` / `SetLinkWaitingForScript` / `QueueExitLinkRoomKey` |
| `field_fadetransition.c: sub_8080AC4` / `sub_8080A5C` | `FieldCB_ReturnToFieldWirelessLink` / `Task_ReturnToFieldWirelessLink` (`em: field_screen_effect.c`) |
| `sub_8080A3C` / `task_mpl_807DD60` | `FieldCB_ReturnToFieldCableLink` / `Task_ReturnToFieldCableLink` |
