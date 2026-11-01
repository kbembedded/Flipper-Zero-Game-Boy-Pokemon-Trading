#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GEN3_EOS 0xFF

/* App-internal gender symbols: '\200' is the female symbol (0xB6), '\201' the male symbol (0xB5).
 * They are compared as char literals so signed (host) and unsigned (ARM) char behave the same. */

/* Encodes up to max_chars chars of ascii, then fills the rest of field_len with GEN3_EOS.
 * Unsupported chars encode as '?' (0xAC). Returns chars encoded. Requires max_chars <= field_len. */
size_t gen3_text_encode(const char* ascii, uint8_t* out, size_t field_len, size_t max_chars);

/* Decodes until GEN3_EOS or in_len. Unknown bytes become '?'; 0xB6 and 0xB5 become '\200' and '\201'. Always NUL-terminates. Returns strlen(out). */
size_t gen3_text_decode(const uint8_t* in, size_t in_len, char* out, size_t out_size);

/* decode(encode(ascii)) == ascii for a field of max_chars: at most max_chars chars, each one the
 * game can store ('?' itself is fine; an unsupported char that would turn into '?' is not). */
bool gen3_text_encodable(const char* ascii, size_t max_chars);
