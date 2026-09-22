/* test_jsonw.c — il serializzatore. Qui ciò che conta non è "esce JSON": è che il writer
 * NON POSSA emettere una risposta a metà, né un testo che non è UTF-8, né una struttura
 * che il suo stesso stato ammetta.
 *
 * I casi sono la lista dei modi in cui una risposta HTTP si corrompe:
 *  - capienza superata a metà oggetto (`Content-Length` che promette più di quanto c'è);
 *  - valore scritto in un oggetto senza chiave, e chiave senza valore;
 *  - una chiave che contiene `"` (la prima versione del writer la scriveva cruda);
 *  - `NaN`/`Infinity`, che C stampa e JSON non ha;
 *  - un `tool_use.input` tronco, che il piano vieta di emettere (§11.4).
 *
 * Sui REALI questo test NON confronta testo se non per tre formati che si possono contare
 * a mano (`0.7`, `1.0`, `1e+300`): per gli altri verifica la PROPRIETÀ — il renderizzato
 * rilegge lo stesso double, e togliere l'ultima cifra no. Le cifre di `%.17g` indovinate
 * sono il tipo di aspettativa che ho già scritto falsa due volte in questi giorni.
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
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                                  \
            printf(__VA_ARGS__);                                                                                 \
            printf("\n");                                                                                        \
        }                                                                                                        \
    } while (0)

#define BIG (1u << 20)
static FmlxJsonW W;

static void expect(const char *want, const char *cosa) {
    size_t n = 0;
    FmlxJsonStatus e = fmlx_jsonw_finish(&W, &n);
    checks++;
    if (e != FMLX_JSON_OK) {
        failures++;
        printf("  FALLIMENTO %s: finish -> %s\n", cosa, fmlx_json_status_str(e));
        fmlx_jsonw_free(&W);
        return;
    }
    CK(n == strlen(want) && !memcmp(W.buf, want, n), "%s:\n    ottenuto %.*s\n    atteso  %s",
       cosa, (int)n, W.buf, want);
    fmlx_jsonw_free(&W);
}

/* Chiude e si aspetta che `finish` dia `want` (l'errore resta nel writer: è il punto). */
static void expect_err(FmlxJsonStatus want, const char *cosa) {
    size_t n = 0;
    FmlxJsonStatus e = fmlx_jsonw_finish(&W, &n);
    checks++;
    if (e != want) {
        failures++;
        printf("  FALLIMENTO %s: finish -> %s, atteso %s\n", cosa, fmlx_json_status_str(e),
               fmlx_json_status_str(want));
    }
    fmlx_jsonw_free(&W);
}

/* ---- 1. forma ---- */
static void test_forma(void) {
    puts("-- forma prodotta");
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "a");
    fmlx_jsonw_arr_begin(&W);
    fmlx_jsonw_i64(&W, 1);
    fmlx_jsonw_i64(&W, 2);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "b");
    fmlx_jsonw_str(&W, "x", 1);
    fmlx_jsonw_obj_end(&W);
    fmlx_jsonw_arr_end(&W);
    fmlx_jsonw_key(&W, "c");
    fmlx_jsonw_null(&W);
    fmlx_jsonw_key(&W, "d");
    fmlx_jsonw_bool(&W, 1);
    fmlx_jsonw_key(&W, "e");
    fmlx_jsonw_arr_begin(&W);
    fmlx_jsonw_arr_end(&W);
    fmlx_jsonw_key(&W, "f");
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_obj_end(&W);
    fmlx_jsonw_key(&W, "z");
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "y");
    fmlx_jsonw_cstr(&W, "1.5");
    fmlx_jsonw_obj_end(&W);
    fmlx_jsonw_obj_end(&W);
    expect("{\"a\":[1,2,{\"b\":\"x\"}],\"c\":null,\"d\":true,\"e\":[],\"f\":{},"
           "\"z\":{\"y\":\"1.5\"}}",
           "oggetto annidato completo");

    /* Radice che NON è un oggetto: il writer non deve darlo per scontato. */
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_arr_begin(&W);
    fmlx_jsonw_i64(&W, -9223372036854775807LL - 1);
    fmlx_jsonw_u64(&W, 18446744073709551615ULL);
    fmlx_jsonw_arr_end(&W);
    expect("[-9223372036854775808,18446744073709551615]", "radice array, bordi int64/uint64");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_str(&W, "", 0);
    expect("\"\"", "stringa vuota come radice");

    /* I due casi che il marcatore "radice emessa" ricavato da `need_comma[0]` sbagliava:
     * un contenitore VUOTO come radice chiudeva a 0 e `finish` rispondeva "nessun valore". */
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_obj_end(&W);
    expect("{}", "oggetto vuoto come radice");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_arr_begin(&W);
    fmlx_jsonw_arr_end(&W);
    expect("[]", "array vuoto come radice");

    /* buffer del chiamante: lo usa senza allocare e NON lo libera */
    char mem[8];
    fmlx_jsonw_init(&W, mem, sizeof mem, BIG);
    fmlx_jsonw_i64(&W, 12345);
    size_t n = 0;
    CK(fmlx_jsonw_finish(&W, &n) == FMLX_JSON_OK && n == 5 && !memcmp(mem, "12345", 5),
       "radice in buffer del chiamante (%s, %zu)", fmlx_json_status_str(W.err), n);
    CK(W.buf == mem, "il writer non deve aver allocato");
    fmlx_jsonw_free(&W);
    CK(W.buf == NULL, "free deve azzerare il writer");
}

/* ---- 2. escaping ---- */
static void test_escape(void) {
    puts("-- escaping e UTF-8");
    static const char VAL[] = "due\nrighe\ttab\\control\x01\x1f";
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "a\"b");
    fmlx_jsonw_str(&W, VAL, strlen(VAL));
    fmlx_jsonw_key(&W, "emoji");
    fmlx_jsonw_str(&W, "\xf0\x9f\x98\x80", 4);
    fmlx_jsonw_key(&W, "slash");
    fmlx_jsonw_str(&W, "a/b", 3);
    fmlx_jsonw_obj_end(&W);
    expect("{\"a\\\"b\":\"due\\nrighe\\ttab\\\\control\\u0001\\u001f\","
           "\"emoji\":\"\xf0\x9f\x98\x80\",\"slash\":\"a/b\"}",
           "chiave con virgolette e controlli nel valore");

    /* Forma CORTA `\n`, come tutti i producer JSON (docs/json-contract.md): 7 byte, non 11 */
    char out[32];
    size_t k = fmlx_json_esc_into("due\n", 4, out, sizeof out);
    CK(k == 7 && !memcmp(out, "\"due\\n\"", 7), "esc_into -> %zu «%s»", k, out);
    memset(out, 0, sizeof out);
    k = fmlx_json_esc_into("due\n", 4, out, 6);
    CK(k == 7 && out[0] == '\0', "esc_into sotto cap NON scrive niente (%zu, «%s»)", k, out);

    /* UTF-8 invalido: RIFIUTATO, non sostituito. Il caso reale è il codepoint spezzato fra
     * due chunk dello streaming: il server tiene i byte, il writer non mente sul testo. */
    static const char BAD[] = {'a', (char)0xed, (char)0xa0, (char)0x80, 'b'};
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "t");
    FmlxJsonStatus e = fmlx_jsonw_str(&W, BAD, sizeof BAD);
    CK(e == FMLX_JSON_ERR_UTF8, "surrogate codificato -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_obj_end(&W);
    expect_err(FMLX_JSON_ERR_UTF8, "valore con UTF-8 invalido");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "\xff");
    fmlx_jsonw_null(&W);
    fmlx_jsonw_obj_end(&W);
    expect_err(FMLX_JSON_ERR_UTF8, "chiave con UTF-8 invalido");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    e = fmlx_jsonw_str(&W, "\xf0\x9f\x98", 3);
    CK(e == FMLX_JSON_ERR_UTF8, "codepoint troncato -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    CK(fmlx_json_utf8_ok("\xc3\xa9", 2) == 1, "accentata valida");
    CK(fmlx_json_utf8_ok("\xc3", 1) == 0, "inizio di sequenza da sola non e' valida");
    CK(fmlx_json_utf8_ok("", 0) == 1, "vuota valida");
    CK(fmlx_json_utf8_ok("\xf4\x90\x80\x80", 4) == 0, "oltre U+10FFFF non valida");
    CK(fmlx_json_utf8_ok("\xc0\x80", 2) == 0, "sovrallungamento non valido");
    CK(fmlx_json_utf8_ok("\x80", 1) == 0, "byte di continuazione solo");

    /* chiave con NUL dentro: `keyn` la scrive intera (una `strlen` la taglierebbe) */
    static const char KEY[] = {'a', '\0', 'b'};
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    e = fmlx_jsonw_keyn(&W, KEY, 3);
    CK(e == FMLX_JSON_OK, "keyn con NUL -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_i64(&W, 1);
    fmlx_jsonw_obj_end(&W);
    expect("{\"a\\u0000b\":1}", "chiave con NUL resa come \\u0000");
}

/* ---- 3. numeri ---- */
static void test_numeri(void) {
    puts("-- numeri");
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_arr_begin(&W);
    fmlx_jsonw_f64(&W, 0.7);
    fmlx_jsonw_f64(&W, 1.0);
    fmlx_jsonw_f64(&W, 1e300);
    fmlx_jsonw_arr_end(&W);
    expect("[0.7,1.0,1e+300]", "i tre formati che si possono contare a mano");

    /* Per gli altri: la PROPRIETA' del piu' corto che round-trippa, non il testo. */
    static const double V[] = {0.1 + 0.2, 3.141592653589793, 1e-300, 2.2250738585072014e-308,
                               1.7976931348623157e308, -0.0, 1.0 / 3.0, 123456789012345678.0};
    for (size_t i = 0; i < sizeof V / sizeof *V; i++) {
        fmlx_jsonw_init(&W, NULL, 0, BIG);
        FmlxJsonStatus e = fmlx_jsonw_f64(&W, V[i]);
        size_t n = 0;
        if (e == FMLX_JSON_OK) e = fmlx_jsonw_finish(&W, &n);
        checks += 2;
        if (e != FMLX_JSON_OK) {
            failures += 2;
            printf("  FALLIMENTO f64 %.17g -> %s\n", V[i], fmlx_json_status_str(e));
            fmlx_jsonw_free(&W);
            continue;
        }
        double back = 0;
        FmlxJsonDoc d;
        fmlx_json_doc_init(&d);
        int ok = fmlx_json_parse(W.buf, n, 0, &d) == FMLX_JSON_OK &&
                 fmlx_json_as_f64(fmlx_json_root(&d), &back) && back == V[i];
        CK(ok, "round-trip di %.17g -> «%.*s» -> %.17g", V[i], (int)n, W.buf, back);
        fmlx_json_doc_free(&d);
        /* il più corto: togliere l'ultima cifra deve dare un altro numero o non numero */
        char shorter[64];
        if (n > 1 && n < sizeof shorter) {
            memcpy(shorter, W.buf, n - 1);
            shorter[n - 1] = '\0';
            double other = 0;
            FmlxJsonDoc d2;
            fmlx_json_doc_init(&d2);
            int parses = fmlx_json_parse(shorter, n - 1, 0, &d2) == FMLX_JSON_OK &&
                         fmlx_json_as_f64(fmlx_json_root(&d2), &other);
            CK(!parses || other != V[i], "%.17g: «%s» era gia' sufficiente", V[i], shorter);
            fmlx_json_doc_free(&d2);
        }
        fmlx_jsonw_free(&W);
    }

    /* NaN e Inf non esistono in JSON: errore, mai "null" e mai la parola che stampa C */
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "x");
    FmlxJsonStatus e = fmlx_jsonw_f64(&W, 0.0 / 0.0);
    CK(e == FMLX_JSON_ERR_NUMBER, "NaN -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_obj_end(&W);
    expect_err(FMLX_JSON_ERR_NUMBER, "NaN nel valore");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    e = fmlx_jsonw_f64(&W, 1.0 / 0.0);
    CK(e == FMLX_JSON_ERR_NUMBER, "Inf -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    e = fmlx_jsonw_f64(&W, -(1.0 / 0.0));
    CK(e == FMLX_JSON_ERR_NUMBER, "-Inf -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);
}

/* ---- 4. la macchina degli stati ---- */
static void test_stato(void) {
    puts("-- la macchina degli stati non ammette uso scorretto");
    FmlxJsonStatus e;

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    e = fmlx_jsonw_i64(&W, 1);
    CK(e == FMLX_JSON_ERR_BAD_ARG, "valore senza chiave -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_obj_end(&W);
    expect_err(FMLX_JSON_ERR_BAD_ARG, "membro scritto senza chiave");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    CK(fmlx_jsonw_key(&W, "a") == FMLX_JSON_OK, "prima chiave");
    e = fmlx_jsonw_key(&W, "b");
    CK(e == FMLX_JSON_ERR_BAD_ARG, "chiave due volte di seguito -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    e = fmlx_jsonw_key(&W, "a");
    CK(e == FMLX_JSON_ERR_BAD_ARG, "chiave a depth 0 -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "a");
    e = fmlx_jsonw_obj_end(&W);
    CK(e == FMLX_JSON_ERR_BAD_ARG, "chiusura con chiave in sospeso -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    /* `{"a":[1` chiuso da `}`, e poi `finish`: l'errore deve RESTARE */
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "a");
    fmlx_jsonw_arr_begin(&W);
    fmlx_jsonw_i64(&W, 1);
    e = fmlx_jsonw_obj_end(&W);
    CK(e == FMLX_JSON_ERR_BAD_ARG, "] chiuso da } -> %s", fmlx_json_status_str(e));
    size_t n = 0;
    e = fmlx_jsonw_finish(&W, &n);
    CK(e == FMLX_JSON_ERR_BAD_ARG, "l'errore non si autoguarisce -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_i64(&W, 1);
    e = fmlx_jsonw_i64(&W, 2);
    CK(e == FMLX_JSON_ERR_BAD_ARG, "seconda radice -> %s", fmlx_json_status_str(e));
    expect_err(FMLX_JSON_ERR_BAD_ARG, "la radice doppia non passa il finish");

    /* e il caso simmetrico, che `need_comma[0]` lasciava passare: `{}` poi `1` */
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_obj_end(&W);
    e = fmlx_jsonw_i64(&W, 1);
    CK(e == FMLX_JSON_ERR_BAD_ARG, "seconda radice dopo {} -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    expect_err(FMLX_JSON_ERR_SYNTAX, "contenitore non chiuso");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    expect_err(FMLX_JSON_ERR_EMPTY, "writer senza alcun valore");

    /* Il cap di annidamento del writer è FMLX_JSON_MAX_DEPTH, LO STESSO del parser: non un
     * numero a parte. Il test lo deriva dalla costante perché "40" qui dentro era un 32
     * travestito — e un documento legale da 64 livelli non round-trippava. */
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    FmlxJsonStatus last = FMLX_JSON_OK;
    for (uint32_t i = 0; i < FMLX_JSON_MAX_DEPTH; i++) last = fmlx_jsonw_arr_begin(&W);
    CK(last == FMLX_JSON_OK, "%u annidamenti -> %s", FMLX_JSON_MAX_DEPTH,
       fmlx_json_status_str(last));
    last = fmlx_jsonw_arr_begin(&W);
    CK(last == FMLX_JSON_ERR_TOO_DEEP, "il livello %u oltre il cap -> %s",
       FMLX_JSON_MAX_DEPTH + 1u, fmlx_json_status_str(last));
    /* Il rifiuto avvelena il writer e il veleno resta: `finish` deve fallire, non emettere
     * 64 parentesi aperte. È la garanzia di sempre — "mai una risposta a metà" — che qui
     * si applica al livello troppo profondo come al buffer pieno. */
    size_t dopo = W.len;
    fmlx_jsonw_arr_end(&W);
    CK(W.len == dopo, "dopo il rifiuto il buffer si e' mosso di %zu byte", W.len - dopo);
    expect_err(FMLX_JSON_ERR_TOO_DEEP, "il writer rifiutato non chiude");
}

/* ---- 5. capienza ---- */
static void test_capienza(void) {
    puts("-- capienza: mai una risposta a meta'");
    char mem[16];
    fmlx_jsonw_init(&W, mem, sizeof mem, 64u << 10);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "unachiavelunga");
    fmlx_jsonw_str(&W, "un valore lungo anch'esso", 23);
    fmlx_jsonw_obj_end(&W);
    size_t n = 0;
    FmlxJsonStatus e = fmlx_jsonw_finish(&W, &n);
    CK(e == FMLX_JSON_ERR_BUF_SMALL, "buffer del chiamante pieno -> %s", fmlx_json_status_str(e));
    CK(n == 0, "in errore la lunghezza non e' affidabile");
    size_t prima = W.len;
    fmlx_jsonw_key(&W, "x");
    fmlx_jsonw_i64(&W, 1);
    fmlx_jsonw_obj_begin(&W);
    CK(W.len == prima, "dopo l'errore il buffer si e' mosso di %zu byte", W.len - prima);
    CK(W.buf == mem, "il writer NON deve aver reallocato la memoria del chiamante");
    fmlx_jsonw_free(&W);

    fmlx_jsonw_init(&W, NULL, 0, 40);
    fmlx_jsonw_arr_begin(&W);
    for (int i = 0; i < 100; i++) fmlx_jsonw_i64(&W, i);
    fmlx_jsonw_arr_end(&W);
    e = fmlx_jsonw_finish(&W, &n);
    CK(e == FMLX_JSON_ERR_BUF_SMALL, "max 40 byte per 100 numeri -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    /* e sotto lo stesso limite la stessa cosa passa: non proviamo solo i rifiuti */
    fmlx_jsonw_init(&W, NULL, 0, 4096);
    fmlx_jsonw_arr_begin(&W);
    for (int i = 0; i < 100; i++) fmlx_jsonw_i64(&W, i);
    fmlx_jsonw_arr_end(&W);
    e = fmlx_jsonw_finish(&W, &n);
    CK(e == FMLX_JSON_OK && n > 200, "100 numeri sotto max -> %s (%zu byte)",
       fmlx_json_status_str(e), n);
    fmlx_jsonw_free(&W);
}

/* ---- 6. raw e render ---- */
static int same_tree(const FmlxJsonValue *a, const FmlxJsonValue *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case FMLX_JSON_NULL: return 1;
    case FMLX_JSON_BOOL: return a->v.b == b->v.b;
    case FMLX_JSON_INT: return a->v.i == b->v.i;
    case FMLX_JSON_REAL: return a->v.f == b->v.f;
    case FMLX_JSON_STR:
    case FMLX_JSON_KEY:
        return a->str_len == b->str_len &&
               !memcmp(a->d->arena + a->str_off, b->d->arena + b->str_off, a->str_len);
    case FMLX_JSON_ARR: {
        if (fmlx_json_arr_n(a) != fmlx_json_arr_n(b)) return 0;
        for (size_t i = 0; i < fmlx_json_arr_n(a); i++)
            if (!same_tree(fmlx_json_arr_at(a, i), fmlx_json_arr_at(b, i))) return 0;
        return 1;
    }
    case FMLX_JSON_OBJ: {
        if (fmlx_json_obj_n(a) != fmlx_json_obj_n(b)) return 0;
        for (size_t i = 0; i < fmlx_json_obj_n(a); i++)
            if (!same_tree(fmlx_json_obj_key_at(a, i), fmlx_json_obj_key_at(b, i)) ||
                !same_tree(fmlx_json_obj_value_at(a, i), fmlx_json_obj_value_at(b, i)))
                return 0;
        return 1;
    }
    }
    return 0;
}

static void test_raw_render(void) {
    puts("-- raw e render");
    FmlxJsonStatus e;
    /* La lunghezza si deriva dal letterale, non a mano: `23` contato a occhio su una
     * stringa di 21 byte fa leggere i due byte oltre il terminatore, e il parser risponde
     * TRAILING per colpa del test, non colpa sua. */
    static const char GOODRAW[] = "{\"city\":\"Roma\",\"n\":2}";
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "input");
    e = fmlx_jsonw_raw(&W, GOODRAW, sizeof GOODRAW - 1);
    CK(e == FMLX_JSON_OK, "raw valido -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_obj_end(&W);
    expect("{\"input\":{\"city\":\"Roma\",\"n\":2}}", "oggetto grezzo incorporato");

    /* il caso del piano: gli argomenti di un tool TRONCATI a metà non devono poter uscire,
     * e il writer deve restare in errore (nessuna risposta "aggiustata" a metà) */
    fmlx_jsonw_init(&W, NULL, 0, BIG);
    fmlx_jsonw_obj_begin(&W);
    fmlx_jsonw_key(&W, "input");
    e = fmlx_jsonw_raw(&W, "{\"city\":\"Ro", 12);
    CK(e != FMLX_JSON_OK, "raw tronco accettato!");
    fmlx_jsonw_key(&W, "dopo");
    fmlx_jsonw_null(&W);
    fmlx_jsonw_obj_end(&W);
    expect_err(e, "dopo un raw rifiutato il finish non riesce");

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    e = fmlx_jsonw_raw(&W, "1 2", 3);
    CK(e == FMLX_JSON_ERR_TRAILING, "raw con trailing -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    fmlx_jsonw_init(&W, NULL, 0, BIG);
    e = fmlx_jsonw_raw(&W, "", 0);
    CK(e == FMLX_JSON_ERR_EMPTY, "raw vuoto -> %s", fmlx_json_status_str(e));
    fmlx_jsonw_free(&W);

    /* render: il passaggio OpenAI(stringa) <-> Anthropic(oggetto) del piano §11.4. Si parsea
     * una volta e si renderizza nell'altra forma, mai manipolando le virgolette a mano.
     * `OUT[i]` è la forma che il NOSTRO writer produce: NON sempre il byte-for-byte di
     * `SRC[i]`, perché `1e300` si scrive `1e+300` e una coppia di surrogate si scrive come
     * UTF-8. Le due forme sono lo STESSO JSON; l'uguaglianza testuale qui sotto vale solo
     * dove il sorgente è già nella forma canonica del writer. */
    static const char *SRC[] = {
        "{\"a\":[1,2,{\"b\":\"x\"}],\"c\":null,\"d\":true,\"e\":\"\xc3\xa9\",\"f\":1.5}",
        "{\"a\\\"b\":\"c\\\"d\"}",
        "[[],{},[[]]]",
        "{\"n\":-9223372036854775808,\"big\":1e300}",
        "{\"\":{\"\":[]}}",
        "\"\\ud83d\\ude00\"",
        "{\"arguments\":\"{\\\"city\\\":\\\"Roma\\\",\\\"n\\\":2}\"}",
        "[1,-0.5,\"x\",[],{}]",
    };
    static const char *OUT[] = {
        "{\"a\":[1,2,{\"b\":\"x\"}],\"c\":null,\"d\":true,\"e\":\"\xc3\xa9\",\"f\":1.5}",
        "{\"a\\\"b\":\"c\\\"d\"}",
        "[[],{},[[]]]",
        "{\"n\":-9223372036854775808,\"big\":1e+300}",
        "{\"\":{\"\":[]}}",
        "\"\xf0\x9f\x98\x80\"",
        "{\"arguments\":\"{\\\"city\\\":\\\"Roma\\\",\\\"n\\\":2}\"}",
        "[1,-0.5,\"x\",[],{}]",
    };
    for (size_t i = 0; i < sizeof SRC / sizeof *SRC; i++) {
        FmlxJsonDoc d, d2;
        fmlx_json_doc_init(&d);
        fmlx_json_doc_init(&d2);
        e = fmlx_json_parse(SRC[i], strlen(SRC[i]), 0, &d);
        checks++;
        if (e != FMLX_JSON_OK) {
            failures++;
            printf("  FALLIMENTO render %zu: parse -> %s [%s]\n", i, fmlx_json_status_str(e),
                   d.err_msg);
            continue;
        }
        fmlx_jsonw_init(&W, NULL, 0, BIG);
        e = fmlx_json_render(fmlx_json_root(&d), &W);
        size_t n = 0;
        if (e == FMLX_JSON_OK) e = fmlx_jsonw_finish(&W, &n);
        checks++;
        if (e != FMLX_JSON_OK) {
            failures++;
            printf("  FALLIMENTO render %zu: -> %s\n", i, fmlx_json_status_str(e));
            fmlx_jsonw_free(&W);
            fmlx_json_doc_free(&d);
            continue;
        }
        e = fmlx_json_parse(W.buf, n, 0, &d2);
        checks += 2;
        if (e != FMLX_JSON_OK) {
            failures += 2;
            printf("  FALLIMENTO render %zu: reparse di «%.*s» -> %s [%s]\n", i, (int)n, W.buf,
                   fmlx_json_status_str(e), d2.err_msg);
        } else {
            CK(same_tree(fmlx_json_root(&d), fmlx_json_root(&d2)), "render %zu non fedele", i);
            /* e il TESTO deve essere quello canonico: ordine dei membri preservato, numeri
             * nella forma più corta, surrogate ricodificate in UTF-8 */
            CK(n == strlen(OUT[i]) && !memcmp(W.buf, OUT[i], n), "render %zu: «%.*s» != «%s»", i,
               (int)n, W.buf, OUT[i]);
        }
        fmlx_jsonw_free(&W);
        fmlx_json_doc_free(&d);
        fmlx_json_doc_free(&d2);
    }

    /* `render` di un nodo CHIAVE come se fosse un valore: errore, non `"a"` */
    FmlxJsonDoc d;
    fmlx_json_doc_init(&d);
    if (fmlx_json_parse("{\"k\":1}", 7, 0, &d) == FMLX_JSON_OK) {
        const FmlxJsonValue *root = fmlx_json_root(&d);
        const FmlxJsonValue *k = root ? fmlx_json_obj_key_at(root, 0) : NULL;
        fmlx_jsonw_init(&W, NULL, 0, BIG);
        e = fmlx_json_render(k, &W);
        CK(e == FMLX_JSON_ERR_BAD_ARG, "render di una chiave -> %s", fmlx_json_status_str(e));
        fmlx_jsonw_free(&W);
        fmlx_jsonw_init(&W, NULL, 0, BIG);
        e = fmlx_json_render(NULL, &W);
        CK(e == FMLX_JSON_ERR_BAD_ARG, "render di NULL -> %s", fmlx_json_status_str(e));
        fmlx_jsonw_free(&W);
    }
    fmlx_json_doc_free(&d);
}

int main(void) {
    puts("== test_jsonw ==");
    test_forma();
    test_escape();
    test_numeri();
    test_stato();
    test_capienza();
    test_raw_render();
    printf("\n%d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
