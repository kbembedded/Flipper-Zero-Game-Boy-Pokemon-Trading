#include "test.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <hp_link.h>
#include <hp_version.h>
#include <src/gen3/gen3_mb.h>

/* The committed helper image, files/gen3_helper.mb, written only by make -C gba/helper install.
 * The tests run from tests/. */

#define HELPER_IMAGE_PATH "../files/gen3_helper.mb"
#define HELPER_MAKEFILE_PATH "../gba/helper/Makefile"
#define HELPER_IMAGE_MAX 0x40000u /* the BIOS's limit; the sender's cap is lower */

/* The 156-byte logo bitmap at 0x04..0x9F of every GBA cartridge header, which the BIOS checks
 * before it boots an image. Interop data, transcribed here, not code from any project. */
static const uint8_t logo[156] = {
    0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21, 0x3D, 0x84, 0x82, 0x0A, 0x84, 0xE4, 0x09, 0xAD,
    0x11, 0x24, 0x8B, 0x98, 0xC0, 0x81, 0x7F, 0x21, 0xA3, 0x52, 0xBE, 0x19, 0x93, 0x09, 0xCE, 0x20,
    0x10, 0x46, 0x4A, 0x4A, 0xF8, 0x27, 0x31, 0xEC, 0x58, 0xC7, 0xE8, 0x33, 0x82, 0xE3, 0xCE, 0xBF,
    0x85, 0xF4, 0xDF, 0x94, 0xCE, 0x4B, 0x09, 0xC1, 0x94, 0x56, 0x8A, 0xC0, 0x13, 0x72, 0xA7, 0xFC,
    0x9F, 0x84, 0x4D, 0x73, 0xA3, 0xCA, 0x9A, 0x61, 0x58, 0x97, 0xA3, 0x27, 0xFC, 0x03, 0x98, 0x76,
    0x23, 0x1D, 0xC7, 0x61, 0x03, 0x04, 0xAE, 0x56, 0xBF, 0x38, 0x84, 0x00, 0x40, 0xA7, 0x0E, 0xFD,
    0xFF, 0x52, 0xFE, 0x03, 0x6F, 0x95, 0x30, 0xF1, 0x97, 0xFB, 0xC0, 0x85, 0x60, 0xD6, 0x80, 0x25,
    0xA9, 0x63, 0xBE, 0x03, 0x01, 0x4E, 0x38, 0xE2, 0xF9, 0xA2, 0x34, 0xFF, 0xBB, 0x3E, 0x03, 0x44,
    0x78, 0x00, 0x90, 0xCB, 0x88, 0x11, 0x3A, 0x94, 0x65, 0xC0, 0x7C, 0x63, 0x87, 0xF0, 0x3C, 0xAF,
    0xD6, 0x25, 0xE4, 0x8B, 0x38, 0x0A, 0xAC, 0x72, 0x21, 0xD4, 0xF8, 0x07,
};

static uint8_t image[HELPER_IMAGE_MAX + 1];
static uint32_t image_len;

static bool image_load(void) {
    FILE* f = fopen(HELPER_IMAGE_PATH, "rb");
    if(!f) {
        printf("cannot open %s\n", HELPER_IMAGE_PATH);
        return false;
    }
    image_len = (uint32_t)fread(image, 1, sizeof(image), f);
    fclose(f);
    return true;
}

/* The image as the sender takes it: within its cap, a multiple of 16 bytes, the header checks pass
 * with the helper's protocol version as the version byte, and the title is the helper's. */
TEST(test_helper_image) {
    ASSERT_TRUE(image_load());
    ASSERT_TRUE(image_len <= GEN3_MB_MAX_LEN);
    ASSERT_EQ(image_len % 16, 0);
    ASSERT_EQ(gen3_mb_image_check(image, image_len, HP_PROTO_VERSION), Gen3MbImageOk);
    ASSERT_MEM_EQ(image + 0xA0, "FLIPPERGEN3 ", 12);
}

/* The sender checks only the logo's first word; the BIOS compares all 156 bytes. */
TEST(test_helper_image_logo) {
    ASSERT_EQ((uint32_t)~hp_crc32_bytes(0xFFFFFFFFu, logo, sizeof(logo)), 0xD0BEB55Eu);
    ASSERT_TRUE(image_load());
    ASSERT_TRUE(image_len >= 4 + sizeof(logo));
    ASSERT_MEM_EQ(image + 4, logo, sizeof(logo));
}

/* The helper's build caps the image at its Makefile's MAX_LEN, which it passes to fix_header.py;
 * the sender caps it at GEN3_MB_MAX_LEN. The two must agree. */
TEST(test_helper_cap_matches) {
    FILE* f = fopen(HELPER_MAKEFILE_PATH, "r");
    ASSERT_TRUE(f != NULL);
    char line[256];
    long max_len = -1;
    while(fgets(line, sizeof(line), f)) {
        if(strncmp(line, "MAX_LEN = ", 10) == 0) max_len = strtol(line + 10, NULL, 0);
    }
    fclose(f);
    ASSERT_EQ(max_len, GEN3_MB_MAX_LEN);
}

int main(void) {
    RUN(test_helper_image);
    RUN(test_helper_image_logo);
    RUN(test_helper_cap_matches);
    TEST_EXIT();
}
