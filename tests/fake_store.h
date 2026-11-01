#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>

/* Test-only: a Gen3Store of files in RAM, as strict as an SD card behind FatFs: a write that
 * doesn't fit in the capacity writes nothing and fails; Read and Update need the file; at most 2
 * files are open at once, never one twice, and an open file can't be removed or renamed (a misuse
 * aborts the test). Names are flat strings: a folder exists as soon as a name uses it. It uses
 * Gen3Store only as a type.
 *
 * Its data is on the heap: a FakeStore is static (zero) before its first fake_store_init, and each
 * init frees what the last one held. */

#define FAKE_STORE_FILES 16u
#define FAKE_STORE_NAME_MAX 64u
#define FAKE_STORE_OPEN 2u

typedef struct {
    bool used;
    char name[FAKE_STORE_NAME_MAX];
    uint8_t* data;
    uint32_t size;
    uint32_t cap; /* bytes allocated */
} FakeStoreFile;

typedef struct {
    int32_t file; /* index in files, or -1: not open */
    bool can_write;
} FakeStoreHandle;

typedef struct FakeStore FakeStore;

struct FakeStore {
    uint64_t capacity;
    bool report; /* the next free_bytes() answers report_bytes */
    uint64_t report_bytes;
    bool write_limit;
    uint32_t writes_left;
    bool corrupt;
    char corrupt_name[FAKE_STORE_NAME_MAX];
    uint32_t corrupt_off;
    bool read_fault;
    uint32_t reads_left; /* reads that succeed before the one that fails */
    bool close_fault;
    uint32_t closes_left; /* closes that succeed before the one that fails */
    bool corrupt_write;
    char corrupt_write_name[FAKE_STORE_NAME_MAX];
    uint32_t corrupt_write_off;
    uint32_t writes; /* write() calls that succeeded */
    FakeStoreFile files[FAKE_STORE_FILES];
    FakeStoreHandle handles[FAKE_STORE_OPEN];
};

/* No files; capacity bytes across all files; no fault. */
void fake_store_init(FakeStore* s, uint64_t capacity);
/* dst becomes a copy of src, files, capacity and armed faults included, with its own data; what
 * dst held before is freed. No file of src may be open. */
void fake_store_copy(FakeStore* dst, const FakeStore* src);
/* One-shot: the next free_bytes() returns bytes instead of the truth; later ones the truth,
 * capacity minus the bytes the files hold. */
void fake_store_report_free(FakeStore* s, uint64_t bytes);
Gen3Store fake_store_api(FakeStore* s);
/* The next `writes` write() calls succeed (as far as the capacity allows), and every one after
 * them fails, writing nothing. */
void fake_store_fail_write_after(FakeStore* s, uint32_t writes);
/* Every read of that byte of that file returns it XORed with 0x01; the file stays as it is. One at
 * a time; a NULL name ends it. */
void fake_store_corrupt_read(FakeStore* s, const char* name, uint32_t off);
/* Once: the next `after` read() calls succeed, and the one after them fails, reading nothing. */
void fake_store_fail_read(FakeStore* s, uint32_t after);
/* Once: the next `after` close() calls succeed, and the one after them fails, as a failed flush
 * does; that file is closed all the same. */
void fake_store_fail_close(FakeStore* s, uint32_t after);
/* Once: the next write to that file that covers byte off stores it XORed with 0x01, and succeeds,
 * as a card that wrote it wrong would. */
void fake_store_corrupt_write(FakeStore* s, const char* name, uint32_t off);
/* The file's bytes and *size; NULL if absent */
const uint8_t* fake_store_file(FakeStore* s, const char* name, uint32_t* size);
uint32_t fake_store_count(FakeStore* s, const char* suffix); /* files ending with suffix */
