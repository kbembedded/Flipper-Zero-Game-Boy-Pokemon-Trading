#ifndef POKEMON_CHAR_ENCODE_H
#define POKEMON_CHAR_ENCODE_H

#include <stdint.h>
#include <stddef.h>

char pokemon_char_to_encoded(int byte);
int pokemon_encoded_to_char(char byte);

void pokemon_str_to_encoded_array(uint8_t* dest, char* src, size_t n);
void pokemon_encoded_array_to_str(char* dest, uint8_t* src, size_t n);

void pokemon_encoded_array_clear(uint8_t* dest, size_t n);

#endif // POKEMON_CHAR_ENCODE_H
