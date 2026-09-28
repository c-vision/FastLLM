/* test_tools.c — il riconoscitore delle chiamate di tool, senza motore e senza rete.
 *
 * Il caso che vale il file è lo SPLIT: un marcatore tagliato fra due delta di token non deve
 * uscire come testo. Poi ci sono le convenzioni delle famiglie (Qwen/Hermes a tag, Llama-3
 * nativo a JSON nudo, `<|python_tag|>`, `<function=…>`), ognuna con la sua forma.
 */
#include "fmlx/fmlx_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CK(cond, ...)                                                                             \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            g_fail++;                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                           \
            printf(__VA_ARGS__);                                                                  \
            printf("\n");                                                                         \
        }                                                                                         \
    } while (0)

static FmlxToolCall *find(FmlxToolCalls *tc, const char *name) {
    for (int i = 0; i < tc->n; i++)
        if (!strcmp(tc->call[i].name, name)) return &tc->call[i];
    return NULL;
}

static void t_parse_qwen(void) {
    printf("== parse Qwen/Hermes <tool_call>\n");
    const char *s = "Let me check.\n<tool_call>\n{\"name\": \"get_weather\", "
                    "\"arguments\": {\"city\": \"Roma\"}}\n</tool_call>\nBye.";
    FmlxToolCalls tc;
    fmlx_tools_parse(s, strlen(s), &tc);
    CK(tc.n == 1, "una chiamata, %d", tc.n);
    FmlxToolCall *c = find(&tc, "get_weather");
    CK(c != NULL, "nome estratto");
    CK(c && c->args && !strcmp(c->args, "{\"city\":\"Roma\"}"), "args compatti: %s",
       c ? c->args : "(null)");
    fmlx_tools_free(&tc);
}

static void t_parse_multi(void) {
    printf("== più chiamate in un blocco\n");
    const char *s = "<tool_call>{\"name\":\"a\",\"arguments\":{\"x\":1}}</tool_call>"
                    "<tool_call>{\"name\":\"b\",\"arguments\":{}}</tool_call>";
    FmlxToolCalls tc;
    fmlx_tools_parse(s, strlen(s), &tc);
    CK(tc.n == 2, "due chiamate, %d", tc.n);
    CK(find(&tc, "a") && find(&tc, "b"), "nomi a e b");
    fmlx_tools_free(&tc);
}

static void t_parse_bare_llama(void) {
    printf("== Llama-3 nativo: JSON nudo\n");
    const char *s = "{\"name\": \"get_weather\", \"parameters\": {\"city\": \"Roma\"}}";
    FmlxToolCalls tc;
    fmlx_tools_parse(s, strlen(s), &tc);
    CK(tc.n == 1 && find(&tc, "get_weather"), "JSON nudo riconosciuto, %d", tc.n);
    fmlx_tools_free(&tc);

    /* JSON nudo che NON è un tool (manca name) non deve diventare una chiamata */
    const char *s2 = "{\"temperature\": 3, \"unit\": \"c\"}";
    fmlx_tools_parse(s2, strlen(s2), &tc);
    CK(tc.n == 0, "JSON senza name non è una chiamata, %d", tc.n);
    fmlx_tools_free(&tc);
}

static void t_parse_python_tag(void) {
    printf("== <|python_tag|>\n");
    const char *s = "<|python_tag|>{\"name\": \"f\", \"arguments\": {\"a\": [1, 2]}}";
    FmlxToolCalls tc;
    fmlx_tools_parse(s, strlen(s), &tc);
    CK(tc.n == 1 && find(&tc, "f"), "python_tag, %d", tc.n);
    CK(tc.n == 1 && !strcmp(tc.call[0].args, "{\"a\":[1,2]}"), "args: %s",
       tc.n == 1 ? tc.call[0].args : "?");
    fmlx_tools_free(&tc);
}

static void t_parse_function_tag(void) {
    printf("== <function=NAME>\n");
    const char *s = "<function=get_weather>{\"city\":\"Roma\"}</function>";
    FmlxToolCalls tc;
    fmlx_tools_parse(s, strlen(s), &tc);
    CK(tc.n == 1 && find(&tc, "get_weather"), "function tag, %d", tc.n);
    CK(tc.n == 1 && !strcmp(tc.call[0].args, "{\"city\":\"Roma\"}"), "args: %s",
       tc.n == 1 ? tc.call[0].args : "?");
    fmlx_tools_free(&tc);
}

static void t_parse_param_xml(void) {
    printf("== <function=NAME><parameter=K>V\n");
    const char *s = "<function=get_weather><parameter=city>Roma</parameter></function>";
    FmlxToolCalls tc;
    fmlx_tools_parse(s, strlen(s), &tc);
    CK(tc.n == 1 && find(&tc, "get_weather"), "param xml, %d", tc.n);
    CK(tc.n == 1 && !strcmp(tc.call[0].args, "{\"city\":\"Roma\"}"), "args: %s",
       tc.n == 1 ? tc.call[0].args : "?");
    fmlx_tools_free(&tc);
}

static void t_no_tool(void) {
    printf("== nessun tool\n");
    const char *s = "Ciao, come stai? Nessuna funzione qui.";
    FmlxToolCalls tc;
    fmlx_tools_parse(s, strlen(s), &tc);
    CK(tc.n == 0, "zero chiamate, %d", tc.n);
    fmlx_tools_free(&tc);
}

static void t_stream_split(void) {
    printf("== stream: marcatore spezzato fra i delta\n");
    FmlxToolStream *st = fmlx_toolstream_new(0);
    char text[512];
    size_t tn = 0;
    int tools = 0, name_seen = 0;
    char args[256];
    size_t an = 0;
    args[0] = '\0';

    const char *deltas[] = {"Ecco", " il risul", "tato:\n<tool_", "call>\n{\"name\": \"get_",
                            "weather\", \"arguments\": {\"city\": \"Roma\"}}\n</tool_", "call>"};
    for (size_t i = 0; i < sizeof deltas / sizeof deltas[0]; i++) {
        FmlxToolOut o;
        fmlx_toolstream_feed(st, deltas[i], strlen(deltas[i]));
        while (fmlx_toolstream_pop(st, &o)) {
            if (o.kind == FMLX_TS_TEXT) {
                memcpy(text + tn, o.text, o.n);
                tn += o.n;
            } else if (o.kind == FMLX_TS_TOOL_NAME) {
                name_seen = !strcmp(o.name, "get_weather");
                tools++;
            } else if (o.kind == FMLX_TS_TOOL_ARGS) {
                memcpy(args + an, o.text, o.n);
                an += o.n;
            }
        }
    }
    fmlx_toolstream_finish(st);
    {
        FmlxToolOut o;
        while (fmlx_toolstream_pop(st, &o)) {
            if (o.kind == FMLX_TS_TEXT) {
                memcpy(text + tn, o.text, o.n);
                tn += o.n;
            } else if (o.kind == FMLX_TS_TOOL_NAME) {
                name_seen = !strcmp(o.name, "get_weather");
                tools++;
            } else if (o.kind == FMLX_TS_TOOL_ARGS) {
                memcpy(args + an, o.text, o.n);
                an += o.n;
            }
        }
    }
    text[tn] = '\0';
    args[an] = '\0';
    CK(!strcmp(text, "Ecco il risultato:\n"), "il testo attorno al tool resta testo: '%s'", text);
    CK(tools == 1 && name_seen, "una chiamata get_weather");
    CK(!strcmp(args, "{\"city\":\"Roma\"}"), "gli args arrivano interi: '%s'", args);
    CK(fmlx_toolstream_ntools(st) == 1, "conteggio chiamate");
    fmlx_toolstream_free(st);
}

static void t_stream_bare_split(void) {
    printf("== stream: JSON nudo spezzato (Llama)\n");
    FmlxToolStream *st = fmlx_toolstream_new(1);
    char args[256];
    size_t an = 0;
    int tools = 0;
    const char *deltas[] = {"{\"name\": \"get_", "weather\", \"parameters\": ", "{\"city\":\"Roma\"}}"};
    for (size_t i = 0; i < sizeof deltas / sizeof deltas[0]; i++) {
        FmlxToolOut o;
        fmlx_toolstream_feed(st, deltas[i], strlen(deltas[i]));
        while (fmlx_toolstream_pop(st, &o)) {
            if (o.kind == FMLX_TS_TOOL_NAME) tools++;
            else if (o.kind == FMLX_TS_TOOL_ARGS) {
                memcpy(args + an, o.text, o.n);
                an += o.n;
            } else if (o.kind == FMLX_TS_TEXT) {
                CK(0, "un JSON nudo non deve uscire come testo: '%.*s'", (int)o.n, o.text);
            }
        }
    }
    fmlx_toolstream_finish(st);
    {
        FmlxToolOut o;
        while (fmlx_toolstream_pop(st, &o)) {
            if (o.kind == FMLX_TS_TOOL_NAME) tools++;
            else if (o.kind == FMLX_TS_TOOL_ARGS) {
                memcpy(args + an, o.text, o.n);
                an += o.n;
            }
        }
    }
    args[an] = '\0';
    CK(tools == 1, "una chiamata, %d", tools);
    CK(!strcmp(args, "{\"city\":\"Roma\"}"), "args: '%s'", args);
    fmlx_toolstream_free(st);
}

static void t_stream_bare_junk(void) {
    printf("== stream: JSON nudo con prefisso spurio (Llama)\n");
    FmlxToolStream *st = fmlx_toolstream_new(1);
    char text[64];
    size_t tn = 0;
    char args[256];
    size_t an = 0;
    int tools = 0;
    const char *s = ">{\"type\":\"function\",\"name\":\"get_weather\",\"parameters\":{"
                    "\"city\":\"Paris\"}}";
    FmlxToolOut o;
    fmlx_toolstream_feed(st, s, strlen(s));
    while (fmlx_toolstream_pop(st, &o)) {
        if (o.kind == FMLX_TS_TOOL_NAME) tools++;
        else if (o.kind == FMLX_TS_TOOL_ARGS) {
            memcpy(args + an, o.text, o.n);
            an += o.n;
        } else if (o.kind == FMLX_TS_TEXT) {
            memcpy(text + tn, o.text, o.n);
            tn += o.n;
        }
    }
    fmlx_toolstream_finish(st);
    while (fmlx_toolstream_pop(st, &o)) {
        if (o.kind == FMLX_TS_TOOL_NAME) tools++;
        else if (o.kind == FMLX_TS_TOOL_ARGS) {
            memcpy(args + an, o.text, o.n);
            an += o.n;
        }
    }
    text[tn] = '\0';
    args[an] = '\0';
    CK(tools == 1, "una chiamata, %d", tools);
    CK(!strcmp(args, "{\"city\":\"Paris\"}"), "args: '%s'", args);
    CK(!strcmp(text, ">"), "il prefisso spurio resta testo: '%s'", text);
    fmlx_toolstream_free(st);
}

int main(void) {
    t_parse_qwen();
    t_parse_multi();
    t_parse_bare_llama();
    t_parse_python_tag();
    t_parse_function_tag();
    t_parse_param_xml();
    t_no_tool();
    t_stream_split();
    t_stream_bare_split();
    t_stream_bare_junk();
    if (g_fail) {
        printf("\n%d VERIFICHE FALLITE\n", g_fail);
        return 1;
    }
    printf("\ntest_tools: tutte le verifiche passate\n");
    return 0;
}
