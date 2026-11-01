"""Turns the helper's raw binary into a multiboot image (multiboot-and-save.md §4.5 steps 1-4).

Usage: python3 -I fix_header.py in.bin out.mb max_len

1. Writes the logo at 0x04..0x9F.
2. Writes the header complement at 0xBD.
3. Pads with 0x00 to a multiple of 0x10 and to at least 0x1C0 bytes.
4. Fails unless the logo's CRC-32 is 0xD0BEB55E, [0xB2] is 0x96, 0xC0 holds an ARM branch, and
   the image fits max_len: the Makefile's MAX_LEN, which equals the Flipper's cap GEN3_MB_MAX_LEN
   (src/gen3/gen3_mb.h), below the BIOS's 0x40000.
"""

import sys
import zlib

# The 156-byte logo bitmap at 0x04..0x9F of every GBA cartridge header, which the BIOS checks
# before it boots an image. Interop data, transcribed here, not code from any project.
LOGO = bytes.fromhex(
    "24FFAE51699AA2213D84820A84E409AD11248B98C0817F21A352BE199309CE2010464A4AF82731EC58C7E83382E3CEBF85F4DF94"
    "CE4B09C194568AC01372A7FC9F844D73A3CA9A615897A327FC039876231DC7610304AE56BF38840040A70EFDFF52FE036F9530F1"
    "97FBC08560D68025A963BE03014E38E2F9A234FFBB3E0344780090CB88113A9465C07C6387F03CAFD625E48B380AAC7221D4F807"
)
LOGO_CRC32 = 0xD0BEB55E
MIN_LEN = 0x1C0
BIOS_MAX_LEN = 0x40000


def fail(msg):
    sys.stderr.write("fix_header.py: " + msg + "\n")
    sys.exit(1)


def main(argv):
    if len(argv) != 4:
        fail("usage: python3 -I fix_header.py in.bin out.mb max_len")
    try:
        max_len = int(argv[3], 0)
    except ValueError:
        fail("max_len %r is not a number" % argv[3])
    if not MIN_LEN <= max_len <= BIOS_MAX_LEN or max_len % 0x10:
        fail("max_len %d is not a multiple of 16 in 0x1C0..0x40000" % max_len)
    with open(argv[1], "rb") as f:
        img = bytearray(f.read())
    if len(img) < 0xE4:
        fail("%s: %d bytes, too short for the header and entry points" % (argv[1], len(img)))
    if len(LOGO) != 156 or zlib.crc32(LOGO) != LOGO_CRC32:
        fail("the logo table is damaged")
    img[0x04:0xA0] = LOGO
    chk = 0
    for b in img[0xA0:0xBD]:
        chk -= b
    img[0xBD] = (chk - 0x19) & 0xFF
    img += bytes(-len(img) % 0x10)
    if len(img) < MIN_LEN:
        img += bytes(MIN_LEN - len(img))
    if zlib.crc32(bytes(img[0x04:0xA0])) != LOGO_CRC32:
        fail("the logo didn't land at 0x04..0x9F")
    if img[0xB2] != 0x96:
        fail("[0xB2] is 0x%02X, not 0x96" % img[0xB2])
    if img[0xC3] != 0xEA:
        fail("0xC0 holds 0x%08X, not an ARM branch" % int.from_bytes(img[0xC0:0xC4], "little"))
    if len(img) > max_len:
        fail("%d bytes, over the cap of %d" % (len(img), max_len))
    with open(argv[2], "wb") as f:
        f.write(img)


if __name__ == "__main__":
    main(sys.argv)
