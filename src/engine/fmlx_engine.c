/* fmlx_engine.c — risoluzione del modello, cache residente, tokenizer, eos, chat template.
 *
 * La risoluzione è deliberatamente TOLLERANTE su COME il client nomina il modello, ma
 * STRETTA su cosa accetta come directory: un path esplicito va bene; altrimenti si prova il
 * cache HF (layout `models--org--name`); altrimenti le directory di `fastllm paths`
 * ($FASTLLM_HOME/paths). NON esiste un default implicito legato a una macchina. Ogni fallimento
 * ha la sua frase: "non ho trovato il modello" e "l'ho trovato ma non carica" sono due da-fare
 * diversi.
 */
#include "fmlx_engine_internal.h"

#include "llmengine_mlx.h" /* llm_ai_mlx_gguf_tokenizer_json: tokenizer DS4 dal GGUF */

#include "fmlx/fmlx_hf.h"
#include "fmlx/fmlx_json.h"
#include "fmlx/fmlx_paths.h" /* percorsi di ricerca da file di config ($FASTLLM_HOME/paths) */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_host.h>
#endif
#include <dlfcn.h>

/* Risolve un accessor d'engine (`llmengine_llm_engine_*`) per NOME: il consumatore NON linka
 * hard i simboli degli engine opzionali (ds4 vendored, ds4-slow, ds41), cosi' le loro cartelle
 * restano cancellabili senza errori di compilazione/link. Fallback: l'engine MLX. */
static const LlmEngineLlmEngine* fmlx_engine_symbol(const char* name) {
    const LlmEngineLlmEngine* (*fn)(void);
    *(void**)(&fn) = dlsym(RTLD_DEFAULT, name);
    return fn ? fn() : NULL;
}

/* Offload esperti: `--offload-experts` (CLI) o attivato dal prompt Y/N (vedi sotto). */
static int g_offload_experts = 0;
void fmlx_engine_set_offload_experts(int on) { g_offload_experts = on ? 1 : 0; }

/* THINKING. Regola: DISATTIVARLO sempre, se il modello lo permette. Nei chat template dei Qwen il
 * flag `enable_thinking=false` pre-riempie `<think>\n\n</think>\n\n` -> niente ragionamento. Lo si
 * inietta SEMPRE nel render Jinja (i template che non lo usano lo ignorano). `--think` lo riattiva. */
static int g_think = 0;
void fmlx_engine_set_think(int on) { g_think = on ? 1 : 0; }

/* Contesto/KV a runtime per QUALUNQUE modello (CAP_KV_MAX del motore). 0 = ok. */
int fmlx_engine_set_ctx(FmlxEngineModel *m, int n) {
    if (!m || !m->eng) return -1;
    if (!m->eng->kv_set_max) return -2;   /* motore senza la capability */
    m->ctx = n;
    return m->eng->kv_set_max(m->model, n);
}

/* Nuova sessione: riapplica il contesto -> il motore rialloca/azzera la cache KV (per DS4 il KV si
 * svuota davvero; per gli altri l'effetto e' funzionale col clear della cronologia lato CLI). */
int fmlx_engine_new_session(FmlxEngineModel *m) {
    if (!m) return -1;
    return fmlx_engine_set_ctx(m, m->ctx);
}

/* Registro opzioni del motore (parametri senza env): inoltra all'API del motore. */
int fmlx_engine_opt_set(const char *name, const char *value) {
    return llm_ai_mlx_opt_set(name, value);
}
int fmlx_engine_opt_get_bool(const char *name, int *out) {
    return llm_ai_mlx_opt_get_bool(name, out);
}
int fmlx_engine_opt_get_int(const char *name, int *out) {
    return llm_ai_mlx_opt_get_int(name, out);
}
int fmlx_engine_opt_count(void) { return llm_ai_mlx_opt_count(); }
int fmlx_engine_opt_at(int i, const char **name, const char **desc, char *value, int cap) {
    return llm_ai_mlx_opt_at(i, name, desc, value, cap);
}
int fmlx_engine_opt_example(int i, char *buf, int cap) {
    return llm_ai_mlx_opt_example(i, buf, cap);
}
int fmlx_engine_think_enabled(void) { return g_think; }

/* 1 = sessione interattiva (run/chat da terminale): abilita il PROMPT Y/N per l'offload. Il server
 * (`serve`) NON lo imposta: un server non deve bloccarsi su una domanda. */
static int g_interactive = 0;
void fmlx_engine_set_interactive(int on) { g_interactive = on ? 1 : 0; }

/* Memoria unificata DISPONIBILE **e reclamabile** (pagina libera + inattiva + purgeable +
 * speculative), come la vede la VM. 0 se non determinabile. */
static size_t available_memory_bytes(void) {
#ifdef __APPLE__
    mach_port_t host = mach_host_self();
    vm_size_t page = 0;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

    if (host_page_size(host, &page) != KERN_SUCCESS) return 0;
    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) return 0;
    {
        uint64_t pages = (uint64_t)vm.free_count + (uint64_t)vm.inactive_count +
                         (uint64_t)vm.purgeable_count + (uint64_t)vm.speculative_count;
        return (size_t)(pages * (uint64_t)page);
    }
#else
    return 0;
#endif
}

/* Impronta su disco del modello: per un `.gguf` la sua dimensione; per una dir MLX la somma dei
 * file `*.safetensors` (i pesi). 0 se non determinabile. */
static size_t model_footprint_bytes(const char *dir) {
    struct stat st;

    if (!dir || !*dir) return 0;
    if (stat(dir, &st) == 0 && S_ISREG(st.st_mode)) return (size_t)st.st_size;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        size_t total = 0;
        if (!d) return 0;
        while ((e = readdir(d)) != NULL) {
            size_t nl = strlen(e->d_name);
            char p[FMLX_ENGINE_PATH_MAX];
            struct stat fs;
            if (nl < 12 || strcmp(e->d_name + nl - 12, ".safetensors")) continue;
            if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
            if (stat(p, &fs) == 0 && S_ISREG(fs.st_mode)) total += (size_t)fs.st_size;
        }
        closedir(d);
        return total;
    }
}

/* Se il modello supera il 50% della memoria libera+reclamabile e l'utente NON ha già chiesto
 * `--offload-experts`, chiede SÌ/NO (una volta). Default = SÌ (invio vuoto attiva). Solo su
 * terminale interattivo (run/chat): `serve` non deve bloccarsi su una domanda. */
static void maybe_prompt_offload(const char *dir) {
    size_t need, avail;

    if (g_offload_experts || !g_interactive || !isatty(STDIN_FILENO)) return;
    /* DS4 (GGUF "deepseek4"): --offload-experts non si applica a questo motore -> nessun prompt. */
    if (llm_ai_mlx_gguf_supported(dir) == 1) return;
    need = model_footprint_bytes(dir);
    avail = available_memory_bytes();
    if (need == 0 || avail == 0 || need <= avail / 2) return;
    fprintf(stderr,
            "\nIl modello pesa %.1f GiB — oltre il 50%% della memoria disponibile (~%.1f GiB).\n"
            "Attivare --offload-experts (streaming degli esperti MoE da disco, più lento ma entra)? [Y/n] ",
            (double)need / 1073741824.0, (double)avail / 1073741824.0);
    fflush(stderr);
    {
        char line[32];
        if (fgets(line, sizeof line, stdin) == NULL || (line[0] != 'n' && line[0] != 'N')) {
            g_offload_experts = 1;
            fprintf(stderr, "--offload-experts: attivato.\n");
        } else {
            fprintf(stderr, "offload non attivato (il load potrebbe non entrare in memoria).\n");
        }
    }
}

/* ---- stringa dinamica ---- */

int fmlx_sb_add(FmlxSb *b, const char *s, size_t n) {
    if (!b) return -1;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) return -1;
        b->p = p;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

int fmlx_sb_cstr(FmlxSb *b, const char *s) { return fmlx_sb_add(b, s, s ? strlen(s) : 0); }

void fmlx_sb_free(FmlxSb *b) {
    if (!b) return;
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ---- file ---- */

static int path_is_dir(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_is_file(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* 1 se il path è un GGUF single-file (ds4/DeepSeek V4 Flash). Un modello MLX è una
 * DIRECTORY; il GGUF è un FILE: i due convivono in questa lane. */
static int is_gguf_path(const char *p) {
    size_t n = p ? strlen(p) : 0;
    return n > 5 && strcmp(p + n - 5, ".gguf") == 0;
}

/* DS4.1: directory con config.json che dichiara `"deepseek_v41"` (release HF root). Rilevato
 * dalla firma, non dal nome: instrada al motore `ds41-native` (stessi 9 metodi dell'ABI). */
static int is_dsv41_dir(const char *dir) {
    char p[4096];
    FILE *f;
    long n;
    char *b;
    int hit = 0;
    if (!dir || !*dir) return 0;
    if ((size_t)snprintf(p, sizeof p, "%s/config.json", dir) >= sizeof p) return 0;
    f = fopen(p, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || n > (1 << 20) || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    b = (char *)malloc((size_t)n + 1);
    if (b) {
        size_t rd = fread(b, 1, (size_t)n, f);
        b[rd] = 0;
        hit = strstr(b, "\"deepseek_v41\"") != NULL;
        free(b);
    }
    fclose(f);
    return hit;
}

/* Legge un file intero in una stringa malloc (NUL-terminata). NULL su errore. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = (size_t)n;
    return buf;
}

/* Espande un `~` iniziale e copia in `out`. 0 = ok. */
static int expand_tilde(const char *in, char *out, size_t cap) {
    const char *home;
    int n;

    if (!in || !*in) return -1;
    if (in[0] != '~') {
        n = snprintf(out, cap, "%s", in);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    home = getenv("HOME");
    if (!home || !*home) return -1;
    n = snprintf(out, cap, "%s%s", home, in + 1);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* toglie l'ultimo componente a `path` in place */
static void dir_name(char *path) {
    char *s = strrchr(path, '/');
    if (s && s != path)
        *s = '\0';
    else if (s == path)
        path[1] = '\0';
}

static void set_err(char *err, size_t errcap, const char *fmt, const char *a) {
    if (err && errcap) snprintf(err, errcap, fmt, a ? a : "");
}

/* ---- eos: config.json, poi generation_config.json ---- */

/* Riempie `out` con la lista eos (al più FMLX_ENGINE_EOS_MAX) e ritorna il numero letti.
 * In config.json/generation_config.json `eos_token_id` può essere un intero o una LISTA:
 * la lista HF significa "fermati su UNO QUALSIASI". L'ABI MLX ne accetta uno solo: si
 * conserva l'INTERA lista, e il consumer fa scattare gli eos secondari token per token. */
static int read_eos_list(const char *dir, int32_t *out, int cap) {
    char path[FMLX_ENGINE_PATH_MAX];
    const char *files[] = {"config.json", "generation_config.json"};
    int n = 0;

    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char *txt;
        size_t len;
        FmlxJsonDoc doc;
        const FmlxJsonValue *root, *v;

        if (snprintf(path, sizeof path, "%s/%s", dir, files[i]) >= (int)sizeof path) continue;
        txt = read_file(path, &len);
        if (!txt) continue;
        fmlx_json_doc_init(&doc);
        if (fmlx_json_parse(txt, len, 0, &doc) != FMLX_JSON_OK) {
            fmlx_json_doc_free(&doc);
            free(txt);
            continue;
        }
        root = fmlx_json_root(&doc);
        v = fmlx_json_obj_get(root, "eos_token_id");
        if (v) {
            int64_t x;
            if (fmlx_json_as_i64(v, &x)) {
                if (x >= 0 && x <= INT32_MAX && n < cap) out[n++] = (int32_t)x;
            } else if (v->type == FMLX_JSON_ARR) {
                size_t na = fmlx_json_arr_n(v);
                for (size_t k = 0; k < na && n < cap; k++) {
                    const FmlxJsonValue *e = fmlx_json_arr_at(v, k);
                    int64_t x2;
                    if (e && fmlx_json_as_i64(e, &x2) && x2 >= 0 && x2 <= INT32_MAX)
                        out[n++] = (int32_t)x2;
                }
            }
        }
        fmlx_json_doc_free(&doc);
        free(txt);
        if (n > 0) break;
    }
    return n;
}

/* Legge `bos_token` (stringa o nodo {content}) da tokenizer_config.json / config.json.
 * Solo la forma stringa viene copiata: la template la rende come testo. */
static void read_bos(const char *dir, char *out, size_t cap) {
    char path[FMLX_ENGINE_PATH_MAX];
    const char *files[] = {"tokenizer_config.json", "config.json"};

    out[0] = '\0';
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char *txt;
        size_t len;
        FmlxJsonDoc doc;
        const FmlxJsonValue *root, *v;

        if (snprintf(path, sizeof path, "%s/%s", dir, files[i]) >= (int)sizeof path) continue;
        txt = read_file(path, &len);
        if (!txt) continue;
        fmlx_json_doc_init(&doc);
        if (fmlx_json_parse(txt, len, 0, &doc) != FMLX_JSON_OK) {
            fmlx_json_doc_free(&doc);
            free(txt);
            continue;
        }
        root = fmlx_json_root(&doc);
        v = fmlx_json_obj_get(root, "bos_token");
        if (v && v->type == FMLX_JSON_STR) {
            const char *s = fmlx_json_str(v);
            size_t sn = fmlx_json_str_len(v);
            if (s && sn < cap) {
                memcpy(out, s, sn);
                out[sn] = '\0';
            }
        } else if (v && v->type == FMLX_JSON_OBJ) {
            const FmlxJsonValue *ct = fmlx_json_obj_get(v, "content");
            if (ct && ct->type == FMLX_JSON_STR) {
                const char *s = fmlx_json_str(ct);
                size_t sn = fmlx_json_str_len(ct);
                if (s && sn < cap) {
                    memcpy(out, s, sn);
                    out[sn] = '\0';
                }
            }
        }
        fmlx_json_doc_free(&doc);
        free(txt);
        if (out[0]) return;
    }
}

/* ---- chat template ---- */

/* Classifica la FAMIGLIA di un template dai token letterali che usa e ne ritorna una COPIA
 * malloc (per il motore Jinja). `*out_tpl` resta NULL se `tpl` è NULL. */
static FmlxTplKind classify_tpl(const char *tpl, size_t tpl_len, char **out_tpl, size_t *out_len) {
    FmlxTplKind kind = FMLX_TPL_GENERIC;

    if (!tpl) return kind;
    /* Una scansione per prefisso: il template è testo, e la firma di famiglia sta nei token
     * letterali che usa. `tpl_len` limita la ricerca a ciò che il DOM possiede. */
    char *tmp = malloc(tpl_len + 1);
    if (tmp) {
        memcpy(tmp, tpl, tpl_len);
        tmp[tpl_len] = '\0';
        if (strstr(tmp, "<|start_header_id|>") && strstr(tmp, "<|begin_of_text|>"))
            kind = FMLX_TPL_LLAMA3;
        else if (strstr(tmp, "<|im_start|>"))
            kind = FMLX_TPL_CHATML;
        if (out_tpl) *out_tpl = tmp;
        else free(tmp);
        if (out_len) *out_len = tpl_len;
    }
    return kind;
}

/* Estrae `chat_template` (stringa o lista di {name,template}) da tokenizer_config.json; se non
 * c'è, prova il file `chat_template.jinja` (la forma che HF scrive oggi). Ritorna la famiglia e
 * una copia malloc del template in `*out_tpl` (NULL se il modello non ne ha uno). */
static FmlxTplKind detect_template(const char *dir, char **out_tpl, size_t *out_len) {
    char path[FMLX_ENGINE_PATH_MAX];
    char *txt;
    size_t len;
    FmlxJsonDoc doc;
    const FmlxJsonValue *root, *v;
    const char *tpl = NULL;
    size_t tpl_len = 0;
    FmlxTplKind kind = FMLX_TPL_GENERIC;

    if (out_tpl) *out_tpl = NULL;
    if (out_len) *out_len = 0;
    if (snprintf(path, sizeof path, "%s/tokenizer_config.json", dir) >= (int)sizeof path)
        return kind;
    txt = read_file(path, &len);
    if (txt) {
        fmlx_json_doc_init(&doc);
        if (fmlx_json_parse(txt, len, 0, &doc) == FMLX_JSON_OK) {
            root = fmlx_json_root(&doc);
            v = fmlx_json_obj_get(root, "chat_template");
            if (v && v->type == FMLX_JSON_STR) {
                tpl = fmlx_json_str(v);
                tpl_len = fmlx_json_str_len(v);
            } else if (v && v->type == FMLX_JSON_ARR && fmlx_json_arr_n(v)) {
                const FmlxJsonValue *e = fmlx_json_arr_at(v, 0);
                const FmlxJsonValue *t = fmlx_json_obj_get(e, "template");
                if (t && t->type == FMLX_JSON_STR) {
                    tpl = fmlx_json_str(t);
                    tpl_len = fmlx_json_str_len(t);
                }
            }
            kind = classify_tpl(tpl, tpl_len, out_tpl, out_len);
            if (tpl) {
                fmlx_json_doc_free(&doc);
                free(txt);
                return kind;
            }
        }
        fmlx_json_doc_free(&doc);
        free(txt);
    }
    /* Fallback: il template in un file SUO (`chat_template.jinja`), come nei repo HF recenti. */
    if (snprintf(path, sizeof path, "%s/chat_template.jinja", dir) < (int)sizeof path &&
        path_is_file(path)) {
        txt = read_file(path, &len);
        if (txt) {
            kind = classify_tpl(txt, len, out_tpl, out_len);
            free(txt);
        }
    }
    return kind;
}

/* ---- risoluzione della directory ---- */

static int resolve_dir(const char *cache_root, const char *name, char *out, size_t cap, char *err,
                       size_t errcap) {
    char expanded[FMLX_ENGINE_PATH_MAX];

    if (expand_tilde(name, expanded, sizeof expanded) != 0) {
        set_err(err, errcap, "model name is not a usable path: %s", name);
        return -1;
    }
    if (path_is_dir(expanded)) {
        snprintf(out, cap, "%s", expanded);
        return 0;
    }
    /* GGUF single-file (ds4): un FILE `.gguf` è un modello valido in questa lane. */
    if (path_is_file(expanded) && is_gguf_path(expanded)) {
        snprintf(out, cap, "%s", expanded);
        return 0;
    }

    /* cache HF (layout models--org--name): la radice che il server dichiara, non una a caso */
    {
        char root[FMLX_HF_PATH_MAX];
        FmlxHfRef ref;
        if (cache_root && *cache_root) {
            snprintf(root, sizeof root, "%s", cache_root);
        } else if (fmlx_hf_cache_root(root, sizeof root) != FMLX_HF_OK) {
            root[0] = '\0';
        }
        if (root[0] && fmlx_hf_locate(root, name, "main", "config.json", &ref) == FMLX_HF_OK &&
            path_is_file(ref.path)) {
            snprintf(out, cap, "%s", ref.path);
            dir_name(out);
            return 0;
        }
    }

    /* Percorsi di ricerca da FILE DI CONFIGURAZIONE ($FASTLLM_HOME/paths), cercati IN ORDINE.
     * NON esiste un default implicito legato a una macchina (~/ai/Models e' stato rimosso): i
     * percorsi li decide l'utente con `fastllm paths add` e vivono in un file leggibile. Il nome
     * puo' arrivare con o senza namespace: si prova la forma intera e la sola basename. */
    {
        int np = fmlx_paths_count(), pi;
        for (pi = 0; pi < np; pi++) {
            char pdir[FMLX_PATHS_DIR_MAX];
            char try1[FMLX_ENGINE_PATH_MAX];
            if (fmlx_paths_get(pi, pdir, sizeof pdir) != 0 || !pdir[0]) continue;
            if (snprintf(try1, sizeof try1, "%s/%s", pdir, name) < (int)sizeof try1 &&
                (path_is_dir(try1) || (path_is_file(try1) && is_gguf_path(try1)))) {
                snprintf(out, cap, "%s", try1);
                return 0;
            }
            if (snprintf(try1, sizeof try1, "%s/%s", pdir, base_name(name)) < (int)sizeof try1 &&
                (path_is_dir(try1) || (path_is_file(try1) && is_gguf_path(try1)))) {
                snprintf(out, cap, "%s", try1);
                return 0;
            }
        }
    }
    set_err(err, errcap, "model '%s' is not a local path, not in the HF cache, and not in any "
                         "`fastllm paths` search directory",
            name);
    return -1;
}

/* ---- tokenizer ---- */

static llmtoken_handle load_tokenizer(const char *dir) {
    char path[FMLX_ENGINE_PATH_MAX];
    llmtoken_handle t;

    /* GGUF single-file (ds4): il tokenizer si COSTRUISCE dal GGUF (BPE + special token DS4)
     * e si carica come JSON — non c'è un tokenizer.json accanto al file. */
    if (is_gguf_path(dir) && path_is_file(dir)) {
        size_t jl = 0;
        char *js = llm_ai_mlx_gguf_tokenizer_json(dir, &jl);
        if (js) {
            if (jl) t = llmtoken_load_json(js, jl);
            else t = NULL;
            free(js);
            if (t) return t;
        }
    }
    if (snprintf(path, sizeof path, "%s/tokenizer.json", dir) < (int)sizeof path &&
        path_is_file(path)) {
        t = llmtoken_load(path);
        if (t) return t;
    }
    if (snprintf(path, sizeof path, "%s/tiktoken.model", dir) < (int)sizeof path &&
        path_is_file(path)) {
        char cfg[FMLX_ENGINE_PATH_MAX];
        snprintf(cfg, sizeof cfg, "%s/tokenizer_config.json", dir);
        t = llmtoken_load_tiktoken(path, path_is_file(cfg) ? cfg : NULL);
        if (t) return t;
    }
    return NULL;
}

/* ---- caricamento ---- */

int fmlx_engine_load(const char *cache_root, const char *name, FmlxEngineModel *out, char *err,
                     size_t errcap) {
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);
    if (resolve_dir(cache_root, name, out->dir, sizeof out->dir, err, errcap) != 0) return -1;

    out->tok = load_tokenizer(out->dir);
    if (!out->tok) {
        snprintf(err, errcap, "no usable tokenizer (tokenizer.json / tiktoken.model / GGUF) in %s",
                 out->dir);
        return -1;
    }
    if (is_gguf_path(out->dir)) {
        /* GGUF/DS4: niente config.json/tokenizer_config.json accanto al file — template di
         * famiglia DS4 e eos dedotto dal tokenizer (l'`<｜end▁of▁sentence｜>` è nel vocabolario). */
        int32_t eid = -1;
        out->tpl = FMLX_TPL_DS4;
        out->chat_tpl = NULL;
        out->chat_tpl_len = 0;
        if (llmtoken_encode(out->tok, "<｜end▁of▁sentence｜>", &eid, 1) == 1) {
            out->eos_list[0] = eid;
            out->n_eos = 1;
        }
        out->eos = out->n_eos > 0 ? out->eos_list[out->n_eos - 1] : -1;
        out->bos[0] = '\0';
    } else {
        out->n_eos = read_eos_list(out->dir, out->eos_list, FMLX_ENGINE_EOS_MAX);
        /* L'ABI accetta UN eos: si passa l'ULTIMO della lista (nei modelli instruct è il token di
         * fine turno, es. `<|eot_id|>`, mentre i primi sono fine-testo/fine-sequenza). Gli altri
         * restano in `eos_list` e li ferma il consumer token per token. */
        out->eos = out->n_eos > 0 ? out->eos_list[out->n_eos - 1] : -1;
        out->tpl = detect_template(out->dir, &out->chat_tpl, &out->chat_tpl_len);
        read_bos(out->dir, out->bos, sizeof out->bos);
    }

    out->eng = llmengine_llm_engine_mlx();
    /* DS4 (firma GGUF "deepseek4"): motore unico DS4 nativo (LLMEngine/mlx/ds4) via ABI. */
    {
        size_t dn = strlen(out->dir);
        if (dn > 5 && strcmp(out->dir + dn - 5, ".gguf") == 0 && llm_ai_mlx_gguf_supported(out->dir) == 1) {
            const LlmEngineLlmEngine* e = fmlx_engine_symbol("llmengine_llm_engine_ds4");
            if (e != NULL) out->eng = e;   /* altrimenti resta l'engine MLX */
        } else if (is_dsv41_dir(out->dir)) {
            /* DS4.1 (deepseek_v41): motore `ds41-native`, stessi 9 metodi dell'ABI. */
            const LlmEngineLlmEngine* e = fmlx_engine_symbol("llmengine_llm_engine_ds41");
            if (e != NULL) out->eng = e;
        }
    }
    if (!out->eng || !out->eng->load_model) {
        snprintf(err, errcap, "the MLX engine vtable is not available in this build");
        return -1;
    }
    /* Offload esperti (ABI v5): se richiesto, il motore streama gli esperti da disco dalla dir del
     * modello. Va PRIMA di load_model. Per i modelli dense è un no-op.
     * Prima del load: se il modello supera il 50% della memoria libera+reclamabile e l'utente non
     * ha già dato --offload-experts, lo si chiede (Y/N) su terminale interattivo (run/chat). */
    maybe_prompt_offload(out->dir);
    if (out->eng->set_expert_swap)
        out->eng->set_expert_swap(g_offload_experts ? out->dir : NULL);
    out->model = out->eng->load_model(out->dir);
    if (!out->model) {
        snprintf(err, errcap, "the engine could not load the model at %s", out->dir);
        return -1;
    }
    /* Default di contesto: scelto dall'ARCH del modello (firma del file, non il nome) e MOSTRATO.
     * DS4 (deepseek4) ha cache KV preallocate -> valori alti saturano la RAM. Il modello resta
     * caricato; il KV riparte da zero. Regolabile a runtime con `/prompt <n>`. */
    {
        int ctx;
        int is_dsv4 = 0;
        {
            size_t dn = strlen(out->dir);
            if (dn > 5 && strcmp(out->dir + dn - 5, ".gguf") == 0)
                is_dsv4 = (llm_ai_mlx_gguf_supported(out->dir) == 1);
        }
        /* Il PARAMETRO `context` (registro) vince; altrimenti default per arch (DS4 più basso). */
        if (llm_ai_mlx_opt_is_set("context") == 1) {
            llm_ai_mlx_opt_get_int("context", &ctx);
        } else {
            ctx = is_dsv4 ? FMLX_DSV4_DEFAULT_CTX : FMLX_ENGINE_DEFAULT_CTX;
        }
        out->ctx = ctx;
        if (out->eng->kv_set_max) {
            int crc = out->eng->kv_set_max(out->model, ctx);
            fprintf(stderr, "contesto: %d token%s\n", ctx, crc == 0 ? "" : " (motore: non impostabile)");
        }
    }
    /* `dtype` e `kvquant` (registro): applicati subito al modello, per qualunque modello MLX. */
    {
        char d[16];
        if (llm_ai_mlx_opt_get_str("dtype", d, sizeof d) == 0 && strcmp(d, "auto") != 0)
            llm_ai_mlx_model_set_bfloat16(out->model, strcmp(d, "bf16") == 0);
    }
    {
        char kq[32];
        /* `auto` e `off` li gestisce il MOTORE al load (policy per contesto / nessuna quant):
         * qui si applica solo un valore esplicito (bits numerici). Evita che `auto` venga
         * interpretato come "bits=0 -> ON da subito". */
        if (llm_ai_mlx_opt_get_str("kvquant", kq, sizeof kq) == 0 &&
            strncmp(kq, "off", 3) != 0 && strncmp(kq, "auto", 4) != 0) {
            int bits = atoi(kq), thr = 0;
            char *c = strchr(kq, ':');
            if (c) thr = atoi(c + 1);
            llm_ai_mlx_model_set_kv_quantize(out->model, 1, thr, bits);
        }
    }
    return 0;
}

/* ---- cache residente ---- */

#define FMLX_ENGINE_MODELS_MAX 3

static struct {
    int used;
    FmlxEngineModel m;
} g_slot[FMLX_ENGINE_MODELS_MAX];

static FmlxEngineModel *model_load_direct(const char *cache_root, const char *name, char *err,
                                          size_t errcap) {
    if (!name || !*name) {
        set_err(err, errcap, "model name is empty", NULL);
        return NULL;
    }
    for (int i = 0; i < FMLX_ENGINE_MODELS_MAX; i++)
        if (g_slot[i].used && strcmp(g_slot[i].m.name, name) == 0) return &g_slot[i].m;

    for (int i = 0; i < FMLX_ENGINE_MODELS_MAX; i++) {
        if (!g_slot[i].used) {
            if (fmlx_engine_load(cache_root, name, &g_slot[i].m, err, errcap) != 0) return NULL;
            g_slot[i].used = 1;
            return &g_slot[i].m;
        }
    }
    /* Tutti gli slot pieni: nessuna eviction silenziosa, un errore esplicito. */
    set_err(err, errcap, "too many models resident (%d): restart the server to load another",
            NULL);
    return NULL;
}

/* M3: il LOAD deve avvenire SUL thread del motore (coordinatore), perché MLX lega device/stream
 * al thread del load. Con il coordinatore attivo si instrada lì; senza, si carica qui come prima. */
typedef struct {
    const char *cache_root;
    const char *name;
    char *err;
    size_t errcap;
    FmlxEngineModel *out;
} FmlxLoadArgs;
static void *model_load_thunk(void *p) {
    FmlxLoadArgs *a = (FmlxLoadArgs *)p;
    a->out = model_load_direct(a->cache_root, a->name, a->err, a->errcap);
    return a->out;
}

FmlxEngineModel *fmlx_engine_model(const char *cache_root, const char *name, char *err,
                                   size_t errcap) {
    if (fmlx_engine_batch_active()) {
        FmlxLoadArgs a;
        a.cache_root = cache_root;
        a.name = name;
        a.err = err;
        a.errcap = errcap;
        a.out = NULL;
        return (FmlxEngineModel *)fmlx_engine_batch_call(model_load_thunk, &a);
    }
    return model_load_direct(cache_root, name, err, errcap);
}

void fmlx_engine_shutdown(void) {
    fmlx_engine_batch_disable(); /* ferma il coordinatore di lotto PRIMA di liberare i modelli */
    for (int i = 0; i < FMLX_ENGINE_MODELS_MAX; i++) {
        if (!g_slot[i].used) continue;
        if (g_slot[i].m.eng && g_slot[i].m.eng->free_model && g_slot[i].m.model)
            g_slot[i].m.eng->free_model(g_slot[i].m.model);
        if (g_slot[i].m.tok) llmtoken_free(g_slot[i].m.tok);
        free(g_slot[i].m.chat_tpl);
        g_slot[i].used = 0;
    }
}

int fmlx_engine_resident_count(void) {
    int n = 0;
    for (int i = 0; i < FMLX_ENGINE_MODELS_MAX; i++)
        if (g_slot[i].used) n++;
    return n;
}

const char *fmlx_engine_resident_name(int i) {
    if (i < 0 || i >= FMLX_ENGINE_MODELS_MAX || !g_slot[i].used) return NULL;
    return g_slot[i].m.name;
}

int fmlx_engine_unload(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < FMLX_ENGINE_MODELS_MAX; i++) {
        if (!g_slot[i].used || strcmp(g_slot[i].m.name, name) != 0) continue;
        if (g_slot[i].m.eng && g_slot[i].m.eng->free_model && g_slot[i].m.model)
            g_slot[i].m.eng->free_model(g_slot[i].m.model);
        if (g_slot[i].m.tok) llmtoken_free(g_slot[i].m.tok);
        free(g_slot[i].m.chat_tpl);
        memset(&g_slot[i].m, 0, sizeof g_slot[i].m);
        g_slot[i].used = 0;
        return 1;
    }
    return 0;
}
