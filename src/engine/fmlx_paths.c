/* fmlx_paths.c — gestione del file di configurazione dei percorsi di ricerca modelli.
 * Vedi include/fmlx/fmlx_paths.h per il motivo e il formato. */
#include "fmlx/fmlx_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Directory di stato: $FASTLLM_HOME, default ~/.fastllm (creata se manca). */
static int paths_dir(char *out, size_t cap) {
    const char *home = getenv("FASTLLM_HOME");
    char dir[FMLX_PATHS_DIR_MAX];

    if (home && *home) {
        snprintf(dir, sizeof dir, "%s", home);
    } else {
        const char *h = getenv("HOME");
        if (!h || !*h) return -1;
        snprintf(dir, sizeof dir, "%s/.fastllm", h);
    }
    mkdir(dir, 0755); /* idempotente */
    if (snprintf(out, cap, "%s", dir) >= (int)cap) return -1;
    return 0;
}

int fmlx_paths_file(char *out, size_t cap) {
    char dir[FMLX_PATHS_DIR_MAX];
    if (!out || paths_dir(dir, sizeof dir) != 0) return -1;
    if (snprintf(out, cap, "%s/paths", dir) >= (int)cap) return -1;
    return 0;
}

/* Una riga "utile"? (non vuota, non commento). Ritorna il puntatore al primo carattere utile, con
 * i whitespace di coda azzerati IN PLACE. */
static char *line_trim(char *line) {
    char *s = line;
    size_t l;
    while (*s == ' ' || *s == '\t') s++;
    l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
    return s;
}

/* Legge i percorsi configurati in `dirs` (max voci). Ritorna il numero letto (0 se il file non
 * esiste o è vuoto). */
static int paths_read(char (*dirs)[FMLX_PATHS_DIR_MAX], int max) {
    char f[FMLX_PATHS_DIR_MAX + 16];
    char line[FMLX_PATHS_DIR_MAX + 16];
    FILE *fp;
    int n = 0;

    if (fmlx_paths_file(f, sizeof f) != 0) return 0;
    fp = fopen(f, "r");
    if (!fp) return 0;
    while (n < max && fgets(line, sizeof line, fp)) {
        char *s = line_trim(line);
        if (!*s || *s == '#') continue;
        snprintf(dirs[n], FMLX_PATHS_DIR_MAX, "%s", s);
        n++;
    }
    fclose(fp);
    return n;
}

int fmlx_paths_count(void) {
    char(*dirs)[FMLX_PATHS_DIR_MAX] = malloc((size_t)FMLX_PATHS_MAX * FMLX_PATHS_DIR_MAX);
    int n;
    if (!dirs) return 0;
    n = paths_read(dirs, FMLX_PATHS_MAX);
    free(dirs);
    return n;
}

int fmlx_paths_get(int i, char *out, size_t cap) {
    char(*dirs)[FMLX_PATHS_DIR_MAX] = malloc((size_t)FMLX_PATHS_MAX * FMLX_PATHS_DIR_MAX);
    int n, rc = -1;
    if (!dirs || !out) {
        free(dirs);
        return -1;
    }
    n = paths_read(dirs, FMLX_PATHS_MAX);
    if (i >= 0 && i < n) rc = (snprintf(out, cap, "%s", dirs[i]) < (int)cap) ? 0 : -1;
    free(dirs);
    return rc;
}

int fmlx_paths_add(const char *dir, char *err, size_t errcap) {
    char(*dirs)[FMLX_PATHS_DIR_MAX] = malloc((size_t)FMLX_PATHS_MAX * FMLX_PATHS_DIR_MAX);
    char f[FMLX_PATHS_DIR_MAX + 16];
    char clean[FMLX_PATHS_DIR_MAX];
    FILE *fp;
    int n, i, rc = 0;

    if (!dirs) {
        if (err) snprintf(err, errcap, "memoria esaurita");
        return -1;
    }
    if (!dir || !*dir) {
        free(dirs);
        if (err) snprintf(err, errcap, "percorso vuoto");
        return -1;
    }
    /* normalizza: senza slash finale (tranne root) */
    snprintf(clean, sizeof clean, "%s", dir);
    { size_t l = strlen(clean); while (l > 1 && clean[l - 1] == '/') clean[--l] = 0; }

    n = paths_read(dirs, FMLX_PATHS_MAX);
    for (i = 0; i < n; i++) {
        if (strcmp(dirs[i], clean) == 0) {
            free(dirs);
            if (err) snprintf(err, errcap, "percorso gia' presente (#%d): %s", i + 1, clean);
            return -1;
        }
    }
    if (n >= FMLX_PATHS_MAX) {
        free(dirs);
        if (err) snprintf(err, errcap, "massimo %d percorsi raggiunto", FMLX_PATHS_MAX);
        return -1;
    }
    if (fmlx_paths_file(f, sizeof f) != 0) {
        free(dirs);
        if (err) snprintf(err, errcap, "impossibile localizzare il file dei percorsi");
        return -1;
    }
    fp = fopen(f, "a");
    if (!fp) {
        free(dirs);
        if (err) snprintf(err, errcap, "impossibile scrivere %s", f);
        return -1;
    }
    fprintf(fp, "%s\n", clean);
    rc = (fclose(fp) == 0) ? 0 : -1;
    if (rc != 0 && err) snprintf(err, errcap, "errore di scrittura su %s", f);
    free(dirs);
    return rc;
}

int fmlx_paths_rm(int index_1based, char *err, size_t errcap) {
    char(*dirs)[FMLX_PATHS_DIR_MAX] = malloc((size_t)FMLX_PATHS_MAX * FMLX_PATHS_DIR_MAX);
    char f[FMLX_PATHS_DIR_MAX + 16];
    FILE *fp;
    int n, i;

    if (!dirs) {
        if (err) snprintf(err, errcap, "memoria esaurita");
        return -1;
    }
    n = paths_read(dirs, FMLX_PATHS_MAX);
    if (index_1based < 1 || index_1based > n) {
        free(dirs);
        if (err) snprintf(err, errcap, "indice %d fuori range (1..%d)", index_1based, n);
        return -1;
    }
    if (fmlx_paths_file(f, sizeof f) != 0) {
        free(dirs);
        if (err) snprintf(err, errcap, "impossibile localizzare il file dei percorsi");
        return -1;
    }
    fp = fopen(f, "w");
    if (!fp) {
        free(dirs);
        if (err) snprintf(err, errcap, "impossibile riscrivere %s", f);
        return -1;
    }
    for (i = 0; i < n; i++)
        if (i != index_1based - 1) fprintf(fp, "%s\n", dirs[i]);
    if (fclose(fp) != 0 && err) snprintf(err, errcap, "errore di scrittura su %s", f);
    free(dirs);
    return 0;
}
