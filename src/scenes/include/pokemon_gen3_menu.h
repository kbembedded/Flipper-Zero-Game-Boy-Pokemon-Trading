#pragma once

#include <src/include/pokemon_app.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_trade_sync.h>

/* The Gen III scenes' shared header. The scene files have no header each, so what one of them
 * exports to the others is declared here: the menu's own helpers (pokemon_gen3_menu.c, and
 * pokemon_gen3_common.c for those the pokemon_cart app shares), and a few from other scene files,
 * each saying which file defines it. Every name is gen3_<scene>_..., after the scene it belongs
 * to, like the scene files' static helpers. The trade tick and gen3_trade_begin_edit are declared
 * in pokemon_app.h instead, with the app that calls the tick.
 *
 * The pokemon_cart app (Cartridge) registers its own hub as Gen3Menu, so there "the Gen III
 * menu" below means that hub. */

/* Copies the session's latest status (gen3_session_status; log_ok and cable_ok may be NULL) into
 * one static buffer and returns it. Gen3ClubStatus is never a stack local (no app stack local is
 * over 128 bytes). App thread only; pokemon_fap->gen3_session must exist. The next call
 * overwrites it: don't hold the pointer across another call. */
const Gen3ClubStatus* gen3_menu_status(PokemonFap* pokemon_fap, bool* log_ok, bool* cable_ok);

/* Custom event a Gen III editor scene may send itself (from a view callback) to reach
 * gen3_menu_show_data_missing from its on_event. Clear of scene ids and the other Gen III
 * events. */
#define GEN3_EVENT_DATA_MISSING 0x47330102u

/* OK on a dialog gen3_menu_show_dialog shows ("Data missing" among them). Kept clear of the menu
 * items (scene ids) and the other Gen III events. */
#define GEN3_MENU_EVENT_DATA_OK 0x47330101u

/* gen3_menu_show_dialog, gen3_menu_dialog_up, gen3_menu_show_menu, gen3_menu_show_data_missing,
 * gen3_menu_show_notice and gen3_menu_editor_ready: defined in pokemon_gen3_common.c, which both
 * FAPs compile. */

/* Shows a dialog with header, text and OK instead of the Gen III menu's submenu, and
 * gen3_menu_dialog_up is true until gen3_menu_show_menu. The menu's on_event shows the menu again
 * on GEN3_MENU_EVENT_DATA_OK and on Back while the dialog is up. The strings must outlive the
 * dialog (string literals). Call while the Gen III menu is the current scene. */
void gen3_menu_show_dialog(PokemonFap* pokemon_fap, const char* header, const char* text);

/* True while a dialog gen3_menu_show_dialog showed is up instead of the menu's submenu */
bool gen3_menu_dialog_up(void);

/* Shows the menu's submenu (AppViewSubmenu): gen3_menu_dialog_up is false again */
void gen3_menu_show_menu(PokemonFap* pokemon_fap);

/* The bulk data file (gen3_bulk.bin) is missing, corrupt or unreadable: returns to the Gen III menu
 * from any Gen III scene above it, and shows the "Data missing" dialog there, which names the
 * app's own apps_assets folder. OK or Back on the dialog shows the menu. Call from a scene's
 * on_event, not from a view callback. */
void gen3_menu_show_data_missing(PokemonFap* pokemon_fap);

/* Custom events: a Gen III editor scene sends GEN3_EVENT_NO_SHINY itself (from a view callback)
 * to reach gen3_menu_show_notice from its on_event; the notice sends GEN3_EVENT_NOTICE_OK on OK.
 * Clear of scene ids and the other Gen III events. */
#define GEN3_EVENT_NO_SHINY 0x47330103u
#define GEN3_EVENT_NOTICE_OK 0x47330104u

/* Shows a dialog with header, text and OK over the current Gen III editor scene. OK sends that
 * scene GEN3_EVENT_NOTICE_OK, and Back reaches it as a Back event; it then leaves with
 * scene_manager_previous_scene. The strings must outlive the dialog (string literals). Call from
 * a scene's on_event. */
void gen3_menu_show_notice(PokemonFap* pokemon_fap, const char* header, const char* text);

/* Header (up to 24 characters) and text (up to 96) of the Gen3Result scene, copied; set them
 * before scene_manager_next_scene(..., PokemonSceneGen3Result). Defined in pokemon_gen3_pk3.c,
 * with the scene. */
void gen3_result_set(const char* header, const char* text);

/* What loading a .pk3 changed in it, briefly: "\nFixed:" and "mail", "moves", "level" or
 * "ability" (at most 32 characters); "" if nothing. Defined in pokemon_gen3_pk3.c. */
void gen3_load_changes(const Gen3ImportInfo* info, char* out, size_t out_size);

/* The Pokemon being edited lives until the app exits. Its nickname and moves come from the bulk
 * file, so it is created only once the file reads, and Trade, Edit, Load and Save need the file:
 * false if it can't be read. A read that failed since (the SD card pulled) closed it, so it is
 * opened again here first. */
bool gen3_menu_editor_ready(PokemonFap* pokemon_fap);

/* Custom event the trade screen's OK (Edit) sends to Gen3Trade. Clear of scene ids and the other
 * Gen III events. */
#define GEN3_TRADE_EVENT_EDIT 0x47330201u

/* What ending the session takes now (D9, D10, R6), decided from a fresh status at the moment it is
 * asked. Call from Gen3Menu, Gen3TradeEnd or Gen3LinkExit, on every button that may end or stop
 * it, never from what a dialog showed when it opened; pokemon_fap->gen3_session must exist. A
 * running trade session gets one tick first, so a finished trade's Pokemon is picked up and the
 * session never stops without it; the tick may also end the session itself (then Menu). stop_link:
 * gen3_trade_stop_link_action ("Stop link?" confirmed), else gen3_trade_end_action (End trade,
 * menu Back). A running Link Test session gets Stop with stop_link (Gen3LinkExit's only use for
 * it); without stop_link its linked phases give Ask, but no caller asks that for a Link Test.
 * Defined in pokemon_gen3_trade.c. */
Gen3TradeEndAction gen3_trade_end_decide(PokemonFap* pokemon_fap, bool stop_link);

/* Opens Gen3TradeEnd over the Gen III menu (the current scene, or the one under Gen3LinkExit,
 * which must switch to it first) showing action: Saving, Ask or Pickup. stop_link: the user
 * confirmed "Stop link?", so its buttons re-decide with gen3_trade_stop_link_action. Defined in
 * pokemon_gen3_trade_end.c. */
void gen3_trade_end_open(PokemonFap* pokemon_fap, Gen3TradeEndAction action, bool stop_link);

/* Rebuilds the Gen III menu's items for the session and settings as they are now, keeping the
 * highlighted item (or the first one, if it has gone), and frees nothing. Call while Gen3Menu is
 * the current scene. The trade tick does when it ends the session there, so End trade goes and
 * Link Test comes back; the stopped session and its trade view stay until the menu is entered
 * again, or Link Test or Trade is chosen. */
void gen3_menu_rebuild(PokemonFap* pokemon_fap);
