# Gen III hardware test 2: trading with Ruby

Goal: the Flipper trades Pokemon with a genuine Pokemon Ruby in the Cable Club Trade Center, in both directions. The link keeps running while you use the Flipper's Gen III screens, an edit is never lost to a trade, and the GBA never shows a link error in normal use.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA, GBA link cable, genuine Ruby.

The Game Link module's SD line must be connected (KBEmbedded module: the two SD jumper pads bridged). Without it Ruby never answers, and the log shows `R:FFFF` with a timeout on every frame.

**Build and install.** Run `ufbt` (release build). Copy `dist/pokemon.fap` to `apps/GPIO/` on the SD card with qFlipper, or run `ufbt launch`.

## Notes

- **The log** is `apps_data/pokemon/gen3_link.log`. It is overwritten each time a Trade or Link Test link starts, so after a failed run copy it off the SD card before you start another one. It grows by about 8 KB/s while you are in the Trade Center. Each line carries the link phase as `p0` to `p10`: `p0` waiting for the GBA, `p1`-`p3` linking, `p4` Trade Center room, `p5` closing, `p6` error, `p7` trade menu loading, `p8` trade menu, `p9` trade animation, `p10` the GBA's link save.
- **The trade screen** shows `Gen III Trade` on top, three lines that say what is happening and what to do on the GBA, then `Flipper: <NICKNAME>`, the Pokemon the Flipper offers. `E<n>` at the top right counts checksum errors on the link; it should never appear. `Edit` at the bottom (the OK button) opens the editor; it is hidden while a trade is under way.
- **The link runs behind every Gen III screen** once `Trade` has started it: the editor, its keyboards and lists, the species picker, `.pk3` load and save. `Trade` in the Gen III menu returns to the trade screen. While the link runs, the Gen III menu has `End trade` in place of `Link Test`.
- **Between the GBA's links** (sitting down, the trade, after the save, back to the room) the trade screen shows `Please wait...` as a link closes (also just after the save), then `Linking...`, each for a moment; during a trade it shows `Trading...` instead. While you stay in the Trade Center it never shows `Go to the Cable Club`.
- The trade screen's texts are shown on three lines; this checklist writes them on one, except where it quotes the lines.
- On any failure, report the step, what each screen showed (Flipper and GBA, including any `E<n>`), and the log file.

## A. Setup and Link Test regression

1. **Ruby.** Pokedex received; at least 3 Pokemon in the party, one you're happy to trade away; none holding Mail or the ENIGMA BERRY. Save the game.
2. **Cable.** Plug the cable as in test 1 (`docs/gen3/hardware-test-1.md`, step 4), with the same plug in the Flipper.
3. **Link Test.** Gen III → `Link Test`: run test 1 steps 5–8 once. The Flipper now sends its LinkPlayerBlock after the GBA's (D2); the Link Test must still reach `in Trade Center` and leave cleanly.
4. **ENIGMA BERRY fixture.** On a computer, copy `docs/gen3/fixtures/enigma-berry.pk3` from this repository to `apps_data/pokemon/gen3/` on the SD card with qFlipper. It is a TREECKO holding the ENIGMA BERRY, which Ruby can't take in a trade; section H uses it to check the refusal. Don't load it before then.

## B. Start a trade

1. **Start.** Start the app fresh and don't edit the Pokemon first. Gen III → `Trade`: the screen shows `Plug in the cable (or swap its ends).` until the cable reads OK, then `Go to the Cable Club: PC 2F, Trade Center: RS middle, FRLG right`. Ruby's Trade Center is the middle counter.
2. **Enter.** On Ruby, go to the Trade Center as in test 1. The Flipper shows `Linking...`, then `In the Trade Center: sit at the table on the GBA.`
3. **OT.** Back to the Gen III menu (the link keeps running) → `Edit Pokemon`: `OT Name` is your Ruby name, and `OT TID`/`OT SID` are yours. The default Bulbasaur took your OT (D7). Back to the Gen III menu, then `Trade` to return to the trade screen.

## C. First trade (GBA → Flipper)

1. **Sit.** Walk onto either seat: the Flipper shows `Opening the trade menu...` (with `Linking...` for a moment, never `Go to the Cable Club`). Ruby shows the trade menu with BULBASAUR alone on the right.
2. **Trade.** Choose one of yours → TRADE → YES. The Flipper shows `Confirm the trade on the GBA.`, then `Trading... Watch the GBA.`, then during "SAVING..." `The GBA is saving. Don't unplug or turn off. Back is locked.` Press Back on the Flipper during the save: nothing happens.
3. **After the save** Ruby shows the trade menu again, with your Pokemon on the Flipper side. The Flipper shows `Flipper: <its nickname>` and `Edit it (OK), or pick the next trade on the GBA.` (The Flipper recalculates a received Pokemon's stats. If it did so only after the menu reopened, Ruby goes back to the room once instead and the Flipper asks you to sit again, as in D.3. Note it if that happens.)
4. **Ruby:** BULBASAUR is in your party, with you as OT.

## D. Edit and trade back

1. **Edit.** On the trade screen press OK (`Edit`): the editor opens.
2. **The GBA goes back.** As soon as the editor opens, the GBA leaves the trade menu for the room, with your avatar still on its seat. Change the species to TREECKO, then Back to the trade screen. (If you are back before the GBA reaches the room, the screen first shows `Updating the trade menu: the GBA goes back to the room.`)
3. **Sit again.** The Flipper shows:

   ```
   Sit at the table
   again (step off and
   back on if seated).
   ```

   Hold Up for one step, then Down for one step. If Up is blocked, try Down then Up, and note it. The trade menu opens with TREECKO on the Flipper side.
4. **Trade back.** Trade any of yours for TREECKO. Ruby: TREECKO is in your party with you as OT; it obeys in battle and gets no "boosted" Exp.
5. **Again.** Repeat C and D once without leaving the room.
6. **Edit while the GBA asks.** With the GBA in the trade menu:
   1. On the GBA, choose one of yours → TRADE. The Flipper answers within about 0.2 s, and Ruby asks "Is this trade okay?". Ruby waits at that prompt for as long as you take.
   2. While the prompt is up, press OK (`Edit`) on the Flipper: the editor opens. Ruby stays at its prompt.
   3. With the Flipper's editor still open, answer YES on the GBA.

   Expected: no trade starts, and Ruby goes back to the room. On the Flipper, Back to the trade screen, then sit again: the menu opens and trading works. (If you close the editor without a change before answering, YES trades exactly the Pokemon Ruby showed, which is safe. To check this step, keep the editor open until you have answered.)

## E. Background link while you work

With the GBA in the Trade Center (opening the editor sends it from the trade menu back to the room; leave it there), for about 3 minutes on the Flipper:

1. **Typing.** Edit Pokemon → Nickname: type 10 letters slowly, then save.
2. **Sprites.** Open the species picker (the `Pokemon` row) and scroll through 60 species with sprites. Back without picking.
   - **2a. Move and item lists.** Edit Pokemon → Select Moves → Move 1. The letter list shows within about 1 s; pick `S`, and its moves show within about 1 s. Do the same for Held Item (its letter list, then `S`). Move and item names are read from the SD card. Ruby shows no link error. Write down roughly how long each of the four screens took to show:

     | Screen | Time |
     |---|---|
     | Move 1 letter list | |
     | Moves under `S` | |
     | Held Item letter list | |
     | Items under `S` | |
3. **Files.** Back to the Gen III menu → `Save .pk3` → `Load .pk3` (browse to the file and load it) → `Trade`.
4. **Expected:** Ruby never shows a link error, the trade screen's title shows no `E<n>`, and trading still works afterwards (sit again, and trade once).
5. **If Ruby shows a link error** here, write down what you were doing. For the rest of the test use the fallback: walk out of the Trade Center before editing, and back in afterwards. The Flipper waits at `Go to the Cable Club` meanwhile.

## F. Cancels and refusals

1. **No.** Choose one of yours → TRADE → NO. Ruby: "The trade has been canceled."; A returns to the trade menu.
2. **Cancel.** CANCEL → "Cancel trade?" YES. Ruby goes back to the room; the Flipper shows `Sit at the table again (step off and back on if seated).`
3. **Mail.** Before entering the Cable Club, give one party Pokemon Mail on the GBA. In the trade menu, offer it. The Flipper shows `Can't take that one: it holds Mail. Pick another one.`; Ruby shows "The trade has been canceled."
4. **Egg.** If you have an Egg, the same gives `Can't take that one: it's an Egg. Pick another one.`

## G. Summary screen and the save lock

1. **Summary.** In the trade menu, open SUMMARY of any Pokemon on the GBA. On the Flipper, press OK (`Edit`), change the level, and Back to the trade screen. Close the summary on the GBA: within about 3 s Ruby leaves the menu for the room. Sit again; the menu opens with the new level.
2. **Back during the save.** Pick a trade on the GBA, and before answering YES go Back to the Gen III menu on the Flipper. Answer YES. During "SAVING...", press Back on the Gen III menu: `The GBA is saving` / `Wait until it's done, then end the trade.` with only `OK`, and the link keeps running. `OK` returns to the menu; after the save the trade finished normally on both sides.
3. **End during a trade** (R6). Pick a trade and answer YES on the GBA. While `Trading...` shows, go Back to the Gen III menu → `End trade` → `End`: the trade screen shows `Ending: the GBA walks out of the Trade Center by itself.` Press Back on the trade screen during the animation (it goes to the Gen III menu; `Trade` returns) and during the save (nothing happens): `Stop link?` never appears, also not in the moment before the animation while the GBA relinks for it. `End trade` in that moment, or in the one after the save while the GBA relinks for the trade menu, shows `End trade?` (or `The GBA is saving`), never ends the link at once. After the save Ruby returns to the room and walks out by itself, and the trade went through on both sides. If you were on the Gen III menu when Ruby walked out, `End trade` went from the menu by itself and `Link Test` came back.
4. **A dialog left open while the trade ends.** Do G.3 again up to `End`, then go Back to the Gen III menu during the animation and choose `End trade` again: `End trade?` shows. Leave it open. Press `End` once during "SAVING...": the dialog becomes `The GBA is saving` (no End while it saves). `OK` returns to the menu. Once Ruby is back in the trade menu or the room after the save, choose `End trade` again and leave `End trade?` open. Wait until Ruby has saved, gone back to the room and walked out, then press `End`. Expected: the Gen III menu, with `Link Test` and no `End trade`; no crash; Edit Pokemon shows the Pokemon you just received.

## H. End trade

1. **From the room.** Start a trade session again (Gen III → `Trade`) and enter the Trade Center. Gen III menu → `End trade` → `End`. Ruby's player walks out by itself; the Flipper shows `Link ended. You can unplug the GBA.` Back to the Gen III menu: `End trade` has gone and `Link Test` is back.
2. **From the trade menu.** H.1 ended the trade session, so start a new one (Gen III → `Trade`), enter the Trade Center on Ruby again and sit, so the trade menu opens. Then Gen III menu → `End trade` → `End`: Ruby goes back to the room first, then walks out, and the Flipper shows `Link ended. You can unplug the GBA.`
3. **A Pokemon Ruby can't take** (D8, R7). Copy `docs/gen3/fixtures/enigma-berry.pk3` to `apps_data/pokemon/gen3/` on the SD card if you skipped A.4. Start a trade session and enter the Trade Center. Gen III → `Load .pk3` → `enigma-berry` → `Trade`. The screen shows `Can't offer it: it holds a bad item. Edit it first.` Sit on the GBA: Ruby's "Please wait..." stays, and the Flipper shows `Can't offer it: it holds a bad item. Edit it; stay seated.`
4. **Late sit** (R5). Stay seated. On the trade screen press OK (`Edit`) → `Held Item` → `No Item`, then Back to the trade screen. Within a few seconds the GBA opens the trade menu with TREECKO, and you never stood up.
5. **End while seated.** CANCEL → YES on the GBA; Ruby goes back to the room. Load `enigma-berry` again (Gen III → `Load .pk3`), go to the trade screen, and sit on the GBA ("Please wait..." stays). Then Gen III menu → `End trade` → `End`. Ruby cancels the seat and walks out, and the Flipper shows `Link ended. You can unplug the GBA.`
6. **Back asks first.** Start a trade session and enter the Trade Center. Back on the Gen III menu shows `End trade?`; `Stay` (or Back) keeps the link, and the trade screen still works. Walk out of the Cable Club on the GBA yourself: once the trade screen shows `Go to the Cable Club`, Back on the Gen III menu goes straight to the main menu.

## I. National Dex (call #8)

1. **Default.** After the trades above, Ruby's Pokedex offers no NATIONAL mode.
2. **Optional, permanent on your save.** Gen III menu → `National Dex: Off` → `Unlock National Dex?` → `Turn on`; the item now reads `National Dex: On`. Trade once; Ruby's Pokedex now offers NATIONAL. Skip this if you don't want it. Choosing `National Dex: On` turns it off again at once, with no dialog.

## J. OT row (D7)

1. **Use your OT.** With a trade session running and the GBA in the Trade Center, Edit Pokemon → `OT TID` → any number but yours (say `12345`). After `OT SID` a row `OT: use <your Ruby name>` appears. Choose it: `OT Name`, `OT TID` and `OT SID` are yours again, `Shiny` is unchanged, and the row goes. If the Pokemon can't stay shiny under your OT ID, `Shiny lost` / `No shiny PID for this OT ID.` shows first; `OK` returns to the editor with your OT.
2. **A loaded Pokemon.** Gen III → `Load .pk3` → `enigma-berry` (its OT is `FLIPPER`, TID `22222`). Edit Pokemon shows `OT: use <your Ruby name>`; choose it, and the OT rows are yours. Like any edit, this recalculates the stats, so a loaded Pokemon that was hurt comes back with full HP. That is by design.
3. **Remembered after End trade.** Gen III menu → `End trade` → `End`, and wait for `Link ended`. Gen III → `Load .pk3` → `enigma-berry` again: its OT is `FLIPPER`, TID `22222`; don't use the `OT: use` row this time. Edit Pokemon → `Pokemon` → pick another species: `OT Name`, `OT TID` and `OT SID` become yours. The Flipper remembers the last GBA player until the app exits.

## K. Memory

1. **Free heap.** Start a trade session and enter the Trade Center (Gen III → `Trade`), then open `Edit Pokemon`. In the qFlipper CLI (or `ufbt cli`), run `free` and write down `Free heap size`. It must be at least 12,000.

## Results

| Section | Pass / fail | What each screen showed (Flipper, Ruby) |
|---|---|---|
| A Setup and Link Test regression | | |
| B Start a trade | | |
| C First trade (GBA → Flipper) | | |
| D Edit and trade back | | |
| E Background link (with the E.2a times) | | |
| F Cancels and refusals | | |
| G Summary screen and the save lock | | |
| H End trade | | |
| I National Dex | | |
| J OT row | | |
| K Memory: free heap (bytes) | | |

## On failure

Report the step (for example `D.3`), what each screen showed (the trade screen's three lines and any `E<n>` on the Flipper, Ruby's message on the GBA), and the log `apps_data/pokemon/gen3_link.log`. Copy the log off the SD card before you start another Trade or Link Test, because starting one overwrites it.

If section E fails (Ruby shows a link error while you edit), use the fallback for the rest of the test: walk out of the Trade Center before editing, and back in afterwards. Report the log; the follow-up keeps the link protocol as it is.

To finish a run, use `End trade` → `End`. If you had to use `Stop link?` and Ruby shows a link error, restart the GBA; it saved before linking.

## If it fails (for whoever reads the log)

Each log line carries the link phase as `p0` to `p10` (see Notes). The trade tunables are `#define`s at the top of `src/gen3/gen3_club.h`. The link-level hints in `docs/gen3/hardware-test-1.md` still apply, and `docs/gen3/link-protocol-trade.md` (Part B) has the background.

| Symptom | Where to look |
|---|---|
| Black screen on the GBA after sitting down | the party exchange (`p7`): check the `CCCC` sequence and `GEN3_CLUB_ROUND1_DELAY_BURSTS` (§B2.2) |
| "Link standby..." on the GBA forever | a reply the Flipper didn't send in the trade menu (`p8`, §B3.4) |
| A link error when the GBA goes back to the room | the 0x1111 identity check (§B6.3): the Flipper's LinkPlayerBlock must match the one from the first linkup |
| `E<n>` on the trade screen, or checksum errors in the log | the wire timing, as in test 1 |
