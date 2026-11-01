#include <src/gen3/gen3_letters.h>
#include <src/gen3/gen3_data.h>

_Static_assert(
    GEN3_ITEM_NAME_LEN + 1 <= GEN3_LETTERS_NAME_SIZE &&
        GEN3_MOVE_NAME_LEN + 1 <= GEN3_LETTERS_NAME_SIZE,
    "GEN3_LETTERS_NAME_SIZE too small for a move or item name");

size_t gen3_letters(const Gen3NameList* list, char* out, size_t max) {
    char name[GEN3_LETTERS_NAME_SIZE];
    size_t n = 0;

    for(size_t i = 0; i < list->count && n < max; i++) {
        uint16_t id = list->id_at(i);
        if(id == 0 || !list->name(id, name)) return 0;
        /* The list is sorted, so a letter is only ever repeated by its neighbour */
        if(n == 0 || out[n - 1] != name[0]) out[n++] = name[0];
    }
    return n;
}

size_t gen3_letter_ids(const Gen3NameList* list, char letter, uint16_t* out, size_t max) {
    char name[GEN3_LETTERS_NAME_SIZE];
    size_t n = 0;

    for(size_t i = 0; i < list->count && n < max; i++) {
        uint16_t id = list->id_at(i);
        if(id == 0 || !list->name(id, name)) return 0;
        if(name[0] == letter) out[n++] = id;
    }
    return n;
}
