# Gen III editor test

Goal: the Gen III Pokemon editor on the Flipper shows and changes a Pokemon correctly, and Gen I and Gen II still work. No GBA is needed for this test.

**Build and install.** Run `ufbt` (release build). Copy `dist/pokemon.fap` to `apps/GPIO/` on the SD card with qFlipper, or run `ufbt launch`.

## Notes

- The editor's Pokemon is created the first time you choose `Edit Pokemon`, `Load .pk3` or `Save .pk3` (once the Gen III data file reads) and is kept until the app exits. Leaving the editor with Back does not reset it.
- Nature, Shiny, Gender, Ability and Unown Form are all stored in the Pokemon's PID. Changing one of them picks a new PID that keeps the others, so the other rows must not change. Choosing the value a row already shows changes nothing.
- Saving the Level, Nickname or OT Name input without changing it changes nothing either: a loaded Pokemon keeps its current HP, and characters the Flipper shows as `?` stay as they were.
- A Pokemon loaded from a file keeps its current HP until the first edit. Any edit then heals it to full HP. That is by design.
- The Flipper keyboard types lower case, except the first character, which comes out as a capital. A long press types the other case. `_` typed as the first character (or with a long press) becomes a space, so type it with a short press after another character.
- Error messages are shown on two lines; this checklist writes them on one.
- Run this test in a fresh app run without `Trade`. Once a GBA has linked for a trade, new species take the GBA player as OT and an `OT: use <NAME>` row appears, until the app exits (see `docs/gen3/hardware-test-2.md`).

## A. Editor basics

1. **Gen III menu.** Main menu → `Gen III  (GBA non-JPN)` shows 7 items: `Trade`, `Edit Pokemon`, `Load .pk3`, `Save .pk3`, `National Dex: Off`, `Link Test` and `Cartridge` (while no trade runs; this test opens no trade). `Link Test` still opens the Link Test screen, and Back from it returns to this menu. (`Trade` is for `docs/gen3/hardware-test-2.md`; don't open it in this test.)
2. **Defaults.** `Edit Pokemon` shows, in this order: Pokemon `BULBASAUR`, Nickname `BULBASAUR`, Level `5`, `Random IV, Zero EV`, Nature (any nature), Ability `OVERGROW`, Shiny `No`, Gender `Male` or `Female`, Ball `POKE BALL`, Held Item `No Item`, `Select Moves`, Pokerus `Clean`, OT Name `FLIPPER`, OT TID `22222`, OT SID `00000`. For Unown only, an Unown Form row comes between Gender and Ball. The menu has no header. The values line up in one column, except the preset row (`Random IV, Zero EV`, which has no label) and `Select Moves` (which has no value).
3. **Nickname.** Set it to `Leafy` → the row shows `Leafy`. Clear the field and save it empty → the row shows `BULBASAUR`. Type `A_B` (`a` comes out as `A`, `_` with a short press, then long-press `b` for `B`) → the keyboard shows `Unsupported character!` and stays open.
4. **OT Name.** Set it to `Ash` → the row shows `Ash`. Clear the field and save it empty → the keyboard shows `Name can't be empty!` and stays open; Back leaves the name as `Ash`.
5. **Level.** `0` and `101` are refused with `Level must be 1-100!`, and so is an empty field. `50` is accepted and the row shows `50`.
6. **Preset.** The list shows the 6 presets with `Random IV, Zero EV` highlighted. Choose `Max IV, Max EV` → the row now reads `Max IV, Max EV`.
7. **Nature.** The list shows 25 natures, with the current one highlighted, e.g. `HARDY`, `LONELY +Atk -Def`, `ADAMANT +Atk -SpA`. Choose `ADAMANT +Atk -SpA` → the row shows `ADAMANT`, and the Gender and Shiny rows are unchanged.
8. **Gender.** The list shows `Female` and `Male`, with the current gender highlighted. Choose the other one → the Gender row changes, and the Nature (`ADAMANT`) and Shiny rows are unchanged.
9. **Shiny and OT ID.** Set Shiny to `Shiny` → the row shows `Yes`. Then set OT TID to `12345` → the TID row shows `12345` and Shiny is still `Yes`. Set OT SID to `54321` → the SID row shows `54321` and Shiny is still `Yes`. OT TID and OT SID refuse `65536` with `ID must be 0-65535!`.
10. **Ability.** On Bulbasaur, pressing OK on the Ability row does nothing (one ability).
11. **Ball.** The list shows 12 balls, `MASTER BALL` to `PREMIER BALL`, with `POKE BALL` highlighted. Choose `MASTER BALL` → the Ball row shows `MASTER BALL`.
12. **Kept on Back.** Press Back to the Gen III menu, then open `Edit Pokemon` again → the same values as before, and the last row you used is highlighted.
13. **No regression.** Back to the main menu. `Gen I` and `Gen II` each open and show their own editors as before.

## B. Species

1. **Sprites.** Open the `Pokemon` row. #001 to #386 each show a sprite; step through a few with Left/Right, some from #252 up.
2. **Gen III sprites.** #252 TREECKO shows its sprite and the type `GRASS`, and #386 DEOXYS its sprite and `PSYCHC`. None shows the `?` frame (that is only for a sprite the Flipper can't read from the SD card).
3. **Wrapping and jumps.** Left from #001 goes to #386 DEOXYS. Up and Down jump by 10 (Down from #005 and Up from #380 wrap around). Holding Right scrolls.
4. **Pick a species.** OK on TREECKO → the menu shows `Pokemon: TREECKO` with nickname `TREECKO`, the level unchanged and the OT unchanged.
5. **Nickname kept.** Set the nickname to `X`, then pick TREECKO again → the nickname is still `X`.
6. **Unown.** Pick UNOWN → an `Unown Form` row appears. Set `?` → the row shows `?`.
7. **Stress.** Switch #251 <-> #252 <-> #001 20 times → no crash.
8. **Back.** Open the `Pokemon` row, move to another species, press Back → the Pokemon is unchanged.
9. **Same species.** Open the `Pokemon` row and press OK on the species already shown → nothing changes (nickname, moves and the other rows stay).
10. **Shiny Unown.** For some OT IDs, some Unown nature and letter pairs have no shiny PID at all.
    - Pick UNOWN. Set OT TID `64893` (OT SID `00000`), Nature `HARDY` and Unown Form `P`. Then Shiny → `Shiny` → a dialog `No shiny PID` / `for this letter, nature and OT ID.` appears at once. OK (or Back) returns to the editor, with Shiny still `No` and the other rows unchanged.
    - Set OT TID `22222`, then Shiny → `Shiny` → the row shows `Yes`.
    - Set OT TID `64893` → a dialog `Shiny lost` / `No shiny PID for this OT ID.`. OK returns to the editor: OT TID `64893`, Shiny `No`, Nature `HARDY`, Unown Form `P`.

## C. Moves, item, Pokerus

1. **Moves.** Set the species to TREECKO and the level to 30 → `Select Moves` shows `Move 1: QUICK ATTACK`, `Move 2: PURSUIT`, `Move 3: SCREECH`, `Move 4: MEGA DRAIN`, then `Default moves`.
2. **Remove a move.** Move 1 → `No Move` → PURSUIT moves up to Move 1, and Move 4 shows `No Move`.
3. **No duplicates.** Move 4 → letter `P` does not list PURSUIT while it is known.
4. **Same move.** Move 1 → its own letter → the move it already has → nothing changes.
5. **Default moves.** `Default moves` restores QUICK ATTACK, PURSUIT, SCREECH and MEGA DRAIN.
6. **Last move.** Remove moves until one is left → on that slot the letter screen has no `No Move`.
7. **Held item.** `Held Item` → `L` → LEFTOVERS → the row shows `LEFTOVERS`. `Held Item` → `No Item` → the row shows `No Item`.
8. **Item filters.** Letter `E` has no ENIGMA BERRY, and `M` has no MACH BIKE (nor any Mail).
9. **Pokerus.** Strain `A`, days `3` → the row shows `Infected`. Days `0` → `Cured`. Strain `None` → `Clean`, and Days is hidden. Back and reopen → the strain and days are as left.
10. **Empty letter.** On a move slot, a letter whose only move is in another slot shows an empty list. That is expected; press Back.

## D. Files

1. **Save.** `Save .pk3` suggests the nickname → OK → `Saved` / `<name>.pk3`. The file `apps_data/pokemon/gen3/<name>.pk3` is 80 bytes (check with qFlipper). A name with a bad character shows `Use A-Z, 0-9,` / `_ or -`.
2. **Overwrite.** Save again with the same name → `Overwrite?`. `Cancel` returns to the name input and keeps the old file. `Overwrite` → `Saved`.
3. **Load.** Change the level, then `Load .pk3` that file → `Loaded` / `<NICK>` / `<SPECIES> Lv. <n>`, and the Edit menu shows the saved level, nature, moves and OT. Back in the file browser returns to the Gen III menu.
4. **Not a .pk3.** Copy any non-`.pk3` file to `gen3/` renamed to `x.pk3` → `Load failed` / `Not a .pk3 (size)`.
5. **PKHeX (optional).** Open the saved file → no checksum warning, and species, level, nature, moves and OT match. Export a Gen III Pokemon from PKHeX as `.pk3` → it loads on the Flipper. If the file had a held Mail item or a mail index other than none (a 100-byte party file from a save where it held mail), a duplicate move, a move gap, an out-of-range level, or a wrong ability bit on a one-ability species, the result also says `Fixed: mail moves level ability` (only the parts that changed).
6. **No SD card.** After opening `Edit Pokemon` in this session (so the Gen III data is ready), remove the SD card → `Load .pk3` shows `No SD card`.
7. **Data missing.** With `gen3_bulk.bin` renamed (see E), `Load .pk3` and `Save .pk3` show `Data missing`.
8. **SD card back.** Open `Edit Pokemon` once, then Back to the Gen III menu. Remove the SD card, wait a few seconds and put it back, without leaving the app. Open `Edit Pokemon`. The first time, the Pokemon row may show no name: a read through the old file handle failed, and that closed the data file. The Ball row, and the Held Item row unless it shows `No Item`, may then be blank too: their names come from the same file. Press Back, then open `Edit Pokemon` again → `Pokemon: <SPECIES>` shows, and picking a species and `Default moves` work. If `Data missing` appears with the card in place, report it.

## E. Data file

The species, move and item names and the learnsets are not in the app's memory. They are in `gen3_bulk.bin`, which ships inside the `.fap`. The Flipper unpacks the app's files to `apps_assets/pokemon/` when the app starts and that folder is missing or out of date. Reinstalling the same `.fap` doesn't unpack them again while `apps_assets/pokemon/.assets.signature` still matches, so the remedy is to delete the folder.

1. **Data missing.**
   - Exit the app. In qFlipper, rename `apps_assets/pokemon/gen3_bulk.bin` to `gen3_bulk.bak`.
   - Start the app → Gen III → `Edit Pokemon` → a dialog `Data missing` with `Gen III data file missing.` / `Delete apps_assets/pokemon` / `on the SD card, then reopen.` and an `OK` button. OK returns to the Gen III menu; open `Edit Pokemon` again and press Back, which also returns to the Gen III menu (not the main menu). `Link Test` still opens.
   - Exit the app. In qFlipper, delete the whole folder `apps_assets/pokemon`. Start the app → the Flipper unpacks its files again (the folder is back, with `gen3_bulk.bin` and `all_sprites.fxbm`) → Gen III → `Edit Pokemon` opens the editor with `Pokemon: BULBASAUR`, and the species picker shows sprites.

## F. Memory

1. Open Gen III → `Edit Pokemon`. In qFlipper's CLI (or `ufbt cli`), run `free` and record `Free heap size`. It should be at least 20,000. If it is lower, or the app does not start, report the number.

## Results

On any failure, report the step, what the screen showed, and the `.pk3` file involved. Fill in the table.

| Step | Pass / fail | What the Flipper showed |
|---|---|---|
| A1 Gen III menu | | |
| A2 Defaults | | |
| A3 Nickname | | |
| A4 OT Name | | |
| A5 Level | | |
| A6 Preset | | |
| A7 Nature | | |
| A8 Gender | | |
| A9 Shiny and OT ID | | |
| A10 Ability | | |
| A11 Ball | | |
| A12 Kept on Back | | |
| A13 Gen I / Gen II | | |
| B1 Sprites | | |
| B2 Gen III sprites | | |
| B3 Wrapping and jumps | | |
| B4 Pick a species | | |
| B5 Nickname kept | | |
| B6 Unown | | |
| B7 Stress | | |
| B8 Back | | |
| B9 Same species | | |
| B10 Shiny Unown | | |
| C1 Moves | | |
| C2 Remove a move | | |
| C3 No duplicates | | |
| C4 Same move | | |
| C5 Default moves | | |
| C6 Last move | | |
| C7 Held item | | |
| C8 Item filters | | |
| C9 Pokerus | | |
| C10 Empty letter | | |
| D1 Save | | |
| D2 Overwrite | | |
| D3 Load | | |
| D4 Not a .pk3 | | |
| D5 PKHeX | | |
| D6 No SD card | | |
| D7 Data missing | | |
| D8 SD card back | | |
| E1 Data missing | | |
| F1 Memory: free heap (bytes) | | |
