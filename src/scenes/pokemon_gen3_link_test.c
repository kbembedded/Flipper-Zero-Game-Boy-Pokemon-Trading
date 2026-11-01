#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_link_test.h>

/* Custom event from the view's 250 ms timer. Kept clear of the submenu (scene ids) and DialogEx
 * events, since a tick still queued when this scene is left reaches the next one. */
#define GEN3_LINK_TEST_EVENT_TICK 0x47330001u

/* Scene state: whether this Link Test's session has been started. */
#define GEN3_LINK_TEST_IDLE    0
#define GEN3_LINK_TEST_STARTED 1

/* Timer thread: hand the tick to the app thread, which owns the session. */
static void gen3_link_test_tick_callback(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_LINK_TEST_EVENT_TICK);
}

/* Until the session runs, check the cable and start it as soon as the cable reads OK. Then show
 * the latest status, with the driver's live cable guard on the cable line. */
static void gen3_link_test_refresh(PokemonFap* pokemon_fap) {
    Gen3Session* session = pokemon_fap->gen3_session;
    bool started =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3LinkTest) ==
        GEN3_LINK_TEST_STARTED;
    bool cable_ok = true;
    bool log_ok;

    if(!started) {
        cable_ok = gen3_session_check_cable(session) == MlinkCableOk;
        started = cable_ok && gen3_session_start(session);
        if(started) {
            scene_manager_set_scene_state(
                pokemon_fap->scene_manager, PokemonSceneGen3LinkTest, GEN3_LINK_TEST_STARTED);
        }
    }

    const Gen3ClubStatus* status =
        gen3_menu_status(pokemon_fap, &log_ok, started ? &cable_ok : NULL);
    gen3_link_test_update(pokemon_fap->gen3_link_test, cable_ok, status, log_ok);
}

/* Leaving now would drop the GBA to a link error screen. */
static bool gen3_link_test_in_club(Gen3ClubPhase phase) {
    switch(phase) {
    case Gen3ClubPhaseLinked:
    case Gen3ClubPhasePlayers:
    case Gen3ClubPhaseCards:
    case Gen3ClubPhaseRoom:
        return true;
    default:
        return false;
    }
}

void pokemon_scene_gen3_link_test_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* Coming back with "Stay" from the "Stop link?" dialog finds the session still running. */
    if(!pokemon_fap->gen3_session) {
        pokemon_fap->gen3_link_test =
            gen3_link_test_alloc(pokemon_fap->view_dispatcher, AppViewGen3LinkTest);
        gen3_link_test_set_tick_callback(
            pokemon_fap->gen3_link_test, gen3_link_test_tick_callback, pokemon_fap);
        pokemon_fap->gen3_session = gen3_session_alloc(pokemon_fap->gblink_handle);
        scene_manager_set_scene_state(
            pokemon_fap->scene_manager, PokemonSceneGen3LinkTest, GEN3_LINK_TEST_IDLE);
    }

    gen3_link_test_refresh(pokemon_fap);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewGen3LinkTest);
}

bool pokemon_scene_gen3_link_test_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == GEN3_LINK_TEST_EVENT_TICK) {
        gen3_link_test_refresh(pokemon_fap);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        if(gen3_link_test_in_club(gen3_menu_status(pokemon_fap, NULL, NULL)->phase)) {
            scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3LinkExit);
            consumed = true;
        } else {
            /* Not consumed: the scene manager goes back to the Gen III menu, which frees it */
            gen3_session_stop(pokemon_fap->gen3_session);
        }
    }

    return consumed;
}

void pokemon_scene_gen3_link_test_on_exit(void* context) {
    /* The session is not stopped here: it keeps running behind the "Stop link?" dialog. The
     * Gen III menu frees it once the Link Test is left for good. */
    UNUSED(context);
}
