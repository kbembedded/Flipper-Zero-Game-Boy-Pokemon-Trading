#pragma once

#include <stdbool.h>
#include <furi.h>
#include <storage/storage.h>
#include <src/gen3/gen3_pk3.h>

/* The pokemon FAP's APP_DATA_PATH("gen3"), as an absolute path: the pokemon_cart FAP (Cartridge)
 * keeps its backups, ROM copies and working copy here, and loads .pk3 files from here. */
#define GEN3_SHARED_DATA_DIR EXT_PATH("apps_data/pokemon/gen3")

/* Resolves APP_DATA_PATH("gen3") (storage_common_resolve_path_and_ensure_app_directory) into out, so
 * the file browser, which runs outside this app's thread, gets /ext/apps_data/pokemon/gen3; creates
 * it. false if the SD card isn't ready. */
bool gen3_pk3_dir(Storage* storage, FuriString* out);

/* FileError if it can't be read, BadSize unless 80 or 100 bytes, else gen3_pk3_import (which fills
 * info, may be NULL). *out is unchanged unless Ok. */
Gen3Check gen3_pk3_load_file(
    Storage* storage,
    const char* path,
    Gen3Pkm* out,
    Gen3ImportInfo* info);

/* 80 bytes (gen3_pk3_export), FSOM_CREATE_ALWAYS. false if it can't be written completely. */
bool gen3_pk3_save_file(Storage* storage, const char* path, const Gen3Pkm* p);
