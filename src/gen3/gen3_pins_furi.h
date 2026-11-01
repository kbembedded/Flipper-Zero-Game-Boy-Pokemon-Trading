#pragma once

#include <stdbool.h>

/* File APP_DATA_PATH(".gen3_sd_pin"), FlipperFormat header "Pokemon Gen3 SD Pin" v1,
 * key "SD" (uint32 gpio_pins index). The SD pin is stored here because gblink's own pinconf
 * file only holds SI/SO/CLK. */
#define GEN3_SD_PIN_FILE_NAME ".gen3_sd_pin"

/* If the file is present and holds a usable pin, applies it with gblink_pin_set(PIN_SD) and
 * returns true. Returns false (SD unchanged) if absent or invalid. */
bool gen3_sd_pin_load(void* gblink_handle);

/* Writes gblink_pin_get(handle, PIN_SD). */
bool gen3_sd_pin_save(void* gblink_handle);
