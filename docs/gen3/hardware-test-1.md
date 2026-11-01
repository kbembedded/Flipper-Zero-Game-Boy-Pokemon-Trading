# Gen III hardware test 1: Link Test

Goal: the Flipper acts as the other player in Pokemon Ruby's Cable Club Trade Center, stays linked, and leaves cleanly. No Pokemon is traded in this test.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA, GBA link cable, genuine Ruby. FireRed has its own checklist, `docs/gen3/hardware-test-frlg.md`.

The Game Link module's SD line must be connected (KBEmbedded module: the two SD jumper pads bridged). Without it Ruby never answers, and the log shows `R:FFFF` with a timeout on every frame.

## Steps

1. **Build and install.** Run `ufbt` (release build). Copy `dist/pokemon.fap` to `apps/GPIO/` on the SD card with qFlipper, or run `ufbt launch`.
2. **Ruby prerequisites.** Pokedex received, at least 2 Pokemon in the party, no Enigma Berry held. Ruby saves at the counter before linking, so a power-cycle cannot lose progress.
3. **Pinout check.** Main menu, Select EXT Pinout, Original: it shows `SD: PA4`.
4. **Cable orientation.** Plug the cable into the Flipper's module only; leave the other end free. On the Flipper open `Gen III  (GBA non-JPN)` then `Link Test` and look at the cable line. If it shows `Cable: plug in/swap ends`, put the cable's other plug into the Flipper instead (a missing and a reversed cable look the same). It must read `Cable: OK`, and `Frm` stays 0 until then. Only then plug the other end into the GBA. Write down which plug (colour) went into the Flipper. The cable line stays live for the whole test: if it ever shows `Cable: plug in/swap ends`, the Flipper pauses its transfers until it reads `Cable: OK` again.
5. **Ruby.** Pokemon Center 2F, middle counter (Trade Center; the right one is the Record Corner), answer YES, let it save, then "Please wait".
6. **Expected on the Flipper** (the `Link:` line): `linked`, then `players exchanged` (partner name and `TID` shown), then `trainer cards`, then `in Trade Center`. `linked` and `players exchanged` may flash by too quickly to see; what matters is reaching `in Trade Center`. **Expected on Ruby:** "Please enter.", then you are in the room with a second avatar.
7. **Walk around for 60 s.** Talk to the other avatar; it shows the trainer card `FLIPPER` (TID 22222). `Err` stays 0. Stepping onto a trade seat shows "Please wait..." forever in this test, because the Flipper never sits down; press B to stand up.
8. **Leave.** Walk to the exit, face south, choose YES. Ruby returns to the Pokemon Center with no error, and the Flipper shows `waiting for GBA`.
9. **Repeat steps 5 to 8 once** without leaving the Link Test screen.
10. **Fill in the results table** below.

## Notes

- The log is overwritten each time the Link Test screen is opened. After a failed run, copy `apps_data/pokemon/gen3_link.log` off the SD card BEFORE reopening Link Test. The screen's last line is `Log: on` when logging works, `Log: off` when it does not.
- Leaving the Link Test while linked shows a `Stop link?` dialog (`Stay` / `Stop`; Back is the same as `Stay`). Its text: "Your GBA may show a link error and need a restart. It saved before linking." After `Stop`, power-cycle the GBA if it shows a link error; Ruby saved before linking, so nothing is lost.
- Screen layout: `Gen III Link Test`, `Cable: ...`, `Link: ...`, partner line (`name TID nnnnn`), `Frm n Err n T/O n`, `Log: ...`.
- Other `Link:` texts on failure: `error: bad data`, `GBA chose other room`, `error: unexpected`. `error: unexpected` shows when the Link Test meets a GBA that a trade session left in the middle of a relink (for example after `Stop link?` on the trade screen): restart the GBA and go to the Trade Center again. A checksum error does not change the `Link:` line: it counts in `Err`, and if the GBA saw it too, the GBA drops the link and the Flipper goes back to `waiting for GBA`.

## Results

| Step | Pass / fail | What each screen showed (Flipper, Ruby) |
|---|---|---|
| 1 Build and install | | |
| 2 Ruby prerequisites | | |
| 3 Pinout `SD: PA4` | | |
| 4 `Cable: OK` (plug colour in Flipper: ____) | | |
| 5 Ruby to "Please wait" | | |
| 6 Link phases, "Please enter." | | |
| 7 60 s walk, trainer card `FLIPPER`, `Err` 0 | | |
| 8 Leave, `waiting for GBA` | | |
| 9 Second linkup | | |

On any failure report: the step number, what each screen showed (Flipper and GBA, including `Frm`/`Err`/`T/O`), and the file `apps_data/pokemon/gen3_link.log` from the SD card. Finish a run by pressing Back on the Link Test (then `Stop` if it asks), and copy the log off the SD card before reopening Link Test, because reopening overwrites it.

## If it fails (for whoever reads the log)

Tunables are `#define`s at the top of `src/gen3/mlink_hw_furi.c`. See `docs/gen3/link-protocol.md` (A2 "Wire level") for background.

| Symptom in the log | Try |
|---|---|
| Received words look bit-reversed (e.g. `059D` instead of `B9A0`) | `MLINK_LSB_FIRST` 1 to 0 |
| Garbled or missing words, slow rising edges on SD | `MLINK_SD_PUSH_PULL` 1 to 0 (open drain) or back to 1 |
| Frequent `T/O` (child did not answer), unless the cable line showed `plug in/swap ends` (a frame skipped for the cable during a link adds 9 `T/O` and drops the link) | raise `MLINK_CHILD_TIMEOUT_US` |
| Link drops after a fixed time, or GBA error | check `MLINK_TICK_DECI_US` / `MLINK_FRAME_DECI_US` (burst timing) and the 751.5 us end-to-start gap. `MLINK_TICK_DECI_US` must change together with `MLINK_TIM_ARR` (a compile-time check fails otherwise) |
| `Frm` stays 0 | cable (step 4), pin config (step 3), `MLINK_CABLE_SETTLE_US` |
