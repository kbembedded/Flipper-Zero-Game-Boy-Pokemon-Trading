#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A list of ids sorted by name, for the first-letter pickers of moves and held items. The ids and
 * names may come from the bulk file, so either read can fail. */
#define GEN3_LETTERS_NAME_SIZE 16 /* any move or item name + NUL (13 or 15) */
typedef struct {
    size_t count;
    uint16_t (*id_at)(size_t i); /* the id at sorted position i < count; 0: unreadable */
    bool (*name)(uint16_t id, char* out); /* copies the name + NUL; false: unreadable */
} Gen3NameList;

/* The distinct first characters of the names, in list order, at most max. Returns how many, or 0
 * if any id or name it needs can't be read (fail closed). */
size_t gen3_letters(const Gen3NameList* list, char* out, size_t max);
/* The ids whose name starts with letter, in list order, at most max. Returns how many, or 0 if any
 * id or name it needs can't be read (fail closed). */
size_t gen3_letter_ids(const Gen3NameList* list, char letter, uint16_t* out, size_t max);

/* Room that covers every letter list and every letter's ids for moves and holdable items
 * (asserted in tests/test_gen3_letters.c). */
#define GEN3_LETTERS_MAX    40
#define GEN3_LETTER_IDS_MAX 96
