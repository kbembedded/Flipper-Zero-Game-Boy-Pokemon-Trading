# Pokemon Trade Tool for Flipper

## Watch it in Action
The video below trades a Bulbasaur from the Flipper to a Game Boy Color with Pokemon Silver. The Game Boy trades its Cyndaquil. The Flipper is then used to modify the Cyndaquil to infect it with Pokerus, modify its EVs and IVs, and have the Cyndaquil hold an Antidote before it is traded back to the Game Boy.

[![Watch a Trade in Action](docs/images/yt-vid-preview-frame.png)](http://www.youtube.com/watch?v=zONctHIPgEo "Trade in action")

## Contents
- [Introduction](#introduction)
- [Hardware Interface](#hardware-interface)
- [Installation Directions](#installation-directions)
- [Instructions for Use](#instructions-for-use)
  - [Main Menu](#main-menu)
    - [Pinout](#pinout)
    - [Customizing Pokemon](#customizing-pokemon)
  - [Trade](#trade-pkmn)
    - [Modifying Traded Pokemon](#modifying-traded-pokemon)
    - [Reset Trade Connection State](#reset-trade-connection-state)
- [Gen III (Game Boy Advance)](#gen-iii-game-boy-advance)
- [How it Works / Build your own Interface](#how-does-it-work)


## Introduction

This is a Pokemon exchange application from Flipper Zero to Game Boy supporting Generation I and II games; Red, Blue, Yellow, Gold, Silver, and Crystal (all non-Japanese regions[^JPN]). It allows trading Pokemon to and from a Game Boy; allowing for customization and/or modification of Pokemon.

Version 3.0 adds [Generation III](#gen-iii-game-boy-advance): Ruby, Sapphire, FireRed and LeafGreen (non-Japanese). Version 3.1 adds [Cartridge](#cartridge-save-access), a second app that backs up, edits and writes back the save of a Gen III cartridge over the link cable (experimental).

[^JPN]: Japanese releases of all Generation I and II games use a different data layout, this application implements the data layout used in all other global regions. Attempting to trade between a Japanese cartridge and the Flipper [risks corrupting or erasing save data](https://web.archive.org/web/20221009111851/http://www.nintendo.com/consumer/systems/gameboy/trouble_specificgame.jsp#erasing)

Generation I customization options:

- Pokemon
- Nickname
- Level
- Move set
- Types[^TYPES]
- EV/IV[^EVIV]
- OT ID Number[^OT]
- OT Name[^OT]

Generation II customization options:

- Pokemon
- Nickname
- Level
- Held item
- Move set
- EV/IV[^EVIV]
- Shininess[^IV]
- Gender[^IV]
- Pokerus
- Unown form[^IV]
- OT ID Number[^OT]
- OT Name[^OT]

Once a Pokemon is traded between the Flipper and a Game Boy the Pokemon is kept in memory on the Flipper. This allows a user to back out of the trade screen on the Flipper, modify the Pokemon, and trade it back. Any of the above customization can be modified and the Pokemon traded back to the Game Boy for updated game play. Want a Pokemon to learn a move before it should? Make a Pokemon Shiny? Change its OT ID/Name to get an exp boost? This is how you do it!

[^TYPES]: Types are overwritten with the defaults when a Pokemon evolves or at the end of a battle where a Pokemon used a move that would temporarily change its types
[^EVIV]: EV/IV can be configured in combinations of the following: Random IV, Max IV, Max EV, Max EV for the current level, Zero EV
[^OT]: Setting this to match the trainer details of the Pokemon game will cause the game to treat the Pokemon as if it were caught by the player rather than traded
[^IV]: Changing this option will modify the Pokemon's IVs and may affect other attributes that are calculated from IVs

## Hardware Interface
The Game Boy is connected to the Flipper Zero's GPIO pins via a GBC style Game Link Cable. The [Flipper GB Link module](https://www.tindie.com/products/kbembedded/game-link-gpio-module-for-flipper-zero-game-boy/) is an easy way to connect a Game Boy via a Game Link Cable to the Flipper Zero.

<p align='center'>
  <a href="https://www.tindie.com/stores/kbembedded/?ref=offsite_badges&utm_source=sellers_kbembedded&utm_medium=badges&utm_campaign=badge_large">
<img src="https://i.imgur.com/WQIJK8G.png" alt="Flipper GB Link module" width="480">
  </a>
</p>
<p align='center'>
<a href="https://www.tindie.com/stores/kbembedded/?ref=offsite_badges&utm_source=sellers_kbembedded&utm_medium=badges&utm_campaign=badge_large"><img src="https://d2ss6ovg47m0r5.cloudfront.net/badges/tindie-larges.png" alt="I sell on Tindie" width="200" height="104"></a>
</p>

Additionally, the [MALVEKE - GAME BOY Tools for Flipper Zero](https://www.tindie.com/products/efuentealba/malveke-game-boy-tools-for-flipper-zero/) is supported by this tool. As of version 2.0 of the Pokemon Trading tool, older style MALVEKE pinouts can be used without affecting the OK button after a trade -- this allows users to customize and modify traded Pokemon and there is no need to hard reset the flipper after a trade!

<p align='center'>
<a href="https://www.tindie.com/stores/efuentealba/?ref=offsite_badges&utm_source=sellers_efuentealba&utm_medium=badges&utm_campaign=badge_large">
<img src="https://i.imgur.com/xdbLXBP.png" alt="MALVEKE" width="480">
</a>
</p>
<p align='center'>
<a href="https://www.tindie.com/stores/efuentealba/?ref=offsite_badges&utm_source=sellers_efuentealba&utm_medium=badges&utm_campaign=badge_large"><img src="https://d2ss6ovg47m0r5.cloudfront.net/badges/tindie-larges.png" alt="I sell on Tindie" width="200" height="104"></a>
</p>

Details on the hardware interface, as well as how to create your own adapter board, can be found in the [How Does It Work](#how-does-it-work) section below.

## Installation Directions

This application can be installed through the [Flipper Application Catalog](https://lab.flipper.net/apps/pokemon), the [Flipper Mobile Application](https://docs.flipper.net/mobile-app), or a pre-compiled application binary can be found on the [Releases](https://github.com/KBEmbedded/Flipper-Zero-Game-Boy-Pokemon-Trading/releases) page of this repo.

The application can also be built from source using [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) or compiling the application against a firmware of choice using [fbt](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/documentation/AppsOnSDCard.md). This will install the application to the `apps/GPIO/` folder on the Flipper's SD card.

From version 3.1 the application is two apps, built together from this source:

- `pokemon.fap`, Pokemon Trade Tool: Gen I, Gen II and Gen III trading and the editors;
- `pokemon_cart.fap`, Pokemon Cartridge: the Gen III [cartridge save access](#cartridge-save-access).

Install both, side by side in `apps/GPIO/` on the SD card. `ufbt` builds both into `dist/`; `ufbt launch` needs `APPID=pokemon` or `APPID=pokemon_cart` to say which one to install and start. Whether an install from the Application Catalog or the mobile app brings `pokemon_cart.fap` along is not known yet: if Gen III → `Cartridge` shows `App missing`, copy `pokemon_cart.fap` from the [Releases](https://github.com/KBEmbedded/Flipper-Zero-Game-Boy-Pokemon-Trading/releases) page next to `pokemon.fap`.

Manually built applications (both `.fap` files) can be copied to the SD card using [qFlipper](https://docs.flipper.net/qflipper).

<p align='center'>
<img src="./docs/images/qFlipper.png" width="400" /><br />
</p>

## Instructions for Use
### Main Menu
From the main menu, select the generation of Pokemon game to trade with, first changing the `Pinout` if needed. Note that Gen I can trade with Gen I games as well as to Gen II games using Time Capsule trade. Gen II can only trade with Gen II games.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-main-menu.png" width="400" />
    <br />
</p>

### Pinout

If you're using a MALVEKE PCB that is Rev. 2.5.1 or newer, a Flipper-GB-Link PCB, or a custom wired interface based on the [wiring diagram listed here](#how-does-it-work), then there is no need to worry about setting the pinout; the application default is for these devices. If you're using a MALVEKE PCB Rev. 2.5 or older, select the `MLVK2.5` pinout. For custom wired interfaces that don't follow either standard, select the `Custom` pinout and set each pin individually.

- If you're using a MALVEKE PCB version, verify the **Pinout** configuration:

    <p align='center'>
        <br />
        <img src="./docs/images/flipper-zero-pinout-select.png" width="400" />
        <br />
    </p>

    - MALVEKE **2.5** PCB Version: If you're using this version, select the `MLVK2.5` configuration.
        <p align='center'>
            <br />
            <img src="./docs/images/flipper-zero-pinout-malveke.png" width="400" />
            <br />
        </p>

    - MALVEKE **2.5.1** PCB Version: If you're using this version, select the `ORIGINAL`
        <p align='center'>
            <br />
            <img src="./docs/images/flipper-zero-pinout-original.png" width="400" />
            <br />
        </p>

### Customizing Pokemon
- [Select Pokemon](#select-pokemon-gen-i--gen-ii) (Gen I & II)
- [Nickname](#nickname-gen-i--gen-ii) (Gen I & II)
- [Level](#level-gen-i--gen-ii) (Gen I & II)
- [Held Item](#held-item-gen-ii-only) (Gen II only)
- [Select Moves](#select-moves-gen-i--gen-ii) (Gen I & II)
- [Select Types](#select-types-gen-i-only) (Gen I only)
- [Stats](#stats-gen-i--gen-ii) (Gen I & II)
- [Shiny](#shiny-gen-ii-only) (Gen II only)
- [Gender](#gender-gen-ii-only) (Gen II only)
- [Pokerus](#pokerus-gen-ii-only) (Gen II only)
- [Unown Form](#unown-form-gen-ii-only) (Gen II only)
- [OT ID# / Name](#ot-id--name-gen-i--gen-ii) (Gen I & II)

#### Select Pokemon (Gen I / Gen II)
To select a Pokemon, use `LEFT` and `RIGHT` buttons to select the Pokemon; `UP` and `DOWN` are used to page up/down by 10 Pokemon. Press `OK` to confirm selection, or `BACK` to cancel selection. If a different Pokemon is selected, the remaining customization options are set to the default for that Pokemon.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-pokemon-select-1.png" width="400" />
    <br />
</p>

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-pokemon-select-2.png" width="400" /><br />
</p>

---

#### Nickname (Gen I / Gen II)
The selected Pokemon's nickname can be set. When a Pokemon is selected, the nickname defaults to the species name in all caps. This mimics a Pokemon without a customized nickname. In order to reset this nickname to its default, clear the text entry field, press `OK` on the `Save` button. This will fill the text box with the default name. Press `Save` again to set this name.

> [!IMPORTANT]
> Nidoran♀ and Nidoran♂ names will not properly render. This is because the Flipper currently cannot print Unicode characters to screen. Following the above instructions will fill the text entry field with `NIDORAN ` with a space after it. This space is the unrenderable ♀/♂ symbol. Once traded, it will be correctly named.

> [!NOTE]
> (Gen II only) The hyphen in `HO-OH` cannot be selected on the Flipper's keyboard input; however resetting the name as noted above will restore the default name.

> [!NOTE]
> Only English alphabetical characters are supported in the Pokemon's nickname at this time.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-nickname.png" width="400" />
    <br />
</p>

---

#### Level (Gen I / Gen II)
Set the Pokemon's level from `2` to `100`. This will recalculate the Pokemon's stats based on the new level and the current EV/IV settings.

> [!TIP]
> Levels below 2 cause an underflow glitch in Gen I and II games that would cause the level to jump to 100, so if you want this just set the Pokemon's level to 100.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-level.png" width="400" />
    <br />
</p>

---

#### Held Item (Gen II only)
The traded Pokemon can be given an item to hold. All of the valid items (including items a Pokemon can normally not be given to hold) are listed in alphabetical order.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-held-item.png" width="400" />
    <br />
</p>

---

#### Select Moves (Gen I / Gen II)
The `Select Moves` menu is used to modify the traded Pokemon's moves. They are pre-populated with the moveset that the Pokemon would know at level 1. Selecting a move slot will bring up an alphabetical index of moves. Additionally, `No Move` and `Default` can be quickly selected.

> [!NOTE]
> Any move in the moveset after the first `No Move` is ignored and is treated as `No Move` by the game.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-move-1.png" width="400" />
    <br />
</p>

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-move-2.png" width="400" />
    <br />
</p>

---

#### Select Types (Gen I only)
The `Select Types` menu can change the traded Pokemon's types. The type(s) are pre-set to what the selected Pokemon normally is. Gen II games do not store temporary types alongside the Pokemon, therefore they are not kept during trades.

> [!NOTE]
> Unlike other menus, changing either type immediately saves it. Pressing `Back` will keep any changes. This will be addressed in a later version. If you need to revert to the default types, a different Pokemon can be selected and the desired Pokemon re-selected.

> [!NOTE]
> When changing the type(s), the Pokemon's in-game stats will _NOT_ reflect the chosen type(s). Additionally, these will be overwritten back to default in-game if the Pokemon uses a move that affects types (e.g. `Transform`) or the Pokemon evolves.

> [!TIP]
> Pokemon with a single type will have the same type set for both types.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-type.png" width="400" />
    <br />
</p>

---

#### Stats (Gen I / Gen II)

The Pokemon's stats can also be influenced. The current settings are:
- `Random IV, Zero EV` Mimics stats of a caught wild Pokemon.
- `Random IV, Max EV / Level` IV is randomized, but EV is set to the maximum a trained Pokemon could be for its current level.
- `Random IV, Max EV` IV is randomized, EV is set to the absolute max for a perfectly trained Pokemon.
- `Max IV, Zero EV` Mimics stats of a caught wild Pokemon, but with the maximum IV possible.
- `Max IV, Max EV / Level` IV is max, EV is set to the maximum a trained Pokemon could be for its current level.
- `Max IV, Max EV` Absolutely perfect and overly powerful Pokemon.
 
<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-stats.png" width="400" />
    <br />
</p>

---

#### Shiny (Gen II only)
The Pokemon can be modified to be shiny or not. 

> [!NOTE]
> Modifying this option will affect the IV, but this will not be visible anywhere in the current application. For example, if any of the `Max IV` settings are selected, this will never result in a shiny Pokemon; but modifying the Pokemon to be shiny will adjust the IVs until the Pokemon is shiny. The Stats will still list `Max IV` however.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-shiny.png" width="400" />
    <br />
</p>

---

#### Gender (Gen II only)
The Pokemon's gender can be selected.

> [!NOTE]
> Modifying this option will affect the IV, but this will not be visible anywhere in the current application. For example, if any of the `Max IV` settings are selected and the Pokemon's gender is modified, the IVs will be adjusted so the Pokemon is the desired gender. The Stats will still list `Max IV` however.

> [!TIP]
> Some Pokemon are always a specific gender, `Miltank` for example is always female. Other Pokemon are gender-less (officially stated in-game as `Unknown`) and this is reflected if such a Pokemon is selected. In both of these cases, it is not possible to modify the Pokemon's gender.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-gender.png" width="400" />
    <br />
</p>

---

#### Pokerus (Gen II only)

The traded Pokemon can have its [Pokerus](https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9ru) status modified. There are 4 in-game strains, the only effect is their default infection days remaining. However, it is possible to select an arbitrary strain, or none, as well as the number of days remaining. The days remaining can be set all the way up to `15`, which is not naturally occurring. In-game, strain `D` randomly infecting a Pokemon has the highest number of days remaining, `4`. If a strain is set and the number of days remaining is `0`, the Pokemon is cured and will no longer infect other Pokemon.

The three states listed in the trade menu are `Clean` (having never been infected), `Infected` (any strain with a non-zero number of days remaining and is infectious), and `Cured` (any strain with 0 days remaining and is no longer infectious).

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-pokerus-setup.png" width="400" />
    <br />
</p>

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-pokerus-infected.png" width="400" />
    <br />
</p>

---

#### Unown Form (Gen II only)
When the Pokemon `Unown` is selected, this option appears. The Pokemon can be set to any single letter of the English alphabet.


> [!NOTE]
> Modifying this option will affect the IV, but this will not be visible anywhere in the current application. For example, if any of the `Max IV` settings are selected and Unown's form is modified, the IVs will be adjusted so the Pokemon is the desired form. The Stats will still list `Max IV` however.

> [!TIP]
> In Gen II games, only two Unown forms, `I` and `V` are able to be shiny due to how both the form and shininess are based on the Pokemon's IVs.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-unown-form.png" width="400" />
    <br />
</p>

---

#### OT ID# / Name (Gen I / Gen II)

The `OT ID#` and `OT Name` of the Pokemon can also be set. The `OT ID#` must be between `0` and `65535`. Setting the `OT ID#` and `OT Name` to the same as your current trainer's causes the game to believe it was a wild caught Pokemon and not one that was traded. This means high level Pokemon will still obey you without badges, but, will not get the experience boost of a traded Pokemon.

> [!NOTE]
> Only English alphabetical characters are supported in the OT Name at this time.

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-ot-id.png" width="400" /><br />
</p>

<p align='center'>
    <br />
    <img src="./docs/images/flipper-zero-ot-name.png" width="400" /><br />
</p>

---

### Trade PKMN
The last option in both generations is to start the trade. The first time this is entered the Flipper will prompt to connect a Link Cable to the Game Boy. On the Game Boy, enter a **Pokemon Center** and speak with the NPC at the **Cable Club** to start a trade. Once the Game Boy goes through the prompts to enter a trade, the Flipper will say `READY` and the Game Boy will enter the trade room.

> [!NOTE]
> At any point the Flipper says `READY`, the back button on the Flipper can be pressed to go back to the customization menu. Re-entering the trade screen from there will remain connected to the Game Boy.

On the Game Boy, approaching the trade table and pressing `A` will start a trade. The Flipper will display `WAITING` while the Game Boy is on the screen showing both parties.

> [!NOTE]
> At any point the Flipper says `WAITING`, the `BACK` button on the Flipper can be pressed to request to cancel the trade. The Flipper will display `CANCEL` and will remain there until the Game Boy selects `CANCEL` as well (attempts to trade from the Game Boy will just be aborted) and the Flipper will revert to the `READY` state. If the Flipper displays `WAITING` and the Game Boy selects `CANCEL`, the Flipper will return to the `READY` state.

On the Game Boy, select a Pokemon to trade with the Flipper. Confirming the trade will begin the trade animation on both the Flipper and Game Boy. Once the trade is complete, both the Game Boy and the Flipper will return to the `WAITING` state. If the Game Boy selects `CANCEL` at this point, the Flipper will return to the `READY` state. The `BACK` button can be pressed to return to the main menu.

> [!WARNING]
> At any point while on the trade screen on the Flipper, it is possible to return to the customization menu by holding the `BACK` button. However doing this risks desyncing the trade state between the Flipper and Game Boy.

---

#### Modifying Traded Pokemon
Once a trade is complete, the Pokemon traded from the Game Boy to the Flipper is kept in the Flipper's memory. It is possible to go back from the trade screen to the Gen I/II customization menu. There, the Pokemon can be modified; e.g. adjust the EV/IV, levels, move set, nickname, OT ID#/Name, etc. and traded back by re-entering the `Trade PKMN` option.

The Game Boy should remain on and in the trade room. When the Flipper re-enters the `Trade PKMN` option, the Game Boy can re-select the trade table by pressing `A` at it and the whole trade process can be restarted. If the Game Boy is turned off, then the Flipper currently must back all the way out to reset the trade status so the Game Boy can re-establish the initial connection.

---

#### Reset Trade Connection State
It is possible to reset the Flipper's connection state to having never been connected to a Game Boy. This allows the Flipper to restart communication with a Game Boy if it was accidentally turned off, for example, and the Game Boy needs to re-enter the Trade Center. Doing this keeps the current Pokemon the Flipper has in memory intact.

This can also be used to leverage the Flipper to trade a pokemon between two Pokemon games with only a single Game Boy. For example, trade from Pokemon Blue to the Flipper (and then modifying the Pokemon on the Flipper if desired), resetting the trade state on the Flipper, swap the Game Boy game to Pokemon Silver, and on both the Game Boy and Flipper, start the trade process (using Time Capsule trade mode on the Pokemon Silver game).

Once a connection has been established between the Flipper and a Pokemon game, the option `Reset Connection` will appear in the configuration screen under `Trade PKMN`. The Flipper will then ask for confirmation of the operation.

---

#### Special Note on MALVEKE PCB Rev. <= 2.5
Version 2.0 of the Pokemon Trade tool fixes a bug on MALVEKE boards that are Rev. 2.5 or lower where after a trade is completed the `OK` button no longer functions. However, while on the trade screen the `OK` button will continue to not function. For example, if you try to press the `OK` button to turn the backlight on, the Flipper will not respond. The `OK` button functionality will be restored once the Flipper leaves the Trade screen.


## Gen III (Game Boy Advance)
The main menu's `Gen III  (GBA non-JPN)` entry is for the Game Boy Advance games. Its menu has `Trade`, `Edit Pokemon`, `Load .pk3`, `Save .pk3`, `National Dex: Off`, `Link Test` and `Cartridge` (while trading, `End trade` in place of the last two).

### Games
English Ruby, Sapphire, FireRed and LeafGreen. Not yet tested on hardware: the checklists in `docs/gen3/` (see [Link Test, log and checklists](#link-test-log-and-checklists)) are the test. Emerald is untested.

The Flipper links as a player of the game it meets: with FireRed or LeafGreen, as a player of that game (a FireRed/LeafGreen avatar and a Kanto-style trainer card); with Ruby or Sapphire, as a Ruby player.

### Cable
Plug one end of the GBA link cable into the Flipper's module first, with the other end free, then open `Trade` or `Link Test`. The Link Test shows `Cable: plug in/swap ends`, and the trade screen `Plug in the cable (or swap its ends).`, until the right plug is in the Flipper. A missing and a reversed cable look the same, so if it stays, put the cable's other plug into the Flipper instead. Once the cable reads right (`Cable: OK` in the Link Test), plug the other end into the GBA. Note which plug works for you. The cable check stays live: if it fails while linked, the Flipper pauses its transfers.

The GBA link also uses the SD pin, which the [Pinout](#pinout) screen shows. Trading needs a GBA link cable and a Game Link module whose SD line is connected (KBEmbedded module: the two SD jumper pads bridged); without SD the GBA never answers. [Cartridge](#cartridge-save-access) also works with a Game Boy Color link cable on a module without SD.

### Trading
The Flipper trades as the other player at the Cable Club Trade Center table. Its party is one Pokemon, the one in `Edit Pokemon`. The Pokemon it gets back becomes the editor's, so you can change it and trade it back, as often as you like, without leaving the Trade Center.

How to trade:

1. On the Flipper, choose `Gen III  (GBA non-JPN)` → `Trade`. The trade screen says what is happening and what to do on the GBA, and its last line, `Flipper: <NICKNAME>`, is the Pokemon on offer.
2. Plug in the cable as in [Cable](#cable), the Flipper's end first.
3. On the GBA, go to the Pokemon Center 2F and talk to the Trade Center attendant. In Ruby and Sapphire that is the middle one (the right-hand one is the Record Corner); in FireRed and LeafGreen, talk to the right-hand one and choose TRADE CENTER. Let the game save. The trade screen says `Go to the Cable Club: PC 2F, Trade Center: RS middle, FRLG right` until the GBA links.
4. In the Trade Center, sit at the table. The Flipper sits too, and the trade menu opens with the Flipper's Pokemon on the right.
5. Choose one of yours → TRADE → YES. The Flipper accepts every trade by itself. The GBA then saves: don't unplug or turn off either side until its trade menu is back.
6. Your Pokemon is now on the Flipper's side, in `Edit Pokemon` and on offer. Pick the next trade on the GBA, or edit it first. The Flipper recalculates a received Pokemon's stats, as it does for a loaded file. If that changes the Pokemon after the GBA has already reopened its trade menu, the GBA goes back to the room once, and you sit again.

**Save what you receive.** A Pokemon the Flipper receives lives only in the editor, in the Flipper's memory, until you `Save .pk3` it. Exiting the app loses it, so save it if you want to keep it.

The link keeps running behind every Gen III screen: the editor, its keyboards and lists, the species picker, and `.pk3` load and save. `Trade` in the Gen III menu returns to the trade screen. While the link runs, the Gen III menu shows `End trade` in place of `Link Test`.

**Edit between trades.** Press OK (`Edit`) on the trade screen, or open `Edit Pokemon` or `Load .pk3` from the Gen III menu. If the GBA is in the trade menu, it goes back to the room, with your avatar still on its seat. If you already chose TRADE on the GBA, the Flipper has answered within about 0.2 s and the GBA asks "Is this trade okay?"; it waits there for your answer, and goes back to the room once you answer YES while the Flipper's editor is open (no trade starts). If you close the editor before answering without a change, YES trades exactly the Pokemon the GBA showed. So an edit is never lost to a trade, and the GBA never gets a Pokemon you are still changing. When you are done, go back to the trade screen and sit again: step off the seat and back on (Up, then Down). The trade menu opens with the new Pokemon. If you sit while the Flipper is still editing, the Flipper joins as soon as you are back on the trade screen (or the Gen III menu). During a trade, from YES until the GBA has saved, `Edit` is hidden, and `Edit Pokemon` and `Load .pk3` show the trade screen instead. They do the same while a Pokemon you received waits for the SD card (`Can't take the traded Pokemon: data file unreadable. Retrying.`). If the GBA ever shows a link error while you use the Flipper, walk out of the Trade Center before editing and back in afterwards. The trade screen waits at `Go to the Cable Club` meanwhile.

**Original trainer.** Once the GBA has linked, the Flipper knows the GBA player's name, TID and SID. The default Pokemon, if you haven't changed it, and every species you pick from then on take that player as their OT, so the Pokemon obeys them and counts as their own. Any other Pokemon, such as a loaded one, keeps its OT. For it, `Edit Pokemon` shows a row `OT: use <NAME>` after `OT SID`, which gives it the player's OT and keeps it shiny where it can. If it can't, a `Shiny lost` notice says so. Like any edit, this recalculates the stats, so a hurt loaded Pokemon comes back with full HP. The Flipper remembers the last GBA player until the app exits, also after `End trade`. Before any GBA has linked, the OT stays as it is: `FLIPPER`, TID `22222` for the default Pokemon. On the link, the Flipper itself is always `FLIPPER`, TID `22222`.

**Another cartridge.** To trade with another game in the same session, walk out of the Trade Center on the GBA (don't use `End trade`), wait for `Go to the Cable Club`, then turn the GBA off and change the cartridge. The Flipper follows the game that is linked now. For a moment while the new game links, the trade screen may still show the previous game's `Can't offer it` reason.

**Saving and ending.**

- While the GBA saves after a trade, the trade screen says `The GBA is saving. Don't unplug or turn off. Back is locked.`, Back does nothing there, and `End trade` only shows `The GBA is saving`. If the link breaks during the save anyway, the trade screen says whether the GBA kept its old save (the Flipper then keeps its Pokemon too) or likely finished the trade (the Flipper takes the GBA's Pokemon; check the GBA's party), also when you chose `End trade` during that trade. If it breaks after YES but before the trade animation, the trade screen says `Link lost before the trade. On a GBA link error, restart it.` or, in two short windows, `Link lost. Nothing was traded.`: the GBA hasn't traded or saved yet. FireRed and LeafGreen show a link error if that link takes them too long; restart the GBA and enter the Trade Center again.
- `End trade` in the Gen III menu asks `End trade?`; `End` makes the GBA's player walk out of the Trade Center by itself. From the trade menu, the GBA goes back to the room first; during a trade, it waits until the save is done. Then the trade screen shows `Link ended. You can unplug the GBA.`, and `Link Test` is back in the menu. Back on the Gen III menu asks the same while a GBA is linked. With no GBA linked (the trade screen says `Go to the Cable Club`), both end the trade at once.
- If the GBA doesn't walk out after `End`, Back on the trade screen offers `Stop link?`. The GBA may then show a link error and need a restart; it saved before linking, so nothing is lost.
- If a received Pokemon couldn't be loaded because the SD card can't be read, `End trade` first offers `Retry` (put the card back in) or `End anyway`, which loses that Pokemon.

### What the Flipper won't offer or take
- It doesn't take an Egg, a Bad Egg or a Pokemon holding Mail. The trade screen says `Can't take that one: <why>. Pick another one.`, and the GBA cancels that trade.
- It doesn't offer, to any game, a Pokemon holding Mail, the ENIGMA BERRY, or an item Ruby doesn't have (from FireRed, LeafGreen or Emerald). The editor never gives a Pokemon one of these, and loading a file removes Mail, so only a loaded `.pk3` holding such an item gets here. The trade screen says `Can't offer it: it holds a bad item. Edit it first.`, and the Flipper doesn't sit until it has a Pokemon it can offer; if you are already seated, it joins you then. [`docs/gen3/fixtures/enigma-berry.pk3`](docs/gen3/fixtures/enigma-berry.pk3), a TREECKO holding the ENIGMA BERRY, shows this.
- With FireRed or LeafGreen, it also doesn't offer:
  - A Pokemon from #152 up while that game has no National Dex. FireRed and LeafGreen themselves trade away only #1–151 then, and the Flipper keeps to the same rule. The trade screen says `Can't offer it: GBA has no National Dex. Get it, or pick 1-151`: get it in that game first, or trade one from #1–151.
  - A Mew or Deoxys unless the editor shows `Fateful: Yes` (see [Editor](#editor)); FireRed and LeafGreen refuse it otherwise. The trade screen says `Can't offer it: FRLG needs Fateful: Yes on Mew/Deoxys: edit it.`

  The Flipper learns these rules from the game's link, so the reason shows once the GBA is in the Trade Center or on its way in. Here too the Flipper doesn't sit until it has a Pokemon the game can take (the GBA's "Please wait. … … B Button: Cancel" stays), and if you are already seated, it joins you then.

### National Dex option
`National Dex: Off` in the Gen III menu is the default each time the app starts. It is for Ruby and Sapphire only; FireRed and LeafGreen ignore it. Choosing it asks `Unlock National Dex?`; with `Turn on`, the item reads `National Dex: On`, and the next trade with Ruby or Sapphire unlocks that game's National Dex for good on that save. Choosing `National Dex: On` turns it off again for the following trades, but a save that has traded with it on keeps its National Dex.

### Editor
`Edit Pokemon` can change:

- Pokemon (all 386 species, each with its sprite)
- Nickname
- Level
- IV/EV preset (Max EV means 85 per stat, 510 in total)
- Nature
- Ability (only for species with two abilities)
- Shininess (for some OT IDs, some Unown nature and letter pairs can't be shiny; the editor says so)
- Gender
- Unown form
- Poke ball
- Fateful, for Mew and Deoxys only, right after `Ball`: OK switches between `Fateful: Yes` and `Fateful: No`. FireRed and LeafGreen trade a Mew or Deoxys only with `Yes`. Picking the species sets it; a loaded `.pk3`, or a Mew or Deoxys from a Ruby trade, can come without it.
- Held item
- Moves
- Pokerus
- OT name, OT TID and OT SID (and `OT: use <NAME>` once a GBA has linked for a trade, see [Trading](#trading))

The species, move and item names and the learnsets are in `files/gen3_bulk.bin`, which ships inside the `.fap` as an asset and is unpacked to `apps_assets/pokemon/` on the SD card. If the editor says `Data missing`, delete the `apps_assets/pokemon` folder on the SD card and reopen the app.

### .pk3 files
Files are PKHeX `.pk3` files in `apps_data/pokemon/gen3/` on the SD card. `Save .pk3` writes 80 bytes; `Load .pk3` reads 80 or 100 bytes. A loaded file is checked and tidied so the Pokemon is valid in game (stats recalculated, duplicate moves and move gaps removed, a held Mail item and any mail index removed), and the result says what was fixed.

### Cartridge (save access)
Experimental. `Cartridge` reads the save of the cartridge in the GBA over the link cable, backs it up to the SD card, and lets you browse and edit the Pokemon in its party and its 14 boxes, put new Pokemon into empty slots, and write the save back. It can also restore a backup and copy the ROM. Writing is allowed only for English Ruby, Sapphire, FireRed and LeafGreen. Tested on hardware with Ruby and FireRed over a Game Boy Color link cable (connect, backup, browse, edit, write); restore and ROM copy not yet: [`docs/gen3/hardware-test-3.md`](docs/gen3/hardware-test-3.md) is the test.

**A second app.** Cartridge is its own app, Pokemon Cartridge (`pokemon_cart.fap`), installed next to `pokemon.fap` (see [Installation Directions](#installation-directions)): the Flipper loads an app into its RAM whole, and the cartridge code doesn't fit beside the trade code. `Cartridge` is in the Gen III menu while no trade runs. Choosing it closes Pokemon Trade Tool and opens Pokemon Cartridge, which you can also open from the app list; leaving it returns to the app list. Closing Pokemon Trade Tool loses the Pokemon in its editor, so `Save .pk3` it first if you want to keep it. If `pokemon_cart.fap` isn't next to `pokemon.fap`, an `App missing` dialog says where to copy it. The pins are set in Pokemon Trade Tool's [Pinout](#pinout) screen, and Cartridge uses the same ones.

**Connecting.** With the GBA off and the cartridge in it, put the cable's small plug into the Flipper and the other into the GBA, and press `Start`. With a Game Boy Color link cable (for a module whose SD line isn't connected) either end goes in the Flipper; Cartridge uses the GBA's Normal serial mode over it, and the log's `wire` line says which link it used. Hold START+SELECT on the GBA and switch it on, holding them until the Nintendo logo is gone. The Flipper sends the GBA a small helper program over the cable (multiboot; the GBA's screen turns green), reads the cartridge, and backs up its save (about 70 s). Then the hub lists what you can do, headed by the game's name. `Cart info` shows the game code and revision, the save chip, the half of the chip the game loads, the trainer, whether writing is on (or why not), and the backup.

**Backups.** Backups and `.pk3` files are in `apps_data/pokemon/gen3/` on the SD card, the backups in its `backups/` folder, named `<game code>_<TID>_<date>-<time>.sav`. Each is the whole 128 KB save chip, raw, as emulators and PKHeX load it, and is checked three ways before it counts: each piece's checksum on the link, the helper's own re-read of the chip, and a read-back from the SD card. Cartridge backs up at the first connect after it opens, again when a connect finds the save changed, and after every write and every restore. Nothing deletes a backup once it is checked, not even when an SD card error stops Cartridge just after it; delete the ones you no longer need yourself.

**Editing.** `Party and boxes` lists the party and the 14 boxes. Open a Pokemon in the Gen III [editor](#editor), or in an empty slot choose `New Pokemon` (which takes the save's trainer as OT) or `Load .pk3`, then `Put` it there. Changed slots show `*`, and the hub shows `Write changes (<n>)` and `Discard changes`. The edits are kept on the SD card until you write them; closing Cartridge or saving in the game drops them. Eggs and Bad Eggs are left as they are. A Pokemon holding Mail can't be opened: take the Mail off in the game first. A Pokemon holding the ENIGMA BERRY, or an item Ruby doesn't have, can't be opened or put. The game's Pokedex isn't updated for the Pokemon you add.

**Writing.** The game keeps two copies of its save on the chip and loads the newer valid one. `Write changes` writes your edits into the copy the game isn't using, with a save counter one higher, and checks every piece written; the copy the game was loading is kept until the end, as the fallback. Every write ends with a new backup of the cart as it now is, checked against the chip. Then switch the GBA off and on to play; the hub offers `Reconnect` from then on, and hides `Restore backup` and `Copy ROM` until you reconnect or connect again. Nothing is written without a checked backup of the save as it was, and the chip's last four sectors (the Hall of Fame and the e-Reader or Trainer Tower data) are never written.

**A write cut short.** It is designed so that if the cable comes out or the GBA loses power during a write, the game loads either the old save or the whole new one. Until the write is finished, Pokemon can't be opened or put (`Finish the write first`). After a pulled cable, plug it back in and press `Retry`, which reconnects; after a power loss, `Retry` ends in `Reconnect failed`, and a second `Retry` shows the instructions: connect again from there. Either way the reconnect or the connect looks at the cart first. If the write got past its last step, it finishes it (`Written`) and makes its backup; otherwise the edits stay, and `Write changes` writes them again. Don't save in the game before you reconnect: the connect then keeps the game's save and drops the edits (`Edits dropped`).

**Which cartridges.** The Flipper and the helper each check the cartridge before any write: the game code (`AXVE`, `AXPE`, `BPRE` or `BPGE`), the save chip (a 128 KB Macronix or Sanyo flash, ID `09C2` or `1362`) and the ROM's save library. The helper refuses a write on its own if any check fails.

- Genuine English Ruby, Sapphire, FireRed and LeafGreen: writing allowed.
- Emerald, and Ruby, Sapphire, FireRed and LeafGreen in other Western languages: back up, browse and copy the ROM, no writing (`Writing off: Emerald: read only` or `English carts only`).
- Japanese cartridges: back up and copy the ROM only.
- Reproduction cartridges (an SRAM save, an unknown save chip, a ROM patched for another save type) are never written, and an SRAM save or a chip that isn't a 128 KB flash isn't read either. `Cart info` says why.
- A save last written by another tool, or an empty or corrupt one, shows `Writing off: Save not recognised`. Save once in the game, then connect again.

**Restore.** `Restore backup` writes a backup of this game, picked from `backups/`, onto the cart in the same way as a write, and shows both trainers first. It keeps the cart's Hall of Fame and e-Reader or Trainer Tower data. It also works on an empty or corrupt save, but not on a save another tool wrote last, which the game still loads: save once in the game first. Edits not yet written are lost, and the question says so.

**Copy ROM.** `Copy ROM` copies the cartridge's ROM to `apps_data/pokemon/gen3/roms/<game code>_v<revision>.gba`, checking each piece against the helper and the SD card. A 16 MB game takes about 2.5 hours; keep both powered. Back offers to stop, and `Copy ROM` later resumes where it stopped.

**Log.** Cartridge logs to `apps_data/pokemon/gen3_cart.log`, which it creates anew at the first `Start` after it opens. After a problem, copy it off the SD card before you open Cartridge again.

**The helper.** The program the Flipper sends to the GBA is built from `gba/helper/` with the ufbt toolchain (`make -C gba/helper install`), which writes `files/gen3_helper.mb`; both apps ship that file. Never edit it by hand. `make -C gba/helper check` rebuilds it and checks that the result is byte for byte the committed file. Pokemon Cartridge always sends its own built-in copy of that file, and checks only the helper's protocol version (`HP_PROTO_VERSION`, in the file's header and in the helper's first answer). `HP_BUILD_ID` is for logs and support only: the helper reports it, `gen3_cart.log` records it (`hello proto 1 build 1`), and nothing checks it. Releasing: if `files/gen3_helper.mb` differs from the one in the last release, bump `HP_BUILD_ID` in `gba/helper/include/hp_version.h`, so that logs tell the two apart (the tests in `tests/test_hp_link.c` that pin build 1 change with it), then run `make -C gba/helper install`. Development builds keep build 1, which version 3.1 ships.

### Link Test, log and checklists
`Link Test` checks the link without trading: the Flipper acts as the other player in the game's Cable Club Trade Center, and shows the cable, the link's state, and the GBA player's name and TID. Plug in the cable as in [Cable](#cable).

The link is logged to `apps_data/pokemon/gen3_link.log` on the SD card. The log is overwritten each time a trade or Link Test link starts, so after a problem copy it off the SD card before you start another one.

The hardware checklists:

- [Link Test with Ruby](docs/gen3/hardware-test-1.md)
- [Editor and `.pk3` files](docs/gen3/editor-test.md)
- [Trading with Ruby](docs/gen3/hardware-test-2.md)
- [FireRed: Link Test, trading, editor, `.pk3` files and a cartridge swap](docs/gen3/hardware-test-frlg.md)
- [Cartridge: connect, backup, browse, edit and write, restore, ROM copy, with Ruby and FireRed](docs/gen3/hardware-test-3.md)

## How Does it Work?

The method used to communicate 2 Game Boys is based on the SPI protocol, which is a very simple serial communication protocol in which a master device communicates with one or more slave devices. The protocol is bidirectional and synchronous, and uses three basic signals:

- A clock signal (CLK).
- An output signal (Serial Out or SO).
- An input signal (Serial In or SI).

In the Game Boy, games store data in an internal shift register that is used to send and receive information. The SPI protocol used by the Game Boy uses the clock signal to indicate when data is being transferred.

The Game Boy link protocol is synchronous and requires the slave device to respond at the same rate as the master device. The master device supplies an 8KHz clock (data transfer rate of 1KB/s). The time window for responding is only **~120μs**. However, the slave device has no restrictions and can respond when it receives data. The clock can vary and there is no lower limit.

<p align='center'>
<br />
<img src="./docs/images/gb_spi.png" width="400" /><br />
</p>

_An example GB SPI transfer. Here, the master sends 0xD9 (217) and the slave sends 0x45 (69)._

<br />

You can learn more about it in the following video. [**Analyzing the Different Versions of the Link Cable**](https://youtu.be/h1KKkCfzOws?t=151).

## Board for Flipper Zero with PortData EXT Link.

For the Flipper Zero board, a [PortData EXT Link](https://s.click.aliexpress.com/e/_Dm3EqlR) and a 2x8  [prototype board](https://s.click.aliexpress.com/e/_DETrjpL) were used.

<p align='center'>
<br />
<img src="./docs/images/EXT-Link.png" width="400" /><br />
</p>

_PortData EXT Link for Game Boy Color, Game Boy Pocket, GBC, GBP, GBL._

<p align='center'>
<br />
<img src="./docs/images/pcb.png" width="400" /><br />
</p>
<p align='center'>
<br />
<img src="./docs/images/flipper-zero-pcb.png" width="400" /><br />
</p>
I used 33kΩ resistor on CLK, but it's optional, it can be connected directly.


## Connection: Flipper Zero GPIO - Game Boy

The pins should be connected as follows:

<p align='center'>
<br />
<img src="./docs/images/wgbl-0.png" width="400" /><br />
</p>

<picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/images/GPIO-GBPIN_light-v2.png">
    <source media="(prefers-color-scheme: light)" srcset="./docs/images/GPIO-GBPIN-v2.png">
    <img
        alt="Connect Flipper Zero GPIO to Game Boy Pins"
        src="./docs/images/GPIO-GBPIN-v2.png">
</picture>

| Cable Game Link (Socket) | Flipper Zero GPIO |
| ------------------------ | ----------------- |
| 6 (GND)                  | 8 (GND)           |
| 5 (CLK)                  | 6 (B2)            |
| 3 (SI)                   | 7 (C3)            |
| 2 (SO)                   | 5 (B3)            |

## Connect to Flipper Zero without Socket PortData EXT Link

It's possible to cut a cable directly without using a socket, but it's important to note that the cable is a crossover cable SI-SO.

<p align='center'>
<br />
<img src="./docs/images/cut-cable-v3.png" width="400" /><br />
</p>

*"Cable Game Link" cut and directly connected to the Flipper Zero pins.*

**NOTE**: Don't rely on the cable colors as they may vary depending on the manufacturer. Use a multimeter to measure continuity and identify which cable corresponds to each pin.



## GUI

To generate the graphical interface, the [**FUI-Editor**](https://ilin.pt/stuff/fui-editor/) tool was used.

The sprites for each Pokemon were hand-made for the monochrome screen of the Flipper. The source models are based on sprites from a selection of all Gen I and Gen II games; the ones used we're our favorites. Huge thanks to [ProteanReverie](https://github.com/ProteanReverie) for all of the help and support!

The Gen III sprites (#252-386) are converted from Pokemon Ruby's front sprites by `tools/gen3_sprites.py`; see `sprites/README.md`.

##  Tested In
The Gen I/II trade, with a Game Boy or Game Boy Color game running on:
- Game Boy Color (GBC)
- Game Boy Advance (GBA)
- Game Boy Advance SP (GBA SP)
- Analogue Pocket

The Gen III features (the Game Boy Advance games) are not yet tested on hardware; see [Games](#games).

## Contributors
[![Contributors](https://contrib.rocks/image?repo=KBEmbedded/Flipper-Zero-Game-Boy-Pokemon-Trading)](https://github.com/KBEmbedded/Flipper-Zero-Game-Boy-Pokemon-Trading/graphs/contributors)


## Links

- [Flipper Zero firmware source code](https://github.com/flipperdevices/flipperzero-firmware)
- Adan Scotney's Pokemon [trade protocol specification](http://www.adanscotney.com/2014/01/spoofing-pokemon-trades-with-stellaris.html) and implementation
- Derek Jamison - [Youtube Channel](https://www.youtube.com/@MrDerekJamison)
- Matt Penny - [GBPlay Blog](https://blog.gbplay.io/)
- [Pokémon data structure (Generation I)](<https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_data_structure_(Generation_I)>)
- [Disassembly of Pokemon Yellow](https://github.com/pret/pokeyellow)
- [Arduino-Spoofing-Gameboy-Pokemon-Trades](https://github.com/EstebanFuentealba/Arduino-Spoofing-Gameboy-Pokemon-Trades)
- [🎮 Gameboy link cable breakout PCB](https://github.com/Palmr/gb-link-cable)

<p align='center'>
<br />
<br />
From Talcahuano 🇨🇱 with ❤ 
</p>
