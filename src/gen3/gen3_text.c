#include <src/gen3/gen3_text.h>

#include <string.h>

#define GEN3_QUESTION 0xAC

static uint8_t gen3_char_encode(char c) {
    if(c == ' ') return 0x00;
    /* char is signed on the host and unsigned on ARM; compare against the literals. */
    if(c == '\200') return 0xB6; /* female symbol */
    if(c == '\201') return 0xB5; /* male symbol */
    if(c >= '0' && c <= '9') return 0xA1 + (c - '0');
    if(c >= 'A' && c <= 'Z') return 0xBB + (c - 'A');
    if(c >= 'a' && c <= 'z') return 0xD5 + (c - 'a');
    switch(c) {
    case '!':
        return 0xAB;
    case '?':
        return 0xAC;
    case '.':
        return 0xAD;
    case '-':
        return 0xAE;
    case '"':
        return 0xB2;
    case '\'':
        return 0xB4;
    case ',':
        return 0xB8;
    case '/':
        return 0xBA;
    case ':':
        return 0xF0;
    default:
        return GEN3_QUESTION;
    }
}

static char gen3_char_decode(uint8_t b) {
    if(b == 0x00) return ' ';
    if(b == 0xB6) return '\200';
    if(b == 0xB5) return '\201';
    if(b >= 0xA1 && b <= 0xAA) return (char)('0' + (b - 0xA1));
    if(b >= 0xBB && b <= 0xD4) return (char)('A' + (b - 0xBB));
    if(b >= 0xD5 && b <= 0xEE) return (char)('a' + (b - 0xD5));
    switch(b) {
    case 0xAB:
        return '!';
    case 0xAC:
        return '?';
    case 0xAD:
    case 0xB0:
        return '.';
    case 0xAE:
        return '-';
    case 0xB1:
    case 0xB2:
        return '"';
    case 0xB3:
    case 0xB4:
        return '\'';
    case 0xB8:
        return ',';
    case 0xBA:
        return '/';
    case 0xF0:
        return ':';
    default:
        return '?';
    }
}

size_t gen3_text_encode(const char* ascii, uint8_t* out, size_t field_len, size_t max_chars) {
    size_t n = 0;
    while(n < max_chars && ascii[n] != '\0') {
        out[n] = gen3_char_encode(ascii[n]);
        n++;
    }
    memset(out + n, GEN3_EOS, field_len - n);
    return n;
}

/* Per char: encode never yields GEN3_EOS, so a string round-trips exactly when each of its chars
 * does and none is cut off. */
bool gen3_text_encodable(const char* ascii, size_t max_chars) {
    for(size_t n = 0; ascii[n] != '\0'; n++) {
        if(n >= max_chars) return false;
        if(gen3_char_decode(gen3_char_encode(ascii[n])) != ascii[n]) return false;
    }
    return true;
}

size_t gen3_text_decode(const uint8_t* in, size_t in_len, char* out, size_t out_size) {
    if(out_size == 0) return 0;
    size_t n = 0;
    while(n < in_len && n + 1 < out_size && in[n] != GEN3_EOS) {
        out[n] = gen3_char_decode(in[n]);
        n++;
    }
    out[n] = '\0';
    return n;
}
