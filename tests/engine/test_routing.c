/* test_routing.c — la scelta del motore per un GGUF DS4, senza caricare un modello.
 *
 * Il caso che vale il file: lo STESSO file `deepseek4` deve poter andare sul nativo (ex ds4-slow)
 * o sul vendor a seconda di `--opt ds4_engine=`, perché è così che si misura quale dei due è più
 * veloce. Le altre famiglie hanno un solo motore possibile e la funzione deve dirlo, non indovinare.
 *
 * La funzione è PURA (arch + preferenza): nessun file, nessuna GPU, nessuna rete.
 */
#include "fmlx_engine.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CK(cond, ...)                                                                                              \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            failures++;                                                                                            \
            printf("  FALLIMENTO %s:%d: ", __FILE__, __LINE__);                                                    \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static const char *choice_name(FmlxEngineChoice c) {
    switch (c) {
        case FMLX_ENGINE_MLX: return "mlx";
        case FMLX_ENGINE_DS4: return "ds4";
        case FMLX_ENGINE_DS4N: return "ds4n";
        case FMLX_ENGINE_DS41: return "ds41";
        case FMLX_ENGINE_LLAMACPP: return "llamacpp";
    }
    return "?";
}

static void case_choose(void) {
    /* deepseek4: entrambi i motori lo eseguono — è il solo caso in cui la preferenza conta. */
    CK(fmlx_engine_ds4_choose("deepseek4", FMLX_DS4_AUTO) == FMLX_ENGINE_DS4, "auto/deepseek4 -> %s",
       choice_name(fmlx_engine_ds4_choose("deepseek4", FMLX_DS4_AUTO)));
    CK(fmlx_engine_ds4_choose("deepseek4", FMLX_DS4_NATIVE) == FMLX_ENGINE_DS4, "native/deepseek4");
    CK(fmlx_engine_ds4_choose("deepseek4", FMLX_DS4_VENDOR) == FMLX_ENGINE_DS4N, "vendor/deepseek4");

    /* Famiglie che il nativo non sa eseguire da GGUF: la preferenza non le sposta. */
    CK(fmlx_engine_ds4_choose("deepseek41", FMLX_DS4_NATIVE) == FMLX_ENGINE_DS4N, "ds41 native -> ds4n");
    CK(fmlx_engine_ds4_choose("glm5-next", FMLX_DS4_NATIVE) == FMLX_ENGINE_DS4N, "glm5-next -> ds4n");
    CK(fmlx_engine_ds4_choose("qwen4exp", FMLX_DS4_NATIVE) == FMLX_ENGINE_DS4N, "qwen4exp -> ds4n");

    /* glm-dsa: llama.cpp lo gestisce; il vendor è la scelta quando la si chiede. */
    CK(fmlx_engine_ds4_choose("glm-dsa", FMLX_DS4_AUTO) == FMLX_ENGINE_LLAMACPP, "glm-dsa auto");
    CK(fmlx_engine_ds4_choose("glm-dsa", FMLX_DS4_VENDOR) == FMLX_ENGINE_DS4N, "glm-dsa vendor");

    /* GGUF generico e arch sconosciuta: llama.cpp. */
    CK(fmlx_engine_ds4_choose("llama", FMLX_DS4_AUTO) == FMLX_ENGINE_LLAMACPP, "llama -> llamacpp");
    CK(fmlx_engine_ds4_choose("qualcosa", FMLX_DS4_VENDOR) == FMLX_ENGINE_LLAMACPP,
       "arch generica -> llamacpp anche con vendor");

    /* Non un GGUF DS4: il chiamante resta sull'engine MLX. */
    CK(fmlx_engine_ds4_choose(NULL, FMLX_DS4_AUTO) == FMLX_ENGINE_MLX, "NULL -> mlx");
    CK(fmlx_engine_ds4_choose("", FMLX_DS4_VENDOR) == FMLX_ENGINE_MLX, "vuota -> mlx");
}

static void case_pref(void) {
    /* Senza `ds4_engine` la preferenza è AUTO. */
    CK(fmlx_engine_ds4_pref() == FMLX_DS4_AUTO, "default -> auto");

    /* Le tre forme canoniche (e gli alias). */
    CK(fmlx_engine_opt_set("ds4_engine", "vendor") == 0, "set vendor");
    CK(fmlx_engine_ds4_pref() == FMLX_DS4_VENDOR, "pref vendor");
    CK(fmlx_engine_opt_set("ds4_engine", "native") == 0, "set native");
    CK(fmlx_engine_ds4_pref() == FMLX_DS4_NATIVE, "pref native");
    CK(fmlx_engine_opt_set("ds4_engine", "ds4n") == 0, "set alias ds4n");
    CK(fmlx_engine_ds4_pref() == FMLX_DS4_VENDOR, "alias ds4n -> vendor");

    /* Un valore sconosciuto NON si applica in silenzio: torna AUTO (e lo scrive su stderr). */
    CK(fmlx_engine_opt_set("ds4_engine", "boh") == 0, "set valore ignoto (stringa accettata)");
    CK(fmlx_engine_ds4_pref() == FMLX_DS4_AUTO, "valore ignoto -> auto");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    case_choose();
    case_pref();
    printf("test_routing: %d verifiche, %d fallimenti\n", checks, failures);
    return failures ? 1 : 0;
}
