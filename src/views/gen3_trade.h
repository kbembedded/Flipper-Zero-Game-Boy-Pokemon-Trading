#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <gui/view_dispatcher.h>
#include <src/gen3/gen3_trade_sync.h>

/* Gen III trade screen: what gen3_trade_view_update was last given. No timer of its own: the app
 * tick (gen3_trade_tick) refreshes it. */

typedef struct {
    char guide[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    char flipper_line[24]; /* "Flipper: <NICKNAME>" */
    uint32_t errors; /* status.link.checksum_errors */
    bool can_edit; /* draws the center "Edit" button; OK works only then */
} Gen3TradeViewState;

/* A short OK press while can_edit. App thread (view input): may post an event, nothing more. */
typedef void (*Gen3TradeEditCallback)(void* context);

/* Allocates its own view and adds it to view_dispatcher as view_id. */
void* gen3_trade_view_alloc(ViewDispatcher* view_dispatcher, uint32_t view_id);

/* Removes the view from view_dispatcher and frees it. */
void gen3_trade_view_free(ViewDispatcher* view_dispatcher, uint32_t view_id, void* view);

void gen3_trade_view_set_edit_callback(void* view, Gen3TradeEditCallback cb, void* context);

/* Copies state; redraws only if it changed. */
void gen3_trade_view_update(void* view, const Gen3TradeViewState* state);
