#include <src/gen3/gen3_pins_furi.h>

#include <furi.h>
#include <storage/storage.h>
#include <lib/flipper_format/flipper_format.h>

#include <gblink/include/gblink.h>
#include <gblink/include/gblink_pinconf.h>

#define GEN3_SD_PIN_FILE_TYPE "Pokemon Gen3 SD Pin"
#define GEN3_SD_PIN_FILE_VER  1
#define GEN3_SD_PIN_KEY       "SD"
#define TAG                   "gen3_pins"

static FuriString* gen3_sd_pin_path(Storage* storage) {
    FuriString* path = furi_string_alloc_set(APP_DATA_PATH(""));
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    furi_string_cat_str(path, GEN3_SD_PIN_FILE_NAME);
    return path;
}

bool gen3_sd_pin_load(void* gblink_handle) {
    furi_assert(gblink_handle);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = gen3_sd_pin_path(storage);
    FuriString* type = furi_string_alloc();
    FlipperFormat* file = flipper_format_file_alloc(storage);
    uint32_t val = 0;
    bool ret = false;

    if(!flipper_format_file_open_existing(file, furi_string_get_cstr(path))) {
        goto out;
    }
    if(!flipper_format_read_header(file, type, &val) ||
       strcmp(furi_string_get_cstr(type), GEN3_SD_PIN_FILE_TYPE) != 0 ||
       val != GEN3_SD_PIN_FILE_VER) {
        FURI_LOG_E(TAG, "Missing or incorrect header");
        goto out;
    }
    if(!flipper_format_read_uint32(file, GEN3_SD_PIN_KEY, &val, 1)) {
        FURI_LOG_E(TAG, "Missing SD");
        goto out;
    }
    if(val >= gpio_pins_count || gblink_pin_get_next(val) != (int)val) {
        FURI_LOG_E(TAG, "Invalid SD pin %lu", val);
        goto out;
    }
    if(gblink_pin_set(gblink_handle, PIN_SD, val) != 0) {
        goto out;
    }
    ret = true;

out:
    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_string_free(type);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return ret;
}

bool gen3_sd_pin_save(void* gblink_handle) {
    furi_assert(gblink_handle);
    int pin = gblink_pin_get(gblink_handle, PIN_SD);
    if(pin < 0) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = gen3_sd_pin_path(storage);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    uint32_t val = pin;
    bool ret = false;

    if(!flipper_format_file_open_always(file, furi_string_get_cstr(path))) {
        FURI_LOG_E(TAG, "Error opening file %s", furi_string_get_cstr(path));
        goto out;
    }
    if(!flipper_format_write_header_cstr(file, GEN3_SD_PIN_FILE_TYPE, GEN3_SD_PIN_FILE_VER) ||
       !flipper_format_write_uint32(file, GEN3_SD_PIN_KEY, &val, 1)) {
        FURI_LOG_E(TAG, "Error writing SD pin");
        goto out;
    }
    ret = true;

out:
    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return ret;
}
