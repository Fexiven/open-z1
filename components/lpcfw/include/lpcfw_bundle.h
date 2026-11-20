#pragma once
// Pure checks of Makera's firmware bundle (/sd/firmware.bin, what the app's
// Firmware Update page uploads). Nothing in it is trusted: there is no
// signature, only CRCs against damage.
//
//   off  0  u32 magic 0x4D5173EE         (all little-endian)
//        4  u8  header version (1, 2; 2 seen)
//        5  u8  header length, 32
//        6  u8  flags: bit 0 ESP32 image present, bit 1 LPC1768 image present
//        7  u8  reserved
//        8  u32 ESP32 image size        12 u32 LPC1768 image size
//       16  u32 ESP32 version           20 u32 LPC1768 version (0x01130000 = 1.1.3)
//       24  u32 CRC-32 of bytes 0..23   28 u32 CRC-32 of bytes 0..27 and the images
//       32  the ESP32 image, then the LPC1768 image
//
// (stock's log of a real update and its validation strings;
// split_z1_bundle.py). This firmware only ever uses the LPC1768 image.

#include <stddef.h>
#include <stdint.h>

#define LPCFW_MAGIC 0x4D5173EEu
#define LPCFW_HDR_LEN 32
#define LPCFW_FLAG_ESP 0x01
#define LPCFW_FLAG_LPC 0x02
// The LPC1768 has 512 KiB of flash; its bootloader keeps the first 16 KiB and
// the firmware starts at 0x4000 (the GPL firmware's main.cpp sets VTOR = 0x4000;
// mbed LPC1768.ld: FLASH ORIGIN 16K, LENGTH 512K - 16K).
#define LPCFW_LPC_BASE 0x4000u
#define LPCFW_LPC_MAX (512u * 1024u - LPCFW_LPC_BASE)
// Far above any real ESP32 image (stock's is 1.6 MB): a guard against sizes
// that only add up by overflow.
#define LPCFW_ESP_MAX (16u * 1024u * 1024u)

typedef struct {
    uint8_t hver, hlen, flags;
    uint32_t esp_size, lpc_size, esp_ver, lpc_ver, hdr_crc, file_crc;
} lpcfw_hdr_t;

// CRC-32 as zlib's crc32(): crc is 0 to start, or the previous result to go on.
uint32_t lpcfw_crc32(uint32_t crc, const void *data, size_t n);

// Reads and checks the header (b holds at least n bytes, file_size is the
// whole file's): magic, header version and length, known flags only, each
// flag set exactly when its size is non-zero, the header CRC, and the total
// length. NULL if it passes, else why not (for the log). The file CRC is the
// caller's to check over the whole file.
const char *lpcfw_hdr_parse(const uint8_t *b, size_t n, uint64_t file_size, lpcfw_hdr_t *h);

// After lpcfw_hdr_parse: whether the bundle has an LPC1768 image this firmware
// may use (present, at most LPCFW_LPC_MAX). NULL if so, else why not.
const char *lpcfw_hdr_lpc_ok(const lpcfw_hdr_t *h);

// Of the bundle bytes [pos, pos + n), the ones that belong to the LPC1768
// image: returns how many, starting at *off into those n bytes.
size_t lpcfw_lpc_slice(const lpcfw_hdr_t *h, uint64_t pos, size_t n, size_t *off);

// The first 8 bytes of an LPC1768 image of size bytes are its vector table's
// initial stack pointer and reset handler: the stack must be in the chip's
// RAM and the handler a Thumb address inside the image, linked for 0x4000.
// NULL if so, else why not. Catches an image for another chip or address.
const char *lpcfw_lpc_vectors_ok(const uint8_t first[8], uint32_t size);

// "1.1.3" from a version field (as split_z1_bundle.py reads it).
void lpcfw_version_str(uint32_t v, char *out, size_t cap);
