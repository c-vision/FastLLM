/* test_json.c — il parser. Ogni caso è qui perché ha UN CODICE D'ERRORE DISTINTIVO, non
 * perché "non deve crashare".
 *
 * Da dove vengono le aspettative: la maggior parte dei casi di rifiuto è stata prima
 * passata da un oracolo indipendente — `json.loads` di Python 3.14, vedi
 * tools/json_diff.c (il vecchio oracolo Python tools/json_oracle.py e' stato rimosso) + docs/json-contract.md — e per ciascuno si è scritta qui la
 * decisione PRESA (che non è sempre "come Python": i float troncati e i controllore nudi
 * sono i due punti dove divergiamo deliberatamente). Un reference scritto da me avrebbe
 * nascosto i bug come è già successo per gli hash GGUF (docs/quant-coverage.md §5).
 */
#include "fmlx/fmlx_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

#define CK(cond, ...)                                                                                            \
    do {                                                                                                         \
        checks++;                                                                                                \
        if (!(cond)) {                                                                                           \
            failures++;                                                                                          \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                                   \
            printf(__VA_ARGS__);                                                                                 \
            printf("\n");                                                                                        \
        }                                                                                                        \
    } while (0)

/* Un documento che si aspettava OK: se fallisce, stampa messaggio e offset — è così che
 * questo test dice qualcosa a chi lo legge, non solo "31 fallito".
 * `doc_init` sta QUI dentro, non nei cinque chiamanti: la `parse` inizia liberando il
 * documento che riceve, e un `FmlxJsonDoc d;` non initializzato le fa liberare puntatori
 * a caso (è esattamente come questo test ha abortito la prima volta). */
static FmlxJsonStatus ok_doc(const char *json, FmlxJsonDoc *d) {
    fmlx_json_doc_init(d);
    FmlxJsonStatus s = fmlx_json_parse(json, strlen(json), 0, d);
    checks++;
    if (s != FMLX_JSON_OK) {
        failures++;
        printf("  FALLIMENTO: %s doveva essere ok -> %s [%s]\n", json, fmlx_json_status_str(s),
               d->err_msg[0] ? d->err_msg : "-");
    }
    return s;
}

static void want_err(const char *json, FmlxJsonStatus want) {
    FmlxJsonDoc d;
    fmlx_json_doc_init(&d);
    FmlxJsonStatus s = fmlx_json_parse(json, strlen(json), 0, &d);
    checks++;
    if (s == want) {
        /* ok: anche il messaggio deve dire qualcosa, mai vuoto su un rifiuto */
        CK(d.err_msg[0] != '\0' || want == FMLX_JSON_ERR_EMPTY || want == FMLX_JSON_ERR_NO_MEM,
           "rifiuto %s (%s) senza messaggio", json, fmlx_json_status_str(want));
    } else {
        failures++;
        printf("  FALLIMENTO: %s -> %s [%s], atteso %s\n", json, fmlx_json_status_str(s),
               d.err_msg[0] ? d.err_msg : "-", fmlx_json_status_str(want));
    }
    fmlx_json_doc_free(&d);
}

static void want_off(const char *json, FmlxJsonStatus want, size_t off) {
    FmlxJsonDoc d;
    fmlx_json_doc_init(&d);
    FmlxJsonStatus s = fmlx_json_parse(json, strlen(json), 0, &d);
    checks++;
    if (s != want || d.err_off != off) {
        failures++;
        printf("  FALLIMENTO: %s -> %s off=%zu [%s], atteso %s off=%zu\n", json,
               fmlx_json_status_str(s), d.err_off, d.err_msg, fmlx_json_status_str(want), off);
    }
    fmlx_json_doc_free(&d);
}

static const FmlxJsonValue *root_of(const char *json, FmlxJsonDoc *d) {
    return ok_doc(json, d) == FMLX_JSON_OK ? fmlx_json_root(d) : NULL;
}

/* ---- 1. forma accettata ---- */
static void test_accept(void) {
    puts("-- accettazione");
    static const char *GOOD[] = {
        "null", "true", "false", "0", "-0", "1", "-1", "1.5", "-1.5", "1e3", "1E3", "1e+3", "1e-3",
        "0e0", "123456789012345678901234567890.5", /* reale con troppe cifre per int: ok come double */
        "\"\"", "\"a\"", "\"\\n\"", "\"\\u00e9\"", "\"\\ud83d\\ude00\"", "[]", "{}", "[1,2,3]",
        "{\"a\":1}", "[[1,2],[3]]", "{\"a\":[1,{\"b\":2}]}", "  {  \"a\" : [ ] }  ",
        "{\"\":1}", "[[]]", "{\"a\":{}}",
    };
    for (size_t i = 0; i < sizeof GOOD / sizeof *GOOD; i++) {
        FmlxJsonDoc d;
        fmlx_json_doc_init(&d);
        ok_doc(GOOD[i], &d);
        fmlx_json_doc_free(&d);
    }
    /* Il tetto di profondità si prova GENERANDO la colonna di parentesi: contarle a mano in
     * una stringa letterale è esattamente il tipo di "sembrava giusto" che non è una prova. */
    for (int deep = 1; deep <= 65; deep++) {
        char buf[160];
        int n = 0;
        for (int i = 0; i < deep; i++) buf[n++] = '[';
        for (int i = 0; i < deep; i++) buf[n++] = ']';
        buf[n] = '\0';
        FmlxJsonDoc d;
        fmlx_json_doc_init(&d);
        FmlxJsonStatus s = fmlx_json_parse(buf, (size_t)n, 0, &d);
        FmlxJsonStatus want = deep <= (int)FMLX_JSON_MAX_DEPTH ? FMLX_JSON_OK : FMLX_JSON_ERR_TOO_DEEP;
        if (s != want) {
            failures++;
            printf("  FALLIMENTO profondita' %d: %s [%s], atteso %s\n", deep,
                   fmlx_json_status_str(s), d.err_msg, fmlx_json_status_str(want));
        }
        checks++;
        if (want == FMLX_JSON_OK) CK(fmlx_json_root(&d) != NULL, "radice a profondita' %d", deep);
        fmlx_json_doc_free(&d);
    }
}

static void test_reject(void) {
    puts("-- rifiuto, con il codice giusto per ciascuna causa");
    want_err("", FMLX_JSON_ERR_EMPTY);
    want_err("   \t\n", FMLX_JSON_ERR_EMPTY);
    want_err("nul", FMLX_JSON_ERR_SYNTAX);
    want_err("truthy", FMLX_JSON_ERR_SYNTAX);
    want_err("{\"a\"", FMLX_JSON_ERR_SYNTAX);
    want_err("{\"a\":1 \"b\":2}", FMLX_JSON_ERR_SYNTAX);
    want_err("[1,]", FMLX_JSON_ERR_SYNTAX);
    want_err("{,}", FMLX_JSON_ERR_SYNTAX);
    want_err("[}", FMLX_JSON_ERR_SYNTAX);
    want_err("{\"a\" 1}", FMLX_JSON_ERR_SYNTAX);
    want_err("\"a", FMLX_JSON_ERR_SYNTAX);
    want_err("\"a\\", FMLX_JSON_ERR_SYNTAX);
    want_err("\"\\x\"", FMLX_JSON_ERR_SYNTAX);
    want_err("\"\\u00zz\"", FMLX_JSON_ERR_SYNTAX);
    want_err("[1] x", FMLX_JSON_ERR_TRAILING);
    want_err("{} {}", FMLX_JSON_ERR_TRAILING);
    want_err("01", FMLX_JSON_ERR_NUMBER);
    want_err("-01", FMLX_JSON_ERR_NUMBER);
    want_err(".5", FMLX_JSON_ERR_NUMBER);
    want_err("5.", FMLX_JSON_ERR_NUMBER);
    want_err("+1", FMLX_JSON_ERR_NUMBER);
    want_err("-", FMLX_JSON_ERR_NUMBER);
    want_err("1e", FMLX_JSON_ERR_NUMBER);
    want_err("1e+", FMLX_JSON_ERR_NUMBER);
    want_err("NaN", FMLX_JSON_ERR_SYNTAX);
    want_err("Infinity", FMLX_JSON_ERR_SYNTAX);
    want_err("99999999999999999999", FMLX_JSON_ERR_NUMBER);
    want_err("-99999999999999999999", FMLX_JSON_ERR_NUMBER);
    want_err("[1e400]", FMLX_JSON_ERR_NUMBER); /* il double non lo rappresenta */
    /* UTF-8: le tre maniere di rompere una stringa, tre offset diversi */
    static const char BAD_UTF8[] = "\"\xff\xfe\"";
    want_err(BAD_UTF8, FMLX_JSON_ERR_UTF8);
    static const char OVERLONG[] = "\"\xc0\x80\"";
    want_err(OVERLONG, FMLX_JSON_ERR_UTF8);
    static const char SURROGATE_CODEPOINT[] = "\"\xed\xa0\x80\""; /* U+D800 in UTF-8: vietato */
    want_err(SURROGATE_CODEPOINT, FMLX_JSON_ERR_UTF8);
    want_err("\"\\ud800\"", FMLX_JSON_ERR_UTF8);
    want_err("\"\\udc00\"", FMLX_JSON_ERR_UTF8);
    want_err("\"\\ud800x\"", FMLX_JSON_ERR_UTF8);
    want_err("\"a\nb\"", FMLX_JSON_ERR_CONTROL); /* \n NUDO inside: le virgolette qui sono fonte */
    /* profondità: 64 livelli ok (vedi test_accept), 65 no */
    {
        char deep[1024];
        size_t n = 0;
        for (int i = 0; i < 65; i++) deep[n++] = '[';
        for (int i = 0; i < 65; i++) deep[n++] = ']';
        deep[n] = '\0';
        FmlxJsonDoc d;
        fmlx_json_doc_init(&d);
        FmlxJsonStatus s = fmlx_json_parse(deep, n, 0, &d);
        CK(s == FMLX_JSON_ERR_TOO_DEEP, "65 livelli -> %s [%s]", fmlx_json_status_str(s), d.err_msg);
        fmlx_json_doc_free(&d);
    }
    /* l'offset deve guardare il byte COLPEVOLE, non la fine del buffer */
    want_off("{\"a\":1,}", FMLX_JSON_ERR_SYNTAX, 7);
    want_off("[1,2,xx]", FMLX_JSON_ERR_SYNTAX, 5);   /* la prima `x`, non la seconda */
    want_off("{\"k\":01}", FMLX_JSON_ERR_NUMBER, 6); /* la cifra di troppo dopo lo zero */
}

/* ---- 2. tipi: la distinzione INT/REAL è il punto, non un dettaglio ---- */
static void test_types(void) {
    puts("-- tipi");
    FmlxJsonDoc d;
    const FmlxJsonValue *v = root_of("1", &d);
    int64_t i;
    double x;
    CK(v && v->type == FMLX_JSON_INT, "1 non e' INT");
    CK(v && fmlx_json_as_i64(v, &i) && i == 1, "as_i64(1)");
    CK(v && fmlx_json_as_f64(v, &x) && x == 1.0, "as_f64(1)");
    fmlx_json_doc_free(&d);

    /* `1.0` RESTA un reale: è la regola per cui `max_tokens: 1.5` non può diventare 1. */
    v = root_of("1.0", &d);
    CK(v && v->type == FMLX_JSON_REAL, "1.0 non e' REAL");
    CK(v && !fmlx_json_as_i64(v, &i), "as_i64 ha accettato un reale (troncherebbe)");
    CK(v && fmlx_json_as_f64(v, &x) && x == 1.0, "as_f64(1.0)");
    fmlx_json_doc_free(&d);

    v = root_of("-9223372036854775808", &d);
    CK(v && fmlx_json_as_i64(v, &i) && i == INT64_MIN, "INT64_MIN");
    fmlx_json_doc_free(&d);
    v = root_of("9223372036854775807", &d);
    CK(v && fmlx_json_as_i64(v, &i) && i == INT64_MAX, "INT64_MAX");
    fmlx_json_doc_free(&d);

    v = root_of("false", &d);
    int b = 1;
    CK(v && fmlx_json_as_bool(v, &b) && b == 0, "false -> 0");
    CK(v && !fmlx_json_as_i64(v, &i), "false non e' un numero");
    fmlx_json_doc_free(&d);
    v = root_of("null", &d);
    CK(v && fmlx_json_is_null(v), "null");
    CK(v && !fmlx_json_as_bool(v, &b), "null non e' false");
    fmlx_json_doc_free(&d);

    /* `as_str` su un numero: NO. E `arr_n` su un oggetto: NO (0, non "2 per membro"). */
    v = root_of("42", &d);
    const char *sp;
    size_t sl;
    CK(v && !fmlx_json_as_str(v, &sp, &sl), "42 non e' una stringa");
    CK(v && fmlx_json_arr_n(v) == 0, "arr_n su uno scalare");
    fmlx_json_doc_free(&d);
    v = root_of("{\"a\":1,\"b\":2}", &d);
    CK(v && fmlx_json_obj_n(v) == 2, "obj_n = membri, non elementi di kids");
    CK(v && fmlx_json_arr_n(v) == 0, "arr_n su un oggetto");
    fmlx_json_doc_free(&d);
}

/* ---- 3. contenuti ---- */
static void test_strings(void) {
    puts("-- stringhe");
    struct { const char *json; const char *want; size_t wlen; } T[] = {
        {"\"\"", "", 0},
        {"\"a\\\"b\"", "a\"b", 3},
        {"\"a\\\\b\"", "a\\b", 3},
        {"\"a\\/b\"", "a/b", 3},
        {"\"\\n\\t\\r\\b\\f\"", "\n\t\r\b\f", 5},
        {"\"\\u00e9\"", "\xc3\xa9", 2},
        {"\"\\u20ac\"", "\xe2\x82\xac", 3},
        {"\"\\ud83d\\ude00\"", "\xf0\x9f\x98\x80", 4}, /* U+1F600, la coppia appaiata */
        {"\"\\u0000\"", "\0", 1},                     /* NUL dentro una stringa: legale in JSON */
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++) {
        FmlxJsonDoc d;
        const FmlxJsonValue *v = root_of(T[i].json, &d);
        const char *p;
        size_t n;
        CK(v && fmlx_json_as_str(v, &p, &n), "%s non letta come stringa", T[i].json);
        CK(v && n == T[i].wlen && !memcmp(p, T[i].want, n), "%s -> «%.*s» (%zu), atteso «%.*s» (%zu)",
           T[i].json, (int)n, p ? p : "", n, (int)T[i].wlen, T[i].want, T[i].wlen);
        fmlx_json_doc_free(&d);
    }
    /* l'arena tiene il NUL di una stringa che ne contiene uno: la LUNGHEZZA è la parola */
    FmlxJsonDoc d;
    const FmlxJsonValue *v = root_of("[\"a\\u0000b\",\"c\"]", &d);
    CK(v && fmlx_json_arr_n(v) == 2, "array con NUL dentro");
    const FmlxJsonValue *e0 = v ? fmlx_json_arr_at(v, 0) : NULL;
    CK(e0 && fmlx_json_str_len(e0) == 3, "lunghezza con NUL");
    fmlx_json_doc_free(&d);
}

/* ---- 4. struttura: il caso che ha rotto la prima bozza ---- */
static void test_structure(void) {
    puts("-- struttura annidata");
    FmlxJsonDoc d;
    /* `[[1,2],[3]]`: i figli dell'array ESTERNO NON sono contigui in `nodes` (postordine).
     * Se un accessorio assumesse la contiguità, questo è il caso che lo mostra. */
    const FmlxJsonValue *v = root_of("[[1,2],[3]]", &d);
    CK(v && fmlx_json_arr_n(v) == 2, "[[1,2],[3]] ha 2 figli");
    const FmlxJsonValue *a0 = v ? fmlx_json_arr_at(v, 0) : NULL;
    const FmlxJsonValue *a1 = v ? fmlx_json_arr_at(v, 1) : NULL;
    CK(a0 && fmlx_json_arr_n(a0) == 2, "primo figlio = [1,2]");
    CK(a1 && fmlx_json_arr_n(a1) == 1, "secondo figlio = [3]");
    int64_t i;
    CK(a0 && fmlx_json_as_i64(fmlx_json_arr_at(a0, 1), &i) && i == 2, "[0][1] == 2");
    CK(a1 && fmlx_json_as_i64(fmlx_json_arr_at(a1, 0), &i) && i == 3, "[1][0] == 3");
    CK(!fmlx_json_arr_at(v, 2), "figlio fuori range -> NULL, non il vicino");
    fmlx_json_doc_free(&d);

    v = root_of("{\"a\":[1,{\"b\":2}],\"c\":null}", &d);
    CK(v && fmlx_json_obj_n(v) == 2, "membri");
    const FmlxJsonValue *arr = v ? fmlx_json_obj_get(v, "a") : NULL;
    CK(arr && fmlx_json_arr_n(arr) == 2, "a = [1,{b:2}]");
    const FmlxJsonValue *o = arr ? fmlx_json_arr_at(arr, 1) : NULL;
    CK(o && fmlx_json_as_i64(o ? fmlx_json_obj_get(o, "b") : NULL, &i) && i == 2, "a[1].b == 2");
    /* ordine dei membri preservato, e le chiavi si leggono con key_at.
     * `memcmp` su `str_len`, non `strcmp`: l'arena NON termina le stringhe, e qui dietro
     * c'è la chiave successiva (`strcmp` su `k0` leggerebbe "bc" e confrontarla con "a"
     * fallirebbe — è precisamente il motivo per cui `fmlx_json_str` vieta il `%s`). */
    const FmlxJsonValue *k0 = v ? fmlx_json_obj_key_at(v, 0) : NULL;
    const FmlxJsonValue *k1 = v ? fmlx_json_obj_key_at(v, 1) : NULL;
    const char *p0 = fmlx_json_str(k0);
    const char *p1 = fmlx_json_str(k1);
    CK(p0 && fmlx_json_str_len(k0) == 1 && p0[0] == 'a', "primo membro = a");
    CK(p1 && fmlx_json_str_len(k1) == 1 && p1[0] == 'c', "secondo membro = c");
    CK(fmlx_json_obj_get(v, "zz") == NULL, "chiave assente -> NULL");
    CK(fmlx_json_is_null(v ? fmlx_json_obj_get(v, "c") : NULL), "c == null");
    fmlx_json_doc_free(&d);

    /* chiave che contiene virgolette e unicode: `obj_get` confronta BYTE unescapeati */
    v = root_of("{\"a\\\"b\":1,\"\\u00e9\":2}", &d);
    CK(v && fmlx_json_obj_get(v, "a\"b") != NULL, "chiave con virgoletta");
    CK(v && fmlx_json_obj_get(v, "\xc3\xa9") != NULL, "chiave con accentata");
    CK(v && fmlx_json_obj_get(v, "ab") == NULL, "chiave diversa non confusa");
    fmlx_json_doc_free(&d);
}

/* ---- 5. chiavi duplicate: segnate, con comportamento definito ---- */
static void test_dup_keys(void) {
    puts("-- chiavi duplicate");
    FmlxJsonDoc d;
    const FmlxJsonValue *v = root_of("{\"max_tokens\":8,\"max_tokens\":4096}", &d);
    CK(v && fmlx_json_obj_n(v) == 2, "entrambi i membri restano visibili");
    CK(d.had_dup_keys == 1, "il duplicato non e' segnato");
    int64_t i;
    const FmlxJsonValue *got = v ? fmlx_json_obj_get(v, "max_tokens") : NULL;
    CK(got && fmlx_json_as_i64(got, &i) && i == 8, "obj_get deve dare la PRIMA (%lld)", (long long)i);
    fmlx_json_doc_free(&d);

    v = root_of("{\"a\":1,\"b\":2}", &d);
    CK(d.had_dup_keys == 0, "falso positivo sui duplicati");
    fmlx_json_doc_free(&d);
    /* duplicati in oggetti ANNIDATI contano comunque */
    v = root_of("{\"x\":{\"y\":1,\"y\":2}}", &d);
    CK(d.had_dup_keys == 1, "duplicato annidato non visto");
    fmlx_json_doc_free(&d);
}

/* ---- 6. limiti e riuso ---- */
static void test_limits(void) {
    puts("-- limiti del chiamante e riuso del documento");
    char big[40000];
    size_t n = 0;
    big[n++] = '[';
    for (int i = 0; i < 4000; i++) n += (size_t)snprintf(big + n, sizeof big - n, i ? ",%d" : "%d", i);
    big[n++] = ']';
    big[n] = '\0';

    FmlxJsonDoc d;
    fmlx_json_doc_init(&d);
    FmlxJsonStatus s = fmlx_json_parse(big, n, 500, &d);
    CK(s == FMLX_JSON_ERR_TOO_BIG, "limite di 500 nodi onorato -> %s", fmlx_json_status_str(s));
    fmlx_json_doc_free(&d);

    /* lo stesso documento riusato: la seconda parse NON deve vedere residui della prima,
     * e `had_dup_keys` non deve restare acceso per sempre */
    s = fmlx_json_parse(big, n, 0, &d);
    CK(s == FMLX_JSON_OK, "4000 elementi senza limite -> %s [%s]", fmlx_json_status_str(s), d.err_msg);
    CK(fmlx_json_arr_n(fmlx_json_root(&d)) == 4000, "tutti e 4000 visibili");
    fmlx_json_doc_free(&d);
    /* la lunghezza si prende dal letterale: `14` contato a mano su una stringa di 13 byte
     * fa leggere il terminatore come parte del documento (e il parser risponde TRAILING,
     * correttamente). */
    static const char DUP[] = "{\"a\":1,\"a\":2}";
    s = fmlx_json_parse(DUP, sizeof DUP - 1, 0, &d);
    CK(s == FMLX_JSON_OK && d.had_dup_keys == 1, "duplicato dopo un documento grande -> %s [%s]",
       fmlx_json_status_str(s), d.err_msg);
    fmlx_json_doc_free(&d);
    s = fmlx_json_parse("{\"a\":1}", 7, 0, &d);
    CK(s == FMLX_JSON_OK && d.had_dup_keys == 0, "il flag del duplicato è stato ritirato");
    fmlx_json_doc_free(&d);

    /* un documento con `len` dichiarato più corto dei byte reali: il parser deve fermarsi
     * a `len` (è così che il server passa il corpo, non una stringa C) */
    fmlx_json_doc_init(&d);
    s = fmlx_json_parse("{\"a\":1}XXXXX", 7, 0, &d);
    CK(s == FMLX_JSON_OK, "len esplicito rispettato -> %s", fmlx_json_status_str(s));
    fmlx_json_doc_free(&d);
    fmlx_json_doc_init(&d);
    s = fmlx_json_parse("{\"a\":1}XXXXX", 12, 0, &d);
    CK(s == FMLX_JSON_ERR_TRAILING, "trailing scoperto con len=12 -> %s", fmlx_json_status_str(s));
    fmlx_json_doc_free(&d);

    /* NULL e vuoti: mai un crash, mai un valore inventato */
    fmlx_json_doc_init(&d);
    CK(fmlx_json_parse(NULL, 0, 0, &d) == FMLX_JSON_ERR_BAD_ARG, "src NULL");
    CK(fmlx_json_root(&d) == NULL, "root su documento vuoto");
    CK(fmlx_json_obj_get(NULL, "a") == NULL && fmlx_json_arr_at(NULL, 0) == NULL,
       "accessori su NULL");
    CK(fmlx_json_status_str(FMLX_JSON_OK) && fmlx_json_status_str((FmlxJsonStatus)999),
       "status_str sempre non-NULL");
    fmlx_json_doc_free(&d);
}

int main(void) {
    puts("== test_json ==");
    test_accept();
    test_reject();
    test_types();
    test_strings();
    test_structure();
    test_dup_keys();
    test_limits();
    printf("\n%d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
