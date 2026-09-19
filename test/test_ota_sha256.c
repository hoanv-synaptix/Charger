#include <stdio.h>
#include <string.h>
#include "ota_sha256.h"

static int expect_digest(const char *input, const char *expected)
{
    OtaSha256Context_t ctx;
    unsigned char digest[32];
    char text[65];
    static const char hex[] = "0123456789abcdef";

    OTA_SHA256_Init(&ctx);
    OTA_SHA256_Update(&ctx, (const unsigned char *)input, strlen(input));
    OTA_SHA256_Final(&ctx, digest);
    for (unsigned i = 0U; i < sizeof(digest); ++i) {
        text[i * 2U] = hex[digest[i] >> 4U];
        text[i * 2U + 1U] = hex[digest[i] & 0x0FU];
    }
    text[64] = '\0';
    if (strcmp(text, expected) != 0) {
        printf("[FAIL] SHA-256(%s) = %s\n", input, text);
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!expect_digest("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")) return 1;
    if (!expect_digest("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) return 1;
    puts("[PASS] OTA SHA-256 vectors");
    return 0;
}
