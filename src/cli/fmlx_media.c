/* fmlx_media.c — `fastllm run <modello> <file>` per i modelli OCR / VLM / audio.
 *
 * Un modello OCR/VLM/audio non e' un modello di chat MLX. Si riconosce dal registro (`add --modality ocr|vlm|audio`) oppure,
 * per un GGUF passato per percorso, da `general.architecture`:
 *   - `parakeet`  -> trascrizione vocale NVIDIA Parakeet-TDT (engine C di LLMEnv, llm_parakeet.h), nessun mmproj;
 *   - `paddleocr` -> OCR PaddleOCR-VL tramite la lane vision (llama.cpp/mtmd), con il `*-mmproj.gguf` accanto al modello.
 * Il secondo posizionale di `run` e' il FILE (immagine o audio); l'istruzione al modello vision (es. "OCR:",
 * "Table Recognition:") si passa con --system (default "OCR:"). */
#include "fmlx/fmlx_cli.h"
#include "engine/fmlx_vision.h"
#include "gmb_asr.h"
#include "llm_parakeet.h"
#include "llmcodec_gguf_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int is_file(const char *p) { struct stat st; return p && *p && stat(p, &st) == 0 && S_ISREG(st.st_mode); }

/* general.architecture di un GGUF ("" se non lo e'). */
static void gguf_arch(const char *path, char *out, size_t cap) {
    LlmCodecGgufMetaKVOwned *kv = NULL;
    LlmCodecGgufFileInfo fi;
    int n = 0;
    out[0] = '\0';
    memset(&fi, 0, sizeof fi);
    if (llmcodec_gguf_read_meta(path, &kv, &n, &fi) != 0) return;
    for (int i = 0; i < n; i++)
        if (strcmp(kv[i].key, "general.architecture") == 0 && kv[i].str) snprintf(out, cap, "%s", kv[i].str);
    llmcodec_gguf_meta_free(kv, n);
    llmcodec_gguf_free_info(&fi);
}

int fmlx_media_detect(const char *path, char *modality, size_t mcap, char *mmproj, size_t pcap) {
    char arch[64];
    size_t n = path ? strlen(path) : 0;
    if (n < 6 || strcmp(path + n - 5, ".gguf") != 0 || !is_file(path)) return 0;
    gguf_arch(path, arch, sizeof arch);
    if (!strcmp(arch, "parakeet")) {
        snprintf(modality, mcap, "audio");
        if (pcap) mmproj[0] = '\0';
        return 1;
    }
    if (!strcmp(arch, "paddleocr")) {
        char cand[FMLX_HF_PATH_MAX];
        snprintf(modality, mcap, "ocr");
        snprintf(cand, sizeof cand, "%.*s-mmproj.gguf", (int)(n - 5), path);
        if (!is_file(cand)) snprintf(cand, sizeof cand, "%.*s-mmproj-F16.gguf", (int)(n - 5), path);
        snprintf(mmproj, pcap, "%s", is_file(cand) ? cand : "");
        return 1;
    }
    return 0;
}

/* Audio -> WAV mono 16 kHz in memoria. Formati diversi dal WAV PCM si convertono con afconvert (macOS). */
static float *load_audio(const char *path, int *n) {
    float *pcm = NULL;
    {   /* solo i WAV (RIFF) vanno al lettore interno: gli altri formati passano direttamente da afconvert */
        FILE *f = fopen(path, "rb");
        char magic[4] = "";
        if (f) { if (fread(magic, 1, 4, f) != 4) magic[0] = 0; fclose(f); }
        if (!memcmp(magic, "RIFF", 4)) pcm = gmb_asr_wav_load(path, n);
    }
#ifdef __APPLE__
    if (!pcm) {
        char tmp[] = "/tmp/fastllm_audio_XXXXXX";
        int fd = mkstemp(tmp);
        if (fd >= 0) {
            char cmd[2 * FMLX_HF_PATH_MAX + 128], q[FMLX_HF_PATH_MAX * 2];
            size_t k = 0;
            close(fd);
            for (const char *p = path; *p && k + 5 < sizeof q; p++) {
                if (*p == '\'') { memcpy(q + k, "'\\''", 4); k += 4; } else q[k++] = *p;
            }
            q[k] = '\0';
            snprintf(cmd, sizeof cmd, "afconvert -f WAVE -d LEI16@16000 -c 1 '%s' '%s' >/dev/null 2>&1", q, tmp);
            if (system(cmd) == 0) pcm = gmb_asr_wav_load(tmp, n);
            unlink(tmp);
        }
    }
#endif
    return pcm;
}

static FmlxCliStatus run_parakeet(const FmlxCliArgs *a, const char *model, FILE *out, FILE *err) {
    char emsg[256] = "";
    int n = 0;
    float *pcm = load_audio(a->prompt, &n);
    LlmParakeet *pk;
    char *text;

    if (!pcm) { fprintf(err, "fastllm run: audio non leggibile: %s\n", a->prompt); return FMLX_CLI_FAILED; }
    pk = llm_parakeet_load(model, emsg, sizeof emsg);
    if (!pk) { free(pcm); fprintf(err, "fastllm run: %s\n", emsg); return FMLX_CLI_FAILED; }
    text = llm_parakeet_transcribe(pk, pcm, n, emsg, sizeof emsg);
    free(pcm);
    if (!text) { llm_parakeet_free(pk); fprintf(err, "fastllm run: %s\n", emsg); return FMLX_CLI_FAILED; }
    fprintf(out, "%s\n", text);
    free(text);
    llm_parakeet_free(pk);
    return FMLX_CLI_OK;
}

FmlxCliStatus fmlx_media_run_exec(const FmlxCliArgs *a, const char *model, const char *mmproj, const char *modality,
                                  FILE *out, FILE *err) {
    char emsg[512] = "";
    FmlxVision *v;
    char *text;
    int audio = !strcmp(modality, "audio");
    int max_tokens = a->max_tokens > 0 ? (int)a->max_tokens : 1024;

    if (!out) out = stdout;
    if (!err) err = stderr;
    if (!a->prompt[0]) {
        fprintf(err, "fastllm run %s <file>: il modello e' '%s', serve il file %s come secondo argomento\n", a->repo,
                modality, audio ? "audio" : "immagine");
        return FMLX_CLI_USAGE;
    }
    if (!is_file(a->prompt)) {
        fprintf(err, "fastllm run: file non trovato: %s\n", a->prompt);
        return FMLX_CLI_FAILED;
    }
    if (audio && llm_parakeet_is_gguf(model)) return run_parakeet(a, model, out, err);
    if (!fmlx_vision_available()) {
        fprintf(err, "fastllm run: la lane vision (llama.cpp/mtmd) non e' disponibile in questo build\n");
        return FMLX_CLI_NOT_WIRED;
    }
    v = fmlx_vision_load(model, mmproj, 4096, 99, emsg, sizeof emsg);
    if (!v) { fprintf(err, "fastllm run: %s\n", emsg); return FMLX_CLI_FAILED; }
    text = audio ? fmlx_vision_read_audio(v, a->prompt, a->system, max_tokens, emsg, sizeof emsg)
                 : fmlx_vision_read_image(v, a->prompt, a->system, max_tokens, emsg, sizeof emsg);
    if (!text) {
        fprintf(err, "fastllm run: %s\n", emsg[0] ? emsg : "nessun testo riconosciuto");
        fmlx_vision_free(v);
        return FMLX_CLI_FAILED;
    }
    fprintf(out, "%s\n", text);
    fmlx_vision_free_str(text);
    fmlx_vision_free(v);
    return FMLX_CLI_OK;
}
