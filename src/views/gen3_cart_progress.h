#pragma once

#include <stdint.h>
#include <gui/view_dispatcher.h>

/* The Cartridge app's progress screen: a title, two lines, a bar and a hint at the bottom. While
 * it is shown, a 250 ms timer calls the tick callback, so the owner can refresh it. Texts are
 * copied (title up to 21 characters, the lines and the hint up to 25). */

/* Runs in the timer thread (tick) or the GUI thread (Back): do no more than post an event. */
typedef void (*Gen3CartProgressCallback)(void* ctx);

/* Allocates its own view and adds it to vd as view_id */
void* gen3_cart_progress_alloc(ViewDispatcher* vd, uint32_t view_id);
void gen3_cart_progress_free(ViewDispatcher* vd, uint32_t view_id, void* p);
/* percent -1: no bar */
void gen3_cart_progress_set(
    void* p,
    const char* title,
    const char* line1,
    const char* line2,
    int8_t percent);
/* The bottom row: "Back: stop", "Do not power off" or "" */
void gen3_cart_progress_set_hint(void* p, const char* hint);
/* The Back key: cb on a short press. While cb is set the view keeps every Back event, so the
 * scene manager never sees one; NULL gives Back to it again. */
void gen3_cart_progress_set_back(void* p, Gen3CartProgressCallback cb, void* ctx);
/* Called every 250 ms while the view is shown */
void gen3_cart_progress_set_tick(void* p, Gen3CartProgressCallback cb, void* ctx);
