#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <src/gen3/gen3_data.h>

/* Host-only Gen3BulkRead source over the committed asset. The tests run from tests/. */

#define BULK_FILE_PATH "../files/gen3_bulk.bin"

static FILE* bulk_file_fp = NULL;

/* Set after a good open to make every read fail from then on, as an SD error would.
 * bulk_file_open clears it. */
static bool bulk_file_failing = false;

static inline bool bulk_file_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    FILE* f = ctx;
    return !bulk_file_failing && fseek(f, (long)offset, SEEK_SET) == 0 &&
           fread(buf, 1, len, f) == len;
}

/* Opens the file once and installs it as the bulk source, with reads working. Call from main()
 * before the tests that need species names or learnsets; a test may call it again to reinstall
 * it. */
static inline bool bulk_file_open(void) {
    bulk_file_failing = false;
    if(!bulk_file_fp) bulk_file_fp = fopen(BULK_FILE_PATH, "rb");
    if(bulk_file_fp && gen3_bulk_open(bulk_file_read, bulk_file_fp)) return true;
    printf("cannot open %s as the Gen III bulk source\n", BULK_FILE_PATH);
    return false;
}
