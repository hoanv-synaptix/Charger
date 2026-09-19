/**
 * @file ota_sha256.h
 * @brief Small allocation-free SHA-256 implementation for OTA integrity.
 */

#ifndef OTA_SHA256_H
#define OTA_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
} OtaSha256Context_t;

void OTA_SHA256_Init(OtaSha256Context_t *ctx);
void OTA_SHA256_Update(OtaSha256Context_t *ctx, const uint8_t *data, size_t len);
void OTA_SHA256_Final(OtaSha256Context_t *ctx, uint8_t digest[32]);

#endif /* OTA_SHA256_H */
