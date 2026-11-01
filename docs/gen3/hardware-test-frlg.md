# Gen III hardware test: FireRed

Goal: the Flipper trades Pokemon with a genuine Pokemon FireRed in the Cable Club Trade Center, in both directions, and the Link Test, the editor and `.pk3` files work with FireRed too. LeafGreen works the same way; if you test LeafGreen instead, note it in the results.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA, GBA link cable, genuine FireRed, and genuine Ruby for section H.

The Game Link module's SD line must be connected (KBEmbedded module: the two SD jumper pads bridged). Without it Ruby never answers, and the log shows `R:FFFF` with a timeout on every frame.

**Build and install.** Run `ufbt` (release build). Copy `dist/pokemon.fap` to `apps/GPIO/` on the SD card with qFlipper, or run `ufbt launch`.

## Notes

- **The log** is `apps_data/pokemon/gen3_link.log`. It is overwritten each time a Trade or Link Test link starts, so after a failed run copy it off the SD card before you start another one.
- **FireRed's Trade Center** is on Pokemon Center 2F: talk to the right-hand attendant, then choose TRADE CENTER. FireRed asks you to save first.
- **The National Dex.** First write down whether your FireRed has the National Dex: its Pokedex offers a NATIONAL list. Without it, FireRed itself trades away only #1–151, and the Flipper never offers it one from #152 up. Section C1 needs a FireRed without the National Dex, C2 one with it; do the one that matches your save.
- **The trade screen**, its three guide lines and `Edit` (the OK button) are as in `docs/gen3/hardware-test-2.md` (Notes). This checklist writes the three lines on one line, except where it quotes them.
- On any failure, report the step, what each screen showed (Flipper and GBA, including any `E<n>`), and the log file.

## A. Link Test with FireRed

1. **FireRed.** Pokedex received (FireRed's Cable Club needs it); at least 2 Pokemon in the party, one you're happy to trade away; none holding the ENIGMA BERRY. Save the game.
2. **Cable.** Plug the cable as in test 1 (`docs/gen3/hardware-test-1.md`, step 4), with the same plug in the Flipper.
3. **Link Test.** On the Flipper, Gen III → `Link Test`. Then on FireRed enter the Trade Center.
4. **Linked.** The Flipper's `Link:` line reaches `in Trade Center`, and the partner line shows your FireRed name and TID. On FireRed the second avatar is a FireRed/LeafGreen player (Red), not Brendan or May.
5. **Trainer card.** Talk to the other avatar: a Kanto-style card with `FLIPPER` and ID 22222. Press A to flip it, then flip it back: no garbage, no crash.
6. **Leave.** Walk around for 60 s (`Err` stays 0), then walk out: FireRed shows no error, and the Flipper shows `waiting for GBA`.

## B. Start a trade

1. **Start.** Start the app fresh. Gen III → `Trade`, then enter the Trade Center on FireRed: the Flipper shows `Linking...`, then `In the Trade Center: sit at the table on the GBA.`
2. **OT.** Back to the Gen III menu (the link keeps running) → `Edit Pokemon`: `OT Name` is your FireRed name, and `OT TID`/`OT SID` are yours. Back to the Gen III menu, then `Trade` to return to the trade screen.

## C. National Dex rules

**C1. FireRed without the National Dex.** Skip this if your FireRed has it.

1. **The reason.** With FireRed in the Trade Center, set the species to TREECKO (Gen III menu → `Edit Pokemon` → `Pokemon`), then go back to the trade screen. It shows:

   ```
   Can't offer it: GBA
   has no National Dex.
   Get it, or pick 1-151
   ```

   The reason shows only once FireRed is in the Trade Center (or on its way in): the Flipper learns FireRed's rules when it links, and with no GBA linked the screen shows `Go to the Cable Club` instead.
2. **No sit.** Sit at the table on FireRed: its "Please wait. … … B Button: Cancel" stays, and the reason stays on the Flipper. Press B to stand up.
3. **A Kanto Pokemon.** Set CHARMANDER, go back to the trade screen, and sit: the trade menu opens with CHARMANDER on the Flipper side.
4. **FireRed's own rule.** If FireRed holds a Pokemon from #152 up, choose it → TRADE: FireRed itself refuses ("That POKéMON can't be traded now.").
5. **An edit in the menu.** With the trade menu open, press OK (`Edit`) on the Flipper: FireRed goes back to the room as soon as the editor opens. Set TREECKO, then Back to the trade screen: `Can't offer it: GBA` / `has no National Dex.` / `Get it, or pick 1-151`. Sit again: "Please wait. … … B Button: Cancel" stays, and FireRed never shows TREECKO. Press B to stand up.

**C2. FireRed with the National Dex.** Skip this if your FireRed hasn't got it.

1. Set TREECKO and sit: the trade menu opens with TREECKO on the Flipper side. Trade it in D.

## D. Trade both ways

1. **Trade and trade back.** Run test 2's sections C and D (`docs/gen3/hardware-test-2.md`) with FireRed: trade one of yours to the Flipper, edit it, sit again (Up, then Down), and trade it back. On FireRed it has you as OT and obeys in battle. (Without the National Dex, give it a species from #1–151.)
2. **Summary.** In the trade menu, open SUMMARY of the Flipper's Pokemon: every page shows, with no crash. Write down the "met" text.
3. **YES from the Gen III menu.** Pick a trade on FireRed. Before answering, go Back to the Gen III menu on the Flipper (the link keeps running). Answer YES: the trade starts and finishes with no link error.

## E. Mew and Deoxys

1. **Species.** Set the species to MEW: the editor shows `Fateful: Yes`, right after `Ball`.
2. **Not fateful.** Highlight `Fateful` and press OK: it reads `Fateful: No`, and the highlight stays on it. Back to the trade screen, with FireRed in the Trade Center:

   ```
   Can't offer it: FRLG
   needs Fateful: Yes on
   Mew/Deoxys: edit it.
   ```

   Sitting on FireRed does nothing ("Please wait. … … B Button: Cancel" stays). Press B to stand up.
3. **Fateful again.** Set `Fateful: Yes` the same way, go back to the trade screen, and sit: the trade menu opens with MEW on the Flipper side. Trading it is optional.

## F. Refusals

1. **No and Cancel.** TRADE → NO, then CANCEL → YES, as test 2 F.1–F.2.
2. **Egg.** If FireRed has an Egg, offer it. The Flipper shows `Can't take that one: it's an Egg. Pick another one.`, and FireRed says the trade was canceled. (Without the National Dex, FireRed refuses it itself first.)

## G. End and options

1. **End trade.** With FireRed in the room, Gen III menu → `End trade` → `End`: FireRed walks out by itself, and the Flipper shows `Link ended. You can unplug the GBA.` That ends the trade session, so for the second case start a new one (Gen III → `Trade`), enter the Trade Center on FireRed again and sit, so the trade menu opens. Then Gen III menu → `End trade` → `End` again: FireRed goes back to the room first, then walks out, and the Flipper shows `Link ended. You can unplug the GBA.`
2. **National Dex option.** Gen III menu → `National Dex: Off`: the dialog `Unlock National Dex?` says `Ruby/Sapphire only.` / `The next trade unlocks` / `it for good.` Choose `Cancel`. (If the item reads `National Dex: On`, choose it once to turn it off, then choose it again.)

## H. Swap cartridges

1. **Walk out.** Start a trade session (Gen III → `Trade`) and enter the Trade Center on FireRed. Don't end the trade: walk out on FireRed. Once the Flipper shows `Go to the Cable Club`, turn the GBA off.
2. **Ruby.** Put Ruby in and enter its Trade Center (on Ruby it is the middle counter of the Pokemon Center 2F): the Flipper's avatar is Brendan, and its trainer card is the Hoenn style. Trade once. (For a moment while Ruby links, the trade screen may still show FireRed's `Can't offer it` reason; that is expected. If your FireRed has no National Dex and the Flipper holds a Pokemon from #152 up, Ruby's trade menu shows it.)
3. **FireRed again.** Walk out, turn the GBA off, put FireRed back in, enter the Trade Center and trade once more. No link errors on either game. (Without the National Dex, if the Flipper now holds a Pokemon from #152 up, such as the Hoenn Pokemon Ruby gave it in H.2, set one from #1–151 first: otherwise the Flipper rightly doesn't sit, and the trade screen says `Can't offer it: GBA has no National Dex. Get it, or pick 1-151`.)

## I. `.pk3` with FireRed

1. **Save and load.** Gen III → `Save .pk3` the Pokemon the Flipper received from FireRed in H.3, then `Load .pk3` it: the species, level and OT match.
2. **PKHeX** (optional). PKHeX opens the file with no checksum warning.

## J. Memory

1. **Free heap.** Start a trade session (Gen III → `Trade`) and enter the Trade Center on FireRed, then open `Edit Pokemon`. In the qFlipper CLI (or `ufbt cli`), run `free` and write down `Free heap size`. It must be at least 12,000.

## Results

Game: FireRed or LeafGreen (which one?), with or without the National Dex.

| Section | Pass / fail | What each screen showed (Flipper, FireRed) |
|---|---|---|
| A Link Test with FireRed | | |
| B Start a trade | | |
| C National Dex rules (C1 or C2) | | |
| D Trade both ways (with D.2's "met" text) | | |
| E Mew and Deoxys | | |
| F Refusals | | |
| G End and options | | |
| H Swap cartridges | | |
| I `.pk3` with FireRed | | |
| J Memory: free heap (bytes) | | |

## On failure

Report the step (for example `C1.2`), what each screen showed (the trade screen's three lines and any `E<n>` on the Flipper, FireRed's message on the GBA), and the log `apps_data/pokemon/gen3_link.log`. Copy the log off the SD card before you start another Trade or Link Test, because starting one overwrites it.

To finish a run, use `End trade` → `End`. If you had to use `Stop link?` and FireRed shows a link error, restart the GBA; it saved before linking.

## If it fails (for whoever reads the log)

The hints in `docs/gen3/hardware-test-2.md` and `docs/gen3/hardware-test-1.md` still apply. `docs/gen3/link-protocol-trade.md` Part C has the FireRed/LeafGreen background.

| Symptom | Where to look |
|---|---|
| FireRed closes the link at the counter | the `version` the Flipper claims in its first LinkPlayerBlock: 0x4004 for FireRed, 0x4005 for LeafGreen (§C1, plan 4 D1). With a Ruby `version`, FireRed refuses the linkup unless its save can link with Ruby/Sapphire |
| A link error on FireRed right after YES | the 0x1144 relink took over 300 frames, FireRed's deadline (`fr: trade_scene.c: CB2_LinkTrade` states 3/4 → `CheckLinkTimeout`, about 363 frames after FireRed reopens the link). In the host tests the relink takes about 24 frames, so look in the log for what held the Flipper's 0x1144 LinkPlayerBlock back. **What you see:** if FireRed reopened the link, the Flipper keeps `Trading... Watch the GBA.` until FireRed closes it (about 363 frames after FireRed reopens the link), then shows `Link lost before the trade. On a GBA link error, restart it.` or `Link lost. Nothing was traded.` at once. If FireRed never reconnected, that change happens after the Flipper's own 300-frame relink wait (about 5 s). The second text shows if the Flipper already had FireRed's 0x1144 LinkPlayerBlock when the link closed. Nothing was traded: restart FireRed (it saved before linking) and enter the Trade Center again; the Flipper drops the pending trade at that linkup |
| A garbled trainer card | card byte 0x38 (the version, 4 or 5) or `stars` (at most 4): `link-protocol-trade.md` §C2, and its Uncertain item T13 (card fields not traced) |
