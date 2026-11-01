# Gen III cartridge access: BIOS multiboot, helper program, save layout

Reference for reading and writing the save of a real Pokemon Ruby / Sapphire /
FireRed / LeafGreen / Emerald cartridge, and dumping its ROM, from the Flipper.
The data path is Flipper → Game Link module → AGB-005 cable → GBA. The GBA BIOS
receives a small **helper** program by multiboot. The helper runs from the GBA's
RAM with the Pokemon cart still inserted, and serves save and ROM requests from
the Flipper over the same cable.

User flow: plug the cable in (Flipper on the parent plug). Power the GBA on with
the cart inserted while holding **Start+Select**. The BIOS then waits for
multiboot instead of booting the cart. The Flipper sends the helper and talks to it.
To play again, the user power-cycles the GBA normally.

Companion to `docs/gen3/link-protocol.md` (the Cable Club protocol; §A2 there
describes the Flipper's multiplayer-parent driver).

## Sources and conventions

| Source | Version |
|---|---|
| GBATEK, https://problemkaputt.de/gbatek.htm | downloaded 2026-10-25 |
| pret/pokeruby, pret/pokeemerald, pret/pokefirered (read-only clones) | `5784633c`, `731ad5bf`, `ce6406d8` (same as link-protocol.md) |
| afska/gba-link-connection `lib/LinkCableMultiboot.hpp` (a GBA master that implements the whole SWI 25h data phase in software, in its `Async` class) | `vLinkCableMultiboot/v8.0.3`, master branch 2026-10-25 |
| FIX94/gba-link-cable-dumper `gba/source/main.c`, `gba/source/libSave.c` | master 2026-10-25 |
| kwsch/PKHeX `PKHeX.Core/Saves/SAV3*.cs` | master 2026-10-25 |
| Bulbapedia "Save data structure (Generation III)" | revision of 2025-03-23 (read through the Internet Archive; the live site returns 403) |
| Game Boy hardware database (gbhwdb.gekkio.fi), cartridge entries | 2026-10-25 |

- Citations: **GBATEK "Section Name"**. For pret it is `file: Function`, in
  **pokeruby** unless prefixed `em:` (pokeemerald) or `fr:` (pokefirered).
- **(inferred)** marks a conclusion not stated by a source. Every one is
  collected under [Uncertain](#uncertain) as M1…M22 (M, to keep them apart from
  link-protocol.md's U1…).
- Word = 16 bits. Multiplayer slot *i* = `SIOMULTIi`. P = Flipper (parent, slot
  0); H = GBA (BIOS, then the helper; child, slot 1). Offsets are hex.
- "Verified" means I checked it myself: by running code (the multiboot
  encoder/CRC cross-check in §2.6, the helper skeleton build in §4), by reading
  pret code, or by reading GBATEK text.

### Key numbers

| Item | Value | Source |
|---|---|---|
| Link mode | MultiPlay, 115200 bps; P on the small (parent) plug, H = child 1 | §1 |
| Multiboot client bit for child 1 | `0x02` (handshake reply `0x7202`) | GBATEK "BIOS Multi Boot (Single Game Pak)" |
| Multiboot image | 0xC0-byte header + payload; payload 0x100..0x3FF40 bytes, multiple of 0x10; header loaded to 0x02000000, payload to 0x020000C0 | same |
| Multiboot keys (MultiPlay) | CRC init `0xFFF8`, CRC xor `0xA517`, data xor `0x6465646F`, seed multiplier `0x6F646573` | same |
| Save chip of genuine RSE/FRLG | 128 KB flash: Macronix MX29L010 (ID `0x09C2`) or Sanyo LE26FV10N1TS (ID `0x1362`) | `src/libs/agb_flash_1m.c: IdentifyFlash`, gbhwdb |
| Flash window | 0x0E000000..0x0E00FFFF, 8-bit only, 2 banks of 64 KB, WAITCNT SRAM field = 3 (8 cycles) | GBATEK "GBA Cart Backup Flash ROM", `src/libs/agb_flash.c: ReadFlash` |
| Save layout | 32 × 4 KB sectors: slot A = 0–13, slot B = 14–27, Hall of Fame 28–29, extra 30–31 | `src/save.c` header comment |
| Sector footer | `0xFF4` id u16, `0xFF6` checksum u16, `0xFF8` signature u32 `0x08012025`, `0xFFC` counter u32 | `src/save.c: struct SaveSector` |
| ROM | 16 MiB for all five games (128 Mbit mask ROMs) | gbhwdb (MX23L12807 / MX23L12806) |

---

# 1. The cable question

**Answer: on an AGB-005 cable, Normal mode cannot carry data in both directions
between P1 and P2. Multiboot must use MultiPlay mode. The Flipper must hold the
small (parent) plug and act as the multiplayer parent. The GBA is child 1.**
The existing `mlink_hw_furi.c` driver already has exactly this role.

Evidence:

1. Cable wiring (GBATEK "AUX Link Port", "Cable Diagrams"): the small plug's SI
   is tied to GND. The small plug's SO goes to the big plug's SI. The big plug's
   SO goes only to the middle socket (towards a third GBA). SD and SC are bussed
   through to every plug. "The GBA which is connected to a Small Plug is master,
   the slaves are all connected to Large Plugs."
2. Normal mode on this cable (GBATEK "SIO Normal Mode", "Using Normal mode for
   One-Way Multiplayer communication"): "When using normal mode with
   multiplay-cables, data isn't exchanged between first and second GBA as
   usually. Instead, data is shifted from first to last GBA (the first GBA
   receives zero, because master SI is shortcut to GND)." Normal-mode multiboot
   needs replies from the slave (`720x`, `NN0x`, `73cc`, the CRC). On this cable
   those replies come out of P2's SO into the middle socket and never reach P1.
3. JOY Bus mode, the third mode the BIOS slave understands, also sends the
   GBA's replies on SO (GBATEK "SIO JOY BUS Mode": "SI and SO pins are data
   lines"). The GBA would also have to be the slave there. So JOY Bus is ruled
   out on AGB-005 for the same reason **(inferred, M1)**.
4. GBATEK "AUX Link Port", "Multi-Boot Connection": "MultiBoot (SingleGamepak)
   is typically using Multi-Play communication, in this case it is important
   that the Small plug is connected to the master/sender."

What this means for the roles:

| | Flipper | GBA |
|---|---|---|
| Plug | small (parent): SI grounded (the driver's cable guard checks this) | big |
| Multiplayer slot / ID | 0 (parent): drives SC, sends first, then pulls SO low | 1 (child): `SIOCNT` bits 4–5 = 1 after the first transfer |
| Multiboot client bit `x`/`y` | — | `0x02` (bit 1 = child 1) |
| After boot | keeps clocking every transfer | the helper stays in MultiPlay as child 1. It never starts a transfer and answers the Flipper's transfers |

- Baud: MultiPlay maxes out at 115200 (GBATEK "SIO Multi-Player Mode",
  `SIOCNT` bits 0–1 = 3). The child samples at its own configured rate, so the
  Flipper cannot clock faster.
- A Game Boy Color link cable has SI and SO crossed, so it can carry Normal
  mode. Cartridge uses it when the module has no SD line: see §7.
- (Superseded by §7.) Normal mode would need a cable with SI/SO crossed between
  the two plugs. Examples: a modified AGB-005 or an 8-bit-style cable (GBATEK
  "AUX Link Port", "Using a GBA 8bit cable for 32bit communication"). That is
  not our hardware.
- A possible later speed-up: once the helper runs, it can switch the port to
  General-Purpose mode (`RCNT` bit 15 = 1, bit 14 = 0; GBATEK "SIO
  General-Purpose Mode") and bit-bang SC/SD, which are bussed straight through,
  plus SO→SI. Out of scope here **(inferred, M2)**.

---

# 2. BIOS multiboot in MultiPlay mode (parent side)

The BIOS of the GBA held at Start+Select is the slave. A cartridge master would
run the "initiation" itself and then call SWI 25h for the data phase. The
Flipper has no BIOS, so it implements **both** parts. GBATEK "BIOS Multi Boot
(Single Game Pak)" describes the whole exchange. afska's
`LinkCableMultiboot::Async` is a working software implementation of the SWI 25h
part, and the values below match it.

## 2.1 Image requirements

| Item | Requirement | Source |
|---|---|---|
| Header | 0xC0 bytes, the same layout as a cartridge header. It must hold "valid data … including a copy of the Nintendo logo, correct header CRC" | GBATEK "BIOS Multi Boot", "Multiboot Slave Header" |
| Header load address | 0x02000000–0x020000BF | same |
| Payload | the bytes after the header, loaded to 0x020000C0 and up (max 0x0203FFFF). Absolute addresses are linked at 0x02000000 | same, "Multiboot Slave Program/Data" |
| Payload length L | multiple of 0x10, min 0x100, max 0x3FF40 (total file 0x1C0..0x40000 bytes) | GBATEK SWI 25h; afska `MIN_ROM_SIZE = 0x100 + 0xC0`, `MAX_ROM_SIZE = 256 * 1024` |
| 0x000 | ARM branch; ignored by multiboot | GBATEK "GBA Cartridge Header" |
| 0x004–0x09F | Nintendo logo, 156 bytes, exact copy. The first word is `0x51AEFF24` (FIX94 `main.c: getGameSize` checks it) | same |
| 0x0A0 / 0x0AC / 0x0B0 | title (12) / game code (4) / maker (2), uppercase ASCII | same |
| 0x0B2 | **must be 0x96** | same |
| 0x0B3, 0x0B4, 0x0B5–0x0BB, 0x0BC | 0, 0, zero, version | same |
| 0x0BD | **complement**: `chk = 0; for i in 0xA0..0xBC: chk -= hdr[i]; chk = (chk - 0x19) & 0xFF` | same |
| 0x0C0 | ARM `B start`: the MultiPlay/Normal entry point | GBATEK "Additional Multiboot Header Entries" |
| 0x0C4 | boot mode, **written by the BIOS** (3 = MultiPlay). Put no code or data here | same |
| 0x0C5 | slave ID, **written by the BIOS** (1) | same |
| 0x0E0 | ARM `B start`: JOY Bus entry point (unused here) | same |
| CPU at entry | ARM state, system mode | same ("With all entry points, the CPU is initially set into system mode") |

## 2.2 Word-by-word exchange (one child)

`xfer(w) → r` is one multiplayer transfer: P sends `w` in slot 0 and receives
`r` in slot 1. `r` is what the child loaded **before** this transfer, so every
expected value below is already shifted by one transfer. The table is GBATEK's
protocol table specialised to child 1 (`x = y = 0x02`), with afska's checks.

| # | P sends | P expects | Repeat / on failure |
|---|---|---|---|
| 1 | `0x6200` | `0xFFFF` (no start bit: slave not in MultiPlay yet), then `0x0000` | repeat back to back, 1 per tick, until step 2. The first unmarked reply after starting, or after a restart wait, answers a word sent before it: ignore it. A marked reply (timed out, framing) doesn't count as that one |
| 2 | `0x6200` | `0x7202`: `(r & 0xFFF0) == 0x7200` and `(r & 0xF) == 0x2` | after 255 replies that are neither `0x0000`, `0xFFFF` nor `0x7202`, wait 70–240 ms and restart at 1, not counted as a failure (GBATEK "Repeat 15 times" and afska `DETECTION_TRIES = 16` pause sooner, for no stated reason; §2.6) |
| 3 | `0x6102` (`0x6100 \| client_bit`) | `0x7202` | else restart |
| 4 | header halfword k, k = 0..0x5F, little-endian `hdr[2k] \| hdr[2k+1] << 8` | `((0x60 - k) << 8) \| 0x02`, so `0x6002` … `0x0102` | any mismatch: restart |
| 5 | `0x6200` | `0x0002` | else restart |
| 6 | `0x6202` | `0x7202` | else restart |
| 7 | `0x6300 \| pp` (palette, e.g. `0x6393`) | `0x7202` while the slave is busy, then `0x73cc`: `(r & 0xFF00) == 0x7300`, `cc = r & 0xFF` | back to back, 1 per tick, for up to 4 s (GBATEK sets no limit; afska gives up after 16 tries); then restart. Not counted as a failure if every reply was a valid `0x7202` |
| 8 | `0x6400 \| hh`, `hh = (0x11 + cc + 0xFF + 0xFF) & 0xFF` (absent children count as 0xFF), using the `cc` of the last `0x73cc` reply **(inferred, M19)** | `(r & 0xFF00) == 0x7300` (`0x73uu`, ignore uu) | else restart |
| 9 | — | wait ≥ 1/16 s (GBATEK "DELAY Wait 1/16 seconds at master side"; afska waits 4 frames). Use 100 ms **(inferred, M20)** | |
| 10 | `llll = (file_size - 0x190) / 4` (= L/4 − 0x34) | `0x73rr`, `rr = r & 0xFF` | `(r & 0xFF00) != 0x7300`: restart |
| 11 | encrypted payload, 2 halfwords per 32-bit word (§2.3), L/2 transfers | halfword k (byte offset `0xC0 + 2k`): `r == (0xC0 + 2k) & 0xFFFF`, so `0x00C0, 0x00C2, …` | any mismatch: restart |
| 12 | `0x0065` | anything first (`nnnn`), then `0x0074` while the slave computes the CRC, then `0x0075` | repeat until `0x0075`, once per GBA frame with idle ticks between, ≤ 5 s (afska `MAX_FINAL_HANDSHAKE_ATTEMPS = FPS * 5`) |
| 13 | `0x0066` | `0x0075` | |
| 14 | `crc` (16 bits, §2.3) | `r == crc` (the slave's own CRC) | mismatch: transfer failed; restart |
| — | done: the BIOS jumps to 0x020000C0, a few seconds later (3.5 s on the first GBA measured, 2026-10-31); the first connect after a send keeps trying HELLO for 9 s (`GEN3_CART_BOOT_US`) | | |

- `pp`, the palette byte, sets the logo colours the slave shows:
  `0x81 + color*0x10 + direction*8 + speed*2` (color 0–6, direction 0–1,
  speed 0–3), or `0xF1 + color*2` for a fixed palette (GBATEK SWI 25h). afska's
  default `0b10010011` = `0x93` is fine.
- Do not send `0xFFFF` as a command word. The handshake words never are.
- Steps 1, 2 and 7 poll back to back, one transfer per tick, as afska's
  `LinkCableMultiboot` does (each try right after the previous reply). Only
  step 12's retries are spaced, once per GBA frame, as afska's are (once per
  VBlank).
- "Restart" is a failure that counts, except where the table says otherwise:
  after the fourth one the sender gives up (§2.6).
- One attempt, from the first header word, has 20 s in all, restarts included.
  Past that the sender gives up and asks for a power cycle (§2.6).

## 2.3 Encryption, seed and CRC

From GBATEK "Pseudo Code for SWI 25h Transfer with Checksum and Encryption
calculations", MultiPlay row. The constants match afska `Async`
(`CRCC_MULTI_START`, `CRCC_MULTI_XOR`, `DATA_MULTI_XOR`, `SEED_MULTIPLIER`).

```
c = 0xFFF8            ; CRC
x = 0xA517            ; CRC polynomial (right-shifting)
k = 0x6465646F        ; data xor key ("odede" in ASCII)
m = pp | cc1<<8 | cc2<<16 | cc3<<24      ; seed; absent child -> 0xFF
f = hh | rr1<<8 | rr2<<16 | rr3<<24      ; CRC finaliser; absent child -> 0xFF
; one child:  m = pp | cc<<8 | 0xFFFF0000,   f = hh | rr<<8 | 0xFFFF0000
for ptr = 0xC0 to file_size-4 step 4:
    w = le32(image + ptr)                ; plaintext
    c = crc32step(c, w)
    m = (0x6F646573 * m + 1) mod 2^32
    e = w ^ (0xFE000000 - ptr) ^ m ^ k   ; (-0x2000000 - ptr) mod 2^32
    send e & 0xFFFF, then e >> 16
c = crc32step(c, f)
send c (16 bits)

crc32step(c, d):            ; 32 rounds
    repeat 32: bit = (c ^ d) & 1; d >>= 1; c >>= 1; if bit: c ^= x
    return c                ; stays < 0x10000
```

GBATEK writes the CRC as `c = c xor data; 32× (c = c shr 1; if carry: c ^= x)`.
afska keeps `c` and `d` separate (`calculateCRCData`). The two forms are
algebraically identical. I checked that both give the same encrypted stream and
CRC on 200 random images (a one-off script, not committed).

**Test vector** (for a Flipper unit test; computed with both formulations):
header = 0xC0 zero bytes; payload = bytes `00 01 02 … FF` (L = 0x100, file
0x1C0); `pp = 0x93`; `cc = 0x5A`; `rr = 0xC3`.

| Quantity | Value |
|---|---|
| `hh` | `0x69` |
| `llll` | `0x000C` |
| `m` before the first update / `f` | `0xFFFF5A93` / `0xFFFFC369` |
| first 8 halfwords sent | `3525 4645 0D28 FD10 CF51 4E63 C51C 2615` |
| last 4 halfwords sent | `DAF9 5C4D 4DC4 A30A` (128 data transfers in all) |
| final CRC | `0x2A28` |

## 2.4 Parent pseudo-code (Flipper)

```c
#define CB 0x02u /* client bit, child 1 */
/* xfer(): one transfer, returns slot 1 (MLINK_NO_DATA + timeout bit if no start bit);
 * marked(r): that transfer's timeout or framing bit is set. */

bool mb_send(const uint8_t* img, uint32_t len /* 0x1C0..0x40000, len % 16 == 0 */) {
    uint16_t r;
    int junk, fails = 0;
    goto detect;
fail:                                       /* a counted failure */
    if(++fails > 3) return false;           /* ask for a power cycle */
restart:
    wait_ms(70 + rand_ms(0, 170));          /* afska: 4 + random(1..10) frames */
detect:
    do r = xfer(0x6200); while(marked(r));  /* its reply answers an earlier word: ignore it */
    for(junk = 0;;) {                       /* steps 1-2: back to back, 1 per tick */
        r = xfer(0x6200);
        if((r & 0xFFF0) == 0x7200 && (r & 0xF) == CB) break;
        if(r != 0xFFFF && r != 0x0000 && ++junk >= 255) goto restart; /* not counted */
    }
    /* From here on, every xfer() also checks the attempt clock, started by the first header
     * word ever sent: past 20 s, return false (ask for a power cycle). */
    if(xfer(0x6100 | CB) != (0x7200 | CB)) goto fail;
    for(int k = 0; k < 0x60; k++)            /* step 4: may run 9 per frame */
        if(xfer(img[2*k] | img[2*k+1] << 8) != (((0x60 - k) << 8) | CB)) goto fail;
    if(xfer(0x6200) != CB) goto fail;
    if(xfer(0x6200 | CB) != (0x7200 | CB)) goto fail;
    uint8_t pp = 0x93, cc;
    bool other = false;
    for(uint32_t t0 = now_ms();;) {         /* step 7: back to back, 1 per tick */
        r = xfer(0x6300 | pp);
        if((r & 0xFF00) == 0x7300) { cc = r & 0xFF; break; }
        if(r != (0x7200 | CB)) other = true;
        if(now_ms() - t0 >= 4000) {
            if(other) goto fail;
            goto restart;                   /* only 0x7202: a slow slave, not counted */
        }
    }
    uint8_t hh = (uint8_t)(0x11 + cc + 0xFF + 0xFF);
    if((xfer(0x6400 | hh) & 0xFF00) != 0x7300) goto fail;
    wait_ms(100);
    r = xfer((uint16_t)((len - 0x190) / 4));
    if((r & 0xFF00) != 0x7300) goto fail;
    uint8_t rr = r & 0xFF;
    uint32_t m = pp | (uint32_t)cc << 8 | 0xFFFF0000u;
    uint32_t f = hh | (uint32_t)rr << 8 | 0xFFFF0000u;
    uint32_t c = 0xFFF8;
    for(uint32_t ptr = 0xC0; ptr < len; ptr += 4) {  /* step 11: may run 9 per frame */
        uint32_t w = le32(img + ptr);
        c = crc32step(c, w);
        m = m * 0x6F646573u + 1u;
        uint32_t e = w ^ (0xFE000000u - ptr) ^ m ^ 0x6465646Fu;
        if(xfer(e & 0xFFFF) != (ptr & 0xFFFF)) goto fail;
        if(xfer(e >> 16) != ((ptr + 2) & 0xFFFF)) goto fail;
    }
    c = crc32step(c, f);
    for(uint32_t t0 = now_ms(); xfer(0x0065) != 0x0075;) {  /* 1 per GBA frame */
        if(now_ms() - t0 >= 5000) goto fail;
        wait_frame();
    }
    xfer(0x0066);
    if(xfer((uint16_t)c) != (uint16_t)c) goto fail;
    return true;
}
```

Mapping onto the existing driver (`mlink_hw_furi.c`: frame callback, at most 9
transfers per frame, one frame of reaction latency):

- Steps 1–3, 7, 8, 10, 12–14 have to react to the reply, so run them as 1-word
  frames (`MlinkTx.count = 1`).
- Steps 4 and 11 are open-loop. Queue 9 words per frame and check all 9 replies
  in the callback. A mismatch aborts.
- In every rx, treat `timeout_mask`/`framing_mask` bits as "no reply". Do not go
  by the value `0xFFFF` alone.

## 2.5 Timing

| Constraint | Value | Source |
|---|---|---|
| Minimum gap after each transfer | "wait for Start bit cleared … followed by a 36us delay" | GBATEK "Multiboot Communication" |
| Our gap inside a frame | ≈ 751.5 µs (TIM2 tick 1073 µs start to start) | `mlink_hw_furi.c` header comment |
| Our gap between frames | 16.74 − 9 × 1.073 ≈ 7.1 ms | same |
| Detection | back to back, 1 per tick; after 255 other replies, a 70–240 ms wait (GBATEK: 15 tries, then 1/16 s; afska: 16 tries) | `gen3_mb.c`; GBATEK; afska |
| Palette (step 7) | back to back, 1 per tick, ≤ 4 s | `gen3_mb.c`; afska gives up after 16 tries |
| Before the length word | ≥ 1/16 s | GBATEK |
| Waiting for `0x0075` | afska polls once per frame for ≤ 5 s | afska |
| One whole attempt | 20 s from the first header word, restarts included; then ask for a power cycle | `gen3_mb.c` (§2.6) |
| Maximum gap the BIOS tolerates | not documented. afska's async master leaves a whole frame between transfers in several states, so multi-millisecond gaps work **(inferred, M3)** | — |

Time to send the helper: ≈ 113 + L/2 transfers (detection, header, palette,
length, end handshake, CRC), plus the 100 ms delay, plus ~15 one-word frames
in frame mode (≈ 0.25 s):

| Helper payload L | 9 per frame (today's driver, 537.6 transfers/s) | 1 per tick ("stream" mode, 932 transfers/s) |
|---|---|---|
| 4 KB | ≈ 4.2 s | ≈ 2.4 s |
| 8 KB | ≈ 8.0 s | ≈ 4.6 s |
| 16 KB | ≈ 15.6 s | ≈ 9.0 s |

The sender takes images up to 12 KB (`GEN3_MB_MAX_LEN`, 0x3000).

## 2.6 Failure behaviour

- Detection fails (no `7202`): wrong plug, GBA not held at Start+Select, or GBA
  off. Retry forever, back to back, with a 70–240 ms wait after every 255
  replies that are neither `0x0000`, `0xFFFF` nor `0x7202`. None of this counts
  as a failure, and waiting for the GBA before the first header word doesn't
  count against the attempt budget either. Show "power on the GBA holding
  Start+Select" after ~3 s.
- **The junk limit.** After a glitch, the slave is still mid-transfer: it
  answers each `0x6200` with its own header count or data offset until it has
  had every word it expects. GBATEK's "Repeat 15 times, if failed: delay 1/16s
  and restart" and afska's 16 tries give no reason for the pause, and document
  no slave timeout, so the sender lets it drain at one word per tick and pauses
  only every 255 replies.
- Header, address or CRC mismatch: restart at step 1 after the random delay
  (afska `startMultibootSend`). How the slave BIOS behaves after a bad CRC or a
  bad header (logo, `0x96`, complement) is not documented. Expect it to go back
  to waiting. The fourth counted failure ends the attempt, and the user is asked
  to power-cycle with Start+Select **(inferred, M4)**.
- **A slow palette step isn't a failure.** A 4 s palette wait whose every reply
  was a valid `0x7202` restarts without counting.
- **The attempt budget.** One attempt, from the first header word, has 20 s in
  all, restarts and waits included. A slave left mid-transfer by a glitch can
  take minutes to drain, while turning the GBA off and on takes seconds. Past
  20 s the sender stops with a power-cycle request (`Gen3MbFailPowerCycle`). A
  clean 12 KB send takes about 16 s even with a slow slave (palette and `0x0075`
  waits near their limits), so the budget never cuts a normal run.
- SWI 25h's own result is "r0 0=okay, 1=failed" (GBATEK). The slave only starts
  the program when the CRCs match.

---

# 3. What the helper can do with the Pokemon cart inserted

## 3.1 Memory map and access rules

| Region | Use | Rules |
|---|---|---|
| 0x02000000–0x0203FFFF EWRAM (256 KB) | helper code and data, sector buffers | 16-bit bus, 2 wait states |
| 0x03000000–0x03007EFF IWRAM (32 KB) | hot loops (ARM code), stack. BIOS stacks: `sp_usr=0x03007F00`, `sp_irq=0x03007FA0`, `sp_svc=0x03007FE0`; IRQ handler pointer at 0x03007FFC | GBATEK "BIOS RAM Usage" |
| 0x08000000–0x08FFFFFF | cart ROM (16 MiB), readable in full | GBATEK "Multiboot Slave with Cartridge": the main ROM was "meant to be locked out" but "current GBA models and GBA carts do not seem to contain any such protection". FIX94's dumper reads the whole ROM after a JOY Bus multiboot |
| 0x080000C4–0x080000C9 | cart GPIO (RTC Seiko S-3511A on RSE carts; gbhwdb lists "RTC (U3)" for AXVE/AXPE/BPEE, none for BPRE/BPGE) | **Never write** here. With GPIO control (0x080000C8) = 0, write-only mode, reads return ROM data (GBATEK "GBA Cart I/O Port (GPIO)"); it is 0 after power-on **(inferred, M5)** |
| 0x0E000000–0x0E00FFFF | save flash (64 KB window, bank-switched) | **8-bit LDRB/STRB only**; read "only by opcodes executed in WRAM"; WAITCNT bits 0–1 = 3 (8 cycles) (GBATEK "GBA Cart Backup Flash ROM"; `src/libs/agb_flash.c: ReadFlash` sets `WAITCNT_SRAM_8`) |
| 0x04000204 WAITCNT | | at start-up 0x0000 (ROM 4,2 waits). Use `0x0003` (SRAM 8 cycles, ROM default). `0x4317` (ROM 3,1 + prefetch, what games use) speeds up ROM scanning (GBATEK "GBA System Control") |

The cart must stay inserted while the helper runs (GBATEK "GBA Cart Backup
SRAM/FRAM", "Preventing Data Loss").

## 3.2 Cart header and game identification

The helper reads 0x08000000–0x080000BF and checks:

1. Logo 0x004–0x09F equals the helper's own header logo at 0x02000004. The BIOS
   has already validated that copy.
2. `[0xB2] == 0x96`, and the complement at 0xBD matches (§2.1 formula).
3. Game code at 0xAC and title at 0xA0:

| Game | Title (0xA0) | Game code (0xAC), USA/EU English | Other regions | Maker | Source |
|---|---|---|---|---|---|
| Ruby | `POKEMON RUBY` | `AXVE` | `AXVJ/D/F/I/S` | `01` | pokeruby `config.mk`; gbhwdb |
| Sapphire | `POKEMON SAPP` | `AXPE` | `AXPJ/D/F/I/S` | `01` | same |
| FireRed | `POKEMON FIRE` | `BPRE` | `BPRJ/D/F/I/S` | `01` | fr: `config.mk` |
| LeafGreen | `POKEMON LEAF` | `BPGE` | `BPGJ/D/F/I/S` | `01` | same |
| Emerald | `POKEMON EMER` | `BPEE` | `BPEJ/D/F/I/S` | `01` | em: `Makefile` |

- `0xBC` (software version) = revision 0/1/2 (pokeruby `Makefile` `-r$(GAME_REVISION)` via gbafix).
- **Use the cart header to tell the games apart, not the save.** The u32 at
  SaveBlock2+0xAC, Bulbapedia's "game code", is unreliable (§5.6).
- Write support covers the English codes ending in `E` only. The layouts in §5
  are pret's English/German builds. Japanese builds differ **(inferred, M6)**.

## 3.3 Save-type detection

Step 1, the ROM library string (GBATEK "GBA Cart Backup IDs"): Nintendo's save
libraries embed a word-aligned ID string. All five games link `FLASH1M_V103`
(`src/libs/agb_flash_1m.c`, em: `src/agb_flash_1m.c`, fr: `src/agb_flash_1m.c`:
`AgbLibFlashVersion`).

| String | Type |
|---|---|
| `EEPROM_Vnnn` | EEPROM 512 B / 8 KB |
| `SRAM_Vnnn` | SRAM 32 KB |
| `FLASH_Vnnn`, `FLASH512_Vnnn` | Flash 64 KB |
| `FLASH1M_Vnnn` | Flash 128 KB |

Scan the ROM in 32-bit steps for `0x53414C46` ("FLAS") followed by `0x5F4D3148`
("H1M_"), for "SRAM" (`0x4D415253`) followed by a byte `0x5F`, and for "EEPR"
(`0x52504545`) followed by "OM_" (FIX94 `libSave.c: SaveSize`). A Pokemon ROM
that contains `SRAM_` or `EEPROM_` and no `FLASH1M_V103` has been patched
(§3.6). A scan of 16 MiB at WAITCNT 0x4317 takes ~6.7 s: the helper's loop costs ~27 cycles
per 32-bit word by its disassembly, one HAL ROM read (~12 cycles, 6 of them the cart bus) plus
the test and the loop **(inferred, M7)**. ~7.2 s at the fallback WAITCNT 0x0003.

Step 2, the SRAM probe. **Run it before any flash command.** A command sequence
writes 0xAA/0x55/0x90 straight into an SRAM and corrupts it.

```c
volatile uint8_t* const F = (volatile uint8_t*)0x0E000000;
uint8_t a = F[0x5555];
uint8_t v = (uint8_t)(a ^ 0xFF); if(v == 0xAA) v = (uint8_t)(a ^ 0x0F); /* never 0xAA: no unlock cycle */
F[0x5555] = v;
if(F[0x5555] == v) { F[0x5555] = a; /* restore */ return SAVE_IS_SRAM; }  /* refuse */
```

On flash, a lone write that is not the `AA→5555` unlock cycle is ignored by
the command state machine, so `F[0x5555]` still reads `a`
**(inferred, M8)**.

Step 3, the flash ID. This is the exact sequence of `src/libs/agb_flash.c:
ReadFlashId`, run with WAITCNT SRAM = 8 cycles (`IdentifyFlash`):

```
pre0 = F[0]; pre1 = F[1]                  ; array data, for the "ID mode had an effect" check
F[0x5555]=0xAA; F[0x2AAA]=0x55; F[0x5555]=0x90       ; enter ID mode
delay: for (vu16 i = 20000; i; i--);                  ; pret DELAY()
id = F[1] << 8 | F[0]                                 ; device << 8 | maker
F[0x5555]=0xAA; F[0x2AAA]=0x55; F[0x5555]=0xF0; F[0x5555]=0xF0   ; leave (pret writes F0 twice)
delay
```

Classify:

| Result | Meaning |
|---|---|
| `id == 0x09C2` or `0x1362`, and `(F[0],F[1])` in ID mode ≠ `(pre0,pre1)` | genuine 128 KB flash, as the game itself accepts |
| `id` is a 64 KB ID (§3.4) | not a Pokemon cart; read-only at most |
| anything else, or no difference between ID mode and array mode | no flash, EEPROM cart, open bus, or a reproduction. Refuse |

The game itself accepts only `0x09C2` and `0x1362`. With any other ID,
`IdentifyFlash` returns 1 and the game prints "The 1M sub-circuit board is not
installed." (`src/libs/agb_flash_1m.c: IdentifyFlash`, `src/load_save.c:
CheckForFlashMemory`, `src/main_menu.c` case `SAVE_STATUS_NO_FLASH`,
`src/strings.c: gBoardNotInstalledMessage`).

EEPROM, for completeness: serial, accessed by DMA3 bitstreams at 0x0DFFFF00
(GBATEK "GBA Cart Backup EEPROM"). Not used by these games.

## 3.4 Flash chips, commands and timing

Chip table: GBATEK "GBA Cart Backup Flash ROM", "Device Types" (ID = `dev<<8 |
man`). The 128 KB rows' timeouts and waits come from pret (`agb_flash_mx.c:
mxMaxTime`, `MX29L010`; `agb_flash_le.c: leMaxTime`, `LE26FV10N1TS`).

| ID | Maker / part | Size | Sectors | Timeouts ms (byte write, sector erase, chip erase) | Waits (write, read) |
|---|---|---|---|---|---|
| **`0x09C2`** | Macronix MX29L010 | 128 KB | 32 × 4 KB | 10, 2000, 2000 (pret) | WAITCNT SRAM 3 = 8 cycles for both (pret `{3, 1}` → `wait[0]` = 3; reads at 8) |
| **`0x1362`** | Sanyo LE26FV10N1TS | 128 KB | 32 × 4 KB | 10, 2000, 2000 (pret) | same |
| `0xD4BF` | SST (SST39VF512 **(inferred, M9)**) | 64 KB | 16 × 4 KB | 10, 40, 200 | 3, 2 |
| `0x1CC2` | Macronix (MX29L512 **(inferred, M9)**) | 64 KB | 16 × 4 KB | 10, 2000, 2000 | 8, 3 |
| `0x1B32` | Panasonic (MN63F805MNP **(inferred, M9)**) | 64 KB | 16 × 4 KB | 10, 500, 500 | 4, 2 |
| `0x3D1F` | Atmel (AT29LV512 **(inferred, M9)**) | 64 KB | 512 × 128 B | "…40…", 40 | 8, 8 |

(The brief's "Macronix 0xC2/0x1C2" is maker 0xC2 with device 0x1C, the 64 KB
part, ID `0x1CC2`. The 128 KB Macronix is device 0x09.)

Command sequences (addresses relative to 0x0E000000, byte writes; GBATEK and
pret agree):

| Operation | Sequence | Completion |
|---|---|---|
| Bank select (128 KB only) | `5555←AA, 2AAA←55, 5555←B0, 0000←bank` (0 or 1) | immediate. Sector *n* (0–31) = bank `n/16`, window offset `(n%16) << 12` (`agb_flash_mx.c: EraseFlashSector_MX` calls `SwitchFlashBank(sectorNum / SECTORS_PER_BANK)`) |
| Read | `dat = [addr]` | — |
| Sector erase (all but Atmel) | `5555←AA, 2AAA←55, 5555←80, 5555←AA, 2AAA←55, [n<<12]←30` | poll `[n<<12]` until `0xFF` |
| Byte program (all but Atmel; target must be erased) | `5555←AA, 2AAA←55, 5555←A0, [addr]←dat` | poll `[addr]` until `dat` |
| Atmel 128-byte page write | `IME=0; 5555←AA, 2AAA←55, 5555←A0; write 128 bytes to a 0x80-aligned page; IME=old` | poll `[page+0x7F]` until the last byte |
| Chip erase | `5555←AA, 2AAA←55, 5555←80, 5555←AA, 2AAA←55, 5555←10` | poll `[0]` until `0xFF`. **Never use**: it wipes both slots and the Hall of Fame |
| Abort after timeout | `5555←F0` | GBATEK: Macronix `0x1CC2` only. pret does it on every timeout for both 128 KB chips (`WaitForFlashWrite_Common`) |

Polling: copy pret's `agb_flash_1m.c: WaitForFlashWrite_Common(phase, addr,
expected)` exactly.

```
start timer for phase (1 = byte program 10 units, 2 = sector erase 2000, 3 = chip erase 2000)
loop:
    s = [addr]
    if s == expected: ok
    if s & 0x20:                       ; DQ5 "exceeded time limit"
        if [addr] == expected: ok
        [0x5555] = 0xF0; fail (phase | 0xA000)
    if timer expired:
        if [addr] == expected: ok
        [0x5555] = 0xF0; fail (phase | 0xC000)
```

- Timer unit: prescaler 256, reload 65469 (`mxMaxTime`), so one overflow = 67 × 256
  cycles = 1.0224 ms. Hence 10 ≈ 10.2 ms and 2000 ≈ 2.04 s.
- Erase retry: `EraseFlashSector_MX` retries while `result & 0xA000`, up to 5
  attempts in total.
- Sector program: `ProgramFlashSector_MX` erases, then programs all 4096 bytes in
  ascending order. `ProgramFlashSectorAndVerify` runs program + `VerifyFlashSector`
  up to 3 times.
- Typical (not worst-case) program and erase times are not in these sources.
  Budget ~0.1–0.5 s per 4 KB sector and keep pret's timeouts **(inferred, M10)**.
- SST only: GBATEK describes extra erase repetitions (up to 80, then 1 or 6
  more). Irrelevant for Pokemon.

## 3.5 Which chips genuine Pokemon carts use

- The game code accepts exactly `0x09C2` (MX29L010) and `0x1362`
  (LE26FV10N1TS) (`src/libs/agb_flash_1m.c: sSetupInfos`; em/fr identical).
- gbhwdb entries (photos of opened carts):

| Cart | Flash part found |
|---|---|
| AGB-AXVE-0 (Ruby US) | Macronix MX29L010TC-15 |
| AGB-AXPE-0/1/2 (Sapphire US) | MX29L010TC-15A1 |
| AGB-BPEE-0 (Emerald US), 2 entries | MX29L010TC-15A1 |
| AGB-BPRE-1 (FireRed US rev 1), 2 entries | MX29L010TC-15A1 |
| AGB-BPGE-0/1 (LeafGreen US) | MX29L010TC-15A1 |
| AGB-AXVI-1 (Ruby Italy rev 1) | Sanyo LE26FV10N1TS |
| AGB-AXVF-0 (Ruby France) | MX29L010TC-15A1 |

  Boards: AGB-E05-01 for RSE (with RTC), AGB-E02-20/-30 for FRLG.
- Quirk: in **German Sapphire rev 0** (non-debug), the MX29L010 entry carries
  ID `{0xBF, 0xD4}` = `0xD4BF` instead of `0x09C2` (`src/libs/agb_flash_mx.c`,
  `#if (GERMAN && SAPPHIRE && (REVISION == 0) && !DEBUG)`; the same `#if` was
  copied into fr: `src/agb_flash_mx.c`, where it never triggers). Which chip
  those carts carry is unknown **(M11)**. The helper should accept `0xD4BF`
  only when the header says `AXPD` version 0, and treat it as a 128 KB MX-style
  chip, as the game does.

## 3.6 Refusing reproduction carts

What reproductions do:

- **SRAM repros**: an SRAM (often 1 Mbit, banked by a write to 0x09000000)
  replaces the flash, and the ROM is patched to save to SRAM (bbsan2k
  Flash1M_Repro_SRAM_Patcher README).
- **Batteryless repros**: the same SRAM with no battery. The patched game copies
  the save into the ROM flash chip a few seconds after saving and back at boot
  (metroid-maniac gba-auto-batteryless-patcher README). Booted into multiboot,
  the patched boot code never runs, so 0x0E000000 holds power-on garbage and
  the real save sits somewhere in the ROM area.
- The flash-ID sequence on SRAM simply writes `AA/55/90/AA/55/F0/F0` into SRAM
  at 0x5555/0x2AAA. 0x0E000000/1 then read whatever SRAM holds there: random,
  `FF FF`, `00 00`, or save data. They read the same in "ID mode" and in read
  mode **(inferred, M12)**.
- Repros that carry a real 128 KB flash chip with an unpatched ROM behave like
  originals and cannot be told apart. They are as safe to write as originals
  **(inferred, M13)**.

Refusal policy. Every check must pass before write commands are allowed (§6);
reading may proceed with a warning:

1. Header valid (§3.2), game code in the supported list.
2. SRAM probe negative (§3.3, step 2).
3. Flash ID `0x09C2` or `0x1362` (plus the AXPD rev 0 exception), and ID mode
   changed the bytes read at 0/1.
4. The ROM contains `FLASH1M_V103` and no `SRAM_V`/`SRAM_F_V`/`EEPROM_V` string.
5. Optional but strong: SHA-1 of the whole 16 MiB ROM, computed on the GBA
   (`ROM_SHA1`, §6), matches one of pret's reference hashes:

| Build (pret target) | SHA-1 |
|---|---|
| Ruby US rev 0 / 1 / 2 | `f28b6ffc97847e94a6c21a63cacf633ee5c8df1e` / `610b96a9c9a7d03d2bafb655e7560ccff1a6d894` / `5b64eacf892920518db4ec664e62a086dd5f5bc8` |
| Sapphire US rev 0 / 1 / 2 | `3ccbbd45f8553c36463f13b938e833f652b793e4` / `4722efb8cd45772ca32555b98fd3b9719f8e60a9` / `89b45fb172e6b55d51fc0e61989775187f6fe63c` |
| Ruby DE rev 0 / 1 | `1c2a53332382e14dab8815e3a6dd81ad89534050` / `424740be1fc67a5ddb954794443646e6aeee2c1b` |
| Sapphire DE rev 0 / 1 | `fa0bd1abe04fea17016f585454d0f1392f342a21` / `7e6e034f9cdca6d2c4a270fdb50a94def5883d17` |
| FireRed US rev 0 / 1 | `41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc` / `dd5945db9b930750cb39d00c84da8571feebf417` |
| LeafGreen US rev 0 / 1 | `574fa542ffebb14be69902d1d36f1ec0a4afd71e` / `7862c67bdecbe21d1d69ce082ce34327e1c6ed5e` |
| Emerald US | `f3ae088181bf583e55daf962a92bb46f4f1d07b7` |

(pokeruby `*.sha1`, em: `rom.sha1`, fr: `*.sha1`.) A ROM read through the cart
bus equals the dump these hashes describe **(inferred, M14)**. SHA-1 over 16 MiB
on the ARM7 takes ~30–60 s **(inferred, M15)**.

## 3.7 ROM size and dumping

- Size: past the end of the ROM, the cart bus returns the halfword address,
  `(addr/2) & 0xFFFF` (GBATEK "GBA Unpredictable Things", "Reading from GamePak
  ROM when no Cartridge is inserted"). FIX94 `main.c: getGameSize` checks the
  logo word, then for `i = 1 MiB, 2 MiB, … < 32 MiB`: if the halfwords at
  `0x08000000+i` read `0, 1, …, 0xFFF`, the size is `i`. Also treat "data at
  `i` equals data at 0" (a mirror) as the end. Expect exactly 16 MiB for all
  five games.
- Dump: 16-bit or 32-bit reads in ascending order. Skip writes everywhere in the
  ROM area. At link speed a full dump takes hours (§6.6), so verify with
  `ROM_SHA1`/`ROM_CRC32` and dump only on request.

---

# 4. Building the helper with plain arm-none-eabi-gcc

## 4.1 Toolchain finding

The ufbt toolchain ships a usable compiler:

| Item | Value |
|---|---|
| Path | `~/.ufbt/toolchain/arm64-darwin/bin/arm-none-eabi-gcc` (`~/.ufbt/toolchain/current` → `arm64-darwin`) |
| Version | `arm-none-eabi-gcc (GCC) 12.3.1 20230626`; toolchain `VERSION` file: `39` |
| Defaults | `-mcpu=arm7tdmi -march=armv4t -mfloat-abi=soft` (`-Q --help=target`) |
| Multilib for `-mcpu=arm7tdmi -mthumb` | `thumb/nofp`; its `libgcc.a` is `Tag_CPU_arch: v4T`, Thumb-1, so it is safe on the GBA |
| Also present | binutils (`ld.bfd`, `objcopy`; `--no-warn-rwx-segments` works, so binutils ≥ 2.39), newlib `libc.a`/`libc_nano.a` |
| Missing | `gbafix`; do the header fix in a script (§4.5) |

Verified: the crt0 and linker script below build with this toolchain into a
480-byte image (a scratch build, not committed). Header fields land at
0xA0–0xBF. `0xC0` and `0xE0` hold `b mb_start` (`ea000007`/`eaffffff`). A
Thumb→ARM call into IWRAM gets a linker veneer (`__…_from_thumb`).

## 4.2 Flags

```
CFLAGS  = -mcpu=arm7tdmi -mthumb -mthumb-interwork -Os -ffreestanding -fno-builtin \
          -ffunction-sections -Wall -Wextra
ASFLAGS = -mcpu=arm7tdmi
LDFLAGS = -mcpu=arm7tdmi -mthumb -mthumb-interwork -nostartfiles -nostdlib \
          -T helper.ld -Wl,--gc-sections -Wl,--no-warn-rwx-segments
LIBS    = -lgcc            # __aeabi_uidiv etc.; the ARM7TDMI has no divide instruction
```

- Thumb for code in EWRAM (16-bit bus). `__attribute__((section(".iwram"),
  target("arm")))` for hot loops (CRC, SHA-1, ROM scan) in IWRAM (32-bit bus,
  no waits).
- `-nostdlib` means you supply `memcpy`/`memset` if GCC emits calls to them, or
  link `-lc_nano` from the same v4T multilib.

## 4.3 crt0 (header + start-up), as verified

```asm
    .syntax unified
    .section .header, "ax", %progbits
    .arm
    .global _start
_start:
    b       mb_start                @ 0x00 ROM entry (ignored by multiboot)
    .fill   156, 1, 0               @ 0x04 Nintendo logo (patched in by the build, §4.5)
    .ascii  "FLIPPERGEN3 "          @ 0xA0 title, 12 bytes
    .ascii  "ZFGE"                  @ 0xAC game code (any; avoid real codes)
    .ascii  "00"                    @ 0xB0 maker
    .byte   0x96                    @ 0xB2 fixed value
    .byte   0x00, 0x00              @ 0xB3 unit code, 0xB4 device type
    .fill   7, 1, 0                 @ 0xB5 reserved
    .byte   0x00                    @ 0xBC version
    .byte   0x00                    @ 0xBD complement (patched in by the build)
    .fill   2, 1, 0                 @ 0xBE reserved
    b       mb_start                @ 0xC0 MultiPlay/Normal entry
    .byte   0, 0                    @ 0xC4 boot mode, 0xC5 slave id (BIOS writes 3, 1)
    .fill   26, 1, 0                @ 0xC6..0xDF
    b       mb_start                @ 0xE0 JOY Bus entry
mb_start:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]        @ IME = 0
    msr     cpsr_c, #0xD2           @ IRQ mode, I+F masked
    ldr     sp, =0x03007FA0
    msr     cpsr_c, #0xDF           @ system mode, I+F masked
    ldr     sp, =0x03007F00
    ldr     r0, =__iwram_lma        @ copy .iwram from its EWRAM load address
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b
    ldr     r1, =__bss_start        @ zero .bss (EWRAM after the image; the BIOS does not)
    ldr     r2, =__bss_end
    mov     r3, #0
2:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     2b
    ldr     r0, =main
    mov     lr, pc
    bx      r0                      @ interworking call into Thumb main()
3:  b       3b
    .pool
```

## 4.4 Linker script (EWRAM execution), as verified

```ld
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY {
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x6000  /* 0x03006000..0x03007EFF: stack */
}
SECTIONS {
    .text : { KEEP(*(.header)) *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(4); } > ewram
    .iwram : { __iwram_start = .; *(.iwram .iwram.*) . = ALIGN(4); __iwram_end = .; } > iwram AT > ewram
    __iwram_lma = LOADADDR(.iwram);
    .data : { *(.data .data.*) . = ALIGN(16); } > ewram   /* payload length multiple of 0x10 */
    __image_end = .;
    .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > ewram
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
```

`crt0.o` must be first on the link line. `.data` is linked at its run address
inside the image, so it needs no copy step. `.bss` lies past the transferred
bytes and is zeroed by crt0.

## 4.5 Build steps

```
arm-none-eabi-gcc $CFLAGS -c main.c flash.c link.c ...
arm-none-eabi-gcc $ASFLAGS -c crt0.s
arm-none-eabi-gcc $LDFLAGS crt0.o *.o $LIBS -o helper.elf
arm-none-eabi-objcopy -O binary helper.elf helper.mb
python3 fix_header.py helper.mb   # 1. copy the 156-byte logo into 0x04..0x9F
                                  #    (devkitPro gba-tools gbafix.c, logo table; first word 0x51AEFF24)
                                  # 2. hdr[0xBD] = (-(sum(hdr[0xA0:0xBD])) - 0x19) & 0xFF
                                  # 3. pad with 0x00 to a multiple of 0x10 and to >= 0x1C0 bytes
                                  # 4. fail if len > 0x40000
xxd -i helper.mb > helper_mb.c    # embed in the FAP
```

## 4.6 Flash code in RAM

- pret copies `ReadFlash_Core`, `VerifyFlashSector_Core` and the status reader
  `ReadFlash1` into stack buffers before running them (`src/libs/agb_flash.c:
  ReadFlash`, `VerifyFlashSector`, `SetReadFlash1`). The game runs from ROM, and
  flash "reading anything (data or status/busy information) can be done only by
  opcodes executed in WRAM" (GBATEK "GBA Cart Backup Flash ROM").
- The helper runs entirely from EWRAM/IWRAM, so the rule is met automatically.
  Never call or jump into cart ROM code, such as the game's own flash routines.
- Access the flash only through `volatile uint8_t*`. pret notes the same in
  `ReadFlash_Core`.

## 4.7 Interrupts: off

- Keep `IME = 0` for the helper's whole life (crt0 sets it).
- Poll everything: serial completion and timers. The BIOS IRQ vector at
  0x03007FFC is then irrelevant.
- No interrupt or DMA can then land in the middle of a flash command sequence.
  GBATEK requires interrupts and DMA off during the command/write phase on
  Atmel, and pret's timer code saves and restores IME around commands
  (`StartFlashTimer`/`StopFlashTimer`).
- Timers for timeouts:

  ```
  TM2CNT_L (0x04000108) = 65469
  TM2CNT_H (0x0400010A) = 0x0082     ; enable | prescaler 256: overflow every 1.0224 ms
  TM3CNT_L (0x0400010C) = 0
  TM3CNT_H (0x0400010E) = 0x0084     ; enable | count-up: TM3 counter = elapsed ms units
  ```

  Restart TM3 before each operation.

## 4.8 The helper as multiplayer child

| Register | Value | Notes |
|---|---|---|
| `RCNT` 0x04000134 | `0x0000` | bit 15 = 0 selects Normal/MultiPlay/UART (GBATEK "SIO Multi-Player Mode") |
| `SIOCNT` 0x04000128 | `0x6003` | MultiPlay (bit 13), IRQ request enable (bit 14), 115200 (bits 0–1 = 3). Same value as Ruby's `link.c: EnableSerial` |
| `IE` 0x04000200 | `0x0080` (serial) | with `IME = 0` no interrupt is taken. `IF` bit 7 still latches at every transfer end **(inferred, M16)**. Fallback: poll `SIOCNT` bit 7 (busy) 1→0 |
| `SIOMLT_SEND` 0x0400012A | next word to send | sent in the **next** transfer |
| `SIOMULTI0` 0x04000120 | the Flipper's word | valid after transfer end |
| `SIOCNT` bit 6 | error | "a slave did not receive SI=LOW … or a Stopbit wasn't HIGH". Drop that word |
| `SIOCNT` bits 4–5 | ID | must read 1 |

```c
/* Call only right after the previous transfer completed (or at start-up). */
static inline uint16_t link_xfer(uint16_t next, bool* err) {
    REG_SIOMLT_SEND = next;                  /* goes out in the next transfer */
    while(!(REG_IF & 0x0080)) {}             /* that transfer has ended */
    REG_IF = 0x0080;                         /* acknowledge (write 1) */
    *err = (REG_SIOCNT & 0x0040) != 0;
    return REG_SIOMULTI0;                    /* parent's word of that same transfer */
}
```

Rules:

1. Write `SIOMLT_SEND` only right after acknowledging a transfer-end `IF`. Each
   word then goes out exactly once. With the Flipper's ≈ 751 µs gap, the helper
   has ~12,600 cycles per word.
2. The hardware answers every parent transfer by itself with whatever
   `SIOMLT_SEND` holds (GBATEK "SIO Multi-Player Mode": "The parent unit blindly
   sends data …"). Before a long operation, write `H_BUSY` (§6) right after an
   `IF` and go do the work: the Flipper keeps seeing `H_BUSY`. Afterwards,
   acknowledge the stale `IF`, wait for one fresh `IF`, and only then resume
   `link_xfer()`.
3. At start-up, rewrite `RCNT`/`SIOCNT` once and load `H_READY`. The first
   transfer around that write may be corrupt **(inferred, M17)**.
4. Optional status display: `DISPCNT` (0x04000000) = `0x0000` shows the backdrop,
   palette entry 0 at 0x05000000 (BGR555). Green `0x03E0` = ready, yellow
   `0x03FF` = busy, red `0x001F` = flash write in progress (do not power off),
   blue `0x7C00` = refused cart.

Flash primitives (polling, IME = 0, WAITCNT `0x0003`), mirroring pret:

```c
#define F ((volatile uint8_t*)0x0E000000)
static void cmd(uint8_t c) { F[0x5555] = 0xAA; F[0x2AAA] = 0x55; F[0x5555] = c; }
static void bank(uint8_t b) { cmd(0xB0); F[0x0000] = b; }     /* 128 KB chips only */
static int wait(volatile uint8_t* a, uint8_t want, uint16_t ms, uint16_t phase) {
    TM3_restart();
    for(;;) {
        uint8_t s = *a;
        if(s == want) return 0;
        if(s & 0x20) { if(*a == want) return 0; F[0x5555] = 0xF0; return phase | 0xA000; }
        if(TM3CNT_L >= ms) { if(*a == want) return 0; F[0x5555] = 0xF0; return phase | 0xC000; }
    }
}
int erase_sector(unsigned n) {                       /* n = 0..31 */
    volatile uint8_t* a = F + ((n & 15) << 12);
    int r;
    for(int t = 0; t < 5; t++) {                     /* EraseFlashSector_MX */
        bank(n >> 4);  /* every try: pret selects it once, but nothing should depend on F0 keeping it */
        cmd(0x80); F[0x5555] = 0xAA; F[0x2AAA] = 0x55; *a = 0x30;
        if(!((r = wait(a, 0xFF, 2000, 2)) & 0xA000)) break;
    }
    return r;
}
int program_byte(unsigned n, unsigned off, uint8_t v) { /* ProgramFlashByte_MX */
    bank(n >> 4);
    volatile uint8_t* a = F + ((n & 15) << 12) + off;
    cmd(0xA0); *a = v;
    return wait(a, v, 10, 1);
}
```

---

# 5. Gen III save structure (RS, FRLG, Emerald)

## 5.1 Flash layout (128 KB = 32 sectors of 4 KB)

| Sectors | Bytes | Content | Source |
|---|---|---|---|
| 0–13 | 0x00000–0x0DFFF | save slot A (holds even counters) | `src/save.c` comment; em: `include/save.h` |
| 14–27 | 0x0E000–0x1BFFF | save slot B (holds odd counters) | same |
| 28–29 | 0x1C000–0x1DFFF | Hall of Fame (`HALL_OF_FAME_SECTOR`) | same |
| 30–31 | 0x1E000–0x1FFFF | RS: e-Reader/unused; E: 30 Trainer Hill, 31 Recorded Battle; FR: 30–31 Trainer Tower | `src/save.c` (`sUnusedFlashSectors`), em/fr `include/save.h` |

Sectors 0–15 are bank 0 and 16–31 are bank 1. Sector *n* is at window offset
`(n % 16) * 0x1000`. Select the bank before every access; do not assume
bank 0 after power-on **(M21)**.

## 5.2 Sector format

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x000 | 0xF80 (3968) | data | used length = size[id] (§5.4); the rest is 0 |
| 0xF80 | 0x74 | unused, 0 | em: `struct SaveSector` `unused[SECTOR_FOOTER_SIZE - 12]`; RS declares `data[0xFF4]` and zeroes it |
| 0xFF4 | 2 | `id` | 0–13 for slot sectors |
| 0xFF6 | 2 | `checksum` | §5.3 |
| 0xFF8 | 4 | `signature` = `0x08012025` (bytes `25 20 01 08`) | `FILE_SIGNATURE` / `SECTOR_SIGNATURE` |
| 0xFFC | 4 | `counter` (save index) | not covered by the checksum |

(`src/save.c: struct SaveSector`, em: `include/save.h`; Bulbapedia "Section
format".) The game clears the whole 4 KB buffer before it fills a sector, so
every byte past size[id] up to 0xFF3 is zero (`src/save.c: WriteSingleChunk`
"clear save section"; em: `HandleWriteSector`).

Hall of Fame sectors differ: the u16 at 0xFF4 holds the **checksum**, not an id
(`src/save.c: HandleWriteSectorNBytes`; `ReadSomeUnknownSectorAndVerify`
compares `section->id == checksum`). Never rewrite them.

## 5.3 Checksum

```c
/* src/save.c: CalculateChecksum (identical in em:/fr:) */
u16 CalculateChecksum(void *data, u16 size) {
    u32 checksum = 0;
    for (u16 i = 0; i < (size / 4); i++) { checksum += *((u32 *)data); data += 4; }
    return ((checksum >> 16) + checksum);   /* truncated to u16 */
}
```

It sums the first size[id]/4 little-endian u32 words of the data area,
mod 2^32, and the result is `(hi16 + lo16) & 0xFFFF`. Because the padding is
zero, the sum over all 0xF80 bytes gives the same result for game-written
sectors. PKHeX relies on this (`SAV3.cs`: `Checksums.CheckSum32(sector[..SIZE_SECTOR_USED])`).
The Flipper should still use the exact size when it writes.

## 5.4 Section sizes and chunk mapping

`sSaveBlockChunks` (`src/save.c`), `sSaveSlotLayout` (em:/fr: `src/save.c`):
chunk *j* of a struct is bytes `[j*0xF80, min(sizeof, (j+1)*0xF80))`.

| id | Struct | Struct offset | RS size | FRLG size | E size |
|---|---|---|---|---|---|
| 0 | SaveBlock2 | 0 | 0x890 (2192) | 0xF24 (3876) | 0xF2C (3884) |
| 1 | SaveBlock1 | 0x0000 | 0xF80 | 0xF80 | 0xF80 |
| 2 | SaveBlock1 | 0x0F80 | 0xF80 | 0xF80 | 0xF80 |
| 3 | SaveBlock1 | 0x1F00 | 0xF80 | 0xF80 | 0xF80 |
| 4 | SaveBlock1 | 0x2E80 | 0xC40 (3136) | 0xEE8 (3816) | 0xF08 (3848) |
| 5–12 | PokemonStorage | (id−5) × 0xF80 | 0xF80 each | same | same |
| 13 | PokemonStorage | 0x7C00 | 0x7D0 (2000) | same | same |

Struct sizes:

| Struct | RS | FRLG | E | Source |
|---|---|---|---|---|
| SaveBlock2 | 0x890: `battleTower` at 0xA8 + `BattleTowerData` 0x7E8 | 0xF24 | 0xF2C | `include/global.h` (RS offsets; computed); fr: `include/global.h` "size: 0xF24"; em: `include/global.h` "sizeof=0xF2C" |
| SaveBlock1 | 0x3AC0: `dexSeen3` at 0x3A8C + 52 | 0x3D68 | 0x3D88 | same files |
| PokemonStorage | 0x83D0 | 0x83D0 | 0x83D0 | `include/pokemon.h`; em/fr `include/pokemon_storage_system.h` |

PKHeX agrees (`SAV3.cs` comments: small `[0x890 RS, 0xf24 FR/LG, 0xf2c E]`,
large `3+[0xc40 RS, 0xee8 FR/LG, 0xf08 E]`, storage `[0x83D0]`;
`SAV3RS/FRLG/E.cs` slice `SmallBuffer[..0x890/0xF24/0xF2C]`). So does
Bulbapedia's Emerald table (3884, 3968×3, 3848, 3968×8, 2000).

## 5.5 Picking the active slot (exactly as the game does)

`src/save.c: GetSaveValidStatus` (em:/fr: same logic):

```
for slot in (A = sectors 0..13, B = 14..27):
    any_sig = false; valid = 0; ctr = 0
    for i in 0..13:                         # physical order
        s = sector(slot, i)
        if s.signature == 0x08012025:
            any_sig = true
            if s.checksum == CalculateChecksum(s.data, size[s.id]):   # game does not bound-check id
                ctr = s.counter             # last valid sector wins
                valid |= 1 << s.id
    status = EMPTY if !any_sig else (OK if valid == 0x3FFF else ERROR)
if A OK and B OK:
    if (ctrA == 0xFFFFFFFF and ctrB == 0) or (ctrA == 0 and ctrB == 0xFFFFFFFF):
        gSaveCounter = ctrB if (ctrA + 1) < (ctrB + 1) else ctrA       # wrap case
    else:
        gSaveCounter = ctrB if ctrA < ctrB else ctrA
    result OK
elif A OK: gSaveCounter = ctrA; result = ERROR if B == ERROR else OK
elif B OK: gSaveCounter = ctrB; result = ERROR if A == ERROR else OK
elif both EMPTY: gSaveCounter = 0; first = 0; result EMPTY
else: gSaveCounter = 0; first = 0; result 2 (CORRUPT)
```

Then `sub_81258BC` (em: `CopySaveSlotData`) loads **slot `gSaveCounter % 2`**:
for each physical sector of that slot it copies data to chunk `id` when the
signature and checksum are good. It sets `gFirstSaveSector` (em:
`gLastWrittenSector`) to the physical index `i` of the last sector whose footer
says id 0, valid or not. It runs after every status: for EMPTY and CORRUPT
`gSaveCounter` is 0, so it reads slot A, and `first` ends up as slot A's last
id-0 footer, not the 0 set above.

Consequences:

- **The slot that gets loaded follows the counter's parity, not where the
  counter was found.** Counters in slot A must be even and in slot B odd. A
  Flipper write that breaks this makes the game load the wrong slot.
  Bulbapedia's "ties → save B" is only true for odd counters.
- The first-ever save goes to slot B: counter 0 → 1 (`WriteSaveBlockChunks`
  increments first; Bulbapedia "File structure").
- Load results (`src/main_menu.c`): ERROR prints "The save file is corrupt. The
  previous save file will be loaded."; 2 prints "The save file has been
  deleted..."; NO_FLASH prints the 1M sub-circuit message (`src/strings.c`).
- The Flipper's check before a write should be stricter than the game's: active
  slot OK; all 14 of its sectors with one equal counter N; `N % 2` = that
  slot's index; ids 0–13 each exactly once.

## 5.6 Field offsets

All offsets are within the struct. For ids 0 and 1 the section offset equals
the struct offset, because chunk 0 starts at struct offset 0.

Trainer (SaveBlock2, section 0), the same in all three games:

| Off | Size | Field | Source |
|---|---|---|---|
| 0x00 | 8 | `playerName` (7 chars + `0xFF` terminator, Gen III charset) | `include/global.h`, em/fr `include/global.h` |
| 0x08 | 1 | `playerGender` 0 = male, 1 = female | same |
| 0x0A | 4 | `playerTrainerId`: TID = u16 LE at 0x0A (visible), SID = u16 LE at 0x0C; the full OT ID = u32 LE at 0x0A (unaligned, so read bytewise) | same; Bulbapedia "Trainer ID" |
| 0x0E | 5 | play time: u16 hours, u8 min, u8 sec, u8 frames | same |

Party (SaveBlock1, section 1):

| Field | RS | E | FRLG | Size |
|---|---|---|---|---|
| `playerPartyCount` (u8, then 3 pad bytes) | 0x234 | 0x234 | 0x034 | 1 |
| `playerParty[6]` (`struct Pokemon`, 100 bytes each) | 0x238 | 0x238 | 0x038 | 600 |
| `money` | 0x490 | 0x490 (xor key) | 0x290 (xor key) | 4 |
| `coins` | 0x494 | 0x494 (xor key low 16) | 0x294 (xor key low 16) | 2 |

(`include/global.h` `struct SaveBlock1`; em/fr `include/global.h`; Bulbapedia
"Section 1".) `src/load_save.c: SavePlayerParty` copies `gPlayerPartyCount` and
the 6 mons into these fields on every save.

Boxes (PokemonStorage, sections 5–13; `include/pokemon.h` `struct
PokemonStorage`, same in em/fr):

| Storage offset | Size | Field |
|---|---|---|
| 0x0000 | 1 (+3 pad) | `currentBox` (0–13) |
| 0x0004 | 14 × 30 × 80 = 33600 | `boxes[box][slot]`, `struct BoxPokemon` (80 bytes) |
| 0x8344 | 14 × 9 = 126 | `boxNames` (8 chars + terminator), section 13 offset 0x744 |
| 0x83C2 | 14 | `wallpaper` / `boxWallpapers`, section 13 offset 0x7C2 |

Mon `i = box*30 + slot` (0-based) sits at storage offset `o = 4 + 80*i`,
section `5 + o/3968`, offset `o % 3968`. Eight mons straddle two sections.
Writing one of them changes **both** sections and both checksums (computed from
the formula):

| Mon i | Box, slot (0-based) | Starts in section | at offset | bytes in that section |
|---|---|---|---|---|
| 49 | 1, 19 | 5 | 0xF54 | 44 |
| 99 | 3, 9 | 6 | 0xF74 | 12 |
| 148 | 4, 28 | 7 | 0xF44 | 60 |
| 198 | 6, 18 | 8 | 0xF64 | 28 |
| 247 | 8, 7 | 9 | 0xF34 | 76 |
| 297 | 9, 27 | 10 | 0xF54 | 44 |
| 347 | 11, 17 | 11 | 0xF74 | 12 |
| 396 | 13, 6 | 12 | 0xF44 | 60 |

`struct BoxPokemon` has its own encryption: the 48-byte `secure` part is XORed
with `personality ^ otId` and carries a u16 `checksum` at 0x1C
(`src/pokemon_2.c: EncryptBoxMon`, `src/pokemon_1.c: CalculateBoxMonChecksum`).
This is independent of the section checksum. Empty slots are all zero
(Bulbapedia "PC Boxes Pokémon list").

Security key (`encryptionKey`):

| | RS | E | FRLG |
|---|---|---|---|
| Location | none | SaveBlock2 + 0xAC | SaveBlock2 + 0xF20 |
| XORed fields | — | money (u32, full key), coins (u16, low 16), bag item quantities (u16, low 16; all pockets, **not** PC items), `gameStats[64]` (u32 each, SaveBlock1 0x159C), berry powder (SaveBlock2 0x1F4) | money, coins, bag quantities, `gameStats` (SaveBlock1 0x1200), berry powder (SaveBlock2 0xAF8), `trainerTower[i].bestTime` |
| Source | — | em: `src/load_save.c: ApplyNewEncryptionKeyToAllEncryptedData`, `src/item.c`, `src/money.c`, `src/coins.c`, `src/overworld.c: ApplyNewEncryptionKeyToGameStats`, `src/berry_powder.c` | fr: same files |

- A new random key is made in `MoveSaveBlocks_ResetHeap` (em/fr
  `src/load_save.c`, called from `overworld.c` and `battle_main.c`). Always use
  the key of the same save.
- **Pokemon data is not XORed with this key**, so party and box edits do not
  need it.
- Bulbapedia's FRLG key offset 0x0AF8 (and its "copy at 0x01F4" in Emerald) is
  `berryCrush.berryPowderAmount` (`BerryCrush` + 8; fr: SaveBlock2 0xAF0, em:
  0x1EC). It equals the key only while berry powder is 0.
- The flash layout does not depend on the RAM shuffle of the save-block
  pointers (`SetSaveBlocksPointers`, by TID sum; em/fr `load_save.c`).

"Game code" (u32 at SaveBlock2 + 0xAC):

| Game | Value | Source |
|---|---|---|
| FRLG | `0x00000001` (`unkFlag1 = TRUE` at 0xAC, `unkFlag2 = FALSE` at 0xAD, padding 0) | fr: `src/new_game.c: NewGameInitData` |
| E | the security key (random) | em: `include/global.h` |
| RS | `battleTower.playerRecord.name[0..3]`: 0 until the player records a Battle Tower streak, then the first 4 name bytes | `include/global.h` (BattleTowerData at 0xA8, `name` at +4); `src/battle_tower.c: sub_8135AC4` (`StringCopy8(playerRecord->name, …)`) |

So use the cart header (§3.2) to identify the game. As a cross-check,
section 0's checksum is valid only with the right size[0] when the bytes past
0x890 are non-zero (they are for FRLG/E).

## 5.7 How the games write a save

Ruby/Sapphire (`src/save.c`):

| Save type | Caller | What is written |
|---|---|---|
| `SAVE_NORMAL`, `SAVE_OVERWRITE_DIFFERENT_FILE` | `start_menu.c`, `Save_WriteData` | `WriteSaveBlockChunks(0xFFFF)`: `gFirstSaveSector = (first+1) % 14`, `gSaveCounter++`. Then for id = 0..13, `WriteSingleChunk` builds the sector (cleared, data, footer) into physical `(id + first) % 14 + 14 * (counter % 2)` with `ProgramFlashSectorAndVerify` (erase, 4096 bytes ascending, verify, ≤ 3 tries). **Only the other slot is written**; the current slot remains the backup. On any damaged sector, counter and first are rolled back. OVERWRITE also erases sectors 28–31 first |
| `SAVE_HALL_OF_FAME` | `hall_of_fame.c` | sectors 28–29 (`HandleWriteSectorNBytes`), then a normal full save |
| Trade save | `trade.c` → `sub_8125D80` (`RestoreSaveBackupVarsAndIncrement`: counter++, first++), `sub_8125DA8` × 13 (ids 0..12, one per frame), `sub_8125DDC` (id 13 via `sub_81255B8`: erase, program 0x000–0xFF7 and 0xFF9–0xFFF, **skip 0xFF8**), then after a link sync `sub_8125E04` (`WriteSomeFlashByteToPrevSector`: program byte 0xFF8 = 0x25) | a full new slot whose last sector only becomes valid when its signature's low byte is written: a **commit byte** |
| Start-menu link save, record mixing, Berry Blender | `start_menu.c: Task_8071B64`, `record_mixing.c`, `berry_blender.c` → `sub_8125E2C` (`RestoreSaveBackupVars`, **no** increment) + `sub_8125E6C` | ids 0..4 rewritten **in place in the current slot** with the same counter, each via `sub_81255B8` and then its commit byte (`WriteSomeFlashByte0x25ToPrevSector`) |
| `SAVE_LINK` | only `link.c: LinkTestProcessKeyInput` (debug link test, R button) | ids 0..4 in place via `WriteSingleChunk` with the same counter |
| `SAVE_EREADER` | `battle_tower.c` | id 0 in place |

Emerald/FRLG are the same design under pret's newer names
(`WriteSaveSectorOrSlot`, `HandleWriteSector`, `HandleReplaceSector` +
`WriteSectorSignatureByte*`). Emerald's `SAVE_LINK` rewrites ids 0..4 in place
with the commit byte (em: `src/save.c: HandleSavingData`). FRLG's `SAVE_LINK`
uses `WriteSaveSectorOrSlot(i)` in place (fr: `src/save.c: HandleSavingData`).

Invariants a Flipper write must keep:

1. Each slot holds ids 0–13 exactly once.
2. All 14 sectors of a slot carry the same counter, and `counter % 2` = slot.
3. Id *k* sits at physical `(k + first) % 14` (the game accepts any rotation,
   but keep its convention).
4. Data past size[id] and 0xF80–0xFF3 are zero.
5. Sectors 28–31 are untouched.
6. Never erase the chip.

## 5.8 Minimal safe write strategy (Flipper)

Write a whole new slot into the **inactive** slot with counter N+1, the way the
game's own normal save does, and commit it last, the way the trade save does.
Never write the active slot.

```
0. Backup: read all 32 sectors to the SD card (recommended once per cart/session).
1. Read the footers of sectors 0..27 (READ_FOOTERS) and the needed data sectors;
   run §5.5. Refuse unless: active slot S OK, all 14 counters == N, N % 2 == S,
   game identified, size table known.
2. N' = N + 1 (mod 2^32); T = N' % 2 (= 1 - S); first' = (first_S + 1) % 14,
   where first_S = physical index (within S) of id 0.
3. For each id 0..13:
       image = active sector of that id (4096 bytes)
       if edited: patch its data, then image.checksum = CalculateChecksum(data, size[id])
       image.counter = N'          (id, signature unchanged; padding stays zero)
       dst = 14*T + (id + first') % 14
4. Pass 1, erase:   ERASE_SECTOR dst for all 14 (the slot reads EMPTY from now on).
5. Pass 2, program: WRITE_SECTOR dst, image, DEFER_COMMIT for all 14
   (bytes 0x000..0xFF7 and 0xFF9..0xFFF; byte 0xFF8 stays 0xFF, so the
   signature reads 0x080120FF = invalid). Unchanged sections can be COPY_SECTOR
   (helper-side copy with the counter patched) instead of an upload.
6. Pass 3, commit:  COMMIT_SECTOR dst (byte 0xFF8 = 0x25) for all 14.
7. Verify: READ_FOOTERS + SECTOR_CRC32 of the 14 new sectors; rerun §5.5 on the
   Flipper; it must now pick slot T with counter N'.
```

| Interrupted during | Slot T | Game loads | Message |
|---|---|---|---|
| pass 1 or 2 | no valid signature (erased or uncommitted) → EMPTY | slot S (N) | none |
| pass 3 | some committed sectors, < 14 ids → ERROR | slot S (N) | "The save file is corrupt. The previous save file will be loaded." (harmless; the next in-game save overwrites T) |
| after pass 3 | OK, N' > N | slot T (N') | none; slot S remains the backup |

- Party edits touch only id 1 (both in the same chunk). Trainer edits touch only
  id 0. Box edits touch ids 5–13 (two of them for straddling mons).
- **Increment the counter; do not keep it.** An in-place rewrite with the same
  counter, like Ruby's link save, puts the only good copy at risk. If the other
  slot is EMPTY (the player saved once), a torn active sector leaves nothing
  valid and the game reports "deleted".
- A raw restore of a full backup (all 32 sectors as they were) is the recovery
  path. It does write the active slot, so use it only to undo.

---

# 6. Flipper ↔ helper protocol (MultiPlay, Flipper = parent)

## 6.1 Link rules

- Every transfer carries one word each way: P sends in slot 0, H in slot 1. H's
  word in transfer *t* was loaded after transfer *t−1* (§4.8), so the earliest
  answer to P's word *t* appears in *t+1*.
- Half-duplex packets: one side sends a packet while the other sends idle words.
- Idle and status words (never the start of a packet):

| Word | Sender | Meaning |
|---|---|---|
| `0x0000` | P | idle |
| `0xB0A0` | H | `H_READY`: hunting for a request |
| `0xB0B0` | H | `H_BUSY`: executing (flash, ROM hash). P must send idle words only |

- After multiboot, P waits for **4 consecutive** `H_READY` words: the BIOS's
  last data-phase replies can be any value. Then P sends `HELLO`.
- Words of transfers that P's driver marks in `timeout_mask`/`framing_mask` are
  invalid. Word values `0xFFFF` are legal data: erased flash is all `0xFF`.

## 6.2 Packet format (both directions)

| Word | Content |
|---|---|
| w0 | `SOF = 0x55AA` |
| w1 | `(seq << 8) \| code`: P→H `code` = command; H→P `code` = status (§6.4); H echoes `seq` |
| w2 | `n` = number of payload words (0..2064) |
| w3 … w(2+n) | payload, little-endian (byte 2k = low byte of word k) |
| 2 words | CRC-32 of w1..w(2+n) as bytes (low byte first), low half then high half |

CRC-32 = IEEE/zlib: reflected poly `0xEDB88320`, init `0xFFFFFFFF`, xorout
`0xFFFFFFFF`, check("123456789") = `0xCBF43926`. H can update it per word
inside the transfer gap.

Receiver: hunt for `SOF`, take `n` (reject `n > 2064`), read exactly `n + 2`
more words, check the CRC. On a mismatch, drop the packet and resume hunting at
the word after the false `SOF`.

## 6.3 Commands

`n`/args in words; addresses and lengths as 2 words (low first).

| Code | Name | Args | Response payload | Notes |
|---|---|---|---|---|
| 0x01 | `HELLO` | — | proto version (1), helper build id, max payload words, `[0x020000C4] \| [0x020000C5] << 8` (boot mode 3, slave id 1) | |
| 0x02 | `CART_INFO` | — | 96 words cart header; flags (logo == own header, `0x96` ok, complement ok); ROM size (2 words, §3.7); save-library string kind and offset | scans again each time, and ends any arming: the cart may not be the one the arming checked |
| 0x03 | `SAVE_PROBE` | — | flash ID; class (0 = 128 KB MX-style, 1 = 64 KB, 2 = SRAM-like, 3 = none/unknown); `pre0, pre1`, ID bytes; SRAM-probe result | runs §3.3 once and caches the result; later calls return the cache |
| 0x04 | `ARM_WRITE` | magic `0x5752`, expected flash ID, target slot T, mode, active counter N (2 words) | status | First ends any arming and clears `erased_ok` (below), whatever the outcome. A wrong magic, T > 1 or mode > 1 is `BAD_ARG`; an expected ID other than the probed one is `REFUSED`. H re-reads the slots. Which slot the game loads is §5.5 exactly as the game runs it, with the game's own section sizes (§5.4) from the cart header's code (`AXV`/`AXP` Ruby/Sapphire, `BPR`/`BPG` FireRed/LeafGreen). Mode 0 (a write): the game must load slot 1−T, and slot 1−T must also be a clean save: 14 sectors with valid signature, full-length checksum (§5.3, so the bytes past each section are zero), ids 0–13 once each, one counter N, N % 2 = 1−T. Mode 1 (a restore onto an empty or corrupt save): the game must load neither slot, and neither may be a clean save. Otherwise `REFUSED`. A slot the game takes is never written, whatever its padding. Arms writes for the 14 sectors of slot T only |
| 0x05 | `DISARM` | — | status | also automatic once all 14 sectors of slot T are completed (below), on any flash error, `HELLO`, `CART_INFO`, and every `ARM_WRITE` |
| 0x10 | `READ_SECTOR` | sector 0..31 | 2048 words | |
| 0x11 | `READ_FOOTERS` | — | 32 × 8 words: bytes 0xFF4..0xFFF (6 words), full-length checksum (1 word), 0 | ≈ 260 transfers; enough for §5.5 except the exact-size check |
| 0x12 | `SECTOR_CRC32` | sector | 2 words | read-back verify without re-reading over the link |
| 0x18 | `ERASE_SECTOR` | sector | status, fail code | armed and in slot T only |
| 0x19 | `WRITE_SECTOR` | sector, flags (`DEFER_COMMIT` = 1, required: any other value is `BAD_ARG`), 2048 words image | status, fail code, fail offset | the CRC must pass **before** any flash access. Erase first, unless this session erased the sector OK and nothing has programmed it since (the session's `erased_ok` mask: a successful erase sets the sector's bit; any program or commit to it clears it; so do start-up and `HELLO`). A sector that merely reads all 0xFF is erased again: an erase cut short by a power loss can leave cells that read 0xFF but aren't fully erased. Program in ascending order, skipping 0xFF8, then verify. Up to 3 tries; a failed try erases again. OK marks the sector **programmed** in this arming |
| 0x1A | `COPY_SECTOR` | src sector, dst sector, flags (as `WRITE_SECTOR`), new counter (2 words) | status, fail code, fail offset | src must be in slot 1−T (else `BAD_ARG`). H reads src into RAM, patches the counter at 0xFFC, then proceeds as `WRITE_SECTOR` |
| 0x1B | `COMMIT_SECTOR` | sector | status | only a sector programmed in this arming and not erased or written since (else `REFUSED`); programs 0xFF8 = `0x25` only if it reads `0xFF` and 0xFF9..0xFFB read `20 01 08`; then verifies 0xFF8..0xFFB = `0x08012025` |
| 0x20 | `ROM_READ` | address (2), words (1..2048) | data | the whole range within 0x08000000..0x09FFFFFF, address even |
| 0x21 | `ROM_SHA1` | address (2), length (2) | 10 words | `H_BUSY` while hashing. Not implemented by the helper |
| 0x22 | `ROM_CRC32` | address (2), length (2: 2..0x40000, even) | 2 words | faster than SHA-1; the same range rule as `ROM_READ` |

The flash reads (0x10–0x12) answer `UNSUPPORTED` unless `SAVE_PROBE` ran and
found a 128 KB flash: on an SRAM the bank command would land as data.

All write-class commands (`ARM_WRITE`, 0x18–0x1B) refuse everything when §3.6
checks 1–4 failed: `CART_INFO` must have run and found the logo, `0x96`, the
complement, one of `AXVE AXPE BPRE BPGE`, `FLASH1M_V` and no other library
string, and the code must name a save layout the helper knows; `SAVE_PROBE`
must have found a 128 KB flash with ID `0x09C2` or `0x1362` (never `0xD4BF`,
which only German Sapphire version 0 takes as 128 KB, and which is never
written). 0x18–0x1B also refuse sectors 28–31; then `NOT_ARMED`, or `REFUSED`
for a sector outside slot T.

A sector is **completed** when `COMMIT_SECTOR` gives it its valid signature;
writes never program the signature's first byte themselves. A commit is
refused unless this arming programmed that sector, and while another sector of
slot T holds a valid signature it didn't get in the current arming (`REFUSED`,
fail offset = that sector). Images left in slot T by an earlier arming (a link
drop, a power cut) are therefore never committed. Slot T can become a valid
slot, and the game's choice (§5.5), only as a whole save programmed and
committed by the current arming, and the helper disarms at that moment. The
§5.8 order (erase all 14, program all 14 deferred, commit all 14) always passes.

## 6.4 Status codes

| Code | Name |
|---|---|
| 0x00 | `OK` |
| 0x01 | `BAD_PACKET` (CRC or length) |
| 0x02 | `BAD_ARG` |
| 0x03 | `UNSUPPORTED` (no 128 KB flash) |
| 0x04 | `NOT_ARMED` |
| 0x05 | `REFUSED` (cart checks failed, or the target is the active slot) |
| 0x06 | `ERASE_FAIL` (fail code = pret-style `phase \| 0xA000/0xC000`) |
| 0x07 | `PROGRAM_FAIL` (+ offset) |
| 0x08 | `VERIFY_FAIL` (+ offset) |
| 0x09 | `NOT_ERASED` (commit byte already programmed) |

`REFUSED`'s fail code tells which check refused, for the log: 1 the cart, 2 the
chip, 3 `ARM_WRITE`'s flash ID, 4 the slots, 5 a sector outside slot T, 6 another
sector of slot T still valid, 7 a commit of a sector this arming didn't program,
8 a game code with no known save layout (§6.3).

## 6.5 Busy, retries, duplicates

- Each new command gets the next `seq` (8 bits). H remembers the `seq`, the
  code and the packet CRC-32 (which covers w1, `n` and every argument) of the
  last packet it executed, and its response. A packet right after it with the
  same `seq`, code and CRC, whose command is write-class (`ARM_WRITE`,
  0x18–0x1B), gets that response again **without executing again**. Any other
  valid packet replaces the
  memory, so a `seq` that wraps can never alias; a packet with a bad CRC isn't
  one and changes nothing. Reads simply re-execute.
- P timeouts:
  - No `SOF` within 32 transfers after the request's last word, while H sends
    neither `H_BUSY` nor a packet: resend.
  - `H_BUSY` longer than the command's budget: give up, report a flash failure,
    keep the slot analysis. The budget is per command:
    - `CART_INFO`: 20 s. Its whole-ROM scan takes ~6.7 s for 16 MiB and ~14.5 s
      for 32 MiB (§3.3, M7), so a 32 MiB cart that isn't a Pokemon game still
      gets its refusal instead of a link timeout.
    - `ERASE_SECTOR`, `WRITE_SECTOR`, `COPY_SECTOR`: 12 s. Up to 5 erase tries
      of up to 2.04 s each, then the program and the verify (§3.4).
    - Every other command: 6 s. `ROM_CRC32` is limited to 0x40000 bytes per
      call to stay under it.
  - 3 failed resends: reset the link (wait for `H_READY`, `HELLO`).
- H receive timeout: abort a packet if no transfer arrives for 200 ms (TM3 ≥ 196).

## 6.6 Throughput

Transfer rate with our driver: today's frame mode runs 9 transfers per 16.74 ms
frame, **537.6 transfers/s**. A free-running "stream" mode, one transfer per
1073 µs TIM2 tick with no frame gating, gives **932 transfers/s**. Payload ≈
2 bytes per transfer in one direction. The table assumes no retries
**(inferred, M22)**.

| Operation | Transfers | Frame mode | Stream mode |
|---|---|---|---|
| Send helper (8 KB) | ≈ 4300 | 8.0 s | 4.6 s |
| `READ_FOOTERS` | ≈ 270 | 0.5 s | 0.3 s |
| `READ_SECTOR` (4 KB) | ≈ 2061 | 3.83 s (1.07 KB/s) | 2.21 s (1.85 KB/s) |
| Sections 0 + 1 (trainer + party) | ≈ 4122 | 7.7 s | 4.4 s |
| Active slot (14 sectors) | ≈ 28 900 | 54 s | 31 s |
| Full 128 KB backup | ≈ 66 000 | 123 s | 71 s |
| `WRITE_SECTOR` upload + flash | ≈ 2062 + busy | 3.8 s + ~0.2–0.5 s | 2.2 s + ~0.2–0.5 s |
| Party edit commit (1 upload, 13 copies, 14 erases, 14 commits) | ≈ 2200 + busy | ≈ 4 s + ~4–8 s flash | ≈ 2.5 s + ~4–8 s flash |
| `ROM_SHA1` 16 MiB | ≈ 30 + busy | ≈ 30–60 s, mostly GBA time | same |
| Full ROM dump 16 MiB | ≈ 8.4 M | ≈ 4.4 h | ≈ 2.5 h |

Flash times are estimates (M10). Stream mode keeps interrupts masked for
≈ 321 µs out of every 1073 µs (30 %, against 17 % today). Check its effect on the
Flipper's BLE/USB **(inferred, M18)**.

## 6.7 Error detection summary

| Layer | Mechanism |
|---|---|
| Wire | P: driver `timeout_mask`/`framing_mask`; H: `SIOCNT` bit 6 (error), ID bits = 1 |
| Packet | `SOF` + length bound + CRC-32; `seq` echo |
| Command | H checks arguments, the armed state, slot T only, sectors 28–31 never |
| Flash | DQ5 + timer timeouts (pret), erase ×5, program+verify ×3, `COMMIT_SECTOR` precondition |
| Save | P recomputes every section checksum with the exact size; §5.5 strict checks before and after a write; `SECTOR_CRC32` read-back |

## 6.8 Link drop or power loss in the middle of a write

- **During an upload:** the image is only buffered in RAM. Flash is never
  touched until the whole packet passes its CRC, so a drop changes nothing. H
  aborts the partial packet after 200 ms and returns to `H_READY`.
- **During a flash operation:** the operation does not need the link. H
  finishes erase, program and verify (≤ ~0.5 s typical, ≤ 2.04 s per erase
  attempt worst case) whether or not the Flipper still clocks. It then waits,
  holding `H_BUSY` until the next transfer, and sends the result. A Flipper
  that comes back re-sends the same packet and gets the stored response
  (§6.5). **A link drop therefore never leaves a half-written sector.** Only
  power loss or cart removal during those windows can. The red backdrop (§4.8)
  tells the user not to power off.
- **Why a torn sector is still harmless:** the Flipper only ever writes slot T,
  the slot the game does not load (§5.8). Every write defers the commit byte
  (`WRITE_SECTOR` and `COPY_SECTOR` refuse to do otherwise), so a sector's
  signature can only become valid through `COMMIT_SECTOR`, after the whole
  sector, its counter at 0xFFC included, is programmed and verified. A power cut
  in the middle of programming never leaves a valid signature over a torn
  counter. The helper commits only sectors the current arming programmed
  (§6.3), so a reconnect can't complete images an interrupted write left
  behind; the Flipper erases and programs them again. The slot as a whole
  becomes valid only when all 14 ids are committed. So at every instant the
  game's §5.5 choice is either the untouched old slot S or the complete new
  slot T. That is the same guarantee Ruby relies on: two alternating slots for
  normal saves, plus a deferred signature byte for trade saves.

## 6.9 Driver changes needed (`mlink_hw_furi.c`)

1. Multiboot runs on the existing driver: 1-word frames for the handshake loops,
   9-word frames for header and data (§2.4).
2. Recommended: a free-running mode where each frame starts on the next tick,
   with a larger `MLINK_BURST_LEN` (≤ 16 because of the masks). The Ruby link
   needs its frame cadence; the helper does not.
3. Keep `timeout_mask`/`framing_mask` per word: the protocol uses them, because
   `0xFFFF` is legal data.


---

# 7. Normal mode over a Game Boy Color cable

For a Game Link module whose SD line is open (KBEmbedded's with its SD jumper
not bridged), MultiPlay cannot run at all, and neither can a trade. Cartridge
then talks to the GBA in Normal mode over a Game Boy Color link cable. The
MultiPlay path of §2 and §6 is unchanged and stays the default.

## 7.1 The cable

- A GBC link cable crosses SO and SI and carries SC; it has no SD (GBATEK "AUX
  Link Port", "Cable Diagrams"). That is what Normal mode needs: each end's SO
  goes to the other's SI, and the master drives SC. It works with either end in
  the Flipper.
- MultiPlay needs SD, so a module with SD open never gets a reply: every frame
  logs `T:B9A0 R:FFFF` with a timeout.
- Probes of 2026-10-30:
  1. Ruby linked through the module with SO driven low so that Ruby acted as
     master: SC (PB2) showed Ruby's transfers; SD showed nothing on PA4, PA7,
     PA6, PC1 or PC0. The module's SD jumper is open.
  2. GBC cable, GBA powered on holding START+SELECT, a JS bit-banged Normal
     master (MSB first, SO set after SC falls, SI read after SC rises) sending
     `0x62026202`. The BIOS answered `FFFFFFFF`, then `0000xxxx`, then a steady
     `72026203`. So Normal-mode multiboot reaches the BIOS in both directions.

## 7.2 Multiboot in Normal mode

The BIOS runs the same sender protocol as §2 (GBATEK "BIOS Multi Boot (Single
Game Pak)"), with these differences:

| Item | MultiPlay (§2) | Normal (new) |
|---|---|---|
| Unit on the wire | 16 bits | 32 bits; master's value in the low half, upper half zero; reply in the high half (probe: `7202` in the high half) |
| CRC init / polynomial | `0xFFF8` / `0xA517` | `0xC387` / `0xC37B` |
| Data key xor | `0x6465646F` | `0x43202F2F` |
| Seed multiplier, seed init, `hh`, length word, final CRC form | same | same |
| Data block | two 16-bit halves per word, low first | one 32-bit encrypted word per transfer |
| Data reply | low 16 bits of the byte offset of the halfword (current check) | low 16 bits of the byte offset of the word, `(uint16_t)(0xC0 + 4k)` **(inferred, M23)** |
| Master clock | 115200 bit/s on SD | 256 kHz on SC: bit out after SC falls, sampled after SC rises; slave ready = master's SI low |

Independent vector (Python from GBATEK's formulas), with `pp=0x93` and
`cc=0x5A`:

- seed `0xFFFF5A93`, crc `0xC387`;
- word at `0xC0`, plain `0x12345678` gives `0x7036291D`, crc `0x4DD0`;
- word at `0xC4`, plain `0x9ABCDEF0` gives `0x47EF9D9C`, crc `0x1271`;
- `hh(0x5A) = 0x69`; the final CRC with `rr=0x3C` is `0xF80B`.

The boot mode the BIOS writes at header `0xC4` is 3 for MultiPlay and 2 for
Normal (GBATEK "Multiboot Header"); the helper reads it (§7.4, M25). A sender
that gets an unexpected word restarts, and the log's `mb` line records the
expected and received words (`want %04x got %04x`) for the failures
Header, Data, Handshake, Length and Crc.

## 7.3 The Normal wire

`mlink_hw_furi.c` gains a second wire, `MlinkWireNormal`, beside `MlinkWireMulti`
(`mlink_types.h`). Stream mode only, no cable guard (`mlink_hw_cable_ok` reads
true while it runs). The tick stays 1073 µs, so throughput equals MultiPlay's.
SD is left alone, in analog mode.

- **The word (D2).** The Flipper sends `(uint32_t)tx.hi[t] << 16 | tx.word[t]`
  MSB first and reports the reply's **high** half as `rx.word[t]`. The driver
  zeroes `next.hi[]` before every frame callback, so only multiboot's data words
  set a high half. The helper protocol uses the same convention.
- **Ready, stall, not ready (D3), when gated.** At each tick the driver reads
  SI. SI high: the slave isn't ready; the slot waits for the next tick, up to
  `MLINK_NORMAL_STALL_TICKS` (16). After that the slot is *not ready*:
  `notready_mask` and `timeout_mask` bit set, word `MLINK_NO_DATA`, nothing
  clocked. SI low: the driver masks interrupts, waits 4 µs, checks that SI is
  still low, and clocks 32 bits.
- **The ready gate** (`mlink_hw_set_normal_gate`). Gated is the driver's
  default (`mlink_hw_alloc`). Ungated, every slot clocks on its tick: no SI
  check, no settle, one tick per slot, never not ready. A slave that isn't
  there, or isn't in Normal mode yet, reads `FFFF`. The ISR reads the flag once
  per slot; it may be set while the driver runs and is kept across starts.
- **The BIOS phase is ungated; the helper's is gated.** The Cartridge runner
  clears the gate before it starts multiboot on the Normal wire, and sets it
  right before the pump takes over from the sender. The 2026-10-30 probe
  clocked ungated at about one word per ms, and the BIOS's first reply was
  `FFFFFFFF`: GBATEK's "slave not in mode yet", then `0000`. So the BIOS takes
  Normal mode only once transfers come, and probably doesn't hold SO low before
  the first clocks (M27): a gated start might never clock at all. The helper
  arms each transfer with SO low (§7.4), so the gate is right for it.
  `GEN3_CART_MB_GATED 1` gates the BIOS phase too (§7.5).
- `rx.ticks` is the frame's time in ticks; callers multiply by `MLINK_TICK_US`.
  On the MultiPlay wire it is `max(count, 1)`, so the cores' timing there is
  unchanged.
- **Not ready is BUSY in the pump (D4).** A slave that runs a command can't
  answer, so the helper sends no `0xB0B0` over this wire. `gen3_hlink` handles a
  not-ready word like a valid BUSY wherever it waits (Sending before READY,
  Waiting); in Sync it breaks the run of READYs; inside a response it is a wire
  error, like a timed-out word, and the client's resend recovers.
- **A cable pulled inside a packet.** 16 not-ready words in a row
  (`GEN3_HLINK_NOTREADY_RUN`) inside a packet, a request after its READY or a
  response after its SOF, fail it as `Wire` at once: the rest of a request isn't
  sent, the rest of a response isn't read. 16 slots of 16 ticks are about
  275 ms, past the helper's 200 ms receive timeout, so the helper has dropped
  the packet and hunts again. A delivered word starts the run again. The
  client's resend then waits for READY, and a helper that stays not ready is
  waited for like a BUSY one, within the request's BUSY budget (§6.5). So a pull
  ends a write with `BusyTimeout` within about 12 s, and a read within about
  6 s, instead of after one stall per word left (35–45 s). A resend of a
  write-class command is answered from the helper's stored response, so the
  save is as safe as before (§6.8).

## 7.4 The helper's Normal transport

`main.c` reads the boot mode the BIOS left at header `0xC4`: 2 runs the Normal
slave loop, anything else today's MultiPlay loop. One Normal transfer:

1. `SIODATA32 = out << 16`.
2. `IF = 0x0080`: drops a transfer-end flag latched after the last transfer's
   timeout (a transfer that ended just too late), so it can't end this one
   early.
3. `SIOCNT = 0x5000`: 32-bit, IRQ, external clock, start 0, SO low ("ready").
4. `SIOCNT = 0x5080`: start, SO still low. (GBATEK is ambiguous about whether
   bit 3 drives SO while a transfer is armed; 0x5080 is ready under either
   reading. M24.)
5. Wait for the end of the transfer (IF bit 7, as in MultiPlay) for up to 200 ms.
6. `SIOCNT = 0x5008`: SO high, "not ready", whatever the hardware left.
7. The Flipper's word is `(uint16_t)SIODATA32`.

After a packet's last word, `hp_execute` runs at once. No BUSY transfer happens,
and the next transfer carries the response's SOF. The Flipper sees the helper's
busy time as not-ready slots (§7.3).

## 7.5 The wire choice at Connect

The Cartridge runner picks the wire (`gen3_mb_wire_next(wire, si_low, phase)`):

- MultiPlay stays only while SI reads low, or once the GBA has answered (any
  phase past Detect). A grounded SI is a GBA cable on a module with SD.
- SI high while the sender is in Wait or Detect switches to Normal, and Normal
  never switches back within one Connect. During multiboot the runner polls
  every 20 ms and switches only after SI read high on 2 polls in a row, so a
  GBA cable jiggled in its socket doesn't move to Normal for good.
- Connect starts from `gen3_mb_wire_next(MlinkWireMulti, si_low_now, Detect)`.
  A GBC cable (SI floats high or follows the GBA's SO), a reversed GBA cable or no
  cable ends up on the Normal wire, with `Waiting for GBA` and then the power-on
  hint. `Cable: plug in/swap ends` no longer shows during Connect.
- With the GBA off at Start and a GBC cable, SI may read low through the
  unpowered GBA: Connect then starts on MultiPlay and switches once the GBA is
  powered on. Expected.
- Tunables, at the top of `src/gen3/gen3_cart_furi.c` ("Tunables for the
  hardware test"), for whoever reads a failed run's log and rebuilds:
  - `GEN3_CART_MB_GATED` (0): 1 gates the BIOS phase too (§7.3), in case the
    BIOS does hold SO low until it is ready.
  - `GEN3_CART_FORCE_NORMAL` (0): 1 starts every Connect on the Normal wire,
    whatever SI reads, for a GBC cable on which SI reads low so that Connect
    stays on `wire multi`.
- Log lines (`apps_data/pokemon/gen3_cart.log`):
  - `wire multi`, or `wire normal ungated` (`wire normal gated` with
    `GEN3_CART_MB_GATED`), when Connect starts; `wire normal ungated (SI high)`
    when it switches during multiboot; `wire normal gated (helper)` when the
    pump takes over and the gate goes on.
  - Every `mb` line: `nr` (not-ready slots since the sender started; 0 when
    ungated) and `last` (the latest reply word delivered; `ffff` with nothing at
    the other end, or a BIOS not in Normal mode yet), then `want`/`got` after a
    Header, Data, Handshake, Length or Crc failure, then the counters. The short
    fields come first, and no line is cut.
  - `mb wait` every 5 s while the sender waits for the GBA, and `mb cancel` when
    Back stops Connect during multiboot, besides `mb restart`, `mb done` and
    `mb failed`.
  - `cmd <code> fail <n> nr <k>` for each exchange that failed on the link: the
    command (§6.3), the pump's `Gen3HlinkFail` (1 no READY, 2 no response,
    3 BUSY timeout, 4 bad CRC, 5 wire, 6 seq, 7 too long) and the not-ready
    words it saw. `sync fail nr <k>` when a connect saw no READYs.
  - `hello proto <p> build <b> boot <m>`: `boot` is the boot mode the BIOS
    left at header `0xC4`, 2 for a Normal-mode boot, 3 for MultiPlay (M25).
- Caution: on a reversed GBA cable the Normal wire drives SC, and a GBA acting as
  MultiPlay parent at the other end (a game in the Cable Club) would fight it.
  Use Cartridge only with the GBA held at START+SELECT at power-on.

---

# Uncertain

| # | Item | Why uncertain / how to settle |
|---|---|---|
| M1 | JOY Bus is impossible on AGB-005 | inferred from GBATEK's "SI and SO pins are data lines" plus the cable wiring; untested |
| M2 | General-Purpose bit-bang link as a faster later mode | design idea; needs its own driver and a test |
| M3 | The BIOS slave tolerates our ≈ 7 ms gaps between frames and has no inter-transfer timeout | not documented; afska's async master leaves frame-sized gaps. Hardware test: send the helper in frame mode |
| M4 | Slave behaviour after a bad header, logo, `0x96`, complement or CRC (back to waiting, or a power cycle needed) | not documented. Test by sending a corrupted image |
| M5 | Cart GPIO control reads 0 after power-on, so ROM reads at 0xC4–0xC9 return ROM data | GBATEK describes write-only mode; the power-on value is not stated |
| M6 | Japanese RSE/FRLG save layouts differ; only English/German layouts are covered | pret covers US and German only |
| M7 | ROM string scan takes ~6.7 s (16 MiB, WAITCNT 0x4317) | cycle count of the helper's loop (~27 per word); measure on hardware |
| M8 | A single stray write (not 0xAA) to 0x0E005555 is ignored by MX29L010/LE26FV10N1TS | JEDEC-style state machine behaviour; check against the datasheets or test on a genuine cart with a backup |
| M9 | Part numbers of the 64 KB chips | GBATEK gives maker names only |
| M10 | Typical sector erase / byte program times (we budget 0.1–0.5 s per sector) | only Nintendo's timeouts (10 ms, 2.04 s) are in the sources; measure on hardware |
| M11 | Which flash chip German Sapphire rev 0 carries (the game expects ID `0xD4BF` in its MX entry) | pret constant only; no gbhwdb entry checked |
| M12 | What SRAM/batteryless repros return for the ID sequence | community descriptions; no repro tested |
| M13 | Repros with real 128 KB flash and an unpatched ROM behave like originals | assumption |
| M14 | ROM bytes read by the helper match the pret SHA-1 reference dumps, including 0xC4–0xC9 | dumps are made over the same bus; verify on one cart |
| M15 | SHA-1 of 16 MiB on the GBA takes ~30–60 s | estimate (~30–50 cycles/byte in IWRAM ARM code) |
| M16 | `IF` bit 7 latches with `IME = 0` (`IE` bit 7 set, `SIOCNT` bit 14 set) | standard GBA behaviour (emulators implement it so); fallback: poll `SIOCNT` bit 7 |
| M17 | Rewriting `SIOCNT` at helper start may corrupt one transfer | harmless under the protocol (P waits for 4 × `H_READY`) |
| M18 | Stream mode (30 % masked duty) is acceptable for Flipper BLE/USB | measure |
| M19 | The `cc` byte stays the same across repeated `0x73cc` replies (we use the last one before `0x64hh`) | GBATEK/afska use the reply seen; 1-word frames make "last" unambiguous |
| M20 | Waiting 100 ms instead of exactly 1/16 s before the length word is fine | GBATEK gives a minimum; afska uses ~67 ms |
| M21 | The helper's bank state after power-on is bank 0 | always select the bank explicitly before each access (the code above does) |
| M22 | Throughput rows assume the CRC/turnaround overhead above and no retries | estimates |
| M23 | In Normal-mode multiboot the data reply is the low 16 bits of the next word's offset, `0xC0 + 4k` | GBATEK describes the MultiPlay halfword form only; inferred. A mismatch logs `mb failed … want … got …` (Data). **Confirmed on hardware 2026-10-31** (GBA, Ruby, GBC cable: `mb done … restarts 0`, `hello … boot 2`, a backup and a write with 0 resends) |
| M24 | SO goes high at the end of a Normal transfer (forced anyway by the helper's `0x5008`); and the helper's `0x5080` keeps SO low while armed | GBATEK is ambiguous about SIOCNT bit 3 while armed. If every slot stays not ready (`sync fail nr …` or `cmd … fail … nr …` lines with a high `nr`), the GBA may show the shift register's MSB on SO while armed; record the log. **Confirmed on hardware 2026-10-31** (GBA, Ruby, GBC cable: `mb done … restarts 0`, `hello … boot 2`, a backup and a write with 0 resends) |
| M25 | The BIOS writes boot mode 2 at header `0xC4` for Normal-mode multiboot | GBATEK "Multiboot Header"; unverified on hardware, and the helper runs the MultiPlay loop for any other value. The log's `hello … boot` line shows the value once HELLO gets through; a helper in the wrong loop never answers, which leaves only `sync fail nr …` lines. **Confirmed on hardware 2026-10-31** (GBA, Ruby, GBC cable: `mb done … restarts 0`, `hello … boot 2`, a backup and a write with 0 resends) |
| M26 | The BIOS and the helper sample our 256 kHz bit-banged SC reliably | the probe of 2026-10-30 reached the BIOS at that rate; the helper side is untested. **Confirmed on hardware 2026-10-31** (GBA, Ruby, GBC cable: `mb done … restarts 0`, `hello … boot 2`, a backup and a write with 0 resends) |
| M27 | The BIOS does not hold SO low (our SI low) before transfers come, so the BIOS phase must clock ungated (§7.3) | The probe of 2026-10-30 clocked ungated; its first reply, `FFFFFFFF`, matches GBATEK's "slave not in mode yet", then `0000`: the BIOS takes Normal mode only once clocks come. So the BIOS phase runs ungated (as built) and the gate applies to the helper only. If a GBC-cable Connect never leaves `Waiting for GBA`, the `mb wait … nr … last` lines show what came back; rebuild with `GEN3_CART_MB_GATED 1` to try the gated BIOS phase. **Confirmed on hardware 2026-10-31** (GBA, Ruby, GBC cable: `mb done … restarts 0`, `hello … boot 2`, a backup and a write with 0 resends) |

# References

- GBATEK sections used: "BIOS Multi Boot (Single Game Pak)", "SIO Normal Mode",
  "SIO Multi-Player Mode", "SIO JOY BUS Mode", "SIO General-Purpose Mode",
  "SIO Control Registers Summary", "AUX Link Port", "GBA Cartridge Header",
  "GBA Cartridge ROM", "GBA Cart Backup IDs", "GBA Cart Backup SRAM/FRAM",
  "GBA Cart Backup EEPROM", "GBA Cart Backup Flash ROM", "GBA Cart I/O Port
  (GPIO)", "GBA Unpredictable Things", "GBA System Control", "GBA Interrupt
  Control", "BIOS RAM Usage".
- pret: pokeruby `src/save.c`, `include/save.h`, `src/load_save.c`,
  `src/libs/agb_flash*.c`, `include/gba/flash_internal.h`, `include/global.h`,
  `include/pokemon.h`, `src/main_menu.c`, `src/strings.c`, `src/trade.c`,
  `src/start_menu.c`, `src/battle_tower.c`, `config.mk`, `*.sha1`;
  pokeemerald/pokefirered `src/save.c`, `include/save.h`, `src/load_save.c`,
  `src/agb_flash*.c`, `include/global.h`, `include/pokemon_storage_system.h`,
  `src/new_game.c`, `*.sha1`.
- https://github.com/afska/gba-link-connection/blob/master/lib/LinkCableMultiboot.hpp
- https://github.com/FIX94/gba-link-cable-dumper (`gba/source/main.c`, `gba/source/libSave.c`)
- https://github.com/kwsch/PKHeX (`PKHeX.Core/Saves/SAV3.cs`, `SAV3RS.cs`, `SAV3E.cs`, `SAV3FRLG.cs`)
- https://bulbapedia.bulbagarden.net/wiki/Save_data_structure_(Generation_III)
- https://gbhwdb.gekkio.fi/cartridges/ (AGB-AXVE-0, AGB-AXPE-0/1/2, AGB-BPEE-0, AGB-BPRE-1, AGB-BPGE-0/1, AGB-AXVI-1, AGB-AXVF-0)
- https://github.com/bbsan2k/Flash1M_Repro_SRAM_Patcher, https://github.com/metroid-maniac/gba-auto-batteryless-patcher (READMEs)
- https://github.com/DS-Homebrew/GodMode9i `arm9/source/gba.cpp` (Atmel page-write handling, a second reference)
- devkitPro gba-tools `gbafix.c` (logo table and complement, for the build step)
