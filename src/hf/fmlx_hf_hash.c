/* fmlx_hf_hash.c — hash streaming di un fd, secondo ciò che il nome del blob promette.
 *
 * Perchè `git_header_size`: nei blob a 40 hex il nome NON è lo SHA1 del contenuto (misurato:
 * 6/6 blob locali lo smentiscono) ma il **git blob id**, cioè SHA1 di "blob <len>\0" + contenuto.
 * Chi verifica deve quindi iniettare quell'header nel digest prima dei byte del file.
 *
 * I digest sono quelli di CommonCrypto (Apple): sono nell'SDK di questa macchina, verificato
 * con `ls $(xcrun --show-sdk-path)/usr/include/CommonCrypto/CommonDigest.h`. Non c'è una
 * nostra implementazione di SHA1/SHA256 perché una terza copia di SHA1 sarebbe l'unico modo
 * di sbagliarla in silenzio; il test confronta i nostri output con vettori pubblicati e con
 * `git hash-object`.
 */
#include "fmlx_hf_internal.h"

#include <CommonCrypto/CommonDigest.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 65536

static const char XD[] = "0123456789abcdef";

static void hexa(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = XD[d[i] >> 4];
        out[2 * i + 1] = XD[d[i] & 15];
    }
    out[2 * n] = '\0';
}

int fmlx_hf_hash_fd(int fd, FmlxHfHashKind kind, int64_t git_header_size, char *hex,
                    int64_t *out_bytes) {
    uint8_t buf[CHUNK];
    int64_t total = 0;

    if (kind == FMLX_HF_HASH_GIT_BLOB) {
        char hdr[64];
        int n = snprintf(hdr, sizeof hdr, "blob %lld%c", (long long)git_header_size, '\0');
        if (n < 0 || (size_t)n >= sizeof hdr) {
            errno = EOVERFLOW;
            return -1;
        }
        /* L'header entra nel digest ma NON nei byte contati: `out_bytes` deve restare la
         * lunghezza del file, che è anche il <len> dichiarato nell'header. */
        CC_SHA1_CTX c1;
        CC_SHA1_Init(&c1);
        CC_SHA1_Update(&c1, hdr, (CC_LONG)(size_t)n);
        for (;;) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (r == 0) break;
            CC_SHA1_Update(&c1, buf, (CC_LONG)(size_t)r);
            total += r;
        }
        uint8_t d[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1_Final(d, &c1);
        hexa(d, CC_SHA1_DIGEST_LENGTH, hex);
        if (out_bytes) *out_bytes = total;
        return 0;
    }

    if (kind == FMLX_HF_HASH_SHA256) {
        CC_SHA256_CTX c2;
        CC_SHA256_Init(&c2);
        for (;;) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (r == 0) break;
            CC_SHA256_Update(&c2, buf, (CC_LONG)(size_t)r);
            total += r;
        }
        uint8_t d[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(d, &c2);
        hexa(d, CC_SHA256_DIGEST_LENGTH, hex);
        if (out_bytes) *out_bytes = total;
        return 0;
    }

    errno = EINVAL; /* FMLX_HF_HASH_NONE qui è un uso sbagliato, non un caso da tollerare */
    return -1;
}
