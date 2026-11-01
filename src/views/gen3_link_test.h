#ifndef GEN3_LINK_TEST_H
#define GEN3_LINK_TEST_H

#pragma once

#include <stdbool.h>
#include <gui/view_dispatcher.h>
#include <src/gen3/gen3_club.h>

/* Gen III Link Test screen. Shows what gen3_link_test_update was last given; while the view is
 * shown, a 250 ms timer calls the tick callback so the owner can refresh it. */

/* Runs in the timer thread: do no more than post an event. */
typedef void (*Gen3LinkTestTickCallback)(void* context);

/* Allocates its own view and adds it to view_dispatcher as view_id. */
void* gen3_link_test_alloc(ViewDispatcher* view_dispatcher, uint32_t view_id);

void gen3_link_test_free(ViewDispatcher* view_dispatcher, uint32_t view_id, void* link_test_ctx);

void gen3_link_test_set_tick_callback(
    void* link_test_ctx,
    Gen3LinkTestTickCallback callback,
    void* context);

void gen3_link_test_update(
    void* link_test_ctx,
    bool cable_ok,
    const Gen3ClubStatus* status,
    bool log_ok);

#endif /* GEN3_LINK_TEST_H */
