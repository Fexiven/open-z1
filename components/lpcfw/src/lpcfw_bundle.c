#include "lpcfw_bundle.h"

#include <stdbool.h>
#include <stdio.h>

uint32_t lpcfw_crc32(uint32_t crc, const void *data, size_t n)
{
    // Reflected polynomial 0xEDB88320, a nibble at a time.
    static const uint32_t k_tab[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ k_tab[crc & 0x0F];
        crc = (crc >> 4) ^ k_tab[crc & 0x0F];
    }
    return ~crc;
}

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

const char *lpcfw_hdr_parse(const uint8_t *b, size_t n, uint64_t file_size, lpcfw_hdr_t *h)
{
    if (n < LPCFW_HDR_LEN || file_size < LPCFW_HDR_LEN) {
        return "shorter than its header";
    }
    if (le32(b) != LPCFW_MAGIC) {
        return "wrong magic";
    }
    *h = (lpcfw_hdr_t){
        .hver = b[4],
        .hlen = b[5],
        .flags = b[6],
        .esp_size = le32(b + 8),
        .lpc_size = le32(b + 12),
        .esp_ver = le32(b + 16),
        .lpc_ver = le32(b + 20),
        .hdr_crc = le32(b + 24),
        .file_crc = le32(b + 28),
    };
    if (h->hver != 1 && h->hver != 2) {
        return "unknown header version";
    }
    if (h->hlen != LPCFW_HDR_LEN) {
        return "wrong header length";
    }
    if (lpcfw_crc32(0, b, 24) != h->hdr_crc) {
        return "header CRC mismatch";
    }
    if (h->flags & ~(LPCFW_FLAG_ESP | LPCFW_FLAG_LPC)) {
        return "unknown flags";
    }
    if (!(h->flags & LPCFW_FLAG_ESP) != !h->esp_size || !(h->flags & LPCFW_FLAG_LPC) != !h->lpc_size) {
        return "flags and sizes disagree";
    }
    if (!h->flags) {
        return "no image";
    }
    if (h->esp_size > LPCFW_ESP_MAX || (uint64_t)LPCFW_HDR_LEN + h->esp_size + h->lpc_size != file_size) {
        return "sizes do not add up to the file's";
    }
    return NULL;
}

const char *lpcfw_hdr_lpc_ok(const lpcfw_hdr_t *h)
{
    if (!(h->flags & LPCFW_FLAG_LPC)) {
        return "no motion-controller image";
    }
    if (h->lpc_size > LPCFW_LPC_MAX) {
        return "motion-controller image larger than its flash";
    }
    if (h->lpc_size < 8) {
        return "motion-controller image too short";
    }
    return NULL;
}

size_t lpcfw_lpc_slice(const lpcfw_hdr_t *h, uint64_t pos, size_t n, size_t *off)
{
    uint64_t lo = (uint64_t)LPCFW_HDR_LEN + h->esp_size, hi = lo + h->lpc_size;
    uint64_t a = pos > lo ? pos : lo, e = pos + n < hi ? pos + n : hi;
    *off = 0;
    if (a >= e) {
        return 0;
    }
    *off = (size_t)(a - pos);
    return (size_t)(e - a);
}

const char *lpcfw_lpc_vectors_ok(const uint8_t first[8], uint32_t size)
{
    uint32_t sp = le32(first), reset = le32(first + 4);
    // Local SRAM 0x10000000 (32 KiB) or the AHB SRAM 0x2007C000 (2 x 16 KiB);
    // a full descending stack starts at the top, so the end itself counts.
    bool sp_ok = (sp > 0x10000000u && sp <= 0x10008000u) || (sp > 0x2007C000u && sp <= 0x20084000u);
    if (!sp_ok || (sp & 3)) {
        return "not an LPC1768 image (stack pointer)";
    }
    if (!(reset & 1) || (reset & ~1u) < LPCFW_LPC_BASE || (reset & ~1u) >= LPCFW_LPC_BASE + size) {
        return "not an LPC1768 image for 0x4000 (reset handler)";
    }
    return NULL;
}

void lpcfw_version_str(uint32_t v, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u", (unsigned)(v >> 24 & 0xFF), (unsigned)(v >> 20 & 0xF), (unsigned)(v >> 16 & 0xF));
}
