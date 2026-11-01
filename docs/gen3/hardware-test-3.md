# Gen III hardware test 3: cartridge save access

Goal: the Flipper sends its helper program to a GBA over the link cable (multiboot), reads the cartridge, and backs up its save to the SD card. Later sections cover browsing and editing the save's Pokemon, writing it back, restoring a backup and copying the ROM. A save is never written without a verified backup first, and a write that is cut short (cable pulled, GBA switched off) never leaves the game without a save it loads.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA (or GBA SP), GBA link cable, a genuine Pokemon Ruby cartridge and a genuine Pokemon FireRed cartridge.

**Cable.** With a Game Link module whose SD line is connected (KBEmbedded: SD jumper bridged), use the GBA link cable, small plug in the Flipper. Otherwise use a Game Boy Color link cable, either end. The log's `wire` line says which link Connect used.

**Build and install.** Run `ufbt` (release build). It builds two apps: `dist/pokemon.fap` (Pokemon Trade Tool) and `dist/pokemon_cart.fap` (Pokemon Cartridge). Copy both into the same folder on the SD card, for example `apps/GPIO/` with qFlipper. Or run `ufbt launch APPID=pokemon_cart`, then `ufbt launch APPID=pokemon`: each installs its app in `apps/GPIO/` and starts it.

## Notes

- **Two apps.** Cartridge is its own app, Pokemon Cartridge, because the Flipper loads an app whole into RAM and the cartridge code doesn't fit beside the trade code. Gen III → `Cartridge` in Pokemon Trade Tool closes that app and opens Pokemon Cartridge. When you leave Pokemon Cartridge, the Flipper returns to its app list. Pokemon Cartridge can also be started from the app list directly.
- **Save the Pokemon you are editing first.** Opening Cartridge closes Pokemon Trade Tool, and with it the Pokemon in its editor. Use `Save .pk3` first if you want to keep it.
- **Assets.** After the first Cartridge start, `apps_assets/pokemon_cart/gen3_helper.mb` and `apps_assets/pokemon_cart/gen3_bulk.bin` exist on the SD card. The Flipper unpacks them from `pokemon_cart.fap`.
- **Pins.** Set the pins in Pokemon Trade Tool's Pinout screen. Cartridge copies them at every start; it has no Pinout screen of its own.
- **The log** is `apps_data/pokemon/gen3_cart.log`. Cartridge creates it again at the first `Start` on the instructions after it opens, and later Starts in the same run add to it. So after a failed run, copy it off the SD card **before you open Cartridge again**. Keep the log from every failed run.
- **Backups** are in `apps_data/pokemon/gen3/backups/`, named `<game code>_<TID>_<date>-<time>.sav`, for example `AXVE_12345_20261027-093015.sav`. Each is the whole 128 KB flash chip, as emulators and PKHeX load it. Cartridge never deletes one, not even when an SD card error stops it right after the backup was checked (the hub then offers `Back up again`).
- **Change cartridges only with the GBA off.** The helper reads the cartridge again at every connect, but a cartridge pulled or pushed in while the GBA is on can upset it.
- **After `Written` or `Restored`** the hub shows `Reconnect` and hides `Restore backup` and `Copy ROM`: the result says to power-cycle the GBA, which ends the helper. To go on with the GBA on, choose `Reconnect`; after a power cycle, connect again as in A.3.
- **The instructions screen** (`Cartridge`) reads: `GBA off, cart in.` / `Small plug: Flipper.` / `Hold START+SELECT` / `and power on.`, with `Back` and `Start`.
- **The progress screen** shows what is happening on top, two lines under it, a bar, and at the bottom `Back: stop` (Back stops the step), `Do not power off` (while the save is being written), or nothing.
- **The log's `mb` lines** record each helper upload: `nr` (link slots the GBA never got ready for; 0 unless the upload waits for the GBA's ready) and `last` (the last word the GBA sent; `ffff` when nothing answers), `restarts` (failures counted) and `all` (every restart, counted or not), `cc` and `rr` (the GBA's two handshake bytes), `palette_busy` (how often the GBA answered "busy" to the palette word before it went on, at about 1 ms each, against a 4 s limit) and `final_us` (microseconds from the Flipper's first `0x0065` to the GBA's `0x0075`, against a 5 s limit). A.3 asks for the last two. While the Flipper waits for the GBA, an `mb wait` line comes every 5 s; Back during the upload logs `mb cancel`.
- **The `wire` lines** in the log: `wire multi` or `wire normal ungated` when Connect starts; `wire normal ungated (SI high)` when it switched during multiboot; `wire normal gated (helper)` when the upload is done and the Flipper starts talking to the helper. With a GBC cable and the GBA off at Start, SI may read low through the unpowered GBA, so Connect may begin with `wire multi` and switch to `wire normal ungated (SI high)` once the GBA is powered on. That is expected. A failed upload also logs `mb failed … want %04x got %04x` (expected and received word; Header, Data, Handshake, Length or Crc failures only).
- **The helper's lines** in the log: `hello proto 1 build 1 boot <m>` at each connect, where `boot` is 2 over a GBC cable and 3 over a GBA cable; `cmd <code> fail <n> nr <k>` for each request that failed on the link, and `sync fail nr <k>` when a connect heard nothing from the helper.
- On any failure, report the step, what the Flipper and the GBA showed, and the log file.

## A. Connect (Ruby)

1. **Open Cartridge.** In Pokemon Trade Tool, Gen III → `Cartridge`. Pokemon Trade Tool closes, and after about 1 s Pokemon Cartridge opens with the instructions. While a trade runs, `Cartridge` isn't in the Gen III menu (`End trade` shows instead); end the trade first. If `pokemon_cart.fap` isn't next to `pokemon.fap`, an `App missing` dialog says where to copy it, and Pokemon Trade Tool stays open.
2. **Waiting.** With the GBA off and the Ruby cartridge in it, put the cable's small plug in the Flipper and the other in the GBA. Press `Start`. The Flipper shows `Waiting for GBA`. After 3 s its lines show `Hold START+SELECT` / `and power on the GBA`.
3. **Connect.** Hold START+SELECT on the GBA and switch it on; keep holding until the Nintendo logo is gone. The Flipper shows `Sending helper` with a bar (about 3–5 s), and the GBA screen turns green. Then the Flipper shows `Reading cart`, then `Backing up save` with `about 70 s` and a bar, then `Checking backup` and `Reading save`, and then the hub, headed `POKEMON RUBY`, with `Cart info`.
4. **Without the keys.** Back on the hub closes Cartridge. Switch the GBA off, open Cartridge again, press `Start`, and switch the GBA on without holding START+SELECT. The game starts, and the Flipper keeps waiting (`Waiting for GBA`, with the power-on lines). Switch the GBA off, then on again holding START+SELECT: it connects as in step 3.
5. **Wrong plug.** On the instructions, press `Start` with the cable's other plug in the Flipper (or no cable). The Flipper shows `Waiting for GBA`, then after 3 s the power-on lines (`Hold START+SELECT` / `and power on the GBA`); `Cable: plug in/swap ends` does not show. The log says `wire normal ungated` either way: a reversed cable and no cable look the same to the Flipper (SI reads high), so both go to the Game Boy Color link. Back, then plug the right way round (or the GBC cable) before the next Connect. Don't switch the GBA on in this step. A reversed GBA cable drives SC from the Flipper, so a GBA running a game in the Cable Club would fight it.
6. **Back while sending.** Start a Connect, and press Back while `Sending helper` shows. The Flipper shows `Stopping`, then the instructions. Back again closes Cartridge, and the Flipper returns to its app list. Switch the GBA off before the next Connect.
7. **Data missing.** On a computer, replace `apps_assets/pokemon_cart/gen3_bulk.bin` with an empty file, and open Cartridge: `Data missing` shows (`Gen III data missing.` / `Delete apps_assets/` / `pokemon_cart on the` / `SD card, then reopen.`), and OK closes the app. Then delete the folder `apps_assets/pokemon_cart` and open Cartridge again: the instructions show and a Connect works, because the Flipper unpacked the assets again.

## B. Cart info and backup

1. **Cart info.** Connect as in A.3, then choose `Cart info`. Its header is `AXVE v1` (or `v0`: your cart's version), and its lines are:
   - `Flash 09C2 Macronix` or `Flash 1362 Sanyo`;
   - `Save: A #<n>` or `Save: B #<n>`: the half of the save chip the game loads, and its save counter;
   - your trainer name and TID, for example `BRENDAN 12345`; compare the in-game trainer card;
   - `Writing on`;
   - `Backup <MMDD-HHMMSS>`: the backup's date and time.

   Back returns to the hub.
2. **The backup file.** In `apps_data/pokemon/gen3/backups/` there is `AXVE_<tid>_<date>-<time>.sav`, 131,072 bytes, whose date and time match Cart info's last line.
3. **Optional:** it loads in mGBA (as the save of a Ruby ROM) or in PKHeX and shows your party.

**The hub's items** from here on, each only when it applies: `Party and boxes`; `Write changes (<n>)` (n Pokemon changed and not yet written) or, when the cart can't be written, `Writing off: <why>`, which opens Cart info; `Discard changes` (changes, or a write or restore cut short and not yet finished); `Reconnect`; `Back up again`; `Restore backup` (a cart that can be written, with a current backup, even if its save is empty or damaged); `Copy ROM`; `Cart info`. `Write changes`, `Restore backup` and `Copy ROM` hide while `Reconnect` shows. A save another tool wrote last, or an empty or corrupt one, shows `Writing off: Save not recognised`. On another tool's save, which the game still loads, `Restore backup` is hidden too: save once in the game, then connect again.

## C. Browse (Ruby)

1. **Party.** Choose `Party and boxes`. The first row is `Party (<n>/6)`, n being your party's size. Choose it: one row per Pokemon, `01 <SPECIES> Lv<level>` and so on, with the levels the game shows, and `-- empty --` after the last one when the party has fewer than six.
2. **Boxes.** Back. The next 14 rows are your box names as the game shows them (`BOX 1`, ... or the names you gave them).
3. **A box.** Choose a box holding Pokemon. It lists its 30 slots, each row starting with the slot's number: `01 <SPECIES> Lv<level>`, `05 EGG` for an egg, and `07 -- empty --` for an empty slot. The header is the box name.
4. **Timing.** Time how long the first box screen takes to open after you choose it, and write it down below. It reads about 60 small pieces of the SD card.

## D. Edit and write (Ruby)

1. **Open.** Open party slot 1. The editor shows its species, level, nickname and OT as the game does.
2. **Edit.** Set the level to 50, then Back. The row shows `*` after it, and the hub shows `Write changes (1)` and `Discard changes`.
3. **Write.** Choose `Write changes (1)`. `Write changes?` asks `1 change. The save` / `is backed up. Keep` / `the GBA on.`; choose `Write`. The Flipper shows `Writing` with `Do not power off` (about 10 s), then `Verifying` and `Saving backup`, then `Written`: `Written and verified.` / `Power-cycle the GBA` / `to play.`
4. **Play.** Switch the GBA off and on without the keys, and choose Continue. There is no "save corrupt" message, and party slot 1 is Lv 50. Save in the game, switch off and on, and load again: still fine.
5. **Reconnect.** Back on the hub closes Cartridge (nothing is waiting to be written). Open Cartridge again, then connect as in A.3. The hub shows the save as it is now, and the connect made a new `.sav` in `backups/`.
6. **Link drop.** Edit any Pokemon, choose `Write changes`, then `Write`, and pull the cable while `Writing` shows. The Flipper shows `Write failed` with `Link lost. ...` and `Retry`. Until the write is finished, opening a Pokemon shows `Can't edit` / `Finish the write` / `first: Reconnect or` / `Discard changes.`, and the hub shows `Reconnect` and `Discard changes` but not `Write changes`. Plug the cable back in and press `Retry`: the Flipper reconnects and looks at the cart. If the write had got past its last step, the reconnect finishes it: `Written` / `Written and verified.` / `Power-cycle the GBA` / `to play.`. Otherwise it shows `Reconnected`, and opening a Pokemon now shows `Finish the write` / `first: Write changes` / `or Discard changes.`; choose `Write changes` again: `Written and verified.` Either way the game loads the edit, with no corrupt-save message. (In the log, a reconnect that finished the write ends its `job reconnect end Ok` line with `finished 1`.)
   **If the GBA lost power instead** (switch it off while `Writing` shows): `Retry` tries to reconnect and shows `Reconnect failed`; press `Retry` again for the instructions, and connect as in A.3. That connect finishes the write if it got past its last step (`Written`); otherwise it keeps the edit, and `Write changes` writes it again. Don't play and save in the game before you connect again: the connect would find the game's own save, back that up, and drop the edit (`Edits dropped`). If you switch the GBA on without the keys meanwhile, the game may say `The save file is corrupt. The previous save file will be loaded.` and load the old save. That is expected.
7. **Mail.** Give a party Pokemon a Mail in the game and save, then connect again. Open that Pokemon: `Can't edit` / `Take the Mail off` / `in the game first.` shows, OK returns to the list, and nothing is marked `*`.
8. **Unchanged.** Open any Pokemon and Back without changing anything: its row has no `*`, and the hub has no `Write changes`.

## E. Put Pokemon (Ruby)

**First,** since closing Cartridge or saving in the game drops the edits not yet written: in the game, leave fewer than six Pokemon in the party (put one in a PC box) and save. Close Cartridge (Back on the hub), and in Pokemon Trade Tool make a Pokemon in the Gen III editor and `Save .pk3`; it goes to `apps_data/pokemon/gen3/`. Then open Cartridge again and connect as in A.3.

1. **New.** In box 2's first empty slot, choose `New Pokemon`, set the species to TREECKO and the level to 10, then Back. `Put TREECKO Lv10` asks `in <box name>` / `slot <k>?`; choose `Put`. The slot shows `TREECKO Lv10*`.
2. **Load .pk3.** In another empty box slot choose `Load .pk3` and pick the file saved before E.1. If the load fixed something in it, `Loaded` says what first; OK. The editor shows it; Back, then `Put`.
3. **Party.** In the party's `-- empty --` row (the party has fewer than six), choose `New Pokemon`, then Back and `Put`. The party is one longer.
4. **Write.** Write the changes as in D.3. In the game the boxes show the new Pokemon, and the party has the new member. Those made with `New Pokemon` have your name and ID as OT and obey you in battle; the `.pk3` one keeps the OT it had.
5. **Discard.** Change a Pokemon, then choose `Discard changes`: `Discard changes?` / `1 change.`; choose `Discard`. `Discarded` shows, the `*` is gone, and the hub has no `Write changes`.
6. **Cancel and Back.** In an empty slot choose `New Pokemon`, then Back: on `Put ...?` press Back to return to the editor, Back again, then `Cancel`. The slot stays `-- empty --`.

## F. Restore (Ruby)

The backup to restore is the `.sav` that A.3's connect made, from before D and E: the oldest `AXVE_...` file in `backups/`.

Switch the GBA off. Back on the hub closes Cartridge (nothing is waiting to be written). Open Cartridge again, then connect as in A.3.

1. **Pick.** Choose `Restore backup`. The file list opens in `backups/`; pick that `.sav`. The Flipper shows `Checking file` with a bar, then `Restore backup?`: `Cart: <name> <TID>` / `File: <name> <TID>` / `Hall of Fame is kept.`, both trainers. Back on the file list, or `Cancel` on the question, returns to the hub.
2. **Restore.** Choose `Restore backup` and pick the file again, then `Restore` (about 40 s). The Flipper shows `Writing` with `Do not power off`, then `Verifying` and `Saving backup`; Back does nothing meanwhile. Then `Restored`: `Restored and` / `verified. Power-cycle` / `the GBA to play.`
3. **Play.** Switch the GBA off and on without the keys, and choose Continue. The game loads the save as it was at A: the party levels from before D, and none of E's Pokemon.
4. **A save that was corrupt.** Restoring onto a cart whose save was corrupt: until you save once in the game, it may say `The save file is corrupt…` at each start. Save in the game once.
5. **An edit not written.** Connect again, change a Pokemon, then choose `Restore backup` and pick a `.sav` of this game (an `AXVE_...` file; one of another game gives `Can't restore` / `Not a valid backup` / `for this game.`): the question has a fourth line, `1 change is lost.` Choose `Cancel`: the hub still shows `Write changes (1)`. Discard the change.

## G. Copy ROM (Ruby)

1. **Ask.** Choose `Copy ROM`: `Copy ROM?` / `16 MB, about` / `2 h 30 min.` / `Keep both powered.`, with `Cancel` and `Copy`.
2. **Copy, then stop.** Choose `Copy`. The Flipper shows `Copying ROM`, the percent under it, the bar, and after about 10 s `<h> h <m> min left`; every two or three minutes `Checking ROM` shows for a moment. After 1 min, press Back: `Stop copying?` / `You can resume` / `later.`, with `Keep going` and `Stop`. `Keep going` returns to the copy, which went on meanwhile. Press Back again and choose `Stop`: `Stopping`, then `ROM copy stopped`: `Copy ROM again` / `to resume.`
3. **Resume.** Choose `Copy ROM` again: `Resume copy?` / `<pct>% done`, with `Restart` and `Resume`. Choose `Restart`: it asks again, `Restart copy?` / `<pct>% done is lost:` / `the copy starts` / `again from 0%.`, with `Cancel` and `Restart`. Choose `Cancel`: the hub. Choose `Copy ROM` again, then `Resume`: the percent goes on from there, not from 0.
4. **Optional: finish** (about 2.5 h). `ROM copied`: `Verified.` / `roms/AXVE_v<n>.gba`. On the SD card, `apps_data/pokemon/gen3/roms/AXVE_v<n>.gba` is 16,777,216 bytes, and on a computer its `shasum` matches your revision's SHA-1 in `docs/gen3/multiboot-and-save.md` §3.6.

## H. FireRed

Repeat A–E with the FireRed cartridge in place of Ruby. A.4–A.7 may be skipped this time; D.6 (the link drop) may not. What differs:

1. **Connect (A.3).** The hub is headed `POKEMON FIRE`.
2. **Cart info and backup (B).** Cart info's header is `BPRE v0` or `BPRE v1`, and the backup is `BPRE_<tid>_<date>-<time>.sav`.
3. **Browse, edit and write (C, D).** The party, the box names and the levels are FireRed's. After D.3 the game loads the edit (D.4) with no corrupt-save message, and after D.6 too.
4. **Put (E).** In E.1, use BULBASAUR in place of TREECKO. After E.4, open the new BULBASAUR's summary in the game and write down what its trainer memo says.

LeafGreen, if you have it (optional): the same, with `POKEMON LEAF` and `BPGE`.

## N. Game Boy Color cable

Use a Game Boy Color link cable, either end in the Flipper. The module's SD line may be open (KBEmbedded: SD jumper not bridged); the cable can't carry SD anyway.

1. **Connect.** Connect as in A.3 with the GBC cable. The Flipper shows `Waiting for GBA`, `Sending helper`, `Reading cart`, `Backing up save` and the hub, as with the GBA cable. The log has `wire normal ungated` (or `wire multi` then `wire normal ungated (SI high)`, see Notes), `mb done`, `wire normal gated (helper)` and `hello … boot 2`; write down the `mb done` line's `final_us` and the `hello` line's `boot`. If it never gets past `Waiting for GBA` or `Sending helper`, or `Reading cart` never ends, see "If it fails".
2. **Run the same steps over it.** B.1, C.1, D.1–D.4 and D.6 (the link drop: pull the cable, plug it back, `Retry`). Over this cable the pull in D.6 shows `Write failed` within the time the GBA may take for a flash step, up to about 12 s, and the text may be `The GBA stopped` / `answering. ...` (or `No answer. ...`) rather than `Link lost. ...`. That is expected: the Flipper can't tell a pulled GBC cable from a GBA busy with the flash. `Retry` then reconnects and finishes the write as in D.6.
3. **Time it.** Time the `Backing up save` step, and write it down in the Results table (N.3). Over MultiPlay it takes about 70 s; the Normal wire has the same throughput, so expect about the same.

## R. Regression (Pokemon Trade Tool)

1. **Link Test.** In Pokemon Trade Tool, Gen III → `Link Test` with Ruby still reaches `in Trade Center`: `docs/gen3/hardware-test-1.md` steps 4–8. Cartridge added a second link mode; the trade's own is unchanged.
2. **Cartridge hides during a trade.** Gen III → `Trade`, then Back to the Gen III menu: it shows `End trade`, and neither `Link Test` nor `Cartridge`. Choose `End trade` (and `End` if it asks); once the trade has ended, the menu shows `Link Test` and `Cartridge` again: seven items, ending `Link Test`, `Cartridge`, as `docs/gen3/editor-test.md` A.1 counts them.
3. **Gen I and Gen II.** Back to the main menu: `Gen I` and `Gen II` each open their own editors as before (`docs/gen3/editor-test.md` A.13).
4. **Data missing.** Exit the app. On a computer, rename `apps_assets/pokemon/gen3_bulk.bin` to `gen3_bulk.bak`. Start Pokemon Trade Tool → Gen III → `Edit Pokemon`: `Data missing` shows, with `Gen III data file missing.` / `Delete apps_assets/pokemon` / `on the SD card, then reopen.`, and OK returns to the Gen III menu. Exit, delete the folder `apps_assets/pokemon`, and start the app again: `Edit Pokemon` opens the editor (`docs/gen3/editor-test.md` E.1).

## M. Memory

1. **Pokemon Trade Tool.** Start a trade and enter the Trade Center with Ruby (Gen III → `Trade`), then open `Edit Pokemon`. In the qFlipper CLI (or `ufbt cli`), run `free` and write down `Free heap size`. It must be at least 12,000, as in `docs/gen3/hardware-test-2.md` K.
2. **Pokemon Cartridge.** During a Connect, while `Backing up save` shows, run `free` again and write down `Free heap size`. If it is under 4,000, report it.
3. **No crash.** Neither app crashed or froze anywhere in A–H and N.

## Results

| Step | Pass / fail | What each screen showed (Flipper, GBA) |
|---|---|---|
| A.1 Cartridge opens | | |
| A.2 `Waiting for GBA`, power-on lines after 3 s | | |
| A.3 Connect to the hub (`Sending helper` took ___ s, `Backing up save` ___ s; the log's `mb done` line: `palette_busy` ___, `final_us` ___) | | |
| A.4 Without the keys, then with them | | |
| A.5 Wrong plug: `Waiting for GBA`, power-on lines, log `wire normal ungated` | | |
| A.6 Back while sending | | |
| A.7 Data missing, then unpacked again | | |
| B.1 Cart info (flash ____, save ____, trainer ____) | | |
| B.2 Backup file, 131,072 bytes | | |
| B.3 Backup in mGBA or PKHeX (optional) | | |
| C.1 Party with the right levels | | |
| C.2 Box names | | |
| C.3 A box's slots, `-- empty --` | | |
| C.4 First box screen opened in ___ s | | |
| D.1 Party slot 1 in the editor | | |
| D.2 Level 50: `*`, `Write changes (1)` | | |
| D.3 `Written and verified` (`Writing` took ___ s) | | |
| D.4 The game loads Lv 50, no corrupt-save message | | |
| D.5 Reconnect: new backup | | |
| D.6 Link drop, Retry, written | | |
| D.7 Mail: `Can't edit` | | |
| D.8 Unchanged: no `*` | | |
| E.1 New TREECKO Lv10 put | | |
| E.2 `.pk3` loaded and put | | |
| E.3 New party member put | | |
| E.4 Written: in the game, OT yours, obeys | | |
| E.5 Discard | | |
| E.6 Put: Back to the editor, Cancel | | |
| F.1 `Restore backup?` shows both trainers | | |
| F.2 `Restored and verified` (took ___ s) | | |
| F.3 The game loads the save from A | | |
| F.4 After a corrupt save: one save in the game | | |
| F.5 `1 change is lost.`; Cancel keeps it | | |
| G.1 `16 MB, about 2 h 30 min` | | |
| G.2 Time left shown; Keep going; Stop: `ROM copy stopped` | | |
| G.3 `Resume copy?` (___ % done); the percent goes on | | |
| G.4 ROM copied, size and SHA-1 match (optional) | | |
| H.1 FireRed: hub `POKEMON FIRE` | | |
| H.2 Cart info `BPRE v__`, backup `BPRE_...` | | |
| H.3 FireRed: browse, edit, write; the game loads it (D.6 too) | | |
| H.4 FireRed: put and write; trainer memo: ____ | | |
| H LeafGreen (optional) | | |
| N.1 GBC cable: Connect to the hub (log: `wire` line ____, `mb done` `final_us` ___, `hello` `boot` ___) | | |
| N.2 GBC cable: B.1, C.1, D.1–D.4, D.6 (D.6's message: ____) | | |
| N.3 GBC cable: `Backing up save` took ___ s | | |
| R.1 Link Test reaches `in Trade Center` | | |
| R.2 `Cartridge` hidden during a trade, back after it | | |
| R.3 Gen I and Gen II editors | | |
| R.4 Pokemon Trade Tool's `Data missing` | | |
| M.1 Pokemon Trade Tool, free heap in a trade: ______ bytes | | |
| M.2 Pokemon Cartridge, free heap while backing up: ______ bytes | | |
| M.3 No crash in A–H and N | | |

## On failure

Report the step (for example `D.6`), what the Flipper and the GBA showed, and `apps_data/pokemon/gen3_cart.log`, copied off the SD card before you opened Cartridge again. For R.1, report `apps_data/pokemon/gen3_link.log` instead, as `docs/gen3/hardware-test-1.md` says.

If a write or a restore failed, report it before you play the game. The backups in `backups/` hold the save from before.

## If it fails (for whoever reads the log)

The log's `cart` line is the cart as the helper found it; its `why` is the reason writing is off (1 header, 2 not a Pokemon game, 3 Japanese, 4 SRAM, 5 save chip, 6 save library, 7 region, 8 Emerald, 9 the save). The `mb` lines are the helper upload's; `last_fail` is 2 for a bad header, 8 for a bad CRC, and 9 when its 20 s ran out; `nr` counts link slots the GBA never got ready for, and `last` is the last word it sent. A `cmd <code> fail <n> nr <k>` line is a request that failed on the link: `n` is 1 no READY, 2 no response, 3 BUSY too long, 4 bad CRC, 5 a broken word (or a pulled GBC cable), 6 a stale response, 7 too long; `k` counts the slots the helper wasn't ready for. `docs/gen3/multiboot-and-save.md` §7.5 has the log lines, and the rest of it the background. The tunables named below are `#define`s in "Tunables for the hardware test" at the top of `src/gen3/gen3_cart_furi.c`: whoever reads the log changes one, rebuilds with `ufbt`, and sends a new `pokemon_cart.fap` for the next run.

| Symptom | Likely cause | Try |
|---|---|---|
| Never leaves `Waiting for GBA` | START+SELECT not held while the GBA powered on, or (GBA cable) the plugs: the small one goes in the Flipper | switch the GBA off, then on holding START+SELECT until the logo is gone; check the cable |
| `Waiting for GBA` and `wire multi` in the log with a GBA cable, then `T:B9A0 R:FFFF` | the module's SD line is open (MultiPlay never gets a reply) | bridge the SD jumper, or use a Game Boy Color cable |
| (GBC cable) `wire normal …` in the log, and Connect never leaves `Waiting for GBA` with the GBA held at START+SELECT | the BIOS may want the Flipper to wait for its ready before each word (multiboot-and-save.md M27), or the cable | look at the `mb wait … nr … last` lines (`nr 0 last ffff`: nothing answered); rebuild with `GEN3_CART_MB_GATED 1`, and, if the log began with `wire multi` and switched only later, with `GEN3_CART_FORCE_NORMAL 1` too |
| (GBC cable) only `wire multi` in the log, never `wire normal`, and `Waiting for GBA` | SI reads low on this module and cable, so Connect stays on the GBA cable's link | rebuild with `GEN3_CART_FORCE_NORMAL 1` |
| (GBC cable) the GBA's screen shows the helper's colour, but Connect never gets past `Reading cart` / HELLO | M24: the GBA may show the shift register's MSB on SO while a transfer is armed, instead of the low level (the helper arms with `0x5080`); or M25: the BIOS didn't write boot mode 2, so the helper runs its GBA-cable loop | report the log: `sync fail nr …` or `cmd … fail … nr …` lines with a high `nr` (every slot not ready) point at M24 or M25; a `hello … boot` line, if one came, says which loop the helper chose |
| (GBC cable) `mb failed … want … got …` | M23 (the data reply's offsets) or the Normal constants (multiboot-and-save.md §7.2) | report the log's `want`/`got` and the step before it |
| A reversed GBA cable, or a GBA running a game in the Cable Club, upsets the link | the Normal wire drives SC from the Flipper; a GBA acting as MultiPlay parent would fight it | use Cartridge only with the GBA held at START+SELECT at power-on, as the instructions say |
| The log shows `mb restart` lines with `last_fail 2` (header) or `8` (CRC) | the GBA's boot ROM and the Flipper's timing, or its state after a bad header (multiboot-and-save.md M3, M4) | report the log |
| `Connect failed`: `Power the GBA off,` / `hold START+SELECT,` / `power on, then Retry.` | the upload failed 4 times, or ran out of its 20 s (multiboot-and-save.md M4) | do as it says |
| `Link lost` often during long jobs (backup, ROM copy) | stream mode's load on the Flipper (multiboot-and-save.md M18) | raise `GEN3_CART_GAP_TICKS` in `src/gen3/gen3_cart_furi.c` (1, then 2), and rebuild |
| `Write failed` with `The GBA stopped` / `answering. Cart keeps` / `its last good save.`, or `Flash write failed.` / `The cart still loads` / `its last good save.` | the chip's erase or program time (multiboot-and-save.md M10) | report the log's `cmd` lines |
| `Writing off: ROM patched save type` on a genuine cart | a false hit in the helper's scan of the ROM for save-library names (multiboot-and-save.md §3.3) | report the log's `cart` line (`why 6`) and the game's revision |
| `Writing off: Unknown save chip` on a genuine cart | a chip not in the table (multiboot-and-save.md §3.5) | report the log's `cart` line, whose `flash` is the chip's ID |
| ROM copy ends `Check failed`, or `Bad cartridge header` on a genuine cart | ROM reads unstable at the helper's wait states (WAITCNT) | set `HP_WAITCNT` to `0x0003` in `gba/helper/gba/main.c`, run `make -C gba/helper install`, rebuild with `ufbt`, and install `pokemon_cart.fap` again |
| `Helper file missing.` | `apps_assets/pokemon_cart/gen3_helper.mb` is missing or damaged | install `pokemon_cart.fap` again, or delete `apps_assets/pokemon_cart` and reopen Cartridge |
