# Gen III link protocol, Parts B and C: trading with the Flipper as SIO master

Part B covers everything the Flipper (multiplayer **parent**, slot 0) must do
with a GBA running English **Pokemon Ruby** (child, slot 1), from both players
idling in the Trade Center to one or more completed trades, cancelling, and
leaving. Part C lists what changes for FireRed/LeafGreen.

Prerequisite: `docs/gen3/link-protocol.md` Part A (handshake A3, bursts and
checksum A2, block transfer A4, `LinkPlayerBlock` A5, room key codes and
2FFE/5FFF/CAFE handling A7, checklist A8). Its Part B outline is superseded by
this file; corrections are in [Corrections](#corrections-to-link-protocolmd).

## Sources and conventions

Same as `link-protocol.md`:

| Repo (read-only clone) | Commit |
|---|---|
| pret/pokeruby | `5784633c` |
| pret/pokeemerald | `731ad5bf` |
| pret/pokefirered | `ce6406d8` |

- Citations are `file: Function` in **pokeruby** unless prefixed `em:` or
  `fr:`. Ruby `sub_XXXXXXX` names carry the pokeemerald name in `[brackets]`
  (full map in the [appendix](#appendix-ruby--emerald-names-part-b)).
- F = Flipper (slot 0), G = the GBA (slot 1). "Burst" = 9 transfers, one per
  frame (A2). "Block" = A4 block transfer (`BBBB` header + `8888` chunks).
- **(inferred)** marks deductions; they are collected in [Uncertain](#uncertain).
- `trade.c: sub_804B41C` is `NAKED` in the matching build; its state numbers
  are read from the `#ifdef NONMATCHING` C version of the same function.

---

# Part B: Ruby trade

## B0. Session map

Every arrow that changes the link type is a full close (`5FFF`, A7) and a
fresh `OpenLink` on G, so the Flipper re-runs A3 (handshake) and the
`2222` + `LinkPlayerBlock` exchange each time.

| # | G screen | `gLinkType` | Opened by (Ruby) [Emerald] | Closed by | G-side open timeout |
|---|---|---|---|---|---|
| 1 | Trade Center room | 0x1133 (Part A) or 0x1111 | `cable_club.c: sub_80834E4` [`TryTradeLinkup`] / `sub_8083664` [`CreateTask_ReestablishCableClubLink`] | `sub_8083AAC` [`Task_StartWiredTrade`] (seat) or room exit | 0x1111: 300 frames |
| 2 | Trade menu | **0x1122** | `trade.c: sub_8047EC0` [`CB2_CreateTradeMenu`] state 1 | `sub_8048A50` [`CB_WaitToStartTrade`] (trade) or `sub_8049D9C` [`CB_InitExitCanceledTrade`] (both cancel) | 300 frames |
| 3 | Trade animation, evolution, save | **0x1144** | `trade.c: sub_804B41C` [`CB2_LinkTrade`] state 1 | `sub_804DC88` [`CB2_SaveAndEndTrade`] state 8 | **none** |
| 2' | Trade menu again | 0x1122 | `sub_804E144` [`CB2_FreeTradeAnim`] → `sub_8047CD8` [`CB2_StartCreateTradeMenu`] | as 2 | 300 frames |
| 1' | Room again | **0x1111** | `field_fadetransition.c: task_mpl_807DD60` [`Task_ReturnToFieldCableLink`] → `sub_8083664` | as 1 | 300 frames |

Loops: 1 → 2 → (3 → 2')* → 1' → 2 → ... until G leaves the room (A7, Part A
B7 outline).

## B1. Sitting down → trade menu

### B1.1 How G sits (all `src/overworld.c` unless noted)

| Step | Code | Detail |
|---|---|---|
| Seat tiles | `data/maps/TradeCenter/map.json` coord events | (4,5) → `TradeCenter_EventScript_1A43F0` (`VAR_0x8005 = 0`), (7,5) → `TradeCenter_EventScript_1A43FA` (`VAR_0x8005 = 1`); both `VAR_TEMP_0 == 0`; both run `special sub_8083B5C` [`PlayerEnteredTradeSeat`] + `waitstate` (`data/scripts/cable_club.inc`) |
| Trigger | `sub_8054FF8` [`HandleLinkPlayerKeyInput`] → `sub_8055648` [`TryGetTileEventScript`] | only for a slot in state `IDLE` (0x80) whose link-player `mode == 2` (`MOVEMENT_MODE_SCRIPTED`), i.e. in the update right after a step **onto** the tile finished (`sub_8055D38` [`MovementStatusHandler_TryAdvanceScript`] sets mode 2). Standing on the tile does not re-trigger |
| Who runs it | `sub_8054FF8` | the slot goes `BUSY` (0x81) on every GBA; only the **local** player (`a3->b`) runs the script. The Flipper's avatar stepping on a seat runs nothing on G |
| Facing | `sub_8055758` | seat 0 → forced facing 10 (east), seat 1 → 9 (west) |
| Task | `cable_club.c: sub_8083A84` [`CreateTask_EnterCableClubSeat`] → `sub_80839DC` [`Task_EnterCableClubSeat`] | case 0 `ShowFieldMessage("Please wait.\n... ... B Button: Cancel")` (`gUnknown_081A490C`, `data/text/cable_club.inc`); case 1 when the text has printed (`field_message_box.c: IsFieldMessageBoxHidden`) → `sub_8055574` [`SetInCableClubSeat`] and `link.c: sub_8007270` (`localLinkPlayer.id = seat`); case 2 polls `sub_80554F8` [`GetCableClubPartnersReady`] every frame, **no timeout** |

### B1.2 Key codes G sends while seated

| When | Key intercept (Ruby) [Emerald] | G sends |
|---|---|---|
| text printed (case 1) | `sub_80554A4` [`KeyInterCB_SetReady`] (one call) | `CAFE 0016` **once** |
| waiting | `sub_805546C` [`KeyInterCB_Ready`] | `CAFE 0011` every frame |
| B pressed while its own state is `READY` | `sub_805546C` → `sub_8055468` [`KeyInterCB_ExitingSeat`] | `CAFE 001D` once, then `0011` |
| after cancel, script ended | `sub_80839DC` case 3 → `sub_8055588` [`SetLinkWaitingForScript`] → `sub_80553E4` [`KeyInterCB_DeferToEventScript`] | `0011` while controls are locked, then `CAFE 001A` once (back to `IDLE`, `sub_8054FF8` case 26 restores `sub_8055390` [`KeyInterCB_SelfIdle`]) |

`script.c: ScriptContext_RunScript` unlocks field controls when the seat
script ends, which is what releases the `001A`.

### B1.3 `sub_80554F8` [`GetCableClubPartnersReady`]

Evaluated in order:

| Condition | Returns | `sub_80839DC` does |
|---|---|---|
| any slot `EXITING_ROOM` (0x83) (`sub_8054FC0` [`IsAnyPlayerInLinkState`]) | 2 | case 3: cancel |
| intercept is `KeyInterCB_Ready` and own state is not yet `READY` | 0 | wait |
| intercept is `KeyInterCB_ExitingSeat` and own state is `BUSY` (own `001D` processed) | 2 | cancel |
| every slot `READY` (0x82) (`sub_8054F88` [`AreAllPlayersInLinkState`]) | 1 | `HideFieldMessageBox`, follow-up `sub_8083B44` → task `sub_8083AAC` [`Task_StartWiredTrade`] |
| otherwise | 0 | wait |

Player-state codes come from A7: `0x16` → `READY` in any state; `0x1D` →
`READY` → `BUSY`; `0x1A` → `IDLE` in any state; except that an `IDLE` slot
is forced `BUSY` and its code dropped while anyone is `EXITING_ROOM`
(`sub_8054FF8`).

### B1.4 How the Flipper "sits"

- Send **`CAFE 0016` once**. G sets the Flipper's slot to `READY` regardless
  of the avatar's position (`sub_8054FF8` second `switch`, case 22). The
  Flipper's avatar never moves (it stays at (5,8), A7).
- Recommended policy (inferred, T7): **mirror G.** Send `CAFE 0016` in the
  burst after G's `CAFE 0016` arrives. On G's `CAFE 001D`, optionally send
  `CAFE 001A` (Flipper back to `IDLE`). Reasons: while the Flipper is `READY`,
  G's A-press on the Flipper's avatar shows "This TRAINER is too busy to
  notice..." (`CableClub_TooBusyToNotice`) instead of the trainer card
  (`sub_805568C` [`TryInteractWithPlayer`]: card only when the target slot is
  `IDLE`); staying `READY` is also legal and makes G's next sit start the
  trade at once.
- Send it only while G's `CAFE` stream is running (G in the room). During the
  walk-in, before G's `2FFE`, keep zero bursts (A7).
- A Flipper `READY` slot is not forced `BUSY` by `EXITING_ROOM`, so if G
  leaves instead (`0x17`), the Flipper's `CAFE 0017` is honoured at once (A7
  rules still work).

### B1.5 The transition: `sub_8083AAC` [`Task_StartWiredTrade`] (`cable_club.c`)

| Case | G does | Wire effect |
|---|---|---|
| 0 | `LockPlayerFieldControls`, `FadeScreen(1,0)`, **`ClearLinkCallback_2`** (`link.c`: `gLinkCallback = NULL`) | G stops building `CAFE` from the next frame; G also stops frame-skipping (`main.c: UpdateLinkAndCallCallbacks` skips only while `gLinkCallback == sub_8007B44`) |
| 1 | waits for the fade | G sends zeros |
| 2 | `m4aMPlayAllStop`, `SetCloseLinkCallback` | `link.c: sub_8008350` [`LinkCB_ReadyCloseLink`] sends `5FFF` once when `gLastRecvQueueCount == 0` (refreshed only every 256 serial IRQs on the child, A7/U8: up to ≈ 0.5 s) |
| 3 | when `!gReceivedRemoteLinkPlayers` (set by `CloseLink`) → `SetMainCallback2(sub_8047CD8)` [`CB2_StartCreateTradeMenu`] | G closed: `sub_800837C` [`LinkCB_WaitCloseLink`] ran `CloseLink` → `DisableSerial` once both `5FFF` flags were set |

**Flipper on the wire:**

1. After sending `CAFE 0016`, send **all-zero bursts** (no `CAFE 0011`). A
   non-zero Flipper command every burst keeps G's receive queue from being
   empty when it is sampled and can delay or block G's `5FFF` (inferred, T10).
2. On G's `5FFF`: send one `5FFF` (A7). The `u8_array_3002B78[0]` flag
   persists until G's next `OpenLink`, so an early `5FFF` also works.
3. Keep bursting zeros until slot 1 reads 0xFFFF for a whole burst; then
   handshake mode (A3, one 0xB9A0 transfer per frame).

### B1.6 Re-link as 0x1122 (`trade.c: sub_8047EC0` [`CB2_CreateTradeMenu`])

G timeline, c = frame of G's `CloseLink` (read from the state machines;
±1–2 frames, T5):

| G frame | G state | Event |
|---|---|---|
| c | `sub_8083AAC` case 3 | `SetMainCallback2(sub_8047CD8)` |
| c+1 | `sub_8047CD8` | `SetMainCallback2(sub_8047EC0)` |
| c+2 | 0 | graphics/window setup, `ResetTasks` |
| **c+3** | 1 | `gLinkType = 0x1122`, **`OpenLink`**, `CreateMon(&gEnemyParty[0..5], species 0)`, **`CreateTask(sub_8083C50)`** [`Task_WaitForLinkPlayerConnection`]: 300 frames → `CloseLink` + `CB2_LinkError` unless `gReceivedRemoteLinkPlayers` |
| c+4 | 2 (12 frames) | `LinkMain1` `START0`: `DisableSerial` |
| c+8 | 2 | `Task_TriggerHandshake` fired → `EnableSerial`, `HANDSHAKE` (G answers 0x0000, then 0xB9A0) |
| c+16 | 3 | child: `GetLinkPlayerCount_2() >= sub_800820C()` [`GetSavedPlayerCount`] (= 2 from Part A `sub_80081C8` [`SaveLinkPlayers`]) → state 4 |
| ≥ c+17 | 4 | waits `gReceivedRemoteLinkPlayers && IsLinkPlayerDataExchangeComplete()` → `CalculatePlayerPartyCount` → state 5 (party exchange, B2) |
| c+303 | — | 300-frame deadline for the Flipper's `LinkPlayerBlock` |

**Flipper sequence** (A3/A4/A6 mechanics):

| Step | F sends | G sends | Note |
|---|---|---|---|
| H | handshake | 0x0000, 0xB9A0 | from ≈ c+8 |
| L1 | `2222 1122 0 …` | zeros | word 1 is not read by G |
| L2 | `BBBB 003C 0080`, 5 × `8888` (own `LinkPlayerBlock`) | `BBBB 003C 0081`, 2 idle, 5 × `8888` | G's block has `linkType = 0x1122`, `id = seat` |
| check | parse G's block (magic, `linkType == 0x1122`) | | |

`LinkPlayerBlock` for this and every later relink (A5 layout):

| Field | Value | Checked by G when |
|---|---|---|
| `magic1`/`magic2` | `"GameFreak inc."` | every relink, `link.c: ProcessRecvCmds` → `CB2_LinkError` |
| `version` | 0x4002 | never (Ruby) |
| `lp_field_2` | 0x0000 (0x8000 only by opt-in, B4.5) | read only from the **0x1144** block (`trade.c: sub_804BA64`) |
| `trainerId`, `name` | **byte-identical to the Part A block** | `link.c: sub_800826C` [`CheckLinkPlayersMatchSaved`] on the **0x1111** relink only (compares with `gSavedLinkPlayers` saved at the Part A linkup); the trade menu shows `gLinkPlayers[0].name` (`trade.c: sub_8047CE8`) |
| `gender` | same as Part A | only read at room spawn (Part A); keep constant |
| `linkType` | **must equal G's**: 0x1122 here, 0x1144 in B4, 0x1111 in B6 | `link.c: IsLinkPlayerDataExchangeComplete`: mismatch = **silent hang** (state 4 never passes; the 300-frame task was already destroyed by `gReceivedRemoteLinkPlayers`) |
| `id` | 0 | never |
| `language` | 2 | name conversion only |

## B2. Trade-menu party exchange (`trade.c: sub_8048D44` [`BufferTradeParties`])

### B2.1 Rounds

`unk_0075` is the round state (reset in `sub_8047EC0` state 0). Child = G
(`mpId == 1`); `sub_8007E9C` [`SendBlockRequest`] is master-only, so the
Flipper sends every `CCCC`. All blocks come from / land in
`gBlockSendBuffer` / `gBlockRecvBuffer[slot]` (≤ 256 bytes, A4).

| Round | G copies into `gBlockSendBuffer` (case) | `CCCC` type | Size (hex) | Chunks | G stores the Flipper's block into (case) |
|---|---|---|---|---|---|
| 1 | `gPlayerParty[0..1]`, 200 B (0) | 1 | 200 (0x00C8) | 15 | `gEnemyParty[0..1]` (3) |
| 2 | `gPlayerParty[2..3]` (4) | 1 | 200 | 15 | `gEnemyParty[2..3]` (6) |
| 3 | `gPlayerParty[4..5]` (7) | 1 | 200 | 15 | `gEnemyParty[4..5]` (9) |
| 4 | `gSaveBlock1.mail[0..5]` + 4 bytes = 220 B (10) | 3 | 220 (0x00DC) | 16 | `gUnknown_02029700[0..5]` [`gLinkPartnerMail`], 216 B (12) |
| 5 | `gSaveBlock1.giftRibbons`, 11 B (13) | 4 | 40 (0x0028) | 3 | `unk_00b5[0..10]` (15) |
| — | | | | | case 16: Shedinja nickname fix, then `sub_804AF84` [`SaveTradeGiftRibbons`] |

Other cases: 1 (round 1 only) = wait `IsLinkTaskFinished`, then
**`ResetBlockReceivedFlags` if any block flag is set**; 2/5/8/11/14 =
`CCCC` (master only; child just advances); 3/6/9/12/15 = wait for
`GetBlockReceivedStatus() == 0b11` (Flipper's block **and** G's own echo).
No timeout and no input in any case: a stall freezes G on a black screen
until reset (soft reset works here).

Flipper per round: `CCCC <type>` (one burst), then its own block (`BBBB
<size> 0080` in the next burst, chunks back to back, A4); receive G's block
(header, 2 idle, chunks).

### B2.2 Timing: when G is ready for each `CCCC`

G frame G0 = the frame G processes the later of the two final chunks of the
previous step. Main-loop order is `LinkMain1` (dequeue one command) →
`LinkMain2` (`ProcessRecvCmds`, `gLinkCallback`) → callbacks
(`main.c: UpdateLinkAndCallCallbacks`).

| `CCCC` | Hard constraint (from code) | Failure if violated | Safe margin |
|---|---|---|---|
| Round 1 | G must process `CCCC` at frame **≥ G0 + 3** (G0 = `LinkPlayerBlock`s complete; G1 = case 0 copy; G2 = case 1). If `CCCC` is processed at ≤ G2, G is still sending its party block when case 1 runs, waits, then clears both block flags → case 3 waits forever (deadlock). Before G0 it is either refused (`sBlockSend.active`, G never sends) or sends stale `gBlockSendBuffer` bytes and then hits the same case-1 deadlock | deadlock, black screen | **≥ 8 idle bursts** after the burst carrying the last chunk of both `LinkPlayerBlock`s (the GBA master leaves ≈ 5 frames) |
| Rounds 2–5 | after G's previous block finished (`sBlockSend.active` clear, true once its last chunk is on the wire) and G's copy for this round (frame R0+1) precedes chunk 1 (header frame + 3) | refused request (deadlock) or stale bytes | **≥ 5 idle bursts** after both blocks of the previous round are complete |

- Idle (all-zero) bursts create no queue entries on G, so G's delay is real
  time plus its receive-queue backlog (≈ 0–2 entries at matched rates, A7).
  The margins absorb a backlog of ≈ 4 (inferred, T1).
- Blocks from one slot overwrite each other in `gBlockRecvBuffer`; never
  start the next block before the previous one is complete.

### B2.3 Block contents

**Rounds 1–3 (200 bytes):** bytes 0–99 = party slot 2k, bytes 100–199 =
party slot 2k+1, each a 100-byte party `struct Pokemon` exactly as stored
(encrypted, B2.4). Empty slot = **100 zero bytes**.

**Round 4 (220 bytes):** `struct MailStruct` is **36 bytes** in the binary
(agbcc pads structs to 4 bytes: `SaveBlock1.mail[16]` spans 0x2B4C–0x2D8C =
576 = 16 × 36 in `include/global.h`; `DayCareMail.names` at 0x24), so
6 × 36 + 4 = 220 = the request size (inferred, T3).

| Off in block | Size | Content |
|---|---|---|
| 36·m + 0x00 | 18 | `words[9]` easy-chat words (u16) |
| 36·m + 0x12 | 8 | `playerName` |
| 36·m + 0x1A | 4 | `trainerId[4]` |
| 36·m + 0x1E | 2 | `species` |
| 36·m + 0x20 | 2 | `itemId` (121–132 = mail items, `constants/items.h`) |
| 36·m + 0x22 | 2 | padding |
| 216–219 | 4 | G: first 4 bytes of `mail[6]`; Flipper: don't care |

m = 0..5. G only reads entry m = the Flipper's traded mon's `mail` byte
(B4.4). If the Flipper sends no mail, any content works; zeros or the
`mail_data.c: ClearMailStruct` pattern (words 0xFFFF, name 0xFF, id 0,
species 1, item 0).

**Round 5 (40 bytes):** bytes 0–10 = `giftRibbons[11]`; bytes 11–39 are
stale on G and ignored by the receiver. **Flipper: send zeros.**
`sub_804AF84` [`SaveTradeGiftRibbons`] copies each non-zero Flipper byte
into G's save where G's byte is 0, and it is saved by the next link save.

### B2.4 Party `struct Pokemon` (100 bytes, `include/pokemon.h`)

Little-endian; bitfields are allocated from bit 0 (agbcc, LSB-first).

| Off | Size | Field | Flipper rule |
|---|---|---|---|
| 0x00 | 4 | `personality` | selects substruct order, gender, nature, shininess (with `otId`), Unown form |
| 0x04 | 4 | `otId` | low 16 = TID, high 16 = SID |
| 0x08 | 10 | `nickname` | Gen III charset, 0xFF-terminated if shorter than 10 (`pokemon_2.c: GetBoxMonData` reads up to 10 or EOS) |
| 0x12 | 1 | `language` | 2 (English); 1 adds JP control codes (`text.c: ConvertInternationalString`) |
| 0x13 | 1 | bit 0 `isBadEgg`, bit 1 `hasSpecies`, bit 2 `isEgg`, bit 3 `blockBoxRS`, bits 4–7 unused | `isBadEgg` 0; `hasSpecies` 1 for a real mon (set by `SetMonData(SPECIES)`; read by `field_poison.c`, `daycare.c`, ...); `isEgg` = substruct `isEgg`; `blockBoxRS` 0 |
| 0x14 | 7 | `otName` | 0xFF-terminated if shorter |
| 0x1B | 1 | `markings` | 0 |
| 0x1C | 2 | `checksum` | B2.4.3 |
| 0x1E | 2 | `unknown` | 0 |
| 0x20 | 48 | `secure`: 4 × 12-byte substructs, encrypted | B2.4.1–3 |
| 0x50 | 4 | `status` | 0 = healthy |
| 0x54 | 1 | `level` | must match `experience` |
| 0x55 | 1 | `mail` | **0xFF** unless sending mail (B4.4) |
| 0x56 | 2 | `hp` | ≤ `maxHP` |
| 0x58 | 2 | `maxHP` | as `pokemon_1.c: CalculateMonStats` |
| 0x5A | 2 | `attack` | " |
| 0x5C | 2 | `defense` | " |
| 0x5E | 2 | `speed` | " |
| 0x60 | 2 | `spAttack` | " |
| 0x62 | 2 | `spDefense` | " |

G uses the party-only fields 0x50–0x63 as sent; nothing recalculates them
(except evolution, `evolution_scene.c`, or later game events).

#### B2.4.1 Substructs (plain text)

**Growth (type 0)**

| Off | Size | Field |
|---|---|---|
| 0 | 2 | `species` (internal index) |
| 2 | 2 | `heldItem` |
| 4 | 4 | `experience` |
| 8 | 1 | `ppBonuses` (2 bits per move, move 1 = bits 0–1) |
| 9 | 1 | `friendship` (egg: steps-to-hatch counter) |
| 10 | 2 | unused |

**Attacks (type 1)**: 0 `moves[4]` (u16 each), 8 `pp[4]` (u8 each).

**EVs/condition (type 2)**: 0 HP, 1 Atk, 2 Def, 3 Spe, 4 SpA, 5 SpD EVs, 6
cool, 7 beauty, 8 cute, 9 smart, 10 tough, 11 sheen (u8 each).

**Misc (type 3)**

| Off | Bits | Field |
|---|---|---|
| 0 | 8 | `pokerus` |
| 1 | 8 | `metLocation` |
| 2 | u16: 0–6 | `metLevel` (0 = hatched) |
| | 7–10 | `metGame` (1 S, 2 R, 3 E, 4 FR, 5 LG, 15 Colosseum/XD; `constants/global.h`) |
| | 11–14 | `pokeball` (ball item id 1–12) |
| | 15 | `otGender` |
| 4 | u32: 0–4, 5–9, 10–14, 15–19, 20–24, 25–29 | IVs HP, Atk, Def, Spe, SpA, SpD |
| | 30 | `isEgg` (what `MON_DATA_IS_EGG` reads) |
| | 31 | `altAbility` |
| 8 | u32: 0–2, 3–5, 6–8, 9–11, 12–14 | cool/beauty/cute/smart/tough contest ribbon rank |
| | 15–26 | champion, winning, victory, artist, effort, marine, land, sky, country, national, earth, world |
| | 27–31 | `eventLegal`; **bit 31 = FRLG/Emerald "fateful encounter"** (Mew/Deoxys obedience and tradability; unused by Ruby, `include/pokemon.h` comment) |

#### B2.4.2 Order (`pokemon_2.c: GetSubstruct`)

Order of the four 12-byte blocks in `secure`, by `personality % 24`
(G = Growth, A = Attacks, E = EVs, M = Misc; derived from the
`SUBSTRUCT_CASE` table, which gives each type's position):

| n | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| order | GAEM | GAME | GEAM | GEMA | GMAE | GMEA | AGEM | AGME | AEGM | AEMG | AMGE | AMEG |
| **n** | **12** | **13** | **14** | **15** | **16** | **17** | **18** | **19** | **20** | **21** | **22** | **23** |
| order | EGAM | EGMA | EAGM | EAMG | EMGA | EMAG | MGAE | MGEA | MAGE | MAEG | MEGA | MEAG |

#### B2.4.3 Encryption and checksum

- Encrypt: each of the 12 little-endian u32 words of `secure` ^=
  `personality ^ otId` (`pokemon_2.c: EncryptBoxMon`/`DecryptBoxMon`).
- `checksum` = sum of the 24 plaintext u16 of `secure`, mod 0x10000
  (`pokemon_1.c: CalculateBoxMonChecksum`; order-independent).
- Mismatch: the first `GetMonData` of any field > `MON_DATA_10` sets
  `isBadEgg`, box `isEgg` and substruct `isEgg` in G's RAM copy
  (`pokemon_2.c: GetBoxMonData`). Species then reads as `SPECIES_EGG` (412),
  the name as "Bad EGG", and the slot counts as a party member. If traded, the
  Bad Egg is saved into G's game. **Never send a bad checksum.**
- An all-zero slot decrypts to zeros with checksum 0 = valid, species 0 =
  empty. A 0xFF-filled slot is a Bad Egg.

### B2.5 What Ruby uses and checks in the Flipper's party

| Item | Code | Effect |
|---|---|---|
| Party count | `pokemon_2.c: CalculateEnemyPartyCount` (`sub_8047EC0` state 6) | counts slots until the first `MON_DATA_SPECIES == 0`; later slots are ignored. Keep the Flipper's mons contiguous from slot 0 |
| Checksum | `GetBoxMonData` | Bad Egg (above) |
| Shedinja | `sub_8048D44` case 16 | species `SPECIES_SHEDINJA` (303), language ≠ 1, nickname == "ヌケニン" → nickname set to "SHEDINJA" |
| Icons | `sub_8047EC0` state 6, `party_menu.c: CreateHeldItemIcons` | `SPECIES2` (egg → egg icon), `personality`; held item ≠ 0 → item or mail icon (`ItemIsMail`) |
| Names/levels | `sub_804A41C`, `sub_804A51C` | nickname (egg/Bad Egg substitute), gender (species ratio + personality), `level` |
| HP bars | `sub_804AE3C` | `hp`, `maxHP` |
| "Battle-able" | `sub_804ACF4` [`ComputePartyTradeableFlags`] | computed for both parties but only the **local** party is ever checked (`sub_80499F0` in `sub_8049860`/`sub_8049A20`) |
| Moves | `sub_804A33C` | `gMoveNames[move]` (unchecked index; egg → "????") |
| Summary | `sub_804997C` → `pokemon_summary_screen.c` | reads everything: species tables, nature, ability, met data, ribbons, OT; met location text via `CopyLocationName` only when `metLevel == 0` or `metLocation < 88` and OT matches, or `metGame` is R/S/E and `metLocation < MAPSEC_NONE` |
| **Not checked** | — | species range, national dex, Kanto/Johto species, eggs, legality, partner's remaining battlers, held item validity. Ruby trusts the partner |

**Flipper party rules** (each prevents a crash, a Bad Egg, or bad save data):

1. Species 1–251 or 277–411 (`constants/species.h`: 252–276 are the
   `SPECIES_OLD_UNOWN_*` placeholders, 412 is `SPECIES_EGG`, `NUM_SPECIES` = 412).
2. Moves 0–354 (`NUM_MOVES` = 355), held item 0–348 (`ITEMS_COUNT` = 349).
3. Never hold `ITEM_ENIGMA_BERRY` (175): the Cable Club refuses it for real
   players (`data/scripts/cable_club.inc`, `contest_util.c:
   GetNameOfEnigmaBerryInPlayerParty`) and its hold effect reads G's own
   e-Reader berry (`pokemon_3.c: GetEvolutionTargetSpecies`).
4. `mail` = 0xFF unless the round-4 block has a matching entry; a mail item
   without mail data, or mail data with a non-mail item, is not a state the
   game creates.
5. Correct checksum and encryption; `hasSpecies` = 1; box `isEgg` =
   substruct `isEgg`.
6. Eggs: species must be a valid base form with no `EVO_TRADE` method (Ruby
   does not exclude eggs from trade evolution, T6).
7. Mew (151) and Deoxys (410): set bit 31 of the ribbon word if the mon may
   later travel to FRLG/Emerald.

**Party of 1:** `gEnemyPartyCount = 1`; the partner column shows one mon;
cursor slots 7–11 are inactive (`sub_8048C70`). G still trades normally; the
Flipper offers slot 0 (`DDDD 0000`). Ruby never checks whether the partner
keeps a battle-able mon (the real master checks only its own party). **A
party of 0 is not allowed:** `DDDD` would index an unset icon sprite
(`sub_80491E4` → `sub_8049ED4`).

### B2.6 What the Flipper receives

G's full party (its `gPlayerParty[0..5]`, encrypted as stored, empty slots
species 0) in rounds 1–3, G's party mail `mail[0..5]` in round 4, G's
`giftRibbons` in round 5. This is the **only** time G's Pokemon data is sent:
the traded mon is G's slot i from this exchange, i = the index in G's later
`AABB` block (B3). The summary screen (`PSS_MODE_NO_MOVE_ORDER_EDIT`) cannot
change G's party in between.

## B3. Selecting and confirming (the Flipper is the arbiter)

### B3.1 The 20-byte trade block

`#define Trade_SendData(ptr) SendBlock(..., ptr->linkData, 20)` (`trade.c`);
`linkData` is `u16[10]`. On the wire (A4):

| Burst | Words |
|---|---|
| header | `BBBB 0014 0080` (Flipper) / `BBBB 0014 0081` (G), then 5 × 0 |
| chunk 1 | `8888 <code> <arg> w2 w3 w4 w5 w6` |
| chunk 2 | `8888 w7 w8 w9` + 4 don't-care words |

Only `code` (word 0) is read, plus `arg` (word 1) for `AABB` and `DDDD`. G
does not clear `linkData` between sends, so other words are stale.
The codes are block payload, never word 0 of a burst.

### B3.2 What G sends

| Code [Emerald `LINKCMD_`] | G sends it from (Ruby) [Emerald] | `arg` | G then shows / waits |
|---|---|---|---|
| `AABB` [`READY_TO_TRADE`] | "TRADE" on own mon: `sub_8049860` [`CB_ProcessSelectedMonInput`] → `sub_8049620` [`SetReadyToTrade`] (immediate) | G's party index 0–5 | "Link standby... Please wait.", state 5 (no input) |
| `EEAA` [`REQUEST_CANCEL`] | CANCEL → "Cancel trade?" YES: `sub_8049BC0` [`CB_ProcessCancelTradeInput`] (queued 5 frames, `sub_804AADC` [`QueueAction`]) | 0 | "Waiting for your friend to finish...", state 100 (no input) |
| `BBBB` [`INIT_BLOCK`] | "Is this trade okay?" YES: `sub_8049AC0` [`CB_ProcessConfirmTradeInput`] → `sub_8049A20` [`CommunicateWhetherMonCanBeTraded`] (only if `IsLinkTaskFinished`) | stale | "Link standby...", state 100 |
| `BBCC` [`READY_CANCEL_TRADE`] | NO or B in that prompt (`sub_8049AC0`); or 180 frames after YES if G would be left without a battle-able mon (`sub_8049A20`) | stale | state 100 |
| `0000` (word 0 = 0) | R button in the main menu: `sub_8049680` [`CB_ProcessMenuInput`] sends `linkData[i] = i` | 1 | debug; **ignore** |

G's own-party rule (only check on either side in Ruby): "TRADE" on a mon
that is G's last non-egg, non-fainted mon → "That's your only POKéMON for
battle." locally, nothing sent (`sub_8049860`, `sub_80499F0`
[`CheckValidityOfTradeMons`]).

### B3.3 What the master sends and what G does

The master's logic is `sub_80492D8` [`Leader_HandleCommunication`] (state
pairs `unk_0084`/`unk_0085` for select, `unk_0086`/`unk_0087` for confirm;
1 = ready/yes, 2 = cancel/no; resolves only when both are non-zero). G
handles slot 0 blocks in `sub_80491E4` [`Follower_ReadLinkBuffer`] from
`callback1` (`sub_80494D8` [`CB1_UpdateLink`]), **in any menu state, with no
check that the code was expected**:

| Flipper sends | Master rule (both sides chose) | G reaction |
|---|---|---|
| `DDDD <j>` [`SET_MONS_TO_TRADE`] | both picked a mon; `arg` = master's slot 0–5 | `unk_008a = j + 6`; shows both mons; after the slide-in, "Is this trade okay?", after 120 frames YES/NO (`DisplayMessageAndContinueTask` [`CB_InitConfirmTradePrompt`]) |
| `EECC` [`PARTNER_CANCEL_TRADE`] | master picked a mon, G cancelled | "Your friend wants to trade POKéMON."; **A** → menu (`sub_8049D44` [`CB_HandleTradeCanceled`]) |
| `DDEE` [`PLAYER_CANCEL_TRADE`] | master cancelled, G picked; **or** any `BBCC`/no in the confirm step | "The trade has been canceled."; **A** → menu |
| `EEBB` [`BOTH_CANCEL_TRADE`] | both cancelled | fade out, state 11 → `SetCloseLinkCallback` (`5FFF`) → room (B6) |
| `CCDD` [`START_TRADE`] | both said yes | fade out, state 10 → `sub_8048A50` stores (own index, `unk_008a`) in `gUnknown_020297D8` [`gSelectedTradeMonPositions`], `SetCloseLinkCallback` → trade (B4) |

The master's own `AABB` stays local (`sub_8049620`: `unk_0084 = 1`); its
`EEAA`/`BBBB`/`BBCC` go out as blocks and are read back from its own echo
(`sub_80490BC` [`Leader_ReadLinkBuffer`] slot 0). G ignores those codes from
slot 0, so **the Flipper never sends its own choice, only the result codes**.

### B3.4 Flipper policy (recommended)

| G sent | Flipper decides | Flipper replies |
|---|---|---|
| `AABB i` | offer slot j (default 0) | `DDDD j` (j < Flipper party count) |
| `AABB i` | user refuses / nothing to offer | `DDEE` |
| `EEAA` | let G leave (default) | `EEBB` |
| `EEAA` | keep G in the menu | `EECC` |
| `BBBB` (after its `DDDD`) | user confirms | `CCDD` |
| `BBBB` | user refuses | `DDEE` |
| `BBCC` | — | `DDEE` |
| `0000` / anything else | — | nothing |

Unsolicited (no G block pending):

| Flipper sends | Use | Caveat |
|---|---|---|
| `EEBB` | force G out of the menu (B7) | lost if G is in the summary screen: `sub_80484F4` [`CB2_ReturnToTradeMenu`] state 0 resets `unk_007b` to 0. Retry if no `5FFF` arrives within ≈ 3 s (inferred, T2) |
| `DDEE` | unstick G from "Link standby..." (states 5/100) | G needs **A** to return to the menu |

Timing:

- Send each reply **≥ 5 bursts after the burst carrying the last chunk of G's
  block** (the real master waits 5 frames: `QueueAction(5, 0)`). G's
  `LinkCB_BlockSendEnd` must have run (1 frame) before `CCDD`, because
  `sub_8048A50`'s `SetCloseLinkCallback` is ignored while `gLinkCallback` is
  busy, which hangs G in state 13.
- Send each reply **exactly once**. A second `DDDD` restarts G's
  confirm sequence (state 7); a second `EEBB` is harmless.

**Never:**

- `CCDD` unless the latest exchange was `AABB i` → `DDDD j` → `BBBB` with no
  reply yet. G starts the trade on any `CCDD`, using whatever its cursor and
  `unk_008a` hold (could be CANCEL or a partner slot): wrong mon traded or
  garbage.
- `DDDD` except in reply to `AABB`.

Ruby quirks the arbiter cannot see:

- If G pressed R less than ≈ 5 frames before "TRADE"/YES, its `AABB`/`BBBB`
  is dropped by its own `InitBlockSend` (`sBlockSend.active`) or skipped
  (`IsLinkTaskFinished` guard in `sub_8049A20`/`sub_8049AC0`): G sits in
  "Link standby" forever. Offer a manual "unstick" (`DDEE`).
- The `BBCC` from the B button in `sub_8049AC0` has no `IsLinkTaskFinished`
  guard; the `NO` path does.

### B3.5 Happy-path sequence

| Step | Flipper | G |
|---|---|---|
| 1 | menu idle: zero bursts | zero bursts |
| 2 | | block `AABB i` |
| 3 | ≥ 5 zero bursts, block `DDDD j` | |
| 4 | zeros | slide-in, prompt, 120 frames, player answers |
| 5 | | block `BBBB` |
| 6 | ≥ 5 zero bursts, block `CCDD` | |
| 7 | zeros | fade; `5FFF` (≤ ≈ 0.5 s after the fade) |
| 8 | `5FFF`, zeros until slot 1 = 0xFFFF | closes |

## B4. The trade

### B4.1 `CCDD` → 0x1144 re-link (`trade.c: sub_804B41C` [`CB2_LinkTrade`])

c = G's `CloseLink` frame (after the `5FFF` pair):

| G frame | G state | Event |
|---|---|---|
| c | `sub_8048A90` [`CB_StartLinkTrade`] | `callback1 = NULL`, `SetMainCallback2(sub_804B41C)` |
| c+1 | 0 | `ResetTasks`, `CloseLink`, `gLinkType = 0x1144`, trade graphics |
| c+2 | 1 | `OpenLink` (**no 300-frame task**) |
| c+3…c+63 | 2 | 61 frames; the link layer handshakes meanwhile (`EnableSerial` ≈ c+7) |
| c+64 | 3 | child → state 4 |
| | 4 | waits `gReceivedRemoteLinkPlayers && IsLinkPlayerDataExchangeComplete()` forever (`sub_804B24C` is a debug counter, no error); screen is black |
| | 5–12 | load both mon pictures from `gPlayerParty[own]` and `gEnemyParty[partner % 6]` (`sub_804B2D0` [`LoadTradeMonPic`]), fade in → `sub_804DB84` [`CB2_UpdateLinkTrade`] |

Flipper: handshake, `2222 1144`, own `LinkPlayerBlock` with **`linkType =
0x1144`**, same name/TID, `lp_field_2` per B4.5. Then zero bursts.

### B4.2 Animation

`sub_804C29C` [`DoTradeAnim`] runs a fixed sequence; **no link traffic and no
deadline** in Ruby. Case 70 runs `sub_804E1DC` [`CheckPartnersMonForRibbons`]:
if the Flipper's mon has any of the 12 champion…world ribbons,
`FLAG_SYS_RIBBON_GET` is set on G. Keep zero bursts.

### B4.3 `ABCD` / `DCBA`

| G code | Ruby | Effect |
|---|---|---|
| animation done | `sub_804DB84` | `sub_804BA94` [`TradeMons`] (swap, B4.4), `linkData[0] = ABCD`, `unk_00bd = 1`, `sub_804BB78` [`HandleLinkDataSend`] sends the 20-byte `ABCD` block **once, only if `IsLinkTaskFinished`** (falls through to `unk_00bd = 0`) → `sub_804DC18` [`CB2_WaitTradeComplete`] |
| waiting | `sub_804DC18` → `sub_804D63C` [`HandleLinkDataReceive`] | slot 0 `DCBA` → `SetMainCallback2(sub_804D588)` [`CB2_TryLinkTradeEvolution`]; slot 0 `ABCD` → `unk_009c = 1` (unused by the child) |
| master | `sub_804DC18` (`mpId == 0`) | sends `DCBA` once both its own echo and the child's `ABCD` arrived |

**Flipper: on G's `ABCD`, send one 20-byte `DCBA` block** (≥ 2 bursts after
its last chunk). Sending `ABCD` is optional (G ignores it). **Never send
`DCBA` before G's `ABCD`**: `sub_804D63C` also runs during the animation, so
an early `DCBA` makes G skip `TradeMons` and save the untraded party.

### B4.4 What G writes (`sub_804BA94` [`TradeMons`])

Runs on G with (own index i, partner index j = `gUnknown_020297D8[1] % 6`):

1. If G's sent mon has `mail != 0xFF`: `ClearMailStruct(&gSaveBlock1.mail[mail])`.
2. Swap `gPlayerParty[i]` ↔ `gEnemyParty[j]` (whole 100 bytes).
3. Received mon, if not an egg: `friendship = 70` (re-encrypts, new checksum).
4. If the Flipper's mon had `mail = m != 0xFF`: `mail_data.c: GiveMailToMon2`
   with the Flipper's round-4 entry m: takes G's first `mail[0..5]` slot with
   `itemId == 0` (one is always free after step 1), copies the 36-byte entry,
   sets the mon's `mail` byte to that slot and its **held item to the mail's
   `itemId`**. A stale `mail` byte (e.g. 0) with an empty entry therefore
   wipes the mon's real held item: send 0xFF.
5. `sub_804BA18` [`UpdatePokedexForReceivedMon`]: if not an egg, seen +
   caught for `SpeciesToNationalPokedexNum(species)`.
6. `sub_804BA64` [`TryEnableNationalDexFromLinkPartner`] (B4.5).

Not touched: OT, met location/level/game, ball, IVs, EVs, ribbons, stats.
Then `sub_804D588` [`CB2_TryLinkTradeEvolution`]: `gSoftResetDisabled =
TRUE`; `pokemon_3.c: GetEvolutionTargetSpecies(mon, 1, 0)`: `EVO_TRADE`, or
`EVO_TRADE_ITEM` when the held item matches (item consumed); Everstone
(`HOLD_EFFECT_PREVENT_EVOLVE`) blocks; no egg check. Evolution runs
`TradeEvolutionScene` (may wait for player input to learn moves) with
`gCB2_AfterEvolution = sub_804DC88`, else straight to the save. Keep zero
bursts throughout; no deadline.

To mirror a real partner on its own copy, the Flipper applies steps 2–4 to
G's mon (friendship 70 unless egg, attach G's round-4 mail entry), and trade
evolution if it wants that fidelity.

### B4.5 National Dex (`lp_field_2`)

`sub_804BA64`: if `gLinkPlayers[mpId ^ 1].lp_field_2 == 0x8000` (the
Flipper's **0x1144** block) and `gReceivedRemoteLinkPlayers`,
`event_data.c: EnableNationalPokedex` (`nationalMagic = 0xDA`,
`VAR_NATIONAL_DEX = 0x302`, `FLAG_SYS_NATIONAL_DEX`, dex mode 1). Saved by the
link save, permanent. Real FRLG/Emerald always send 0x8000
(`fr:/em: link.c: InitLocalLinkPlayer`; their own check is commented out), so
it is legitimate, but default to **0x0000** and make it an explicit opt-in.

## B5. Link save (`trade.c: sub_804DC88` [`CB2_SaveAndEndTrade`])

### B5.1 States

`save.c` names: `sub_8125D80` [`LinkFullSave_Init`], `sub_8125DA8`
[`LinkFullSave_WriteSector`], `sub_8125DDC` [`LinkFullSave_ReplaceLastSector`],
`sub_8125E04` [`LinkFullSave_SetLastSectorSignature`].

| State | G does | Frames | Wire |
|---|---|---|---|
| 0 | "Link standby..." | 1 | |
| 1 | `sub_80084A4` [`SetLinkStandbyCallback`] | | **sync 1**: G `2FFE` when `gLastRecvQueueCount == 0` |
| 100/101 | wait `IsLinkTaskFinished`; after 180 frames moves to 101 (same wait, no error) | ∞ | F answers `2FFE` |
| 2 | "SAVING... DON'T TURN OFF THE POWER." | 1 | |
| 50 | `IncrementGameStat(GAME_STAT_POKEMON_TRADES)`, `sub_8125D80`: `SaveSerializedGame`, sector rotation + save counter + 1 (RAM only) | 1 | |
| 51 → 52, ×14 | 5-frame wait, then one sector: `sub_812550C` [`HandleWriteIncrementalSector`] erases and programs chunk k = 0..12 into the **other** slot; 14th call returns done | 14 × 6 + flash time | zeros |
| 4 | `sub_8125DDC`: chunk 13 written except byte 0xFF8 (low byte of signature 0x08012025) (`sub_81255B8` [`HandleReplaceSector`]) | 1 + flash | |
| 40 | wait | 51 | |
| 41 | `sub_80084A4` | | **sync 2**: G `2FFE` |
| 42 | when sync 2 completes: `sub_8125E04` programs byte 0xFF8 = 0x25 (**commit**), `gSoftResetDisabled = FALSE` | 1 | F answers `2FFE` |
| 5 | wait, then `sub_80084A4` | 61 | **sync 3**: G `2FFE` |
| 6 | sync 3 done → fade out | | F answers `2FFE` |
| 7 | fade done → `FadeOutBGM(3)` | | |
| 8 | BGM stopped → `SetCloseLinkCallback` | | G `5FFF`, F answers |
| 9 | `!gReceivedRemoteLinkPlayers` → `sub_804E144` [`CB2_FreeTradeAnim`] → `sub_8047CD8` | | 0x1122 relink (B6) |

`if (!HasLinkErrorOccurred()) RunTasks();` only gates tasks; a link error
replaces `callback2` with `CB2_LinkError` (`link.c: CheckErrorStatus`;
`gSuppressLinkErrorMessage` is FALSE after every `OpenLink`).

### B5.2 Why an interrupted link save never destroys Ruby's save

- Two 14-sector slots; the link save writes the slot **not** holding the
  latest save (`save.c: WriteSingleChunk`: slot = `gSaveCounter % 2` after the
  increment). The previous save is never touched.
- Before the commit byte, the new slot has at least one invalid sector
  (`GetSaveValidStatus` needs all 14 chunk ids with valid signature +
  checksum). The rotation offset of 2 between saves of the same slot means
  surviving old sectors never complete the set (derived from
  `WriteSingleChunk` sector numbering).
- Boot with one slot OK and one slot partial: `GetSaveValidStatus` returns
  `SAVE_STATUS_ERROR` with the OK slot's counter → `main_menu.c` prints
  "The save file is corrupt. The previous save file will be loaded." and
  loads the **pre-trade** save. Next save overwrites the broken slot.
- A link error can only stop the state machine between sector writes
  (each sector is written inside one `callback2` call). A partial sector
  needs power loss or a hard reset; soft reset is disabled from
  `sub_804D588` state 0 to state 42.
- During flash operations IRQs stay enabled (`libs/agb_flash.c:
  StartFlashTimer` sets `REG_IME = 1`), so G keeps servicing serial IRQs and
  VBlank; only its main loop stalls for the duration of a sector erase +
  program (duration not known, T4). Non-zero Flipper commands queue up
  harmlessly (< 50).

### B5.3 Flipper obligations and abort windows

Obligations from `DCBA` until G's final `5FFF`:

1. Keep 9-transfer bursts every frame (never ≥ 11 frames without a
   transfer, A2), all-zero except the replies below.
2. Answer **each** G `2FFE` with **exactly one** `2FFE`, after it arrives.
   The flag (`u8_array_3002B70[0]`) persists until the sync completes, so an
   extra or early `2FFE` lets a later sync finish without the Flipper.
3. Answer the final `5FFF`.
4. **No blocking work on the link thread** (storage writes, UI). A stall in
   the unsafe window below is the only way the Flipper can make Ruby show
   the corrupt-save message.

| Window (G side) | Link error / power loss / Flipper gone | Result on G after reboot |
|---|---|---|
| Animation, evolution, sync 1 not complete | nothing written | previous save, no message; trade did not happen on G |
| **sync 1 complete → commit (state 52 … 42), ≈ 140 frames + flash time** | partial new slot | **"The save file is corrupt..."**, previous (pre-trade) save loaded |
| Waiting in sync 2 forever (Flipper never answers) | same as above once G is reset | same |
| After commit (sync 3, fade, `5FFF`) | save complete | trade saved; G shows a link error if the link breaks |

Flipper's own bookkeeping (recommendation, inferred):

- On `CCDD`: journal "pending trade (give slot j, receive G's slot i +
  mail)".
- On G's sync 2 `2FFE`: answer immediately. G commits 1–2 frames after
  receiving it.
- On G's sync 3 `2FFE` (61 frames after the commit; only sent from state 5):
  the trade is saved on G → finalize the Flipper's storage.
- Link lost between answering sync 2 and seeing sync 3: ambiguous (G most
  likely committed); ask the user to check the GBA.

## B6. After the trade, trading again, cancelling, back to the room

### B6.1 Back to the trade menu (0x1122)

From `sub_804DC88` state 9: `sub_804E144` (waits for the fade) →
`sub_8047CD8` → `sub_8047EC0` state 1 `OpenLink` ≈ 4 frames after G's
`CloseLink`. Same as B1.6 (300-frame deadline) and B2 (all five rounds),
with the post-trade parties: G's slot i now holds the Flipper's former mon
(friendship 70, possibly evolved, mail re-indexed). The Flipper sends its
own post-trade party (G's mon in slot j); **any party may be sent here**,
which is a free refresh point (B7).

### B6.2 Trading again

Repeat B3 → B5. Nothing carries over except G's saved link players and
`gSpecialVar_0x8004 = 3` (set by the counter script, A6; untouched by the
trade code).

### B6.3 Cancel from the menu (`EEBB`) → room

| G step | Code | Wire |
|---|---|---|
| `EEBB` received | `sub_80491E4` | fade, state 11 |
| fade done | `sub_8049D9C` [`CB_InitExitCanceledTrade`] → `SetCloseLinkCallback` | G `5FFF`; F answers |
| closed | `sub_8049DC4` [`CB_ExitCanceledTrade`] → `SetMainCallback2(sub_805465C)` [`CB2_ReturnToFieldFromMultiplayer`] | |
| | `sub_805465C`: `sub_8054F70` [`ResetAllPlayerLinkStates`] (**all slots `IDLE`**, the Flipper is no longer `READY`), `callback1 = sub_8055354` [`CB1_OverworldLink`], `gFieldCallback = sub_8080A3C` [`FieldCB_ReturnToFieldCableLink`] | |
| | `overworld.c: CB2_ReturnToFieldLink` → `sub_8054A9C` [`ReturnToFieldLink`] (14 states; waits while `sub_8055870` sees ≥ 3 queued commands) | room redrawn **without** reloading object events: both avatars stay where they were (G on its seat, F at (5,8)) |
| field callback | `task_mpl_807DD60` [`Task_ReturnToFieldCableLink`] → `sub_8083664`: `gLinkType = 0x1111` (`gSpecialVar_0x8004 == 3`) → `sub_8083710` [`Task_ReestablishLink`]: `OpenLink`, `ResetLinkPlayers`, **`sub_8083C50` (300 frames)** | ≈ 16–18 frames after G's `CloseLink` (inferred, T5) |
| ≥ 10 frames | `sub_8083760` → child → `sub_80837EC` [`Task_ReestablishLinkAwaitConfirmation`] | waits for the exchange |
| exchange complete | **`sub_800826C`** [`CheckLinkPlayersMatchSaved`]: for slots 0–1, `trainerId` equal and `StringCompareWithoutExtCtrlCodes(name)` equal to the Part A copy, else `CloseLink` + `CB2_LinkError`; then `sub_8007B14` [`StartSendingKeysToLink`] | G starts `CAFE` (no `2FFE` on this path) |
| | `task_mpl_807DD60` fades in, unlocks controls | |

Flipper: `5FFF` answer, handshake, `2222 1111`, `LinkPlayerBlock` with
`linkType = 0x1111` and the **Part A name and TID/SID**, then room idle (A7:
zero bursts, answer `2FFE`/`5FFF`/`CAFE 0017`). To trade again, G must sit
again (B7) and the Flipper must send `CAFE 0016` again.

## B7. Editing between trades (product requirement)

**Answer:** the master can make G leave the menu without input, but it
cannot make G re-enter it. The party exchange only runs when
`sub_8047EC0` builds the menu, reached only from `sub_8083AAC` (both
seated) or from `sub_804E144` (after a completed trade's save). Nothing in
`sub_80491E4` re-runs `sub_8048D44`.

Cleanest flow:

| Step | Who | What |
|---|---|---|
| 1 | Flipper (or GBA player: CANCEL → A → "Cancel trade?" YES, Flipper replies `EEBB`) | Flipper sends `EEBB` (unsolicited; retry if no `5FFF`, B3.4) |
| 2 | both | `5FFF`, 0x1111 relink (B6.3); G is in the room on its seat |
| 3 | user | edits the Flipper's party; G idles indefinitely |
| 4 | GBA player | **two D-pad presses**: step off the seat and back on (from (4,5): N (4,4), S (4,6) or W (3,5); from (7,5): N, S or E (8,5); east/west of each seat is the table, `data/layouts/TradeCenter/map.bin` collision bits). Standing on the seat does not re-trigger (B1.1). No A press |
| 5 | Flipper | on G's `CAFE 0016`, sends `CAFE 0016` |
| 6 | both | trade menu with a fresh exchange showing the edited party |

- Without leaving the menu, edits can be shown only by completing a trade:
  any edit made before the post-save 0x1122 exchange (B6.1) appears
  automatically, with no GBA input.
- If the Flipper sends `CAFE 0016` while G is not seated, G's sit later
  starts the menu immediately; harmless.

How long the Flipper can idle (bursts must never stop: ≥ 11 frames without a
transfer = `LAG_SLAVE` error everywhere, A2):

| G state | Game-level limit | Notes |
|---|---|---|
| Room (`CAFE`) | none | G advances one frame per burst carrying a command (A7) |
| Seated, "Please wait... B: Cancel" | none | GBA player can cancel with B |
| Trade menu, browsing | none | |
| Trade menu, after `AABB`/`EEAA`/`BBBB`/`BBCC` | none, **but G has no input** | Flipper must reply |
| Relink 0x1122 / 0x1111 | **300 frames** from G's `OpenLink` to G's `gReceivedRemoteLinkPlayers` | `cable_club.c: sub_8083C50` |
| Relink 0x1144, animation, evolution | none | black screen if stuck |
| Save | none, but stalls in the unsafe window risk the corrupt-save message (B5.3) | |

## B8. Timeouts and error checks on G (Part B)

| Check | Code | Result |
|---|---|---|
| ≥ 11 VBlanks without a serial IRQ | `link.c: LinkVSync` → `LAG_SLAVE` | link-error screen (`CB2_LinkError`, reset needed) |
| Checksum / framing / hardware error / queue full | `link.c: DoRecv`, `SerialCB`, `EnqueueSendCmd` (A2) | link-error screen |
| `LinkPlayerBlock` magic wrong | `link.c: ProcessRecvCmds` 0x8888 | link-error screen |
| No `gReceivedRemoteLinkPlayers` 300 frames after `OpenLink` (0x1122, 0x1111) | `cable_club.c: sub_8083C50` | `CloseLink` + link-error screen |
| Name or TID differs from the Part A copy (0x1111 only) | `link.c: sub_800826C` | `CloseLink` + link-error screen |
| Flipper `linkType` ≠ G's | `link.c: IsLinkPlayerDataExchangeComplete` (trade menu state 4, `sub_804B41C` state 4, `sub_80837EC`) | **silent hang** |
| Player count < 2 | `sub_8047EC0` state 3 | wait (300-frame task then errors) |
| Missing/out-of-order `CCCC` or block | `sub_8048D44` | silent hang (black) |
| Missing reply to `AABB`/`EEAA`/`BBBB`/`BBCC` | menu states 5/100 | silent hang ("Link standby...") |
| Missing `DCBA` | `sub_804DC18` | silent hang (end of animation) |
| Missing `2FFE` / `5FFF` | `link.c: sub_80084F4`, `sub_800837C` | silent hang (sync 2 = unsafe, B5.3) |
| Damaged flash sector | `save.c: sub_8125DA8/DDC/E04` → `save_failed_screen.c: DoSaveFailedScreen` | save-failed screen (hardware) |
| `DDEE` | `sub_80491E4` | recoverable: "The trade has been canceled." + A |
| `EECC` | `sub_80491E4` | recoverable: "Your friend wants to trade POKéMON." + A |
| G's own last battler | `sub_8049860` | recoverable, local: "That's your only POKéMON for battle." |
| Bad Egg (bad checksum) | `pokemon_2.c: GetBoxMonData` | no error; Bad Egg shown and saved if traded |

All errors in Part B are hard errors: `gSuppressLinkErrorMessage` is only set
during the counter linkup (A2).

## B9. Flipper master checklist (Part B)

1. Room: zero bursts; answer `2FFE`, `5FFF`, `CAFE 0017` (A7).
2. On G's `CAFE 0016`: one `CAFE 0016`; then zero bursts only.
3. On G's `5FFF`: one `5FFF`; zeros until slot 1 = 0xFFFF; handshake mode.
4. Relink: handshake, `2222 <type>`, own `LinkPlayerBlock` (Part A bytes,
   `linkType` = G's: 0x1122 / 0x1144 / 0x1111), parse G's; all within 300
   frames of G's `OpenLink`.
5. Party exchange (0x1122 only): ≥ 8 idle bursts, `CCCC 0001` + own 200-byte
   block; ≥ 5 idle after each round: `CCCC 0001` ×2 more, `CCCC 0003` + 220
   bytes, `CCCC 0004` + 40 bytes. Keep G's party, mail, ribbons.
6. Menu: reply to each G 20-byte block per B3.4, ≥ 5 bursts after it, once.
7. After `CCDD`: `5FFF`, relink 0x1144 (`lp_field_2` opt-in), zeros through
   the animation; on `ABCD` send `DCBA`.
8. Save: answer three `2FFE` one-for-one, zeros otherwise, no stalls; on
   sync 3 finalize the Flipper's storage; answer the last `5FFF`; back to 4
   (0x1122) and 5.
9. Leave the menu: `EEBB` (reply or unsolicited) → `5FFF` → relink 0x1111
   (Part A name/TID) → room (step 1).

---

# Part C: FireRed/LeafGreen as the GBA (Flipper still master)

Only differences from Part B are listed. Link layer, command words, block
sizes, link types, seats and save structure are otherwise the same
(`fr: include/link.h`: `LINKTYPE_TRADE` 0x1111, `_CONNECTING` 0x1122,
`_SETUP` 0x1133, `_DISCONNECTED` 0x1144; `fr: src/link.c: sBlockRequests`
200/200/100/220/40).

## C1. What to claim in the Flipper's `LinkPlayer`

FRLG layout (`fr: include/link.h`), 28 bytes as in Ruby:

| LP off | Field | Flipper value | Why |
|---|---|---|---|
| 0x00 | `version` | **0x4004** (FireRed) or 0x4005 (LeafGreen) | receivers use `version & 0xFF`; an FRLG version skips the R/S/E gate (`fr: trade.c: GetGameProgressForLinkTrade` returns `TRADE_BOTH_PLAYERS_READY`), selects the FRLG avatar (`fr: overworld.c: CreateLinkPlayerSprite`) and the FRLG trainer-card format |
| 0x02 | `lp_field_2` | 0x8000 | what FRLG sends (`fr: link.c: InitLocalLinkPlayer`); FRLG never reads it (`fr: trade_scene.c: TryEnableNationalDexFromLinkPartner` is commented out) |
| 0x04 | `trainerId` | same rules as Ruby | `fr: link.c: CheckLinkPlayersMatchSaved` on 0x1111 (TID and `StringCompare(name)`) |
| 0x08 | `name[8]` | ≤ 7 chars + EOS, pad 0xFF | `fr: InitLocalLinkPlayer` uses `StringCopy` into 8 bytes |
| 0x10 | `progressFlags` | **0x01** | bits 0–3 = has National Dex (`IsNationalPokedexEnabled`), 0x10 = `FLAG_SYS_CAN_LINK_WITH_RS`. The FRLG GBA refuses to send eggs or species > 151 to a non-R/S partner whose low nibble is 0 (`fr: trade.c: CanTradeSelectedMon`). 0xF0 bits matter only for an Emerald claim (`GetGameProgressForLinkTrade`) |
| 0x11 | `neverRead` | 0 | |
| 0x12 | `progressFlagsCopy` | = 0x10 value | the receiver overwrites it with `progressFlags` (`fr: link.c: ConvertLinkPlayerName`); for an R/S `version` all three bytes are zeroed (`fr: link.c: ProcessRecvCmds`) |
| 0x13 | `gender` | 0/1 | avatar |
| 0x14 | `linkType` | as Part B | |
| 0x18 | `id` | 0 | |
| 0x1A | `language` | 2 | |

G's own block tells the Flipper G's progress: byte 0x10 bit 0 = G has the
National Dex.

## C2. Trainer card (request type 2, 100-byte block)

- FRLG always sends the FRLG `struct TrainerCard` (0x60 = 96 bytes, `.version
  = GAME_VERSION` at 0x38; `fr: trainer_card.c:
  TrainerCard_GenerateCardForLinkPlayer`).
- Receiver (`fr: cable_club.c: Task_LinkupAwaitTrainerCardData`): partner
  `version & 0xFF` is 4/5 → copies the whole 0x60-byte FRLG card; otherwise
  copies `TrainerCardRSE` (0x38, the Ruby layout of A5) and sets `.version`
  from the `LinkPlayer`.
- Flipper claiming FRLG: send an FRLG card.

| Off | Field (`fr: include/trainer_card.h`) | Flipper |
|---|---|---|
| 0x00–0x37 | `struct TrainerCardRSE rse` (A5 layout; `caughtAllHoenn` means "all Kanto" here) | as A5; `stars` ≤ 4 (indexes `sKantoTrainerCardPals`) |
| 0x38 | `version` u8 | 4 or 5 (selects the Kanto card, `GetCardType`) |
| 0x3A | `hasAllFrontierSymbols` | 0 |
| 0x3C | `berryCrushPoints` u32 | 0 |
| 0x40 | `unionRoomNum` u32 | 0 |
| 0x44 | `berriesPicked` u32 | 0 |
| 0x48 | `jumpsInRow` u32 | 0 |
| 0x4C | `shouldDrawStickers` | 0 |
| 0x4D | `hasAllMons` | 0 |
| 0x4E | `monIconTint` | 0 |
| 0x4F | `facilityClass` | 0 (read only in the Union Room) |
| 0x50 | `stickers[3]` | 0 |
| 0x54 | `monSpecies[6]` u16 | 0 (no icons) |
| 0x60–0x63 | — | 0 |

## C3. Species, National Dex and fateful encounter

| Rule | Code | Effect for the Flipper |
|---|---|---|
| FRLG player without National Dex cannot offer `SPECIES_OR_EGG` > 151 (eggs = 412 included) | `fr: trade.c: CanTradeSelectedMon` | local; G shows "can't be traded now", sends nothing |
| Partner (Flipper) not R/S and `progressFlagsCopy & 0xF == 0` → G cannot offer eggs or species > 151 | same | send `progressFlags = 0x01` |
| G's own Mew (151)/Deoxys (410) without fateful bit → cannot offer | same, `MON_DATA_MODERN_FATEFUL_ENCOUNTER` = Misc ribbon word bit 31 (`fr: include/pokemon.h`) | local |
| **Flipper's** offered Mew/Deoxys without the bit | `fr: trade.c: CheckValidityOfTradeMons` from `CommunicateWhetherMonCanBeTraded` on YES | G: "The other TRAINER's POKéMON can't be traded now." (`gText_OtherTrainersPkmnCantBeTraded`), `BBCC` 180 frames later → reply `DDEE`. Set bit 31 |
| Receiving species > 151 without National Dex | none on the receive side | FRLG relies on the **sender** refusing (row 2). Mirror it: if G's byte 0x10 bit 0 is 0, do not offer eggs or species > 151 (inferred policy, T14) |
| Trade evolution to a non-Kanto species without National Dex | `fr: pokemon.c: GetEvolutionTargetSpecies` `EVO_TRADE_ITEM` | still evolves, item **not** consumed (quirk) |
| National Dex unlock from `lp_field_2` | commented out | none |
| Pokedex on receive | `fr: trade_scene.c: UpdatePokedexForReceivedMon` | seen + `HandleSetPokedexFlag(caught, personality)` |

## C4. Trade menu and trade scene differences

| Item | FRLG | Ruby |
|---|---|---|
| Party exchange | `fr: trade.c: BufferTradeParties`: cases 2/6/10/14/18 wait 11 frames before each `CCCC`, on master and child alike (no `id == 0` test) | no delay |
| Round-1 `CCCC` rule | **≥ G0 + 4**: `fr: trade.c: CB2_CreateTradeMenu` case 5 (wireless only, one frame on a cable) runs between case 4 and `BufferTradeParties`, one frame more than Ruby; FRLG masters leave ≈ 14 frames → use **≥ 15 idle bursts** | ≥ G0 + 3; ≥ 8 |
| Codes, 20-byte blocks, master logic, 5-frame reply delay | identical (`fr: trade.c: Leader_HandleCommunication`, `Follower_ReadLinkBuffer`, `QUEUE_DELAY_DATA` 5) | |
| `5FFF` word 1 | `gReadyCloseLinkType`: 32 before the trade (`CB_WaitToStartTrade`), 12 on both-cancel (`CB_InitExitCanceledTrade`), else 0 (`fr: link.c: BuildSendCmd`); receivers ignore it | 0 |
| 0x1144 relink | `fr: trade_scene.c: CB2_LinkTrade` states 3/4 call `CheckLinkTimeout`: **300 frames → `CloseLink` + `CB2_LinkError`** | no timeout |
| `ABCD`/`DCBA` | identical (`HandleLinkDataSend` single-shot quirk included) | |
| `TradeMons` | friendship 70, mail, Pokedex; no National Dex | + `lp_field_2` |
| Seat/linkup timeout | `fr: cable_club.c: Task_WaitForLinkPlayerConnection` 300 frames (480 if `REVISION >= 0xA`, the Switch build) | 300 |
| Linkup gate failure | `PLAYER_NOT_READY`/`PARTNER_NOT_READY` → plain `CloseLink`, no `5FFF` (`fr: cable_club.c: Task_LinkupExchangeDataWithLeader`) | n/a |

## C5. Trade Center room

- Same map geometry (`fr: data/maps/TradeCenter/map.json`, layout 12×9):
  warps (5,8) and (6,8), seats (4,5) `TradeCenter_EventScript_Chair0` and
  (7,5) `…_Chair1`, attendant at (9,6); table at (5,5)–(6,5).
- Entry: `fr: data/scripts/cable_club.inc: CableClub_EventScript_TradeCenter`
  (≥ 2 mons, no Enigma Berry, save, `TryTradeLinkup`) →
  `CableClub_EventScript_EnterTradeCenter`: `VAR_0x8004 = USING_TRADE_CENTER`
  (3), door at PC 2F (9,1), `setwarp MAP_TRADE_CENTER, 5, 8` +
  `DoCableClubWarp`.
- Spawn identical: player i at (5 + i, 8) (`fr: overworld.c:
  SpawnLinkPlayers`), so F (5,8), G (6,8).
- Walk-in identical: `fr: field_fadetransition.c:
  FieldCB_ReturnToFieldWirelessLink` → `Task_ReturnToFieldRecordMixing`:
  `2FFE` standby, then `StartSendingKeysToLink`.
- Seat and trade start identical (`fr: cable_club.c: Task_EnterCableClubSeat`,
  `Task_StartWiredTrade`); room re-establish identical
  (`Task_ReestablishLink…`, `CheckLinkPlayersMatchSaved`).
- Avatar: FRLG sprite if the partner's `version & 0xFF` is 4/5, else the R/S
  sprite (`fr: overworld.c: CreateLinkPlayerSprite`).

## C6. Link save (`fr: trade_scene.c: CB2_SaveAndEndTrade`)

| Item | FRLG (cartridge, `REVISION` 0/1) | Ruby |
|---|---|---|
| Structure | `fr: save.c: LinkFullSave_Init/WriteSector/ReplaceLastSector/SetLastSectorSignature`: same 14 sectors, 5-frame gaps, signature-byte commit | same |
| Syncs | 3 × `2FFE` (states 1, 41, 5) | 3 |
| Before sync 2 | 51 frames, then **master only** waits `Random() % 30` more (child 0) | 51 |
| Soft reset | disabled until state 9 (after the link closed) | re-enabled at the commit |
| Extra | quest-log event, `SetContinueGameWarpStatusToDynamicWarp` | — |
| Switch build (`REVISION >= 0xA`) | no 5-frame gaps; a **4th** `2FFE` (states 43/44) right before the commit | n/a |

---

# Uncertain

- **T1 Party-exchange margins.** The round-1 deadlock (CCCC processed ≤ G0+2)
  and the ≥ 8 / ≥ 5 burst margins are derived from `sub_8048D44` state order
  and `main.c` frame order, not measured. They assume a G receive-queue
  backlog ≤ ≈ 4. FRLG margin (≥ 15) mirrors its own master's delay.
- **T2 Unsolicited `EEBB`.** Lost when G is in the summary screen
  (`sub_80484F4` resets `unk_007b`); the palette fade it starts may black out
  the summary until the player exits. Retry after ≈ 3 s without `5FFF` is a
  suggestion. Not tested.
- **T3 `MailStruct` = 36 bytes.** Inferred from `SaveBlock1`/`DayCareMail`
  offsets and agbcc's 4-byte struct padding; consistent with the 220-byte
  request (6 × 36 + 4). pret's C shows 34 bytes of fields.
- **T4 Flash timing.** Main-loop stall per sector (erase + 4 KiB program) is
  chip-dependent and not measured; only "IRQs stay on" is from code.
- **T5 Frame offsets.** G's "c+N" timelines (close → `OpenLink` →
  `EnableSerial`) are read from state machines; ±1–2 frames; the 0x1111 path
  offset (≈ 16–18) depends on task ordering in `CB2_Overworld`.
- **T6 Egg trade evolution.** `sub_804D588` does not check `IS_EGG`; an egg
  whose species has `EVO_TRADE` would presumably evolve. Not tested.
- **T7 Seat mirroring policy.** Mirroring `0x16`/`0x1D`→`0x1A` is a design
  choice; staying `READY` is equally valid by the code.
- **T8 Zero bursts after `READY`.** That continuous non-zero `CAFE 0011`
  from the Flipper can delay G's `5FFF` is inferred from the
  `gLastRecvQueueCount` sampling (A7/U8), not observed.
- **T9 2FFE/5FFF latency.** G may need up to ≈ 0.5 s to send each (U8).
- **T10 Corrupt-save message.** The boot path for a partial slot is read from
  `save.c: GetSaveValidStatus` and `main_menu.c`; not reproduced.
- **T11 Summary-screen partner data.** Exact crash conditions for odd met
  locations, abilities (`altAbility` on a one-ability species) or PP above
  maximum were not traced; the party rules in B2.5 avoid them.
- **T12 Trade-menu UI after unsolicited `EEBB` in states 1/4** (sub-menus
  open): assumed cleaned up by the field reload.
- **T13 FRLG card fields.** Only `version`, `stars`, stickers, icons and
  `facilityClass` usage was checked; other display paths not traced.
- **T14 FRLG without National Dex receiving non-Kanto mons.** FRLG's
  receiver accepts them; consequences (e.g. `SpeciesToPokedexNum` returning
  0xFFFF without the National Dex) not analysed. Hence the mirror policy.
- **T15 Re-sit directions.** Free tiles around the seats come from collision
  bits in `map.bin` only (metatile behaviours not checked).
- **T16 Trade animation length.** Not computed; the Flipper waits for
  `ABCD` without a deadline.

---

# Corrections to link-protocol.md

Errors in its Part B outline (that file is unchanged):

| Where | Says | Correct |
|---|---|---|
| B3 table, round 4 | 6 × 34-byte `MailStruct` + 4 = 208 bytes, rest stale | 36-byte entries (padding), 6 × 36 + 4 = 220 bytes, nothing stale (T3) |
| B4 | "The master's own choice stays local (`sub_8049620`)" | only `AABB` is local; `EEAA`/`BBBB`/`BBCC` are sent and read from the master's own echo. No effect on the Flipper (it sends none) |
| B6 | "Soft reset is disabled throughout" | from `sub_804D588` state 0 to the commit (`sub_804DC88` state 42) |
| B5 | `DCBA` after `ABCD` | also: an early `DCBA` skips `TradeMons` (B4.3) |
| A8/B1 | idle `CAFE 0011` acceptable | after `CAFE 0016`, send zeros so G can send `5FFF` (B1.5) |

---

# Appendix: Ruby → Emerald names (Part B)

| Ruby | Emerald |
|---|---|
| `trade.c: sub_8047CD8` / `sub_8047EC0` / `sub_80484F4` | `CB2_StartCreateTradeMenu` / `CB2_CreateTradeMenu` / `CB2_ReturnToTradeMenu` |
| `sub_8048A14` / `sub_8048A50` / `sub_8048A90` / `sub_8048AB4` | `CB_FadeToStartTrade` / `CB_WaitToStartTrade` / `CB_StartLinkTrade` / `CB2_TradeMenu` |
| `sub_8048D44` / `sub_80490BC` / `sub_80491E4` / `sub_80492D8` / `sub_80494D8` | `BufferTradeParties` / `Leader_ReadLinkBuffer` / `Follower_ReadLinkBuffer` / `Leader_HandleCommunication` / `CB1_UpdateLink` |
| `sub_8049620` / `sub_8049680` / `sub_8049860` / `sub_804997C` / `sub_80499F0` | `SetReadyToTrade` / `CB_ProcessMenuInput` / `CB_ProcessSelectedMonInput` / `CB_ShowTradeMonSummaryScreen` / `CheckValidityOfTradeMons` |
| `sub_8049A20` / `sub_8049AC0` / `sub_8049BC0` / `sub_8049C8C` / `sub_8049CC4` | `CommunicateWhetherMonCanBeTraded` / `CB_ProcessConfirmTradeInput` / `CB_ProcessCancelTradeInput` / `CB_SetSelectedMons` / `CB_PrintIsThisTradeOkay` |
| `DisplayMessageAndContinueTask` / `sub_8049D44` / `sub_8049D9C` / `sub_8049DC4` / `sub_8049DE0` | `CB_InitConfirmTradePrompt` / `CB_HandleTradeCanceled` / `CB_InitExitCanceledTrade` / `CB_ExitCanceledTrade` / `RunTradeMenuCallback` |
| `sub_8049E9C` / `sub_8049ED4` / `sub_804AA0C` / `sub_804AADC` / `sub_804AB30` | `SetSelectedMon` / `DrawSelectedMonScreen` / `PrintTradeMessage` / `QueueAction` / `DoQueuedActions` |
| `sub_804ACF4` / `sub_804AE3C` / `sub_804AF84` | `ComputePartyTradeableFlags` / `ComputePartyHPBarLevels` / `SaveTradeGiftRibbons` |
| `sub_804B41C` / `sub_804B2D0` / `sub_804C29C` | `CB2_LinkTrade` / `LoadTradeMonPic` / `DoTradeAnim` (`em: trade.c`) |
| `sub_804BA18` / `sub_804BA64` / `sub_804BA94` / `sub_804BB78` | `UpdatePokedexForReceivedMon` / `TryEnableNationalDexFromLinkPartner` / `TradeMons` / `HandleLinkDataSend` |
| `sub_804D588` / `sub_804D63C` / `sub_804DB84` / `sub_804DC18` | `CB2_TryLinkTradeEvolution` / `HandleLinkDataReceive` / `CB2_UpdateLinkTrade` / `CB2_WaitTradeComplete` |
| `sub_804DC88` / `sub_804E144` / `sub_804E1DC` | `CB2_SaveAndEndTrade` / `CB2_FreeTradeAnim` / `CheckPartnersMonForRibbons` |
| `gUnknown_020297D8` / `gUnknown_02029700` | `gSelectedTradeMonPositions` / `gLinkPartnerMail` (`fr:`), `gTradeMail` (`em:`) |
| `save.c: sub_8125D80` / `sub_8125DA8` / `sub_8125DDC` / `sub_8125E04` | `LinkFullSave_Init` / `LinkFullSave_WriteSector` / `LinkFullSave_ReplaceLastSector` / `LinkFullSave_SetLastSectorSignature` |
| `sub_812550C` / `sub_812556C` / `sub_81255B8` / `WriteSomeFlashByteToPrevSector` | `HandleWriteIncrementalSector` / `HandleReplaceSectorAndVerify` / `HandleReplaceSector` / `CopySectorSignatureByte` |
| `overworld.c: sub_80554A4` / `sub_805546C` / `sub_8055468` / `sub_80553E4` | `KeyInterCB_SetReady` / `KeyInterCB_Ready` / `KeyInterCB_ExitingSeat` / `KeyInterCB_DeferToEventScript` |
| `sub_80554E4` / `sub_80554BC` / `sub_8055648` / `sub_805568C` | `KeyInterCB_SendExitRoomKey` / `KeyInterCB_WaitForPlayersToExit` / `TryGetTileEventScript` / `TryInteractWithPlayer` |
| `sub_8054F70` / `sub_8054F88` / `sub_8054FC0` / `sub_8055D38` | `ResetAllPlayerLinkStates` / `AreAllPlayersInLinkState` / `IsAnyPlayerInLinkState` / `MovementStatusHandler_TryAdvanceScript` |
| `sub_8054A9C` / `sub_8055870` | `ReturnToFieldLink` / `Overworld_IsRecvQueueAtMax` |
| `field_fadetransition.c: task_mpl_807DD60` / `sub_8080A3C` | `Task_ReturnToFieldCableLink` / `FieldCB_ReturnToFieldCableLink` (`em: field_screen_effect.c`) |
