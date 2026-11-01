#include <src/gen3/gen3_store_furi.h>

#include <string.h>

#include <furi.h>
#include <storage/storage.h>

#include <src/gen3/gen3_pk3_furi.h>

/* The most files the jobs hold open at once (gen3_cart.h) */
#define GEN3_STORE_FILES 2u
#define GEN3_STORE_ROOT GEN3_SHARED_DATA_DIR "/"
/* The folder, '/', and a store name with its terminator */
#define GEN3_STORE_PATH_MAX (sizeof(GEN3_STORE_ROOT) - 1u + GEN3_CART_NAME_MAX)

struct Gen3StoreFuri {
    Storage* storage;
    Gen3Store api;
    File* file[GEN3_STORE_FILES];
    bool open[GEN3_STORE_FILES];
    char name[GEN3_STORE_FILES][GEN3_CART_NAME_MAX]; /* the name each open file was opened by */
    char path[2][GEN3_STORE_PATH_MAX]; /* rename takes two */
};

/* The absolute path of name in path[k]; NULL for a name that can't be a store name */
static char* store_path(Gen3StoreFuri* s, uint8_t k, const char* name) {
    const size_t n = name ? strlen(name) : 0;
    if(n == 0 || n >= GEN3_CART_NAME_MAX) return NULL;
    memcpy(s->path[k], GEN3_STORE_ROOT, sizeof(GEN3_STORE_ROOT) - 1u);
    memcpy(s->path[k] + sizeof(GEN3_STORE_ROOT) - 1u, name, n + 1u);
    return s->path[k];
}

/* Makes every folder of path below apps_data, up to its last '/'. Only an app's own data folder
 * is made for it (storage_common_resolve_path_and_ensure_app_directory), and the pokemon_cart
 * FAP's is another one. */
static bool store_mkdirs(Gen3StoreFuri* s, char* path) {
    bool ok = true;
    for(char* p = path + sizeof(EXT_PATH("apps_data")); *p && ok; p++) {
        if(*p != '/') continue;
        *p = '\0';
        ok = storage_simply_mkdir(s->storage, path);
        *p = '/';
    }
    return ok;
}

/* The slot of one of this store's open files; anything else is a caller's bug */
static uint8_t store_slot(const Gen3StoreFuri* s, const void* f) {
    uint8_t k = 0;
    while(k < GEN3_STORE_FILES && !(s->open[k] && s->file[k] == f)) k++;
    furi_check(k < GEN3_STORE_FILES);
    return k;
}

static void* store_open(void* ctx, const char* name, Gen3StoreMode mode) {
    Gen3StoreFuri* s = ctx;
    char* path = store_path(s, 0, name);
    if(!path) return NULL;
    /* Never one file twice: the card would make the second open wait for the first to close
     * (FSE_ALREADY_OPEN), and the worker would hang */
    for(uint8_t i = 0; i < GEN3_STORE_FILES; i++) {
        if(s->open[i] && strcmp(s->name[i], name) == 0) return NULL;
    }
    uint8_t k = 0;
    while(k < GEN3_STORE_FILES && s->open[k]) k++;
    if(k == GEN3_STORE_FILES) return NULL;

    FS_AccessMode access = FSAM_READ_WRITE;
    FS_OpenMode how = FSOM_OPEN_EXISTING;
    if(mode == Gen3StoreRead) {
        access = FSAM_READ;
    } else if(mode == Gen3StoreCreate) {
        how = FSOM_CREATE_ALWAYS;
        if(!store_mkdirs(s, path)) return NULL;
    }
    if(!storage_file_open(s->file[k], path, access, how)) {
        storage_file_close(s->file[k]); /* required even after a failed open */
        return NULL;
    }
    s->open[k] = true;
    strcpy(s->name[k], name); /* store_path checked its length */
    return s->file[k];
}

static bool store_read(void* ctx, void* f, uint32_t off, void* buf, uint32_t len) {
    Gen3StoreFuri* s = ctx;
    File* file = s->file[store_slot(s, f)];
    const uint64_t size = storage_file_size(file);
    if(off > size || len > size - off) return false;
    return storage_file_seek(file, off, true) && storage_file_read(file, buf, len) == len;
}

static bool store_write(void* ctx, void* f, uint32_t off, const void* buf, uint32_t len) {
    Gen3StoreFuri* s = ctx;
    File* file = s->file[store_slot(s, f)];
    /* Never past the end: seeking there would grow the file with whatever the card holds */
    if(off > storage_file_size(file)) return false;
    return storage_file_seek(file, off, true) && storage_file_write(file, buf, len) == len;
}

static bool store_truncate(void* ctx, void* f, uint32_t len) {
    Gen3StoreFuri* s = ctx;
    File* file = s->file[store_slot(s, f)];
    if(len > storage_file_size(file)) return false;
    return storage_file_seek(file, len, true) && storage_file_truncate(file);
}

static bool store_close(void* ctx, void* f) {
    Gen3StoreFuri* s = ctx;
    const uint8_t k = store_slot(s, f);
    /* false when the flush failed; the file is closed either way */
    const bool ok = storage_file_close(s->file[k]);
    s->open[k] = false;
    return ok;
}

static bool store_remove(void* ctx, const char* name) {
    Gen3StoreFuri* s = ctx;
    const char* path = store_path(s, 0, name);
    return path && storage_common_remove(s->storage, path) == FSE_OK;
}

static bool store_rename(void* ctx, const char* from, const char* to) {
    Gen3StoreFuri* s = ctx;
    const char* a = store_path(s, 0, from);
    const char* b = store_path(s, 1, to);
    FileInfo info;
    if(!a || !b) return false;
    /* storage_common_rename replaces an existing destination, and a backup is never replaced:
     * go on only once the card says there is none */
    if(storage_common_stat(s->storage, b, &info) != FSE_NOT_EXIST) return false;
    return storage_common_rename(s->storage, a, b) == FSE_OK;
}

static bool store_size(void* ctx, const char* name, uint32_t* size) {
    Gen3StoreFuri* s = ctx;
    const char* path = store_path(s, 0, name);
    FileInfo info;
    if(!path || storage_common_stat(s->storage, path, &info) != FSE_OK) return false;
    if((info.flags & FSF_DIRECTORY) || info.size > UINT32_MAX) return false;
    *size = (uint32_t)info.size;
    return true;
}

static uint64_t store_free_bytes(void* ctx) {
    Gen3StoreFuri* s = ctx;
    uint64_t total = 0;
    uint64_t free_space = 0;
    if(storage_common_fs_info(s->storage, STORAGE_EXT_PATH_PREFIX, &total, &free_space) !=
       FSE_OK) {
        return 0;
    }
    return free_space;
}

Gen3StoreFuri* gen3_store_furi_alloc(void) {
    Gen3StoreFuri* s = malloc(sizeof(Gen3StoreFuri));
    memset(s, 0, sizeof(*s));
    s->storage = furi_record_open(RECORD_STORAGE);
    for(uint8_t k = 0; k < GEN3_STORE_FILES; k++) s->file[k] = storage_file_alloc(s->storage);
    s->api = (Gen3Store){
        .ctx = s,
        .open = store_open,
        .read = store_read,
        .write = store_write,
        .truncate = store_truncate,
        .close = store_close,
        .remove = store_remove,
        .rename = store_rename,
        .size = store_size,
        .free_bytes = store_free_bytes,
    };
    /* apps_data/pokemon and gen3, from the root's own trailing '/'; no card, no folders */
    memcpy(s->path[0], GEN3_STORE_ROOT, sizeof(GEN3_STORE_ROOT));
    store_mkdirs(s, s->path[0]);
    return s;
}

void gen3_store_furi_free(Gen3StoreFuri* s) {
    furi_check(s);
    for(uint8_t k = 0; k < GEN3_STORE_FILES; k++) {
        storage_file_free(s->file[k]); /* closes it if open */
    }
    furi_record_close(RECORD_STORAGE);
    free(s);
}

const Gen3Store* gen3_store_furi_api(Gen3StoreFuri* s) {
    furi_check(s);
    return &s->api;
}
