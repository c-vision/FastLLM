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
#include <glob.h>

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
    int is_pdf = 0;
    if (headn >= 15 && memmem(head, headn, "application/pdf", 15)) { is_pdf = 1; ext = "pdf"; }
    else if (headn >= 10 && memmem(head, headn, "image/jpeg", 10)) ext = "jpg";
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
    /* PDF inline MULTI-PAGINA: rasterizza tutte le pagine con `pdftoppm` (poppler) e descrivile
     * tutte concatenate; fallback `sips` (prima pagina) se pdftoppm manca. */
    char err[256];
    if (is_pdf) {
        char prefix[600], pattern[620], cmd[1700];
        snprintf(prefix, sizeof prefix, "/tmp/fmlx_pdf_%ld_%ld", (long)getpid(), g_seq);
        snprintf(pattern, sizeof pattern, "%s-*.png", prefix);
        glob_t gl;
        snprintf(cmd, sizeof cmd, "pdftoppm -png -r 150 '%s' '%s' >/dev/null 2>&1", path, prefix);
        int np = (system(cmd) == 0 && glob(pattern, 0, NULL, &gl) == 0) ? (int)gl.gl_pathc : 0;
        size_t used = 0;
        if (outcap > 0) out[0] = '\0';
        for (int i = 0; i < np; i++) {
            char *d = fmlx_vision_read_image(g_vision, gl.gl_pathv[i],
                        "Descrivi questa immagine e trascrivi ogni testo visibile.", 512, err, sizeof err);
            if (d) {
                int wn = snprintf(out + used, (used < outcap) ? outcap - used : 0,
                                  "%s[pagina %d] %s\n", used ? "" : "", i + 1, d);
                if (wn > 0) used += (size_t)wn;
                fmlx_vision_free_str(d);
            }
            remove(gl.gl_pathv[i]);
        }
        if (np) globfree(&gl);
        remove(path);
        if (np > 0) return 0;
        /* fallback: prima pagina via sips */
        char png[620];
        snprintf(png, sizeof png, "%s-f.png", prefix);
        snprintf(cmd, sizeof cmd, "sips -s format png '%s' --out '%s' >/dev/null 2>&1", path, png);
        if (system(cmd) != 0) return -1;
        char *d = fmlx_vision_read_image(g_vision, png,
                    "Descrivi questa immagine e trascrivi ogni testo visibile.", 512, err, sizeof err);
        remove(png);
        if (!d) return -1;
        snprintf(out, outcap, "%s", d);
        fmlx_vision_free_str(d);
        return 0;
    }
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
