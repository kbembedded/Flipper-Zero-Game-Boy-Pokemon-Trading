#pragma once

#include <src/gen3/gen3_cart.h>

/* The cartridge jobs' Gen3Store (gen3_cart.h) on the SD card. Names are relative to
 * GEN3_SHARED_DATA_DIR (gen3_pk3_furi.h), the pokemon FAP's gen3 folder, which both FAPs use:
 * "backups/AXVE_12345_20261025-143005.sav" is <that folder>/backups/... Create makes the name's
 * folders first. free_bytes is the SD card's free space (storage_common_fs_info). rename refuses a
 * destination that exists, or that may exist: storage_common_rename would replace it.
 *
 * Not ISR code. Use it from one thread at a time: the store keeps its paths and its two files in
 * its own struct, not on the caller's stack. */

typedef struct Gen3StoreFuri Gen3StoreFuri;

/* Opens the storage record and makes apps_data/pokemon and its gen3 folder, if the SD card is
 * there; without a card each call fails. */
Gen3StoreFuri* gen3_store_furi_alloc(void);
/* Closes any file still open. */
void gen3_store_furi_free(Gen3StoreFuri* s);
/* Valid until gen3_store_furi_free. */
const Gen3Store* gen3_store_furi_api(Gen3StoreFuri* s);
