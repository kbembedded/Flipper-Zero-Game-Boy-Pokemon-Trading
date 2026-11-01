#include "fake_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fake_store_misuse(const char* what, const char* name) {
    fprintf(stderr, "fake_store: %s (%s)\n", what, name ? name : "");
    abort();
}

static void fake_store_free_files(FakeStore* s) {
    for(uint32_t i = 0; i < FAKE_STORE_FILES; i++) free(s->files[i].data);
}

void fake_store_init(FakeStore* s, uint64_t capacity) {
    fake_store_free_files(s);
    memset(s, 0, sizeof(*s));
    s->capacity = capacity;
    for(uint32_t h = 0; h < FAKE_STORE_OPEN; h++) s->handles[h].file = -1;
}

void fake_store_copy(FakeStore* dst, const FakeStore* src) {
    if(dst == src) return;
    for(uint32_t h = 0; h < FAKE_STORE_OPEN; h++) {
        if(src->handles[h].file >= 0) fake_store_misuse("copy with a file open", NULL);
    }
    fake_store_free_files(dst);
    memcpy(dst, src, sizeof(*dst));
    for(uint32_t i = 0; i < FAKE_STORE_FILES; i++) {
        FakeStoreFile* f = &dst->files[i];
        if(!src->files[i].data) continue;
        f->data = malloc(f->cap);
        if(!f->data) fake_store_misuse("out of memory", f->name);
        memcpy(f->data, src->files[i].data, f->cap);
    }
}

void fake_store_report_free(FakeStore* s, uint64_t bytes) {
    s->report = true;
    s->report_bytes = bytes;
}

void fake_store_fail_write_after(FakeStore* s, uint32_t writes) {
    s->write_limit = true;
    s->writes_left = writes;
}

void fake_store_corrupt_read(FakeStore* s, const char* name, uint32_t off) {
    s->corrupt = name != NULL;
    if(!name) return;
    if(strlen(name) >= FAKE_STORE_NAME_MAX) fake_store_misuse("name too long", name);
    strcpy(s->corrupt_name, name);
    s->corrupt_off = off;
}

void fake_store_fail_read(FakeStore* s, uint32_t after) {
    s->read_fault = true;
    s->reads_left = after;
}

void fake_store_fail_close(FakeStore* s, uint32_t after) {
    s->close_fault = true;
    s->closes_left = after;
}

void fake_store_corrupt_write(FakeStore* s, const char* name, uint32_t off) {
    if(strlen(name) >= FAKE_STORE_NAME_MAX) fake_store_misuse("name too long", name);
    s->corrupt_write = true;
    strcpy(s->corrupt_write_name, name);
    s->corrupt_write_off = off;
}

/* A one-shot fault armed by fake_store_fail_read or fake_store_fail_close: true when this call is
 * the one that fails */
static bool fake_store_fault(bool* armed, uint32_t* left) {
    if(!*armed) return false;
    if(*left) {
        (*left)--;
        return false;
    }
    *armed = false;
    return true;
}

static int32_t fake_store_find(const FakeStore* s, const char* name) {
    for(uint32_t i = 0; i < FAKE_STORE_FILES; i++) {
        if(s->files[i].used && strcmp(s->files[i].name, name) == 0) return (int32_t)i;
    }
    return -1;
}

static bool fake_store_is_open(const FakeStore* s, int32_t file) {
    for(uint32_t h = 0; h < FAKE_STORE_OPEN; h++) {
        if(s->handles[h].file == file) return true;
    }
    return false;
}

static uint64_t fake_store_used(const FakeStore* s) {
    uint64_t used = 0;
    for(uint32_t i = 0; i < FAKE_STORE_FILES; i++) {
        if(s->files[i].used) used += s->files[i].size;
    }
    return used;
}

const uint8_t* fake_store_file(FakeStore* s, const char* name, uint32_t* size) {
    const int32_t i = fake_store_find(s, name);
    if(i < 0) return NULL;
    *size = s->files[i].size;
    return s->files[i].data;
}

uint32_t fake_store_count(FakeStore* s, const char* suffix) {
    const size_t n = strlen(suffix);
    uint32_t count = 0;
    for(uint32_t i = 0; i < FAKE_STORE_FILES; i++) {
        const size_t len = strlen(s->files[i].name);
        if(s->files[i].used && len >= n && strcmp(s->files[i].name + len - n, suffix) == 0)
            count++;
    }
    return count;
}

/* ---- The Gen3Store ---- */

static FakeStoreHandle* fake_store_handle(FakeStore* s, void* f) {
    FakeStoreHandle* h = f;
    if(h < s->handles || h >= s->handles + FAKE_STORE_OPEN || h->file < 0) {
        fake_store_misuse("not an open file", NULL);
    }
    return h;
}

static void* fake_store_open(void* ctx, const char* name, Gen3StoreMode mode) {
    FakeStore* s = ctx;
    if(!name || strlen(name) == 0 || strlen(name) >= FAKE_STORE_NAME_MAX) {
        fake_store_misuse("bad name", name);
    }
    int32_t i = fake_store_find(s, name);
    if(i >= 0 && fake_store_is_open(s, i)) fake_store_misuse("opened twice", name);
    FakeStoreHandle* h = NULL;
    for(uint32_t k = 0; k < FAKE_STORE_OPEN && !h; k++) {
        if(s->handles[k].file < 0) h = &s->handles[k];
    }
    if(!h) fake_store_misuse("more files open than the jobs may hold", name);
    if(mode != Gen3StoreCreate) {
        if(i < 0) return NULL;
    } else if(i < 0) {
        for(uint32_t k = 0; k < FAKE_STORE_FILES && i < 0; k++) {
            if(!s->files[k].used) i = (int32_t)k;
        }
        if(i < 0) fake_store_misuse("no room for another file", name);
        FakeStoreFile* f = &s->files[i];
        free(f->data);
        memset(f, 0, sizeof(*f));
        f->used = true;
        strcpy(f->name, name);
    } else {
        s->files[i].size = 0;
    }
    h->file = i;
    h->can_write = mode != Gen3StoreRead;
    return h;
}

static bool fake_store_read(void* ctx, void* f, uint32_t off, void* buf, uint32_t len) {
    FakeStore* s = ctx;
    const FakeStoreHandle* h = fake_store_handle(s, f);
    const FakeStoreFile* file = &s->files[h->file];
    if(off > file->size || len > file->size - off) return false;
    if(fake_store_fault(&s->read_fault, &s->reads_left)) return false;
    if(len) memcpy(buf, file->data + off, len);
    if(s->corrupt && strcmp(s->corrupt_name, file->name) == 0 && s->corrupt_off >= off &&
       s->corrupt_off - off < len) {
        ((uint8_t*)buf)[s->corrupt_off - off] ^= 0x01;
    }
    return true;
}

static bool fake_store_write(void* ctx, void* f, uint32_t off, const void* buf, uint32_t len) {
    FakeStore* s = ctx;
    const FakeStoreHandle* h = fake_store_handle(s, f);
    if(!h->can_write) fake_store_misuse("write to a file opened to read", s->files[h->file].name);
    FakeStoreFile* file = &s->files[h->file];
    if(off > file->size) fake_store_misuse("write past the end", file->name);
    if(s->write_limit) {
        if(s->writes_left == 0) return false;
        s->writes_left--;
    }
    const uint64_t end = (uint64_t)off + len;
    const uint64_t grow = end > file->size ? end - file->size : 0;
    if(fake_store_used(s) + grow > s->capacity) return false;
    if(end > file->cap) {
        uint32_t cap = file->cap ? file->cap : 4096u;
        while(cap < end) cap *= 2u;
        uint8_t* data = realloc(file->data, cap);
        if(!data) fake_store_misuse("out of memory", file->name);
        file->data = data;
        file->cap = cap;
    }
    if(len) memcpy(file->data + off, buf, len);
    if(s->corrupt_write && strcmp(s->corrupt_write_name, file->name) == 0 &&
       s->corrupt_write_off >= off && s->corrupt_write_off - off < len) {
        s->corrupt_write = false;
        file->data[s->corrupt_write_off] ^= 0x01;
    }
    if(end > file->size) file->size = (uint32_t)end;
    s->writes++;
    return true;
}

static bool fake_store_truncate(void* ctx, void* f, uint32_t len) {
    FakeStore* s = ctx;
    const FakeStoreHandle* h = fake_store_handle(s, f);
    if(!h->can_write)
        fake_store_misuse("truncate of a file opened to read", s->files[h->file].name);
    FakeStoreFile* file = &s->files[h->file];
    if(len > file->size) return false;
    file->size = len;
    return true;
}

static bool fake_store_close(void* ctx, void* f) {
    FakeStore* s = ctx;
    FakeStoreHandle* h = fake_store_handle(s, f);
    h->file = -1;
    return !fake_store_fault(&s->close_fault, &s->closes_left);
}

static bool fake_store_remove(void* ctx, const char* name) {
    FakeStore* s = ctx;
    const int32_t i = fake_store_find(s, name);
    if(i < 0) return false;
    if(fake_store_is_open(s, i)) fake_store_misuse("remove of an open file", name);
    free(s->files[i].data);
    memset(&s->files[i], 0, sizeof(s->files[i]));
    return true;
}

static bool fake_store_rename(void* ctx, const char* from, const char* to) {
    FakeStore* s = ctx;
    const int32_t i = fake_store_find(s, from);
    if(i < 0 || fake_store_find(s, to) >= 0) return false;
    if(fake_store_is_open(s, i)) fake_store_misuse("rename of an open file", from);
    if(strlen(to) == 0 || strlen(to) >= FAKE_STORE_NAME_MAX) fake_store_misuse("bad name", to);
    strcpy(s->files[i].name, to);
    return true;
}

static bool fake_store_size(void* ctx, const char* name, uint32_t* size) {
    FakeStore* s = ctx;
    const int32_t i = fake_store_find(s, name);
    if(i < 0) return false;
    *size = s->files[i].size;
    return true;
}

static uint64_t fake_store_free_bytes(void* ctx) {
    FakeStore* s = ctx;
    if(s->report) {
        s->report = false;
        return s->report_bytes;
    }
    const uint64_t used = fake_store_used(s);
    return used < s->capacity ? s->capacity - used : 0;
}

Gen3Store fake_store_api(FakeStore* s) {
    return (Gen3Store){
        .ctx = s,
        .open = fake_store_open,
        .read = fake_store_read,
        .write = fake_store_write,
        .truncate = fake_store_truncate,
        .close = fake_store_close,
        .remove = fake_store_remove,
        .rename = fake_store_rename,
        .size = fake_store_size,
        .free_bytes = fake_store_free_bytes,
    };
}
