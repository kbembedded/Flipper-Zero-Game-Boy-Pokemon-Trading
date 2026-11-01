#pragma once

#include <stdbool.h>

/* The Gen III bulk tables (src/gen3/gen3_data.h) from APP_ASSETS_PATH("gen3_bulk.bin"), which
 * fap_file_assets ships with the app. The file stays open until gen3_bulk_furi_close. UI thread
 * only: it reads the SD card. */

/* Opens the file and installs it with gen3_bulk_open. false if it is missing or corrupt; then
 * nothing is installed and nothing is left open. */
bool gen3_bulk_furi_open(void);

/* Uninstalls the source and closes the file. Safe to call when not open. */
void gen3_bulk_furi_close(void);
