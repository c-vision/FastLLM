/* fmlx_serve_vision.c — input multimodale nel server.
 *
 * Il motore MLX non ha vision. Quando `serve` è configurato con un VLM (GGUF modello + GGUF
 * `mmproj`), questo modulo carica la lane vision (llama.cpp/mtmd) e registra nell'engine un
 * "descrittore immagine": dato un blocco immagine (url `data:` base64, OpenAI o Anthropic), lo
 * materializza su file temporaneo, lo legge col VLM e restituisce il testo, che l'engine inietta
 * nel prompt. Non configurato -> i blocchi immagine restano ignorati (comportamento storico). */
#include "fmlx/fmlx_serve.h"
#include "engine/fmlx_engine.h"
#include "engine/fmlx_vision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FmlxVision *g_vision = NULL;
static long g_seq = 0;

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static size_t b64dec(const char *s, size_t n, unsigned char *out) {
    size_t o = 0;
    int acc = 0, nb = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64v((unsigned char)s[i]);
        if (v < 0) { if (s[i] == '=') break; continue; }
        acc = (acc << 6) | v;
        nb += 6;
        if (nb >= 8) { nb -= 8; out[o++] = (unsigned char)((acc >> nb) & 0xFF); }
    }
    return o;
}

static int fmlx_img_desc(void *ctx, const char *url, size_t n, char *out, size_t outcap) {
    (void)ctx;
    if (!g_vision || !url) return -1;
    const char *comma = (const char *)memchr(url, ',', n);
    if (!comma) return -1; /* solo data URL: `data:<mime>;base64,<...>` */
    const char *head = url;
    size_t headn = (size_t)(comma - url);
    const char *b64 = comma + 1;
    size_t b64n = n - headn - 1;
    const char *ext = "png";
    if (headn >= 10 && memmem(head, headn, "image/jpeg", 10)) ext = "jpg";
    else if (headn >= 10 && memmem(head, headn, "image/webp", 10)) ext = "webp";
    unsigned char *img = (unsigned char *)malloc(b64n / 4 * 3 + 8);
    if (!img) return -1;
    size_t imgn = b64dec(b64, b64n, img);
    if (imgn < 8) { free(img); return -1; }
    char path[600];
    snprintf(path, sizeof path, "/tmp/fmlx_img_%ld_%ld.%s", (long)getpid(), ++g_seq, ext);
    FILE *f = fopen(path, "wb");
    if (!f) { free(img); return -1; }
    size_t wr = fwrite(img, 1, imgn, f);
    fclose(f);
    free(img);
    if (wr != imgn) { remove(path); return -1; }
    char err[256];
    char *desc = fmlx_vision_read_image(g_vision, path,
                                        "Descrivi questa immagine e trascrivi ogni testo visibile.",
                                        512, err, sizeof err);
    remove(path);
    if (!desc) return -1;
    snprintf(out, outcap, "%s", desc);
    fmlx_vision_free_str(desc);
    return 0;
}

int fmlx_serve_vision_init(const char *model, const char *mmproj, char *err, size_t errcap) {
    if ((!model || !model[0]) && (!mmproj || !mmproj[0])) return 0; /* non configurato */
    if (!model || !mmproj) {
        if (err) snprintf(err, errcap, "vision richiede SIA --vision-model SIA --vision-mmproj");
        return -1;
    }
    if (!fmlx_vision_available()) {
        if (err) snprintf(err, errcap, "lane vision non disponibile in questo build");
        return -1;
    }
    g_vision = fmlx_vision_load(model, mmproj, 4096, -1, err, errcap);
    if (!g_vision) return -1;
    fmlx_engine_set_image_describer(fmlx_img_desc, NULL);
    return 1; /* attivo */
}
