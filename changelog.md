# Changelog - Patch Notes

## Version 3.1
**New Features**
- Gen III cartridge save access (experimental): back up, browse and edit the party and boxes, write back with verify, restore, copy ROM
- Cartridge is a second app, `pokemon_cart.fap`; install it next to `pokemon.fap`
- Cartridge also works over a Game Boy Color link cable (GBA Normal mode), for Game Link modules whose SD line isn't connected; tested on hardware with Ruby and FireRed (connect, backup, edit, write)
- Writing is allowed only for English Ruby, Sapphire, FireRed and LeafGreen; tested on hardware with Ruby and FireRed, not yet with Sapphire or LeafGreen. The hardware checklist is `docs/gen3/hardware-test-3.md`

## Version 3.0
**New Features**
- Gen III (Game Boy Advance, non-Japanese): Cable Club trades with Ruby, Sapphire, FireRed and LeafGreen, one Pokemon at a time and back again without leaving the Trade Center
- Gen III Pokemon editor: species, nickname, level, held item, moves, IVs/EVs, nature, ability, shiny, gender, Pokerus, Poke Ball, OT name/TID/SID, Unown form, fateful flag for Mew/Deoxys
- Load and save Gen III Pokemon as PKHeX `.pk3` files
- Sprites for the Gen III species (#252-386) in the species picker, converted from Pokemon Ruby's sprites by `tools/gen3_sprites.py`
- `files/gen3_bulk.bin` (species, move and item names, and learnsets) ships as an app asset; if the editor says `Data missing`, delete `apps_assets/pokemon` on the SD card and reopen
- Gen III Link Test with a link log, and the SD pin on the Pinout screen
- Hardware checklists in `docs/gen3/`

## Version 2.3
- Fix potential bug in copying incoming trade data to the live working struct

## Version 2.2
**New Features**
- Update to gblink v0.63 which includes saving/loading of pin configurations for the EXT link interface

## Version 2.1
**New Features**
- Add ability to reset trade state without affecting current Pokemon being configured

**Refactor**
- Move files, consolidate files where appropriate, change scene management to be more in-line with Flipper paradigms

## Version 2.0
**Bug Fixes**  
- Older MALVEKE style pinout no longer breaks OK button, MALVEKE users now able to modify traded Pokemon

**New Features**  
- Generation II support (Gold, Silver, Crystal)  
  - Ability to configure name, level, held item, moveset, EV/IV, shininess, gender, pokerus, Unown form, OT ID/name
- Custom Sprite Art for all Pokemon, all sprites are the full 56x56 px size  
- Better state synchronization during trades, able to request canceling a trade
- Exit confirmation to prevent accidentally losing configuration  
- Add main menu to select generation and pinout  
- Adjust Trade and Select views to show full sprite, with proper transparency

**Refactor**
- Sprites moved to file on SD card  
- Create accessors for generically modifying Pokemon struct data  
