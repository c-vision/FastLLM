/* json_diff.c — stampa la CODIFICA CANONICA di ogni record di un corpus JSON col NOSTRO
 * parser, per essere confrontata byte per byte con `tools/json_oracle.py` (che usa
 * `json.loads` di Python: un codice che non abbiamo scritto noi). Senza un confronto del
 * genere i test di questa lane direbbero solo che il parser è d'accordo con sé stesso.
 *
 * Grammatica della codifica (doc: docs/json-contract.md):
 *   N                     null
 *   B0 | B1               booleano
 *   I <decimale>          intero int64
 *   F <16 hex>            reale: i 8 byte IEEE-754 del pattern di bit. NON il suo testo:
 *                         "0.1" e "1e-1" sono lo stesso double e devono codificare uguali,
 *                         e il testo dipenderebbe dal formatter dei due codici.
 *   S <len> <hex>         stringa (byte UTF-8 già unescapeati)
 *   [ ... ]               array: apice, figli in ordine, chiusura
 *   { (K <...> <valore>)* }   oggetto: ogni membro è una riga K (chiave) + il suo valore
 *   E <CODICE>            rifiuto: EMPTY SYNTAX TRAILING TOO_DEEP TOO_BIG NUMBER UTF8
 *                         CONTROL NO_MEM BAD_ARG BUF_SMALL
 *   #W                    segue la ricodifica del DOM dopo round-trip col nostro writer
 *
 * Corpus: UNA RIGA PER RECORD, esadecimallo minuscolo dei byte esatti del documento.
 * Il perché dell'hex, non delle righe `@@@`: i casi che decidono il contratto SONO i byte
 * ambigui al confine — un newline nudo dentro una stringa (CONTROLLO), `\r\n`, un record
 * che è solo whitespace, un byte 0x00. Con un separatore testuale quei casi dipendono da
 * come il tool taglia il file, cioè lo sbaglio si sposta dal parser al driver.
 *
 * Uso: json_diff <corpus-hex>
 */
#include "fmlx/fmlx_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_hex(const unsigned char *s, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        putchar(d[s[i] >> 4]);
        putchar(d[s[i] & 15]);
    }
}

static const char *code_of(FmlxJsonStatus st) {
    switch (st) {
    case FMLX_JSON_OK: return "OK";
    case FMLX_JSON_ERR_EMPTY: return "EMPTY";
    case FMLX_JSON_ERR_SYNTAX: return "SYNTAX";
    case FMLX_JSON_ERR_TRAILING: return "TRAILING";
    case FMLX_JSON_ERR_TOO_DEEP: return "TOO_DEEP";
    case FMLX_JSON_ERR_TOO_BIG: return "TOO_BIG";
    case FMLX_JSON_ERR_NUMBER: return "NUMBER";
    case FMLX_JSON_ERR_UTF8: return "UTF8";
    case FMLX_JSON_ERR_CONTROL: return "CONTROL";
    case FMLX_JSON_ERR_NO_MEM: return "NO_MEM";
    case FMLX_JSON_ERR_BAD_ARG: return "BAD_ARG";
    case FMLX_JSON_ERR_BUF_SMALL: return "BUF_SMALL";
    }
    return "UNKNOWN";
}

static void emit_str(const FmlxJsonValue *v) {
    printf("S%u ", v->str_len);
    put_hex((const unsigned char *)(v->d->arena + v->str_off), v->str_len);
    putchar('\n');
}

/* La riga di una CHIAVE: `K<len> <hex>` — senza la `S` di emit_str, che è il marcatore del
 * VALORE stringa. L'oracolo Python scrive `enc_str(k)[1:]`, cioè la stessa forma: se le due
 * grammatiche divergono di una lettera, ogni oggetto del corpus risulta diverso. */
static void emit_key(const FmlxJsonValue *v) {
    printf("K%u ", v->str_len);
    put_hex((const unsigned char *)(v->d->arena + v->str_off), v->str_len);
    putchar('\n');
}

/* Le chiavi duplicate NON sono un errore di sintassi (RFC 8259 tace) ma decidono cosa
 * risponde un server: una riga `G<len> <hex> <indice>` dice quale MEMBRO vince in
 * `obj_get`. Senza questa riga le due codifiche sarebbero identiche anche con un
 * `obj_get` che prende l'ultimo. Le chiavi con NUL dentro sono fuori dalla portata di
 * `obj_get` (prende una C string): la riga `G` esce con indice `-1`, che è il modo
 * onesto di dire "questo accessorio non le vede". */
static void emit_dup_probe(const FmlxJsonValue *v, size_t n) {
    unsigned char probebuf[256];
    for (size_t i = 0; i < n; i++) {
        const FmlxJsonValue *k = fmlx_json_obj_key_at(v, i);
        const char *kb = k->d->arena + k->str_off;
        size_t dup = 0;
        for (size_t j = 0; j < n; j++) {
            const FmlxJsonValue *k2 = fmlx_json_obj_key_at(v, j);
            if (k2->str_len == k->str_len &&
                !memcmp(k2->d->arena + k2->str_off, kb, k->str_len)) dup++;
        }
        if (dup < 2) continue;
        for (size_t j = 0; j < i; j++) { /* una sola riga per CHIAVE, non per membro */
            const FmlxJsonValue *k2 = fmlx_json_obj_key_at(v, j);
            if (k2->str_len == k->str_len &&
                !memcmp(k2->d->arena + k2->str_off, kb, k->str_len)) { dup = 0; break; }
        }
        if (dup < 2) continue;
        /* `obj_get` prende una C string e ne fa `strlen`: passare l'arena sarebbe una
         * lettura oltre il fine chiave (le chiavi non sono terminate lì dentro). Si copia
         * in un buffer proprio, che è anche ciò che fa un chiamante onesto. */
        long wi = -1;
        if (k->str_len < sizeof probebuf && !memchr(kb, '\0', k->str_len)) {
            memcpy(probebuf, kb, k->str_len);
            probebuf[k->str_len] = '\0';
            const FmlxJsonValue *win = fmlx_json_obj_get(v, (const char *)probebuf);
            for (size_t j = 0; j < n && win; j++) if (fmlx_json_obj_value_at(v, j) == win) wi = (long)j;
        }
        printf("G%u ", k->str_len);
        put_hex((const unsigned char *)kb, k->str_len);
        printf(" %ld\n", wi);
    }
}

static void emit(const FmlxJsonValue *v) {
    switch (v->type) {
    case FMLX_JSON_NULL: puts("N"); break;
    case FMLX_JSON_BOOL: printf("B%d\n", v->v.b ? 1 : 0); break;
    case FMLX_JSON_INT: printf("I%lld\n", (long long)v->v.i); break;
    case FMLX_JSON_REAL: {
        /* Passando da uint64 il pattern di bit è indipendente dall'endianness: e
         * `struct.pack('>d', x)` di Python dà le stesse identiche 16 cifre. */
        uint64_t bits;
        memcpy(&bits, &v->v.f, sizeof bits);
        printf("F%016llx\n", (unsigned long long)bits);
        break;
    }
    case FMLX_JSON_STR: emit_str(v); break;
    case FMLX_JSON_ARR: {
        puts("[");
        size_t n = fmlx_json_arr_n(v);
        for (size_t i = 0; i < n; i++) emit(fmlx_json_arr_at(v, i));
        puts("]");
        break;
    }
    case FMLX_JSON_OBJ: {
        puts("{");
        size_t n = fmlx_json_obj_n(v);
        for (size_t i = 0; i < n; i++) {
            emit_key(fmlx_json_obj_key_at(v, i));
            emit(fmlx_json_obj_value_at(v, i));
        }
        emit_dup_probe(v, n);
        puts("}");
        break;
    }
    case FMLX_JSON_KEY: puts("KEY_INVALORE"); break; /* i padri non espongono mai chiavi */
    }
}

/* -1 su hex non valido: una riga sporca nel corpus è un errore dell'operatore, non un
 * caso di test, e deve fermare il tool invece di diventare un EMPTY di troppo. */
static int unhex(const char *line, unsigned char *out, size_t cap, size_t *out_len) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n % 2) return -1;
    if (n / 2 > cap) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi, lo;
        char a = line[i], b = line[i + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        else return -1;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        else return -1;
        out[i / 2] = (unsigned char)(hi * 16 + lo);
    }
    *out_len = n / 2;
    return 0;
}

/* Il round-trip è parte della codifica: il DOM viene renderizzato dal NOSTRO writer,
 * riparsato e ricodificato. Se le due codifiche divergono, il writer mente (escaping,
 * precisione dei reali, ordine dei membri, capienza). */
static void roundtrip(const FmlxJsonValue *root) {
    FmlxJsonW w;
    fmlx_jsonw_init(&w, NULL, 0, 1u << 22);
    if (fmlx_json_render(root, &w) != FMLX_JSON_OK) {
        puts("#W");
        printf("E %s\n", code_of(w.err));
        fmlx_jsonw_free(&w);
        return;
    }
    size_t wl = 0;
    FmlxJsonStatus fin = fmlx_jsonw_finish(&w, &wl);
    if (fin != FMLX_JSON_OK) {
        puts("#W");
        printf("E %s\n", code_of(fin));
        fmlx_jsonw_free(&w);
        return;
    }
    FmlxJsonDoc d2;
    fmlx_json_doc_init(&d2);
    FmlxJsonStatus st2 = fmlx_json_parse(w.buf, wl, 0, &d2);
    puts("#W");
    if (st2 != FMLX_JSON_OK) printf("E %s\n", code_of(st2));
    else emit(fmlx_json_root(&d2));
    fmlx_json_doc_free(&d2);
    fmlx_jsonw_free(&w);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "uso: json_diff <corpus-hex>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "json_diff: apro %s\n", argv[1]);
        return 1;
    }
    /* Un record può essere lungo quanto un corpo di richiesta: buffer da 16 MB di esadecimale
     * (8 MB di JSON), riallocato una sola volta se serve. */
    size_t cap = 1u << 24;
    char *line = malloc(cap);
    unsigned char *doc = malloc(cap / 2 + 1);
    if (!line || !doc) { fprintf(stderr, "json_diff: memoria\n"); return 1; }

    int nrec = 0, rc = 0;
    size_t len = 0;
    while (fgets(line, (int)cap, f)) {
        size_t raw = strlen(line);
        if (raw && line[raw - 1] != '\n' && !feof(f)) {
            fprintf(stderr, "json_diff: record %d troppo lungo per il buffer\n", nrec);
            rc = 1;
            break;
        }
        /* Bianco o commento: NON è un record. L'oracolo filtra allo stesso modo
         * (`read_corpus` salta le righe vuote), e finché qui non si saltava, una riga vuota
         * nel corpus diventava un `R n` in più: i numbering dei due lati perdevano il passo e
         * il diff attribuiva a un record l'output del vicino — il "65 livelli accettati"
         * che sembrava un bug del parser era SOLO questo. */
        size_t b = 0;
        while (line[b] == ' ' || line[b] == '\t') b++;
        if (line[b] == '\0' || line[b] == '\n' || line[b] == '\r' || line[b] == '#') continue;
        len = 0;
        if (unhex(line + b, doc, cap / 2 + 1, &len) != 0) {
            fprintf(stderr, "json_diff: riga %d non esadecimale\n", nrec);
            rc = 1;
            break;
        }
        printf("R %d\n", nrec);
        FmlxJsonDoc d;
        fmlx_json_doc_init(&d);
        FmlxJsonStatus st = fmlx_json_parse((const char *)doc, len, 0, &d);
        if (st != FMLX_JSON_OK) {
            printf("E %s\n", code_of(st));
        } else {
            emit(fmlx_json_root(&d));
            roundtrip(fmlx_json_root(&d));
        }
        fmlx_json_doc_free(&d);
        nrec++;
    }
    if (ferror(f)) { fprintf(stderr, "json_diff: lettura\n"); rc = 1; }
    fclose(f);
    free(line);
    free(doc);
    return rc;
}
