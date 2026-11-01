# Sprite Assets

In this folder are the Pokemon sprite sources. They should all be 3 digits and numbered in the national Pokedex order.

001 to 251 were made by hand. 252 to 386 (Gen III) are converted from Pokemon Ruby's front sprites by `tools/gen3_sprites.py`, in the same style: 56x56, black outlines, a checkerboard for mid tones. The comment at the top of the script says how.


## Building

The final sprites on the SD card are expected to be in the "fxbm" format as used by [https://github.com/flipperdevices/flipperzero-game-engine](https://github.com/flipperdevices/flipperzero-game-engine). This is XBM format with a short header with the size, width, and height. They are all in one file, `files/all_sprites.fxbm`, one sprite after another from 001 to 386.

`tools/gen3_sprites.py` makes 252 to 386 and then rebuilds `files/all_sprites.fxbm` from all of 001 to 386, so run it after changing any sprite. It needs a clone of pret/pokeruby at the commit pinned in the script. From the root directory of the project:
```
git clone https://github.com/pret/pokeruby
git -C pokeruby checkout 5784633ce4ef7ade1a7f2d2d0c288e3d5e6cdd7f
python3 -I tools/gen3_sprites.py pokeruby
```
It writes 252 to 386 again each time, so change those in the script rather than by hand.

000 (MissingNo) is not in `all_sprites.fxbm`; the app has it built in, in `src/missingno_i.h`. To rebuild that after changing `000.png`, from the root directory of the project:
```
git clone https://github.com/flipperdevices/flipperzero-game-engine
mkdir fxbm
python3 flipperzero-game-engine/scripts/sprite_builder.py sprites fxbm
cd fxbm
xxd -i 000.fxbm > ../src/missingno_i.h
cd -
rm -r fxbm
```
