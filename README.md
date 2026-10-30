# FastLLM

Binario C unico, **`fastllm`**: server OpenAI/Anthropic/embeddings, CLI in stile Ollama, download e
cache da Hugging Face, ispezione dei modelli. **Il motore non è qui**: è [LLMEnv](../LLMEnv)
(`LLMEngine`, `LLMCodec`, `LLMToken`, `LLMJinja`), consumato a sorgente con `add_subdirectory`
(vedi `CMakeLists.txt:16-59`). Stesso schema di GMBasic: due consumatori, una sola sorgente.
Piano e appunti: `AiWiki/01-Projects/FastLLM/` (vault, non versionato qui).

## Cosa FastLLM esegue

FastLLM e' una CLI di chat in stile Ollama sopra il motore LLMEnv. Esegue modelli **MLX** (cartelle safetensors di Hugging Face,
quantizzate 2/3/4/6/8 bit, MXFP4/MXFP8) e modelli **GGUF**. Non e' un'applicazione di OCR o di trascrizione, ma sa instradare
due modelli di questo tipo (PaddleOCR-VL e Parakeet) con lo stesso comando `run`, vedi sotto.

```sh
fastllm pull mlx-community/Qwen3-8B-4bit      # scarica nel cache HF e crea da solo il .gmbarch se manca
fastllm add mio-gguf ~/ai/Models/gguf/modello.gguf
fastllm run Qwen3-8B-4bit "Ciao"               # nome del registro, #n, percorso o id HF
fastllm run modello.gguf "Ciao"                # GGUF: motore llama.cpp
fastllm run parakeet.gguf discorso.wav         # trascrizione (anche mp3/m4a su macOS)
fastllm run PaddleOCR-VL.gguf fattura.jpg      # OCR; il *-mmproj.gguf sta accanto al modello
fastllm gmbarch mlx-community/Mixtral-8x22B-4bit   # crea il descrittore dell'architettura da un URL, senza scaricare i pesi
```

### Modelli provati e funzionanti

Misure su un Mac Apple Silicon con 128 GB, una sola esecuzione per modello (`tools/bench_models.sh`): prompt unico di circa
2000-3000 token per il **prefill**, 128 token generati per il **decode**, campionamento di default. Il prefill e il decode dei
modelli molto grandi, letti dal disco, dipendono dalla velocita' dell'SSD.

| Modello | Formato | Prefill tok/s | Decode tok/s |
|---|---|---:|---:|
| SmolLM2-135M-Instruct | MLX 8 bit | 14105 | 492 |
| Llama-3.2-1B-Instruct | MLX 4 bit | 9501 | 561 |
| Qwen3-0.6B | MLX 4 bit | 5539 | 267 |
| Qwen3-0.6B | MLX 8 bit | 5744 | 342 |
| Qwen3.5-4B | MLX 4 bit | 3638 | 119 |
| Ternary-Bonsai-4B | MLX 2 bit | 2895 | 182 |
| Qwen3-8B | MLX 4 bit | 2041 | 95 |
| LFM2-8B-A1B | MLX 4 bit | 1472 | 208 |
| DeepSeek-V2-Lite-Chat | MLX 4 bit | 1953 | 116 |
| gpt-oss-20b | MXFP4 + Q8 | 4039 | 137 |
| Granite-3.3-2B-Instruct | MLX 4 bit | 2732 | 119 |
| LFM2.5-24B-A2B | MLX 8 bit | 547 | 63 |
| Qwen3.6-27B | MLX 4 bit | 748 | 27.5 |
| Qwen3.6-35B-A3B (OptiQ) | MLX 4 bit | 1871 | 62 |
| Qwen3-Coder-30B-A3B-Instruct | MLX 8 bit | 435 | 67 |
| Qwen3-Coder-Next | MXFP4 | 929 | 77 |
| gemma-4-31b-it | MLX 4 bit | 523 | 19.3 |
| gemma-4-26b-a4b-it (MoE, 128 esperti) | MLX 4 bit | 716 | 69.5 |
| Kimi-Linear-48B-A3B-Instruct | MLX 4 bit | 2198 | 101 |
| GLM-4.5-Air | MLX 4 bit | 143 | 27.5 |
| DeepSeek-R1-Distill-Qwen-32B | MLX 8 bit | 501 | 13.3 |
| Qwen3.5-27B (Claude-4.6-Opus distilled) | MLX 6 bit | 525 | 18.2 |
| Qwen3.8-27B | MLX 8 bit | 490 | 17.6 (37.8 con il draft DFlash2, vedi sotto) |
| Qwen3.8-Flash-Next | MLX 2 bit, pesi letti da disco | 62 | 18.5 |
| Qwen3.8-Flash-Next | MLX 4 bit, pesi letti da disco (104 GB) | 29 | 4.5 |
| Phi-3-mini-4k-instruct | MLX 4 bit | 544 (prompt di 4187 token) | 25.6 (111 su prompt breve) |
| StableLM-2-zephyr-1.6B | MLX 4 bit | 1451 | 41.6 (235 su prompt breve) |
| StarCoder2-3B (modello base, non chat) | MLX 4 bit | 1625 | 86.2 |
| Gemma-3-1B-it | MLX 4 bit | 4126 | 317.6 |
| Mixtral-8x22B (modello base) | MLX 4 bit, pesi letti da disco (74 GB) | 16 (prompt di 1752 token) | 11.2 |

| TinyLlama-1.1B-Chat | GGUF Q4_K_M | 12926 | 252 |
| gemma-3-270m-it | GGUF Q4_K_M | 6907 | 297 |
| OLMoE-1B-7B-Instruct | GGUF Q4_K_M | 6572 | 42 |
| gemma-2-2b-it | GGUF Q4_K_M | 4403 | 44 |
| Llama-2-7B-Chat | GGUF Q4_K_M | 1499 | 9.0 |
| Qwen3.8-Flash-Next (ds4 integrato) | GGUF Q2, 137 GB | 728 | 52 |

Note sulle misure:

- Qwen3.8-27B usa da solo il draft `Qwen3.8-27B-DFlash2` se sta accanto al modello (decodifica speculativa, testo identico):
  37.8 tok/s su un prompt breve di codice contro 16.5 senza draft.
- I modelli con stato ricorrente (Qwen3.5/3.6/3.8, Qwen3-Next, LFM2, Kimi-Linear, Mamba, Jamba...) non riusano i prefissi
  salvati: il prefill si rifa' ad ogni richiesta, ma il testo e' corretto.
- Modelli piu' grandi del 55% della RAM passano da soli ai pesi `packed` (letti dai file a richiesta): Flash-Next 2 e 4 bit.
- gemma-3-270m: il decode e' misurato su un prompt breve (sul prompt lungo di prova risponde subito con la fine del testo).
- La velocita' di Mixtral e dei modelli "letti da disco" include il primo accesso ai pesi.

### Parlato e OCR

| Modello | Cosa fa | Esito |
|---|---|---|
| NVIDIA Parakeet-TDT 0.6B v3 (GGUF Q8) | trascrizione in 25 lingue europee | JFK parola per parola, italiano e inglese corretti; 216 s di audio in 7.6 s su CPU |
| PaddleOCR-VL 1.6 (GGUF + mmproj) | OCR di immagini | legge correttamente una fattura di prova |

Servono i descrittori `Parakeet` e `PaddleOCR-VL` della cartella ABI di LLMEnv; il motore Parakeet e' in
`LLMEnv/LLMEngine/asr/llm_parakeet.c`.

### Modelli con problemi noti

Nessuno tra quelli provati.

Phi-3, StableLM-2, StarCoder2, Gemma-3 e Gemma-4-26b davano testo degenerato o con parole saltate per tre difetti del motore
(cache KV con padding visto dall'attention; bias q/k/v mancanti in StableLM e StarCoder2; ramo MoE di Gemma-4 assente), ora corretti e presenti nella tabella sopra.

### Famiglie che dovrebbero funzionare anche se non sono state provate

Nessuna di queste e' stata eseguita su un checkpoint reale: il codice c'e' e passa i test sintetici, ma la storia di Phi-3, StableLM,
StarCoder2 e Gemma-4 MoE insegna che un'architettura mai provata ha di solito almeno un difetto. Vanno verificate una per una
(`fastllm pull <modello>`, una domanda semplice, confronto col GGUF dello stesso modello).

| Formato | Famiglie |
|---|---|
| **GGUF** | qualunque architettura supportata da llama.cpp: Llama 2/3, Mistral, Mixtral, Gemma 1/2/3, Qwen 2/2.5/3, Phi-2/3/4, OLMo, DeepSeek, GLM, Granite, Command-R, Falcon, StarCoder, Yi, InternLM, MiniCPM. Provati cinque modelli di famiglie diverse |
| **MLX, stessa famiglia di modelli provati** | Qwen2, Qwen2.5, Qwen3, Qwen3-MoE, Qwen3-Next, Qwen3.5-MoE, Llama 3.x, Mistral, OLMo 2/3, OLMoE, GLM-4.x, DeepSeek-V2/V3, Kimi, ERNIE 4.5, Granite/GraniteMoE, LFM2, gpt-oss, Gemma-4 denso |
| **MLX, nel motore ma mai provate** | Qwen2-MoE, DeepSeek-V3.x, Kimi-K2, GLM-4 denso, GLM-4-MoE-Lite, Mamba/Mamba2/Falcon-Mamba, RWKV7, Falcon-H1, Jamba, GraniteMoE-hybrid, GPT-NeoX/Pythia, InternLM2, MiniCPM, Hunyuan, GPT-2, GPT-BigCode, OpenELM, Mistral3, dots1, Telechat3, Youtu, JoyAI, Lille, OLMo-3 |
| **Quantizzazioni** | affine 2/3/4/6/8 bit, MXFP4/MXFP8, AWQ, GPTQ, bitsandbytes (vedi `docs/quant-coverage.md`) |

Il test per ogni famiglia nuova e' sempre lo stesso: una risposta corta e corretta, la stessa risposta con un prompt lungo
(la cache KV con padding ha gia' ingannato una quindicina di architetture) e il confronto con un oracolo, di solito il GGUF su llama.cpp.

### Descrittori delle architetture (`.gmbarch`)

Ogni architettura ha un descrittore nella cartella `ABI` di LLMEnv. `fastllm pull` e `fastllm add` lo creano da soli se manca
(da `config.json` e dai nomi dei tensori); `fastllm gmbarch <url|org/nome|cartella>` lo crea a mano, scaricando solo
`config.json` e l'indice dei tensori. Un descrittore esistente non viene mai sovrascritto. Creati cosi': `mixtral`, `phi3`,
`gemma3`, `granite`, `stablelm`, `starcoder2`, `qwen2_vl`.


> **Stato al 2026-10-30.** La tabella qui sotto è la fotografia del **2026-09-24** e
> contiene voci che oggi **non esistono più**: `src/lane_gguf/` e l'ABI `mlxengine_*` sono state
> **eliminate** (`CMakeLists.txt:85-89`) — il GGUF lo legge e lo dequantizza `llmcodec` di LLMEnv.
> Non ci sono più «due programmi»: c'è un binario `fastllm` (più i tool e i test). Ciò che resta
> valido della tabella sono le lane `src/hf`, `src/json`, `src/server`, `src/cli`, che sono di
> FastLLM. Lo stato corrente è nel vault (`_Index_FastLLM.md`, sezione «Stato al 2026-10-30»).

## Stato reale del codice (fotografia al 2026-09-24; vedi l'avviso sopra)

| componente | stato |
|---|---|
| `src/lane_gguf/fmlx_gguf.c` — lettore GGUF di riferimento | **ELIMINATO 2026-09** (oggi in `LLMEnv/LLMCodec`); era compilato e testato (259 verifiche) |
| `src/lane_gguf/fmlx_quant.c` + `fmlx_quant_ext.c` + `fmlx_quant_iq.c` — dequant ggml → F32 (31 tipi) | **ELIMINATO 2026-09** (oggi `llmcodec_ggml` in LLMEnv); era compilato e testato (510 verifiche + gate differenziale: 250 confronti, 0 ns. bit divergenti) |
| `src/hf/fmlx_hf_cache.c` + `fmlx_hf_hash.c` — cache HF letto, senza rete | **esiste, compilato, testato** (131 verifiche + sonda sul cache reale) |
| `src/hf/fmlx_hf_fetch.c` + `fmlx_hf_curl.c` — download, lock, resume, watchdog di stall, backend classico su repo xet | **esiste, compilato, testato** (199 verifiche senza rete + probe sul server vero + **`tools/hf_stall_probe.c`: il watchdog misurato per la prima volta su un server locale che si ferma a metà — 16 verifiche, e ha già trovato un difetto: lo stall tagliava come `ERR_HTTP`, cioè «non ritentare», e la ripresa non partiva mai**) |
| `src/hf/fmlx_hf_manifest.c` — lista dei file di un repo (ciò che serve a `pull` per sapere COSA scaricare) | **esiste, compilato, testato** (51 verifiche senza rete + sonda sul server vero: `bert-base-uncased` 16 file, commit `86b5e09…`, 7 con `lfs`) |
| `src/json/fmlx_json.c` + `fmlx_jsonw.c` — parser e writer JSON | **esiste, compilato, testato** (327 + 148 verifiche, **e** differenziale contro CPython su 122 record: 8 divergenze dure volute, 0 round-trip — `docs/json-contract.md`) |
| `src/server/fmlx_http.c` — codec HTTP/1.1: framing richiesta, testata risposta, SSE | **esiste, compilato, testato** (127 verifiche byte-esatte) |
| `src/server/fmlx_api*.c` — router + adapter OpenAI/Anthropic sopra il codec | **esiste, compilato, testato** (151 verifiche). Non c'è ancora un motore sotto: la generazione è attaccata al confine `FmlxApiNext`, che il test copre con un doppio scriptato |
| `src/server/fmlx_serve_respond.c` + `fmlx_serve.c` — il server: `/v1/models`, `/health`, il 501 per ogni rotta che genera, e il loop TCP | **esiste, compilato, testato, provato su socket vero** (163 verifiche: framing, `bind` rifiutato e arresto su segnale + uno smoke del binario con `curl` vero). Il motore non c'è e **non serve per provarlo** |
| `include/mlxengine.h` + `src/engine/mlxengine.c` — **MLXEngine, il deliverable ①**: l'ABI del motore come libreria condivisa (`libmlxengine.1.dylib`, 20 simboli `mlxengine_*`) | **ELIMINATO 2026-09**; era compilato e testato (192 verifiche). Copre il lettore GGUF e il dequant; i pesi packed, la cache KV, l'attenzione, il MoE e la generazione NON ci sono ancora ed è il punto d'ingresso loro e del numero d'ABI. Prova di fedeltà del confine: la stessa fixture aperta due volte — attraverso l'ABI e dalla parte di dentro — e i bit confrontati. C'è anche un consumatore esterno vero (`tools/engine_abi_probe.c`: un header e una dylib, niente sorgenti) provato su 6 GGUF reali |
| `src/lane_mlx/` — motore MLX estratto da GMBasic | **mai scritto, e non serve**: il motore è arrivato da LLMEnv il 2026-09-28 (`LLMEngine/mlx`). La D2 dell'epoca è superata |
| `src/cli/fmlx_pull.c` + `fmlx_pull_progress.c` + `fmlx_cli.c` — `fastllm pull`, l'avanzamento, il dispatch e gli esiti | **esiste, compilato, testato, provato sul server vero** (102 + 214 verifiche, di cui 3 piani distinti per la barra: renderer con orologio in mano + terminale finto `openpty` + pipe; pull reale `sshleifer/tiny-gpt2`: 9/9 file, 9 blob il cui nome è il loro hash, snapshot riletto da `hf_probe`) |
| `src/cli/fmlx_models.c` + `src/hf/fmlx_hf_catalog.c` — `fastllm models` / `list` | **esiste, compilato, testato, provato sul cache vero di questa macchina** (139 + 214 verifiche; 27 repo letti in <0,01 s, 3.675.292.998 byte su 76 inode che coincidono con un conteggio di inode fatto a parte con `find`+`stat`; **la riga `[!]` di un repo che non si è potuto aprire è una classe, non un'assenza** — vedi sotto) |
| `src/cli/fmlx_serve.c` — `fastllm serve` | **esiste, compilato, testato, provato**: risolve host/porta/chiave (flag > ambiente > default, piano §14) e passa tutto al loop. `--cache` vale anche qui: `pull`, `models` e `serve` guardano LO STESSO disco |
| `src/hf/fmlx_hf_audit.c` + `src/cli/fmlx_doctor.c` — `fastllm doctor`: gli byte del cache sono quelli che promettono | **esiste, compilato, testato, provato sul cache vero** (179 + 61 verifiche; 27 repo, 71 entry, 71 verificati, **0 problemi**, 19 note in 1,3 s, e `--quiet` esce 0). Verifica con la STESSA `fmlx_hf_verify` del `pull`: un verdetto scritto a parte sarebbe verde qui e rosso al download |
| `src/cli/fmlx_asr.c` — `fastllm asr <model_dir> <file.wav>` | **esiste, compilato, provato**: speech-to-text Qwen3-ASR (engine `llm_asr` di LLMEnv/LLMEngine/asr, **inference-only/Apache** — LA STESSA sorgente usata da GMBasic, nessuna copia). Il testo esce **token-live** su stdout (streaming: chunk 2 s + cache finestre encoder + rollback). Validato su `test_speech.wav`, `45s.wav` (word-for-word), `jfk.wav` |
| `src/cli/` — `run\|chat\|ps\|cp\|rm` | **non collegati, e lo dicono**: `fmlx_cli_exec` ritorna `NOT_WIRED` nominando il pezzo che manca (motore / registro delle sessioni / chi scrive nel cache) ed esce 2. Il parser delle bandiere invece **esiste**, ed è provato per i 5 verbi rimasti — e una bandiera che il verbo non legge è un errore, non un'opzione ignorata |

Qui sotto c'è solo ciò che compila davvero. Nessuna cartella vuota "perché il piano la prevede".
L'enumerazione di ciò che MLX/caricatore loader GGUF sanno davvero fare, con i `file:riga` della
lettura, è in **`docs/quant-coverage.md`** (ed è lì che è finito il bug dei cinque tipi spostati).

## La lane GGUF oggi — **STORICO (ora in LLMEnv/LLMCodec)**

> `src/lane_gguf/` è stato eliminato: lettore, dequant e tassonomia d'errore vivono in
> `LLMEnv/LLMCodec` (`llmcodec`, `llmcodec_ggml`). Questa sezione resta perché descrive i quattro
> difetti dei lettori storici che il nuovo codec ha evitato.

`include/fmlx/fmlx_gguf.h` + `src/lane_gguf/fmlx_gguf.c`: **un solo** lettore di record GGUF,
scritto per non ripetere i quattro difetti misurati nei lettori esistenti (due dei quali sono
bug aperti nel vault GMBasic):

1. **nessun cap** sul numero di tensori (lì: `GMB_GGUF_MAXTI 4096` con lo stesso `false` di un
   file corrotto). Esiste solo una guardia di *plausibilità* del conteggio rispetto ai byte
   disponibili, con codice proprio (`ERR_COUNT`).
2. **`general.alignment` onorato** (lì: ignorato, 32 hardcoded) e validato come potenza di due.
3. **tabella dei tipi keyed per valore di enum**, non per posizione — con 42 righe che devono
   dire li stesso numero di `ggml.h` (lì, nei lettori di casa: posizionale 0..19, con IQ3_XXS/IQ1_S
   sbagliati e **`return 0` silenzioso** per ogni tipo fuori range, **BF16 = 30 compreso** — e BF16
   è già in centinaia di tensori dei file su questa macchina).
   **Questa voce è stata FALSA per un po', nel nostro codice**: trascrivendo l'enum in un array
   posizionale avevo compresso i buchi 31/32/33 e 36/37/38, spostando di uno TQ1_0, TQ2_0, MXFP4,
   NVFP4 e Q1_0 (34/35/39/40/41 i valori veri, non 33/34/38/39/40). Il `test_gguf` non lo vedeva
   perché la sua lista di riferimento era la mia stessa trascrizione: due copie dello stesso
   errore concordano. Ora la tabella è fermata da due cose che non dipendono da me:
   `_Static_assert` contro i macro di `ggml.h` (numero sbagliato = non compila) e l'**asse della
   forma** nel gate (elementi/blocco e byte/riga confrontati con `ggml_row_size` per tutti e 42
   i valori). Nessun file locale esercitava quei cinque tipi, quindi nessun confronto bit a bit
   li avrebbe mai toccati: è `docs/quant-coverage.md` con la storia completa.
4. **ogni rifiuto ha il SUO codice e il SUO messaggio** (19 codici `FMLX_GGUF_ERR_*` + OK):
   tipo ignoto, tipo ritirato, tipo valido ma non coperto, `numel` non multiplo del blocco,
   overlap, nome duplicato, dati corti, NDIMS assurdo, dimensioni negative, KV di tipo
   sconosciuto, array annidato. Mai un byte-count 0 che passa per un tensore vuoto, e
   `fmlx_gguf_last_error` non ritorna **mai** NULL dopo un'apertura fallita (il path e il
   motivo di sistema ci sono nel messaggio).

API: `fmlx_gguf_open_header` (mmap `PROT_READ|MAP_PRIVATE`: la testata senza materializzare i
pesi), `fmlx_gguf_parse` (stesso parser su un buffer in memoria), `fmlx_gguf_read_tensor`,
`fmlx_gguf_find_tensor` / `find_kv` / `kv_str` / `kv_u64`, `fmlx_gguf_last_error`,
`fmlx_gguf_close`. Il file resta di proprietà del chiamante solo se `owns_buffer` è 0.

## La lane di dequant oggi

`include/fmlx/fmlx_quant.h` + `src/lane_gguf/fmlx_quant.c` (smistamento e rifiuto) +
`fmlx_quant_ext.c` (sola aritmetica) + `fmlx_quant_iq.c` (valori che escono da una codebook):
`fmlx_ggml_dequant_row(type, src, n, dst)` converte una riga di `n` elementi in F32. Copre **31**
tipi: F32, F16, BF16, Q4_0, **Q4_1**, **Q5_0**, **Q5_1**, Q8_0, Q2_K, **Q3_K**, Q4_K, Q5_K, Q6_K,
**Q8_K**, IQ2_XXS, **IQ2_XS**, **IQ2_S**, **IQ3_XXS**, **IQ3_S**, **IQ1_S**, **IQ1_M**,
**IQ4_NL**, **IQ4_XS**, I8, I16, I32, **TQ1_0**, **TQ2_0**, **MXFP4**, **NVFP4**, **Q1_0**
(il grassetto è ciò che è entrato dopo la prima enumerazione dei tipi: tre lotti di lavoro). Non è una scelta
di comodo: ogni tipo è entrato perché esiste un **secondo codice** con cui confrontarlo bit a bit —
oggi i codici indipendenti confrontati dal gate sono tre (due di casa + `ggml` di llama.cpp) e i 31
sono esatti su almeno uno, dove gli altri due ce l'hanno. Tutto il resto (Q8_1, Q4_2/3, I64, F64)
ritorna `FMLX_GGUF_ERR_NOT_COVERED` e **non tocca `dst`**: gap dichiarato, non uno stub che
rimette zeri. `fmlx_ggml_type_is_covered` permette di saperlo prima di copiare byte. Quei quattro
non sono pigrizia: **non esiste** un secondo codice con cui confrontarli (ggml non ha
`dequantize_row_q8_1`, Q4_2/Q4_3 sono ritirati upstream, I64/F64 non sono quantizzazioni) —
restano fuori finché non compare una fonte, e `coverage_audit` lo ricorda a ogni esecuzione.

**I sei IQ che erano gap sono chiusi** (IQ2_XS, IQ3_XXS, IQ2_S, IQ3_S, IQ1_S, IQ1_M), e la regola
che avevamo scritto per entrarci è stata rispettata alla lettera: **nessun campione**, una passata
**esaustiva per indice**. `grid_sweep()` nel gate costruisce blocchi che exercano ogni voce di
ognuna delle 11 righe di codebook (256, 512, 256, 512, 1024, 2048×2, 16×4) e batte i float contro
la tabella **compilata dentro** `libggml-base.dylib`: `256/256 … 2048/2048, nessun bit divergente`.
Lo sweep non è ridondante, e il perché è misurato: scambiando due voci adiacenti dell'header
generato, il vecchio `gen_codebooks.py --check` (rimosso: Python-free) diceva «tutte le copie concordano» (confrontava le copie
sorgenti, non rilegge il file scritto) e la forma passa lo stesso (due valori legali, scambiati
di posto); risponde solo lo sweep. Le cinque griglie che mancavano ora ci sono (`src/lane_gguf/
fmlx_iqgrid_tables.h` + `fmlx_iq1s_tables.h`, generate), mentre `iq1s_grid_gpu` **non si copia**:
è referenziata solo da codice device (CUDA/SYCL/Metal/Vulkan, misurato con `grep` su
`~/llamacpp/ggml/src` — zero riferimenti in `ggml-quants.c`) e con un'encoding diversa.

Conseguenza misurata: **ogni tensore dei 7 GGUF presenti su questa macchina è di un tipo
coperto** (istogrammi: `F32 F16 Q8_0 Q2_K Q4_K Q6_K IQ2_XXS I32 BF16`), e il gate sotto lo
conferma bit a bit su quei tensori reali.

Le codebook (`src/lane_gguf/fmlx_iq2_tables.h`, `fmlx_iqgrid_tables.h`, `fmlx_iq1s_tables.h`) sono
**generate**, non trascritte a mano (dal generatore `gen_codebooks.py`, poi rimosso perche' il repo e' Python-free; restano le tabelle e lo sweep di conformita'): il generatore confrontava voce per
voce le copie indipendenti su disco e rifiuta di scrivere se divergono anche di un solo bit. Il
**grado** di prova non è lo stesso ovunque, e i file generati lo dicono:
`iq2xxs_grid`/`iq2_ksigns` hanno **due autori distinti** (ds4 + la linea ggml dei duplicati
GMBasic); `kvalues_iq4nl` ha **due versioni di ggml** (`~/llamacpp` e il vendor dentro GMBasic),
che esclude la deriva di versione ma non un errore di conception; `kvalues_fp4` ha un testimone che
non è una copia — lo script **ricalcola i 16 valori dai bit E2M1** della specifica OCP MX
(`derived_e2m1_doubled`, e il check è stato verificato capace di andare rosso); le cinque griglie
IQ del lotto C hanno **una sola** sorgente, e lì la prova non è il confronto fra copie: è lo sweep
eseguito indice per indice. Licenza MIT ggml: vedi `LICENSES.md`.

## MLXEngine — l'ABI del motore (`include/mlxengine.h`, `libmlxengine.1.dylib`) — **STORICO**

> Rimosso: `include/mlxengine.h`, `src/engine/mlxengine.c` e il target `mlxengine` **non esistono
> più** (eliminati con `src/lane_gguf/` quando il codec è passato a LLMEnv). Il motore oggi è
> `llmengine.h` di LLMEnv. La sezione resta come memoria dei gate di fedeltà dell'ABI.

Il deliverable ① del piano esiste da oggi come **oggetto linkerabile**, non come nome di sezione:
`add_library(mlxengine SHARED)` produce `libmlxengine.1.0.0.dylib` con i symlink
`libmlxengine.1.dylib` e `libmlxengine.dylib`, e esporta **venti simboli `mlxengine_*`**
(`nm -gU` li elenca; nessuno `fmlx_*` esce dalla statica che sta sotto — l'ABI è l'unica
superficie, ed è precisamente il punto per cui una dylib non è un header in un'altra cartella).

Cosa copre oggi: il lettore GGUF e il dequant. **Non** i pesi packed, la cache KV, l'attenzione,
il MoE, la generazione: quelli entrano qui con lo stesso numero d'ABI (`MLXENGINE_ABI_VERSION 1`)
e un gate di parità per ciascuno. Sulla superficie valgono tre idee ferme:

- **i codici del file non sono i codici della chiamata.** Sotto i 32 ci sono quelli del formato
  (`FmlxGgufStatus`, citato e non duplicato), dal 64 quelli d'uso (`MLXENGINE_ERR_*`). Confonderli
  fa fare la cosa opposta a chi ha torto in uno dei due casi: correggere il programma, o
  riscrivere byte che erano perfetti.
- **`at` conta gli elementi, non i byte**, e una finestra che cade in mezzo a un blocco è
  rifiutata (`ERR_MISALIGNED`): gli indici di un blocco condividono la scala, quindi mezzo blocco
  non ha valori corretti da dare. È la ragione per cui questa funzione esiste invece di un
  `memcpy`. E è la ragione per cui esiste `mlxengine_tensor_block_vals`: senza il numero del
  blocco, un chiamante che non ha i ns. sorgenti non può calcolarsi una richiesta legale —
  gli resterebbe «ho chiesto 8 pesi di un Q4_K e mi ha detto no», che è un errore dell'ABI
  travestito da errore del file.
- **aprire non materializza**: la mappatura è `PROT_READ|MAP_PRIVATE`, i pesi restano su disco e il
  SO li porta in RAM se e quando si leggono — lo swap su disco nello spirito di `mlx-lm`, e
  l'unico modo di aprire un file da ~90 GB. Non c'è una bandiera `swap: on/off` proprio perché
  non esiste l'alternativa da nascondere.

`test_engine` (192 verifiche) si comporta da chiamante esterno: costruisce la fixture col writer e,
per provare la **fedeltà del confine**, apre lo stesso file due volte — una attraverso l'ABI, una
dalla parte di dentro — e confronta i bit della finestra centrale. Un ① che «sembra uguale» non
prova niente; la differenza di due float sì.

Fuori dal progetto, `tools/engine_abi_probe.c` è un consumatore che ha **solo** `mlxengine.h` e la
dylib (la riga di compilazione è nel file, e non passa da CMake apposta). È lui che ha trovato il
buco: `fmlx_ggml_type` non esce dalla libreria, quindi un programma esterno non poteva sapere il
blocco di un tipo e non aveva modo di costruire una richiesta accettabile — da lì
`mlxengine_tensor_block_vals`, al suo posto fra i venti simboli. Il probe stampa i pesi
**solo** quando l'esito è OK per la stessa ragione: dopo un rifiuto `dst` contiene i byte di prima,
e chi li stampa li crederà pesi. Sui 6 GGUF reali di questa macchina (`~/ai/Models/gguf`, da
`mmproj-Qwen2.5-Omni-3B-Q8_0` a `qwen2.5-omni-3b-Q4_K_M`, tipi 0/1/8/12/14/30, blocchi 1/32/256)
dà 6 puliti, 0 non aperti, 0 in contraddizione.

## La lane HF (cache) oggi

`include/fmlx/fmlx_hf.h` + `src/hf/fmlx_hf_cache.c` + `fmlx_hf_hash.c` + `fmlx_hf_catalog.c`: legge
il cache di Hugging Face **senza toccare la rete**. Ogni regola è misurata da una fonte, non
ricordata:

- **radice**: `HF_HUB_CACHE` → legacy `HUGGINGFACE_HUB_CACHE` → `HF_HOME/hub` →
  `XDG_CACHE_HOME/huggingface/hub` → `~/.cache/huggingface/hub`, con `expanduser` prima di
  `expandvars` (`huggingface_hub/constants.py:166-189`). La precedenza del legacy su `HF_HOME`
  sembra strana e invece è esatta: in quel file il legacy è il *default* di `HF_HUB_CACHE`.
- **nome dei blob, e qui c'è la trappola**: 64 esadecimali = **SHA256 del contenuto** (verificato
  su 2/2 blob LFS, incluso `model.safetensors` da 335.450.584 byte); 40 esadecimali **NON** è lo
  SHA1 del contenuto — 6/6 blob locali lo smentiscono — ma il **git blob id**, SHA1 di
  `"blob <len>\0" + contenuto` (3/3 confermati con `git hash-object`). Un verificatore che
  assumesse lo SHA1 nudo scarterebbe *ogni* file non-LFS del cache come corrotto.
- **profondità dei symlink**: `(../)×(2 + barre del path) + blobs/<nome>`, confrontata come
  *forma* e non risolta con `realpath`: un symlink che punta altrove è `ERR_LINK_FORM`, non un
  path da seguire. La regola è confermata dal caso reale più profondo trovato su questa macchina
  (`…/coreml/fill-mask/float32_model.mlpackage/Manifest.json`, cinque `../`).
- **quattro "non trovato" diversi**, perché solo uno dei quattro giustifica una richiesta di rete:
  repo assente, snapshot assente, entry non scaricata, file **che il remote non ha** (marker
  `.no_exist`, misurato come file vuoto). `ERR_NOT_ON_REMOTE` è il codice che impedisce al
  downloader di rincorrere per sempre un 404.
- Le entry **file regolare** dentro lo snapshot (scritte da un altro tool, senza symlink) sono
  usabili ma `fmlx_hf_verify` risponde `ERR_NOT_VERIFIABLE`, **non** "ok": il nome non promette
  nulla, e un falso positivo qui è un file corrotto accettato.

Cosa **non** fa ancora: nessun protocollo xet — il che è una scelta, non un buco: quando il server
annuncia `X-Xet-Hash` i byte arrivano comunque dal backend classico HTTP e **lo si dice su stderr una
volta sola per processo** (piano §12.4). Resta non provata dal fake la lettura di quell'header nel
transport curl: il fake setta `FmlxHfMeta.xet_announced` a mano, quindi la riga di `hdr_cb` che la
produce si vede solo sul server reale. Nessuna API `delete`/`upload`.

## Lane download (`src/hf/fmlx_hf_fetch.c` + `fmlx_hf_curl.c`) — esiste, compilato, provato

La logica di cache e il trasporto sono due file separati perché la prima si prova **senza rete**:
`tests/hf/test_hf_fetch.c` usa un transport finto (24 casi, 199 verifiche) e scopre i bug di
DOVE-finiscono-i-byte, che sono gli unici che corrompono un cache condiviso.

Lo **stream fermo** è l'unico fallimento che si ritenta: la soglia (`FmlxHfFetchOpts.stall_timeout_s`,
`--stall-timeout` della CLI) arriva al transport con `configure` e vive nelle opzioni della chiamata,
non in una scelta cablata del transport. **Come taglia davvero è misurato, non dedotto**
(`tools/hf_stall_probe.c`, libcurl 8.7.1 SecureTransport di sistema): su uno stream muto
`CURLOPT_XFERINFOFUNCTION` non viene chiamata **neanche una volta**, quindi non tronca niente; l'unico
a parlare è `LOW_SPEED_TIME`, e lo fa a `soglia + 6` secondi (1→7,03 · 2→8,02 · 3→9,03 · 5→11,04 ·
6→12,04) con `CURLE_OPERATION_TIMEDOUT`. Quel codice è **lo stesso del `CONNECTTIMEOUT`**, quindi il
transport lo traduce in `ERR_STALL` solo SE il server ha già risposto (`saw_status`): senza riga di
stato è una connessione morta e resta `ERR_HTTP`. La ripartenza chiede `Range: bytes=N-` sui byte già
scritti nel `.incomplete`, con `stall_retries` ripartenze e backoff 250→2000 ms. Un 404, un hash che
non torna o un lock in mano ad altri risale subito: ripeterebbe la stessa risposta.

Regole misurate, non ricordate. **Su questa macchina ci sono DUE `huggingface_hub` 0.36.2 che non
sono lo stesso file** (`~/ai/env-vllmlx`, serie `requests`, e `~/ai/env-mflux`, serie `httpx`): i
numeri di riga sottostanno sono di `env-mflux` salvo dove scritto diversamente, e i due set sono
**entrambi in `LICENSES.md`** perché citare una riga senza dire quale copia si è aperta non è una
prova, è una scommessa.

- **Lock**: `<cache>/.locks/<models--org--nome>/<etag>.lock`, preso con `flock(LOCK_EX|LOCK_NB)` e
  rilasciato **chiudendo il fd** — il file NON si cancella (`_unix.py:103-108` fa solo `LOCK_UN` +
  `close`; il cache reale è pieno di `.lock`). Scrivere `O_CREAT|O_EXCL` su quel path — la mia prima
  bozza — avrebbe bloccato il secondo download per sempre: "il file c'è" non vuol dire "è preso".
- **Temporaneo e resume — la lezione delle due copie.** Il comportamento del `.incomplete` **non è
  lo stesso** nelle due installazioni:

  | | `env-vllmlx` (serie `requests`) | `env-mflux` (serie `httpx`) |
  |---|---|---|
  | nome logico del temp | `blobs/<etag>.incomplete` — `:1176` | `blobs/<etag>.incomplete` — `:1237` |
  | nome **reale** del temp | lo stesso, condiviso fra processi | `<etag>.<8 hex uuid4>.incomplete` — `:1908` |
  | fallimento a metà | **resta**: «By default, we will try to resume the download if possible» `:1700-1704` | **cancellato** `:1944-1947` |
  | perché è cambiato | — | `:1900-1907`: su Lustre/GPFS/alcuni NFS `flock(2)` **riesce silenziosamente a tutti**, quindi un temporaneo condiviso con un lock non onorato **corrompe il cache**; col nome per processo un lock rotto costa solo banda duplicata |

  FastLLM sta sulla semantica di **sinistra** — nome determinato, partial mantenuto — perché il caso
  d'uso è un CLI interrotto a 30 GB: un suffisso casuale — la mia seconda bozza — non lo ritrovano
  né l'altro processo né il prossimo avvio. **Costo onesto: siamo più esposti a un lock non
  onorato.** Regge a due condizioni, entrambe verificate qui: `flock` deve essere vero (su APFS lo
  è, misurato: secondo processo in attesa ≥140 ms con timeout 150) e il lock va preso **prima** di
  aprire il temporaneo, sempre. **Su un filesystem di rete questa lane non è sicura**: lì il nome per
  processo è la scelta giusta e il resume cross-processo si perde.
- **Il commit vince sul ref**: i byte vanno nello snapshot di `X-Repo-Commit`, non in quello di
  `refs/<ramo>` di ieri. La misura è `:1732-1737` — **il commit viene dalla risposta e se manca è
  errore**, non si ripiega sul ref locale; il ref si aggiorna DOPO, e un ref in sola lettura non
  trasforma un download riuscito in un errore.
- **Range ignorato = si azzera**: se il server risponde 200 a una richiesta con `from > 0`, il
  transport tronca il temporaneo prima di scrivere (`:384-396`: `seek(0)` + `truncate()` +
  `resume_size = 0`), o il file sarebbe un corpo doppio.
- **Temporaneo già completo ⇒ nessun GET**: `:359-361` (`resume_size == expected_size` → `return`).
  È la guardia che rende il "resume" un non-evento quando il lavoro è finito.
- **Il limite è il file intero**, non "quanto manca": fermare la risposta a metà sarebbe un falso
  positivo; resta la guardia contro il server che dichiara 1 KB e ne streamma 1 TB.
- **Credenziali**: `Accept-Encoding: identity` (altrimenti l'hash non combacia mai, `:1612`), token
  **mai** spedito a un host diverso da quello della richiesta (`:1762-1766`: `headers.pop
  ("authorization")` quando `netloc` cambia — il CDN ha già l'URL firmato) e **mai** in un messaggio
  d'errore.
- **L'etag DIVENTA un nome di file**: passa dalla stessa regola del path letto da un symlink
  (`fmlx_hf_name_is_safe`), perché una risposta ostile non deve scegliere un path nel nostro cache.
- **Verifica prima del `rename`**: il nome definitivo compare solo quando i byte hanno già passato
  l'hash che quel nome promette. Un blob falso non esiste, punto; un `.incomplete` troncato resta.

**Contatto con il server vero** (`build/hf_fetch_probe`, fuori da `ctest` di proposito):
`google-bert/bert-base-uncased/config.json` → `45a2321a7ecfdaaf60a6c1fd7f5463994cc8907d`, e
`git hash-object` sul blob scaricato dice lo STESSO nome: la regola del "git blob id" verificata
contro i byte del server, non contro il nostro codice. Il lato SHA256 (LFS) è verificato sullo
stesso file reale da 335 MB del cache locale (`hash=2 → ok`).

**Trasporto del download, deciso con una misura.** Avevo scritto che su questa macchina non
esistono le intestazioni di curl: **falso**. `MacOSX.sdk/usr/include/curl/curl.h` c'è, e un probe
compilato e linkato con `cc … -lcurl` ha stampato `libcurl/8.7.1 SecureTransport`. Così è: la lane
di download usa **libcurl di sistema** (`find_package(CURL)`, TLS via SecureTransport — nessuna
dipendenza da vendere, nessun CA bundle da distribuire), non un figlio di `/usr/bin/curl` e non
socket nudi senza TLS come `gmb_network.c`. Gli SHA per la verifica vengono da `CommonCrypto`
(`CC_SHA1`/`CC_SHA256`, header nell'SDK): una nostra copia di SHA1 sarebbe l'unico modo di
sbagliarla in silenzio.

### Il watchdog di stall misurato (`tools/hf_stall_probe.c`) — e il difetto che ha trovato

Non è un caso di `ctest` e non lo diventerà: sta nel tempo (≈16 s). Ma è la guardia che rende `pull`
riprendibile su un blob da 30 GB, e **non era mai stata eseguita nemmeno una volta**: il fake
transport copriva la *politica* dei ritentativi, `hf_fetch_probe` parla con huggingface.co dove il
server non lo puoi far fermare. Qui il server che si ferma è nostro, locale (una porta effimera,
`bind(0)` + `getsockname`), e la misura è su libcurl vera.

**La prima esecuzione ha fallito 6 verifiche su 14 sul codice COME ERA.** `esito = risposta HTTP non
valida (… Transferred a partial file)` a 8,00 s: il che vuol dire che il `CURLE_OPERATION_TIMEDOUT`
di `LOW_SPEED_TIME` finiva in `ERR_HTTP` — la classe **non** ritentabile — e il ciclo di
`fmlx_hf_fetch.c:435` (`if (s != FMLX_HF_ERR_STALL …) goto done`) non riprovava. In altre parole:
`--stall-retries` era decorativo per l'unico caso a cui serve, lo scaricamento che si ferma a metà.
Da lì il ramo `curl_stalled_by_lowspeed` descritto sopra.

**Dopo la correzione** (`./build/hf_stall_probe work-hf-stall 2 1`):

| fase | cosa succede | misura |
|---|---|---|
| 1 — `stall_retries 1` | HEAD → GET muta → watchdog → ripartenza `Range: bytes=1500-` → 206 → rinomina | `esito = ok` a **8,29 s**, `[fetch] stream fermo: ripartenza da 1500 byte su 4096`; i 4096 byte sono **uguali byte per byte** a `BODY` e l'hash verifica |
| 2 — `stall_retries 0` | lo stall risale, i byte restano | `esito = ERR_STALL` a **8,02 s** con `nessun byte da 8 secondi (soglia 2): 1500 nuovi, 1500 su 4096`; il `.incomplete` ha **1500** byte |

16 verifiche, 0 fallite. Le prove che contano non vengono dalle parole del server (è finto, le
scriviamo noi): vengono dai **byte** — una ripresa dal numero sbagliato darebbe un file diverso e
l'hash non combacerebbe — e dal **contare i byte del `.incomplete` con `stat`**, non dal fidarsi del
messaggio. L'`ETag` che il finto server promette è il git blob id calcolato con
`fmlx_hf_hash_fd`, la stessa funzione della lane; una copia dell'algoritmo nel probe sarebbe il
secondo parere che non volevamo.

**Tre controlli negativi** (mutazioni su copie fuori dal tree, in `build/nc/`, linkate davanti
all'archive member — mai nel sorgente): **A** `curl_stalled_by_lowspeed` che ritorna 0 → 6 fallimenti
e il messaggio nomina la classe mancante; **B** `Range: bytes=0-` forzato al posto del numero vero →
`5596 byte contro 4096 dichiarati`, cioè una corruzione presa sul fatto; **C** togliere il rinfresco
dell'orologio sui chunk **NON cambia niente** — 16 verifiche, 0 fallite, perché in questa scena i byte
arrivano un istante dopo la riga di stato e i due orologi coincidono. **Quel rinfresco dunque NON è
coperto dal probe**: è lì perché un download lungo che si ferma a metà dica il vero nel messaggio
("nessun byte da N secondi"), e la ragione è la stessa per cui `xfer_cb` resettava a ogni byte.

## Manifesto dei file (`src/hf/fmlx_hf_manifest.c`) — la prerequisita di `pull`

`fmlx_hf_manifest(repo, revision, opts, &out)` chiede **una** GET di metadati
(`{endpoint}/api/models/{repo}[/revision/{quote(rev)}]?blobs=true`, la rotta esatta di
`hf_api.py:2648-2657`) e restituisce `sha` + la lista di `siblings[].{rfilename,size,blobId,lfs}`.
Perché un file suo: il download parla di UN file e scrive nel cache, il manifesto parla di TUTTI i
file e **la nostra lane nel cache non ci mette nulla**. Qui c'è una correzione misurata:
`huggingface_hub` 1.31.0 **una lista la scrive**, `trees/<commit>.json` (`_tree_cache.py:19-31`; su
questa macchina esiste davvero, in `models--bert-base-uncased/trees/`) — ma è **chiavata sul commit**,
e la richiesta arriva per **revisione**: rispondere a «dov'è `main` adesso» con l'albero di un commit
vorrebbe dire servire un'idea vecchia di una spanna. Da cui la regola, che resta: `offline` qui è un
**RIFIUTO**, non "lista dal cache". `tests/hf/test_hf_manifest.c` (19 casi, 51 verifiche) gira senza
rete su un transport finto e prova anche che `meta`/`get` non vengono mai chiamati.

Le due decisioni che sembrano pignole e sono il motivo per cui il file esiste:

- **un `siblings` vuoto è un ERRORE, non un repo da zero file** (`ERR_HTTP`): è ciò che
  `_snapshot_download.py:259-264` dichiara inaffidabile, e un `pull` che non scarica "niente"
  sembra un successo;
- **un `rfilename` ostile non si salta, si rifiuta tutto** (`ERR_NAME`): `../`, path assoluti,
  nomi oltre `FMLX_HF_PATH_MAX`. Saltare un file di un repo che si sta scaricando intero è il
  risultato silenzioso che il piano vieta.

**Misurato sul server vero** (`build/hf_manifest_probe`, fuori da `ctest`): `bert-base-uncased` →
commit `86b5e093…`, 16 file, 7 con `lfs`, `blobId` **40 hex in tutti e 16 i casi**;
`mlx-community/Meta-Llama-3.1-8B-Instruct-4bit` → 8 file. Da lì una trappola smontata prima di
caderci: il `blobId` del manifesto è il **git blob id**, ma un file LFS nel cache si chiama con lo
**sha256** preso dall'etag (misurato sugli stessi repo: `X-Linked-ETag` a 64 hex su
`model.safetensors`, e `config.json` non-LFS dove le due cose coincidono). Il campo si chiama
quindi `git_blob_id`, e `pull` deve chiedere il NOME al download lane — mai dedurlo dal manifesto.

## La lane HTTP (`src/server/fmlx_http.c`)

Il pezzo che tocca per primo i byte che arrivano da internet, e che sta **sotto** gli adapter. Qui
il contratto non è "fare HTTP": è **non poter essere tratti in inganno** su dove finisce una
richiesta e dove comincia la successiva — la classe di bug (request smuggling) in cui un parser
permissivo e uno severo, messi in fila, non sono lo stesso server.

**Ogni ambiguità di framing è un rifiuto con un codice proprio**, perché l'alternativa è
indovinare: due `Content-Length` **diversi** (`CL_AMBIGUOUS`; due valori *uguali* è solo
ridondanza e passa), `Transfer-Encoding` **insieme** a `Content-Length` (`TE_WITH_CL` — vince su
`CHUNKED`, perché la presenza di entrambi è già di per sé il segnale, anche se il TE dice
`chunked`), chunked **in richiesta** (`CHUNKED`), TE senza `chunked` (`BAD_TE`), POST/PUT/PATCH senza `Content-Length`
(`NO_CONTENT_LENGTH` — non si indovina "0 byte"), obs-fold (`FOLDING`), `\r` non seguito da `\n`
(`LONE_CR`). Un `\n` **sciolto** invece è accettato: la severità che serve sta sul corpo, non sui
byte di fine riga. `NEED_MORE` **non** è un errore: è l'unico modo di dire "ho letto, non basta"
senza far perdere i byte già letti — compreso il caso `\r` come ultimo byte, dove un `LONE_CR`
buttrebbe via una richiesta perfetta a metà.

**Zero allocazioni**: il parser non copia e non alloca; `FmlxHttpReq` è fatto di `FmlxSlice`
(`{ptr, len}`, **non** NUL-terminati) che puntano nel buffer del chiamante, che deve restare
valido e immutato. `fmlx_http_req_init` è obbligatorio: un struct non inizializzato non è un
valore, è un `n_hdr` casuale che diventa una lettura fuori array.

**Il writer risponde alla stessa domanda del writer JSON**: "un `Content-Length` che promette più
byte di quelli inviati è una connessione appesa, non una risposta sbagliata". La validazione è
**prima** di scrivere e, al fallimento, `w->len` torna al segno di partenza: nessun pezzo di
testata lasciato indietro. (Il test l'ha scoperto: con la validazione dopo, un `Content-Type` con
CRLF dentro lasciava 36 byte di testata a metà.) `Content-Length` è scritto da `body_len`, quindi
non esiste una chiamata che possa scegliere un valore incoerente col corpo. Gli extra header sono
una lista `"A: b\r\nC: d"` **senza** CRLF finale.

`Connection: close` **vince sempre**, in qualunque posizione della lista: per un server chiudere è
sempre sicuro, restare aperti su una connessione che il client voleva chiudere lascia il corpo
successivo in coda sul socket. SSE: un `\n` dentro il payload diventa **più righe `data:`** — è
l'errore SSE più comune e silenzioso, e lo gestisce il writer, non chi chiama. Il chunk copre
l'evento intero e la proprietà verificata dal test è che il conteggio esadecimale dichiarato
eguagli i byte realmente emessi, confrontato con la stessa scrittura **senza** chunking (il
testimone indipendente).

## La lane API — router e adapter (`src/server/fmlx_api*.c`)

Il pezzo che sta **sopra** il codec e **sotto** il motore, ed è il punto in cui i due dialetti
diventano una cosa sola. Tre file: `fmlx_api.c` (ciò che non ha dialetto: catalogo errori, rotta,
accumulatore, ponte), `fmlx_api_openai.c`, `fmlx_api_anthropic.c` — uno per formato perché il
requisito qui è che un reader possa tenere in testa UN dialetto per volta, e niente nel core
menzioni l'altro.

**Il confine è un puntatore di funzione, non un'assunzione.** Il layer ① (motore) emette eventi
neutrali — `{ev, text, n, tool_id, tool_name, finish, stop_sequence, token}` — attraverso
`FmlxApiNext`; il layer ② (adapter) è l'unico che decide la forma di ciò che esce. Il motore non
sa cosa sia HTTP, l'adapter non campiona nulla. Per questo la lane è testabile **oggi, senza
motore**: il test attacca al confine un doppio scriptato e verifica byte, non uno stub che finge
gettoni.

**Un catalogo, due formati** (piano:513-518). `invalid_request`, `context_length_exceeded`,
`memory_guard_refused`, `model_not_found`, … sono il `code` interno; `type` è per dialetto. Due
codici nuovi perché la regola della casa è "un codice per causa": `not_found` (404 su rotta
sconosciuta, che NON è `model_not_found`) e `not_supported` (501) — quest'ultimo perché
`/v1/messages/count_tokens` **non viene stimato**: senza il chat template esatto del modello il
numero sarebbe falso, e il piano (494) dice che un numero falso si rifiuta, non si inventa.

**Il ponte `arguments` ↔ `input`** è la ragione per cui questa lane esiste. OpenAI mette gli
argomenti di un tool in una STRINGA JSON, Anthropic in un OGGETTO. Il passaggio passa da
`fmlx_api_tool_input`: si parsea una volta e si renderizza nell'altra forma con
`fmlx_json_render` — mai una stringa manipolata a mano. Da lì la regola di piano:505: se il JSON
è troncato, `finish_reason=length` e **nessun** blocco tool viene emesso (nemmeno uno parziale,
nemmeno `{}`). Nel non-stream è applicabile alla lettera; nello stream no, perché un byte già
partito non si ritira, e quindi diventa un invariante: `finish_reason` non è mai `tool_calls` se
gli argomenti raccolti non validano.

**Ciò che non è onorato viene DETTO.** `logit_bias` e `user` sono accettati per nome e dichiarati
in `ignored_params[]` (piano:474); `n:2` è un 400 con `param:"n"`, non una risposta singola
silenziosa; `max_tokens: 1.5` è un errore di tipo, non un troncamento. Due
`max_tokens`/`max_completion_tokens` discordi: vince il più recente, l'altro finisce negli ignored.

**`logprobs` è ONORATO dall'ABI v2** (`fmlx_api_openai_logprobs`): il runner MLX espone
`generate_ids_probs`, che dà la logprob del token SCELTO a ogni passo. Il server la porta in
`choices[].logprobs.content[]` (chat: `token`/`logprob`/`bytes`/`top_logprobs`) e nel formato legacy
di `/v1/completions` (`tokens`/`token_logprobs`/`top_logprobs`/`text_offset`), anche in streaming
(un token per frame). **Limite dichiarato:** l'ABI dà SOLO la logprob del token scelto, non la
distribuzione top-k, quindi `top_logprobs > 0` è un 400 con la causa e `top_logprobs` resta sempre
`[]`/`null`. `logprobs` combinato con `tools` è rifiutato: il testo passerebbe dal riconoscitore e
la corrispondenza token↔logprob non sarebbe più esatta. `response_format` è onorato con
l'iniezione dell'istruzione nel prompt e la post-validazione (non è grammatica nativa: l'ABI non la
espone).

**`|` non sequenzia gli operandi, e qui l'ordine È il contratto.** La prima versione degli adapter
accumulava gli esiti del writer così: `bad |= fmlx_jsonw_key(w, "id") | fmlx_jsonw_cstr(w, id);`.
Il C garantisce che i due valori vengano prodotti, non **in quale ordine**: un compilatore può
calcolare `id` prima di emettere la chiave, o un `arr_end` prima del suo `arr_begin`. La forma
corretta è `if (A || B) return …;` — `||` sequenzia. L'accumulatore `bad` è stato rimosso del tutto
(tutti i punti migrati con uno script una tantum, poi rimosso, che aveva anche la modalità di a capo per il limite
di 100 colonne), e il gate è un `grep`: `bad |=` deve restituire zero in `src/server/`.

**Zero allocazioni anche qui**, come nel codec: gli adapter scrivono nella memoria del chiamante e
`t->d`/`id`/`created` arrivano da `FmlxApiMeta` passati dall'alto — un adapter che leggesse
l'orologio o un RNG renderebbe ogni test non deterministico.

## La lane serve (`src/server/fmlx_serve*.c`) — un server che risponde DAVVERO, senza motore

Il layer ③: socket in, byte out. **DUE file perché hanno due contratti**, ed è la ragione per cui il
95% di questa lane è provabile in memoria: `fmlx_serve_respond.c` è **puro** (una richiesta già
incorniciata entra, una risposta completa esce — zero socket, zero orologio, zero `malloc`),
`fmlx_serve.c` è il **loop** (bind, accept, `recv`, keep-alive, segnali). Nella stessa finestra di
file, l'unica parte provabile in-process avrebbe dipeso dalla build di quella che chiede una porta
libera: un test di framing sarebbe diventato un test di rete.

**Le risposte sono di tre specie, e la distinzione è tutta qui:**

1. **quel che è vero** — `GET /v1/models` legge il catalogo dal disco (`fmlx_hf_catalog`, nessun
   transport) e `GET /health` dice cosa sa fare;
2. **quel che manca lo dice** — le rotte di generazione (`chat/completions`, `completions`,
   `messages`) GENERANO via l'ABI LLMEngine; quelle non ancora cablate (`responses`, `embeddings`)
   rispondono **501** con UNA frase loro e un `code` machine-readable per causa
   (`engine_not_wired`, …). Il piano (§11.2) vieta il risultato che sembra quello che non è: un
   200 con `content: ""` sarebbe il peggiore esito possibile — coerente, vuoto, e falso. `GET
   /metrics` è cablata e risponde testo Prometheus coi contatori VERI del server (richieste
   totali e per endpoint, token, durata, cache hit/miss del prefisso, modelli residenti, e
   `fastllm_kv_tokens_used`/`fastllm_kv_tokens_capacity` letti da `kv_stats` dell'ABI v2);
   `kv_clear` NON è usato: le sessioni sono per-richiesta e il prefisso salvato in KV è
   deliberatamente riusato dalla prefix cache, quindi azzerarlo la romperebbe;
3. **quel che NON richiede il motore viene già validato oggi** — auth, incorniciatura, tetto del
   corpo, JSON, `anthropic-version`. Un 400 che arriva oggi arriverà anche quando il motore ci
   sarà: il comportamento del server non deve cambiare all'arrivo della lane ①.

**`degraded` (200) contro `unavailable` (503)** è la scelta fra «sono limitato» e «sono rotto».
`/health` è `degraded` quando fa le due cose che dichiara e non può generare — dire `ok` a chi
chiede «qui si lavora?» sarebbe bugiardo per omissione, e 503 mentirebbe dall'altra parte. È
`unavailable` quando **nemmeno il catalogo si legge**: il cache non c'è o `HF_HOME` punta nel posto
sbagliato. I contatori (`repos_total`/`repos_listed`/`repos_refs_only`/`repos_non_model`/
`repos_unreadable`/`repos_ambiguous`) arrivano dagli stessi campi che stampa `fastllm models`, non da
un secondo conteggio — e i primi cinque sono una **partizione**: `total = listed + refs_only +
non_model + unreadable`, perché quattro numeri che non sommano al totale sono quattro numeri che
descrivono quattro conteggi diversi. `repos_unreadable` è contato **prima** del `kind`, che è
ortogonale (il tipo si ricava dal nome anche quando niente si è aperto); `ambiguous` è una qualità, non
una classe, e per questo non entra nella somma.

**L'auth viene SOLO da `FASTLLM_API_KEY`**, e non è una bandiera per lo stesso motivo per cui
`HF_TOKEN` non lo è: una chiave in argv finisce nello storico del terminale e in ogni script che la
invoca. Vuota o assente = **nessuna auth**, dichiarata in `/health` (`"auth":"disabled"`), perché
"ascolto su 127.0.0.1 senza chiave" è una scelta che l'operatore deve poter leggere. Il confronto è
a tempo costante, il 401 arriva **prima** della rotta (dire «questa rotta esiste» a un non
autorizzato è già un'informazione), e `/health` resta aperto: è la sonda del load balancer.

**Due bug veri, trovati dal test e non dalla lettura.** (i) il 401 e il rifiuto del corpo grande
dicevano in testata che la connessione restava aperta mentre il loop la **chiudeva**; (ii) la
versione HTTP era sempre `HTTP/1.1`, anche con un client che aveva chiesto `HTTP/1.0`. Corretti in
UN punto solo — `fmlx_serve_agree_head`, l'uscita unica — invece che sito per sito: due letture
divergenti della stessa decisione (tenerla o chiuderla) sono precisamente il buco. `1.1 → 1.0` ha la
stessa lunghezza quindi si scrive in posto; `Connection: close` sono 19 byte e si sposta il resto,
**`Content-Length` non si tocca** perché conta il corpo, non la testata.

**Mai keep-alive dopo un errore di incorniciatura** (regola anti-smuggling), e **sempre** consumato
`head_len + Content-Length` prima di leggere la richiesta successiva: lasciare il residuo nella
finestra non è un bug visibile, è un server che risponde in modo diverso a seconda di chi è arrivato
prima. Il corpo promesso e mai arrivato, la testata oltre il cap, `Transfer-Encoding: chunked` (501,
perché è «non lo implemento») e `chunked` insieme a `Content-Length` (400, perché è una divergenza
fra noi e un proxy, e 501 mentirebbe sulla causa) sono quattro casi con quattro codici.

**Un solo thread, una connessione alla volta: una scelta, non un limite ereditato.** Il decode
serializzato del piano §15 rende inutile un accept concorrente finché non c'è il motore, e i
contatori finta-paralleli sono peggio di un `busy` visibile. Il costo è detto: una `/v1/models` su un
cache grande tiene gli altri in coda.

**Provato su un socket vero**, non solo in memoria: un caso di `test_serve` fa `fork`, chiama il loop
reale sulla loopback e rilegge i byte dall'altra parte, asserendo che il numero di byte del corpo
coincide con `Content-Length` (è il numero, non la frase, che prova l'incorniciatura). E una volta
fuori da `ctest`, il binario: `fastllm serve --cache <fixture> --port 8733` + `curl` →
`{"object":"list","data":[{"id":"org/demo","object":"model","created":…,"owned_by":"local"}]}`,
501 su `POST /v1/chat/completions`, e due richieste sulla STESSA connessione keep-alive contate
`#2` e `#3` nel log. `--once` esiste per questo: serve UNA richiesta e esce, così uno smoke non
lascia un processo appeso.

**Il `bind` rifiutato e il segnale che ferma il loop: erano gli ultimi due cammini dichiarati non
misurati, e ora girano in `ctest`.**
- *Porta occupata* e *porta vietata* sono due esiti **distinti** (`FMLX_SERVE_ADDR_IN_USE`,
  `FMLX_SERVE_NO_PERM`) e non più `BAD_ARG`: «argomento non valido» mandava l'operatore a rileggere i
  flag quando il flag era giusto e la porta era già di un altro processo. L'`errno` si copia **prima**
  della `close`, perché una `close` che fallisce lo sovrascrive e il messaggio accusa il bind
  dell'errore di un'altra chiamata. Che la porta 1 da non-root dia `EACCES` è **misurato** su questa
  macchina, non indovinato; se chi esegue è root il caso lo **dice** («nota: … non è provabile qui»)
  invece di sparire dal conteggio in silenzio.
- *I segnali* si provano nel figlio con `stop == NULL`, che è l'unico cammino in cui
  `install_handlers` gira davvero: una richiesta servita (`/health` → 200), poi `SIGTERM` e poi
  `SIGINT`, e si asserisce che il processo **esce 0** — cioè che il loop ha finito di pulirsi — e che
  **non** è morto di segnale. Il figlio tiene la porta e scrive la log su una pipe: la riga
  «in ascolto» è ciò che il padre aspetta, con un tempo contato.

**Tre controlli negativi, uno per asserzione nuova** (mutazioni costruite su una copia del file fuori
dal tree, mai nel sorgente): declassare i dinieghi a `BAD_ARG` → 4 fallimenti, e nel messaggio il
diniego vero c'è già (`Address already in use`, `Permission denied`); mettere `SIG_DFL` al posto
dell'handler → «il processo è stato UCISO dal segnale 15»; aggiungere `SA_RESTART` → ed è il controllo
che vale di più, perché un `accept` che riparte sembra innocuo («il flag lo vedo al giro dopo») e
invece il giro dopo non arriva mai: `Ctrl-C` non ferma il server. Terzo controllo = terza ragione per
cui l'attesa è limitata e nomina cosa ha atteso.

**Cosa NON è misurato qui**: il ripristino degli handler *precedenti* (`saved_int`/`saved_term`) non
ha modo di essere osservato — sta nel processo che sta per uscire; e il `SIGPIPE` ignorato dipenderebbe
da un client che chiude nel preciso istante in cui il server scrive, cioè da una corsa, non da un
fixture. **`Expect: 100-continue` invece SÌ**, ed è il caso che ha nominato da sé la sua ragione
d'essere: la prima versione mandava un corpo con `messages: []`, e il layer ② rispondeva 400 **prima**
di arrivare al motore — il test passava sul via libera e falliva sull'esito, che è il modo giusto
per fallire. Ora il caso prova tre cose in ordine: i 25 byte del via libera arrivano **prima** che il
corpo sia scritto (l'ordine dei byte È la prova: senza, il client che rispetta il protocollo non
manda niente e lo stallo sembra lentezza), il 501 che segue è lungo esattamente quanto il suo
`Content-Length`, e un corpo da zero byte **non** ottiene un via libera.

## La lane CLI (`src/cli/fmlx_pull.c` + `fmlx_cli.c`) — il primo pezzo eseguibile

`fastllm` è il primo binario di questo repo che si può lanciare da terminale. Fa **una** cosa
(`pull`) e per il resto **dice cosa manca**. Il motivo per cui le due cose stanno nello stesso
commit: un dispatch che esce 0 su un verbo non implementato è il risultato silenzioso che il piano
vieta, e l'unica alternativa onesta è un esito che nomina il pezzo mancante.

**`pull` tiene in mano UN commit per tutti i file.** Chiede il manifesto una volta, prende lo `sha`,
e scarica ogni `rfilename` **a QUEL** commit — non "ultimo" file per file, perché un repo che
carica mentre scarichi ti dà uno snapshot misto, cioè un modello i cui pesi non corrispondono alla
sua configurazione. Poi, e solo dopo che tutto è ok, scrive `refs/<branch>`: la download lane
**non** lo scrive quando la revisione richiesta è già il commit (è il suo contratto), quindi
nessuno lo scriverebbe — e senza quel ref un secondo `pull` rifarebbe la rete. `test_pull` verifica
esattamente questo: la seconda invocazione fa **zero** chiamate di transport.

**Il nome del blob non lo decide `pull`** (è la trappola §12.1, misurata di nuovo sul campo): il
`git_blob_id` del manifesto è sempre a 40 hex, ma un file LFS nel cache si chiama con lo sha256
dall'etag. `pull` prende `ref.blob_path` da chi ha fatto il download e non guarda mai `blobId` per
nominare un file. Nel cache reale di prova si vede la conseguenza: **6 blob a 40 hex e 3 a 64**, e
ciascuno è il proprio hash (`git hash-object` e `shasum -a 256` restituiscono il nome).

**`--dest` è uno specchio, e specchia con hardlink — mai symlink** (piano §10.2). Se `link()` non è
possibile (`EXDEV`/`EPERM`/`EOPNOTSUPP`) si copre in `<dst>.fmlx-part` e si fa `rename`: un file
iniziato e mai rinominato non è mai lo specchio di qualcosa. Un `stat` con stesso inode rende il
mirror idempotente (il secondo `pull` non tocca niente), e il `link=2` misurato con `ls -la` sui tre
file LFS è la prova che non c'è una copia da 2.5 MB per ciascuno.

**L'avanzamento: UNA riga che si riscrive, ma solo dove `\r` vuol dire qualcosa**
(`src/cli/fmlx_pull_progress.c`). Su un terminale il `pull` disegna
`[3/20] model-00002…safetensors  42% [##########----------] 4.1 GB/ 9.8 GB  137 MB/s  eta 42s` e la
riscrive **al massimo una volta al secondo**: a 500 MB/s la callback del corpo gira migliaia di volte,
e scrivere al terminale a ogni giro è più lento del download che si sta mostrando. Su tutto il resto
— pipe, log, `$( )`, `--quiet` — **non esce un solo carattere di controllo**: le righe per-file di
prima restano dov'erano, perché in un file un `\r` non è "torna indietro", è "sovrascrivi ciò che
chi legge sta leggendo".

I numeri sono misurati o non ci sono: velocità ed eta compaiono dal **secondo** campione (con un
campione solo il denominatore è zero, e "0 B/s" è il modo in cui una CLI insegna a non fidarsi delle
sue cifre); senza una lunghezza dichiarata dal server non si disegna nessuna percentuale, si scrivono
i byte; `done` oltre la promessa ferma la barra al 100%, non la fa esplodere; e nella **ripresa** i
byte che c'erano già sono il `base`, non merito di adesso — 900 byte già sul disco con 50 nuovi in un
secondo danno 50 byte/s, non 950. Il `total` che la barra usa è quello **risposto dal server**
(`limit` del GET), non solo quello del manifesto. L'hook arriva al transport con
`FmlxHfFetchOpts.on_bytes`, chiamato da `body_cb` **dopo** la `write()`: l'unico numero onesto è
quello dei byte che ho messo su disco, e una barra basata sui byte *richiesti* racconterebbe un
andamento che nessuno ha misurato.

La prova di questa roba è su **tre piani distinti**, perché sono tre decisioni diverse: il renderer
si prova da solo con l'**orologio in mano** (un `now` passato come parametro — con `time(NULL)`
dentro, «una riga al secondo» si proverebbe dormendo); il `pull` su un **non-terminale vero**
(`open_memstream`) asserisce che non c'è `\r` E che l'hook non è nemmeno offerto al transport; e il
`pull` su un **terminale vero finto** (`openpty`) asserisce che il `\r` arriva, che la riga nomina il
file, e che la riga per-file di prima NON c'è. Il `seg()` del test ha un trucco che vale la pena
dire: la sua prima versione azzerava il contatore a ogni `\r` e quindi restituiva sempre la riga
vuota — sei asserzioni rosse che sembravano sei bug del renderer, ed erano un bug del test.

**Il dispatch:** `fmlx_cli_parse` non sa nulla dei motori e `fmlx_cli_exec` non sa nulla delle
bandiere; il primo produce `FmlxCliArgs`, il secondo mappa gli esiti su exit code (`OK`→0,
`USAGE`→2, `FAILED`→1, `NOT_WIRED`→1). Il token **non è una bandiera**: si passa solo con
`HF_TOKEN`, perché una `--token` finirebbe nello storico del terminale e in ogni script che la
invoca — e per lo stesso motivo `FASTLLM_API_KEY` non ha la sua bandiera. `--help` non è un errore e
non vuole un verbo; un parse fallito azzera `FmlxCliArgs` (un verbo residuo verrebbe eseguito). I
nomi long si ammettono sia staccati sia `--flag=val`.

**Le bandiere sono un INSIEME DI PAROLE, non un valore.** Ogni opzione ha un bit in una tabella
unica, perché la domanda «questa bandiera è stata data?» non si può fare sul valore: `--stall-retries
0` e `--stall-retries` non dato sono lo stesso numero (0). Da lì due regole che senza quel bit non
esistono: **una bandiera che il verbo non legge è un errore** (`fastllm serve --dest x` → «--dest non
è un'opzione di serve», exit 2) — accettarla significherebbe eseguire un comando che sembra dire due
cose e ne fa una, e chi legge l'output non ha modo di sapere quale delle due è saltata; e **una
booleana non prende valore** (`--once=0`, `--quiet=1`, `--offline=` → rifiutati), perché `--once=0`
avrebbe lo stesso effetto di `--once=1`: due semantiche con la stessa grafia. Le condivise —
`--quiet` e `--cache` — non stanno in nessun insieme esclusivo: `--cache` alimenta `pull`, `models` e
`serve` con un solo valore, perché i tre verbi guardano LO STESSO disco e una `--cache` ignorata da
`serve` (col server che legge `HF_HOME`) sarebbe un'opzione che mente.

**La prima regola vale solo fra i verbi che hanno una superficie di opzioni** (`pull`, `models/list`,
`serve`). `fastllm run --port 8000` NON deve sentire «--port non è un'opzione di run»: `run` non ha
ancora opzioni, e dire quali non ha è un'affermazione sul futuro di un verbo che non è scritto. Il
parse passa, e a parlare è il dispatch con l'unica cosa vera oggi: manca il motore (exit 1).

**Misurato sul server vero** (fuori da `ctest`, come le altre tre sonde):
`build/fastllm pull sshleifer/tiny-gpt2 --cache <work> --dest <work>` → esito 0, 9/9 file, 10.9 s,
`refs/main` = `5f91d94bd9cd…`, e `HF_HUB_CACHE=<work> build/hf_probe --tree sshleifer/tiny-gpt2 main`
risolve lo snapshot scritto da `pull` con **`verifica=ok` su tutte le entry** — il cache che
scriviamo è leggibile dal lettore con cui abbiamo verificato il cache di `huggingface_hub`.

**Il terminale vero, per la prima volta** (`script -q log ./build/fastllm pull sshleifer/tiny-gpt2
--cache <work>`, pty allocato, 2026-09-24): **34 caratteri `\r`** su nove file, e la riga che a un
occhio distratto sembra sbagliata è quella che conferma tutto — `[3/9] flax_model.msgpack 3%
[#---…] 16.4 kB/412 kB` è l'UNICO frame «di mezzo» di tutto il download: gli altri otto file danno il
frame iniziale allo 0% e quello finale al 100%, perché finiscono **prima del secondo campione**. Che
non compaia né `MB/s` né `eta` qui non è un buco: è la regola «un campione non è un andamento»
applicata da un server che risponde in meno di un secondo. La prova speculare, stessa macchina un
minuto dopo: lo STESSO `pull` con stdout redirect su file → **zero `\r` in 610 byte**, nove righe
per-file.

**Il telaio duplicato, chiuso il giorno stesso in cui è stato visto**: quando l'ultimo chunk capita in
un **secondo diverso** da quello del campione precedente (`[6/9]`, `[8/9]`), il corpo disegna il 100% e
`end()` lo ridisegnava — due frame **identici e consecutivi**, ~70 byte dove l'occhio legge una riga
sola. Non mentiva, non costava nulla su un terminale, ma era rumore di byte; ora `draw()` confronta il
corpo con quello già a schermo e salta il ridisegno. Il caso è `case_progress_stessa_foto`: tre blocchi
per tre forme diverse (foto duplicata → 2 `\r` e non 3; file già nel cache, zero chunk → 1 `\r`;
quattro numeri diversi → 4 `\r`, perché un dedupe troppo avido è peggio di niente).
Mutanti: `nodedupe` (toglie il confronto) e `prefix` (confronta solo 12 byte) — il primo rende rosse
2 verifiche, il secondo 13.

**La copia dei byte di `--dest`, eseguita per la prima volta da un test.** `link()` rende `EXDEV` solo
su due volumi diversi, e su questa macchina non succede mai: il ramo di fallback non era mai stato
percorso da nessun test. Ora `fmlx_pull_copy_stream` è chiamata diretta (era `static`, ora è nella
header interna della CLI — che è il posto giusto per una funzione che serve a due file della stessa
lane e non a chi usa la libreria): 132 072 byte scritti a mano con la coda storta, e si asserisce che
i byte sono **gli stessi**, che il `.fmlx-part` non resta (né dopo un successo né dopo una copia
interrotta — il `src` che è una directory fa fallire il `read` DENTRO il loop, che è l'unico modo di
arrivare all'`unlink`), e che l'hook di avanzamento è partito con il numero giusto: `t0 ≥ 0`,
`done == 132072`, `total == 132072` anche se il `begin` aveva mentito di proposito. Non si asserisce
sul TESTO disegnato perché qui l'orologio è quello vero. Mutanti: `nohook` (stacca la chiamata),
`chunk` (conta il chunk invece del cumulato), `nostsize` (non legge la `stat`), `keeptmp` (non
cancella il temporaneo) — che rendono rosse, rispettivamente, 3, 1, 1, 1 verifiche.

**Lo specchio ha una riga sua, sul terminale:** `specchio → <dir>` una volta sola prima dei ridisegni
di `[i/n]`, e le righe per-file restano solo dove c'erano prima (pipe e log). `case_progress_pty` ora
passa `--dest` e verifica entrambe le metà: la riga c'è, `"  specchio …"` NO. Mutanti:
`noannounce` (toglie l'annuncio) e `doublelog` (le stampa entrambe): 1 verifica rossa ciascuno.

**Un fatto sulla pty, che ha quasi impedito di scoprirne un altro.** Il buffer del driver di una pty è
dell'ordine del kilobyte: il caso girava a 898 byte di output, e i ~170 byte del mutante `doublelog`
l'hanno **bloccato** — un test che si impalla invece di fallire, cioè il modo peggiore di sbagliare.
Ora il master lo drena un figlio che scrive su file, e il padre chiude lo schiavo prima del
`waitpid`: è quello, non un tempo di attesa, a dire al drenatore che è finita. Con questa forma
`doublelog` è diventato rosso come doveva.

**Cosa NON è misurato qui**: il **trigger** della copia (`link()` → `EXDEV`, cache e `--dest` su due
volumi) resta non eseguito — la funzione sì, il ramo che la sceglie no, e su una macchina con un solo
volume non è provabile; il `time(NULL)` che `pull_on_bytes` passa all'hook è **rilettura d'orologio
non asserita** — il test inietta il proprio `now` (è il motivo per cui il renderer si prova), e
`copy_stream` al massimo prova che *un* orologio è stato preso (`t0 ≥ 0`), non che ne rilegge uno per
campione; la riga di `curl_configure` che copia l'hook **anche quando è NULL** non è provata dai test
del `pull`, perché girano sul transport finto — ciò che è provato è il contratto (l'hook arriva al
transport con i numeri giusti: `mut_hookwire` diventa rosso se non lo si cabla), non il campo azzarato
che protegge dal puntatore vecchio; i 5 verbi non collegati non hanno ancora un motore da
nominare davvero, nominano il pezzo.

## `fastllm models` / `list` — il catalogo locale (`src/hf/fmlx_hf_catalog.c` + `src/cli/fmlx_models.c`)

Secondo verbo vivo, e primo che **non tocca la rete**: legge il cache così com'è. Non prende un
`FmlxHfTransport` nemmeno per finta, perché la lane che legge il disco un transport non ce l'ha.

**L'ingombro è dei blob, non degli symlink.** Uno snapshot è un fascio di link a
`../../blobs/<nome>`: sommare anche quelli raddoppierebbe ogni modello, e chi ha 40 GB liberi si
fida di quel numero. Si contano gli **inode distinti** di `blobs/` (due hardlink allo stesso blob
sono un blob), un solo livello di ricorsione perché i blob sono piatti — e ciò che fosse annidato
NON è un nostro blob. Misurato: la colonna `DIM` della tabella e un
`find models--*/blobs -type f | xargs stat -f "%z %i" | sort -u -k2` indipendente coincidono a
**3.675.292.998 byte** — 76 inode distinti su 27 repo, 18 dei quali `[solo ref]` (è per questo che il
totale in byte non si muove quando il numero di repo sale: le directory nuove non hanno blob, e
`solo ref` esiste proprio per dire che non è la stessa cosa).

**Tre cose che solo il cache vero di questa macchina potevano dire**, e che sono diventate campi
della struttura (non note a piè di pagina):

1. `models--mlx-community--Mixtral-8x22B-4bit` e altri 12 repo hanno **solo `refs/`**: nessun blob.
   Sono `refs_only`, stampati con `[solo ref]` e una legenda — "occupa 0 byte" e "non ce l'hai" sono
   due consigli diversi, e la riga deve dire quale dei due;
2. `models--bert-base-uncased` ha una `trees/` (huggingface_hub ≥ 1.31, `_tree_cache.py:19-31`) e
   directory **reali** dentro lo snapshot (`coreml/`): `trees/` non è un blob, e il ricorsivo dove
   serve (i ref `refs/pr/N`, che HF scrive come sottodirectory) non è paranoia;
3. la radice contiene `version.txt`, `CACHEDIR.TAG` e a volte `xet/`: **non sono repo**, e "non è un
   repo" si **conta** (`n_ignored_out`) e si stampa, perché un elenco che tace su ciò che salta è il
   modo in cui "non ho trovato niente" diventa "non c'è niente".

`fmlx_hf_catalog` distingue anche tre errori che sembrano uno: la radice non esiste
(`ERR_NO_CACHE` — è `HF_HOME` che punta nel posto sbagliato, non un cache vuoto), il cache esiste ed
è vuoto (`OK`, `n = 0`), l'array del chiamante è troppo piccolo (`ERR_TOO_MANY` **con il numero
vero**). La ricostruzione `directory → repo id` è la copia della regola di `huggingface_hub`
(`utils/_cache_manager.py:742-744`: tipo prima del primo `--`, poi ogni `--` → `/`), che è **perdente
per costruzione**: `ambiguous_name` è la parola che lo dice invece del silenzio.

**`[!]` — un repo che non si è potuto aprire è una riga, non un'assenza.** Un `chmod 000`, o un
`sudo huggingface-cli` che ha creato `models--org--nome` come `root:700`, una volta faceva questo: la
directory esisteva, l'`opendir` diceva `EACCES`, e il catalogo **perdeva l'intera lista** perché il
ramo di errore non distingueva «non c'è» da «non apre» — un diritto murato cancellava il lavoro fatto
sugli altri ventidue. Oggi la riga resta, è nominata, e dice tre cose: `[!]` in testa, le colonne
numeriche a **trattini** (`—` = «non so», non «zero»), e in coda il **colpevole** — `non letto dir:
Permission denied (models--org--murato)`. I campi sono `unreadable`, `unread_part` (`"dir"`, `"refs"`,
`"blobs"`: la PRIMA directory che ha rifiutato, non l'ultima dedotta) e `unread_errno`.

Delle due metà del repo si sa cose diverse, ed è il motivo per cui `unreadable` è un campo e non
un `return` anticipato: con `refs/` murato e `blobs/` aperto **i byte si sanno** (li abbiamo contati) e
manca solo il numero delle revisioni; con `blobs/` murato si sanno le revisioni e non gli byte. La
somma in testa all'elenco e il riepilogo (`N repo NON LETTI (la somma è solo ciò che ho potuto aprire)`)
dicono fin dove arriva il numero. `refs_only` non può accendersi su un repo non aperto — ma non per un
`&& !unreadable` in più, che si è rivelato **codice morto** (un mutante che lo toglie non muoveva
nemmeno un'asserzione, ed è stato tolto): lo impedisce la catena di `return` prima del calcolo.

`fmlx_hf_catalog_repo` (la lettura del singolo repo, quella che usa il `doctor`) distingue i due codici
che prima erano uno: `ENOENT` → `ERR_NO_REPO` e la frase «non è nel cache», qualunque altro errno →
`ERR_IO` e «non si è potuto aprire». Restituire `NO_REPO` su un `EACCES` — com'era — voleva dire
chiamare «non scaricato» un modello che l'utente HA scaricato, e un `pull --repair` che legge quel
codice avrebbe ricominciato a scrivere byte perfetti su una directory solo murata. Sulla radice
l'errore resta **fatale** (`ERR_NO_CACHE`, non c'è nessuna lista da salvare) ma la frase segue
l'errno: `non esiste` / `non è una directory` / `non si è potuto aprire`, perché la frase decide quale
controllo fa perdere tempo a chi legge — il path o i permessi. **Provato a mutare**: quindici tentativi
su quattro lane (8 su `fmlx_hf_catalog.c`, 2 su `fmlx_models.c`, 3 su `fmlx_serve_respond.c`, 2 su
`fmlx_doctor.c`) e **uno solo è rimasto verde** — il risultato più utile del giro, perché ha detto che
quel `&& !out->unreadable` non lo guardava nessuno. Gli altri quattordici hanno spento da 1 a 9
asserzioni a testa, con `ctest` 16/16 verde prima e dopo.

**Due difetti trovati solo girando sul campo**, non sul fixture: la prima `parse_dirname` produceva
`org/-due` al posto di `org/due` (dopo aver scritto `/` per la coppia copiava anche il secondo `-`)
e il messaggio di rifiuto dell'argomento posizionale stampava `fastllm: :` con il verbo **vuoto**
(`fail()` azzera `FmlxCliArgs` prima che il `%s` legga `out->verb`). Il secondo è passato inosservato
al test perché l'asserzione guardava solo il frammento finale: ora si asserisce anche il nome del
verbo, per esteso.

**`models <filtro>`: tre forme, e lo zero che parla.** `org/nome` si confronta con l'id intero,
`nome` secco con la parte dopo l'ultimo `/` (e se due org hanno lo stesso modello la tabella le stampa
tutte e due — non si sceglie noi quale "era quello che volevi"), `org/` + asterisco finale è il
prefisso. L'asterisco altrove è `USAGE` (exit 2) e lo si decide **prima** di aprire il cache: un
argomento illegale non è "gira lo stesso e vedi cosa esce". Zero match **con** filtro è `FAILED`
(exit 1) con stdout **vuoto** e stderr che nomina il filtro — la convenzione di `grep`, e il motivo è
che uno script fa `fastllm models x >/dev/null && ...`: se "non c'è" esce 0, lo script mente. La riga
di riepilogo conta i **match** e dice anche il totale del cache, perché "3 su 23" e "3" sono due
informazioni diverse.

Il `*` in mezzo a una stringa non è un glob: non c'è espansione della shell dentro `argv`, e
`fastllm models '*mm*'` significa ciò che significa un `*` che non sta in coda — cioè un errore, non
una scorciatoia. Quattro controlli negativi costruiti sul sorgente copiato fuori dal tree: allargare
il match del nudo (`strstr` al posto del confronto della coda) → 2 rossi; accettare l'asterisco in
mezzo → 5 rossi (è il controllo che vale di più: quasi nessuno lo scrive, tutti però ci cascano);
restituire `OK` su zero match → 2 rossi, uno per ramo (`--quiet` ha un `return` suo, prima della
tabella, e la prima versione del test ne colpiva solo uno); sommare i byte del cache intero invece che
dei match → 1 rosso. **Quest'ultimo è morto solo sull'asserzione `zz/` + asterisco** (7 byte contro
1.5 kB): con `org/nome` il totale filtrato e quello del cache si *renderizzano* uguali — 1503 su 1510
diventano entrambi "1.5 kB" — e il test che lo asseriva sarebbe stato verde per motivi falsi. C'è una
riga nel test che lo dice, perché il punto non è che la formattazione nasconda un bug: è che qualcuno
lo riscopra tra sei mesi e non lo chiami "coperto".

## `fastllm doctor` — gli byte SONO quelli che promettono (`src/hf/fmlx_hf_audit.c` + `src/cli/fmlx_doctor.c`)

Terzo verbo vivo, e il complemento di `models`: quello legge i **nomi**, qui si hashano i **byte**.
`models` può dire «ho 9 file, 4.2 GB» anche se un blob è troncato a metà da un `kill -9` o se uno
symlink punta il nome sbagliato — perché guarda i nomi. Il punto del file è che la verifica è la
**stessa funzione** che usa il `pull` (`fmlx_hf_locate` + `fmlx_hf_verify`, nessuna copia): un
verdetto scritto a parte sarebbe verde qui e rosso al download, e a quel punto nessuno dei due vale
più niente.

**Due ordini, perché «fastllm doctor && echo ok» resti leggibile.** Un `PROBLEMA` (un hash che non
torna, un blob che non c'è, un symlink che punta fuori, un ref che non si risolve) muove l'uscita a
1; una **nota** no: un `.incomplete` lasciato da un download interrotto, un ref che punta un commit
mai scaricato, una directory che non è un commit, i marker `.no_exist` sono **fatti**, non guasti —
far fallire lo script per quelli insegnerebbe a ignorare il codice d'uscita. `--quiet` stampa SOLO i
PROBLEMA: niente intestazioni, niente riepilogo, niente note, e il silenzio continua a voler dire una
cosa sola. Zero match con un filtro è `FAILED` **anche in `--quiet`**, dove dello schermo non arriva
niente e il codice d'uscita è la parte che un script legge: «ho controllato tutto e va bene» detto
senza aver controllato niente è la bugia peggiore che un verificatore può dire.

**Il tetto dei verdetti (32) è della lane, e qui si nomina.** `dump_findings` stampa le righe che ha e
aggiunge `(9 verdetti in più non elencati: 40 problemi, 1 note)` — quaranta problemi in trentadue righe
senza quella riga è il modo in cui un report sembra completo. Contatore e stampa sono due cose separate
di proposito: i numeri totali non dipendono mai dallo spazio del array.

**Il danno non si tocca.** Questo verbo NON cancella, NON ripara, NON tocca mtime: la cura di un hash
che non torna è `pull --repair`, che ha già il permesso esplicito di sostituire un blob. Due verbi che
scrivono dove l'altro legge sarebbe un `rm` senza invocarlo. E «non ho potuto guardare» non è mai
«a posto»: l'audit impossibile esce **fallito** con la sua riga — `org/murato  AUDIT IMPOSSIBILE` sullo
stdout (è il nome di SU COSA guardare i permessi) e il `PROBLEMA` con la ragione sul canale d'errore,
dove stava. `--quiet` toglie l'elenco, **non** il problema: è il contratto che ha costretto a scrivere
il caso, perché la prima versione del `doctor` zittiva tutto e usciva 0 su un cache che non aveva
potuto aprire. Un repo murato non cancella gli altri: gli altri ventidue restano auditati e il
riepilogo li conta lo stesso (`2 ok` accanto a `1 problemi`).

**Ciò che ha detto il cache vero** (27 repo, 71 entry, 71 verificati, **0 problemi**, 19 note,
1 entry che non è un repo, in 1,3 s): tutte le note sono lo stesso fatto — `refs/main` che punta un
commit di cui non esiste lo snapshot, o repo con `0 snapshot`. È così che si distingue «mancherà» da
«perso» — e le entry stanno ferme a 71 fra due misure con quattro repo in più: una directory senza
blob non ha niente da hashare, ed è esattamente il caso che `[solo ref]` (18 righe su 27) esiste per
dire. Il verificatore ha anche un caso suo: un nome che *promette* un hash (64 o 40 esadecimali)
che questa lane non sa verificare NON è un problema — è contato, poi **decontato** dai verificabili con
una nota, perché dire «1 verificabile su 1» di un file che non si poteva verificare sarebbe un verde
rubato.

**Provato a mutare**: 9 mutanti, e il più caro è `verifica_saltata` — togliere la chiamata a
`fmlx_hf_verify` e contare il blob come buono. Quattro asserzioni diventano rosse, distribuite su due
suite: l'hashing non è decorazione, e se salta il pezzo che costa il test se ne accorge. Gli altri tre
che contano sono `colonna_orfani_byte`, `colonna_regolari_lock` e `colonna_no_exist`: ciascuno sposta
UNA colonna della riga di riepilogo sul campo sbagliato (`bytes_hashed` in posto a `bytes_orphan`,
`n_links` in posto a `n_plain`, `n_noexist` in posto a `n_lock`) e ciascuno spegne esattamente la sua
asserzione. Sono i numeri che la lane non può provare: lì si asseriscono i contatori della struttura,
qui il fatto che la riga li STAMPA — e un `%zu` che legge il campo accanto passa inosservato per
mesi, perché il valore atteso è quasi sempre uguale.

## Build e test

```sh
cmake -S . -B build
cmake --build build          # -Wall -Wextra -Werror
ctest --test-dir build --output-on-failure   # 17 suite, 2953 verifiche, nessuna tocca la rete (2,87–3,01 s misurati quattro volte di fila; 4,3–4,7 nella finestra contesa del 24/09)
./build/fmlx_gguf_inspect <file.gguf>   # testata, tipi, allineamento, coda di byte
./build/fastllm pull <org>/<repo> [--revision R] [--dest D] [--cache C]   # lo snapshot nel cache
                                         # su un terminale: UNA riga che si riscrive; su pipe o log:
                                         # una riga per file; `--quiet`: niente
./build/fastllm models [<filtro>] [--cache C] [--quiet]   # cosa c'è già nel cache, senza rete
                                         # (`list` = alias; `org/nome`, `nome`, `org/`+asterisco;
                                         #  nessun match = uscita 1)
./build/fastllm doctor [<filtro>] [--cache C] [--quiet]   # hashà i byte del cache: 0 = nessun
                                         # problema, 1 = PROBLEMA (o audit impossibile, o zero match),
                                         # 2 = argv; NON cancella e NON ripara: la cura è `pull --repair`
./build/fastllm serve [--host I] [--port N] [--cache C] [--max-body N] [--once]
                                         # /v1/models + /health VERI, 501 su tutto il resto;
                                         # l'auth è SOLO FASTLLM_API_KEY (vuota = nessuna)
./build/hf_probe --tree <org>/<repo> [<rev>]   # cache HF reale: locate + verifica, sola lettura
HF_HUB_CACHE=work/probe-cache ./build/hf_fetch_probe <org>/<repo> [<rev>] [<file>] [<max byte>]
                                         # download VERO, in una cache di lavoro: mai la reale
./build/hf_manifest_probe <org>/<repo> [<rev>]
                                         # UNA richiesta di metadati: lista i file, zero byte di modello
```

**Verde che prova qualcosa.** Un'asserzione che non è mai stata vista rossa può essere verde perché
il codice è giusto, o perché non guarda niente: da fuori sono lo stesso colore. `tools/mut_prove.c` (`build-release/mut_prove`)
è il pezzo che chiude il conto — copia il sorgente toccato in `build/probe-<tag>/`, applica UNA
sostituzione letterale (se combacia zero o due volte si ferma: un mutante ambiguo non è una prova),
ricompila e linka il `.o` mutato PRIMA dell'archivio, esegue e dice **quali** verifiche sono diventate
rosse. Un mutante che non muove nulla è riportato come `VERDE-MUTO` e fa uscire 1; uno che si blocca
(pty piena, `wait`, rete) è riportato come `BLOCCATO` dopo `--timeout` secondi invece di appendere la
build. I nomi dei mutanti citati in questo file (`mut_hookwire`, `nodedupe`, `doublelog`, …) stanno in
`build/probe-*/spec.json` e si rieseguono con `--list`. **Le batterie di questa sessione sono invece
versionate in `tests/mutants/*.json`** (le quattro `unreadable-*`), e si rilanciano da lì così — il
bersaglio va detto, perché lo spec elenca i mutanti ma non il binario da ricompilare:

```sh
build-release/mut_prove --build build-release --lib fmlx_cli --target test_hf_catalog --spec tests/mutants/unreadable-catalog.json --tag unreadable-catalog
```

un `rm -rf build` non deve cancellare la prova che un'asserzione è rossa davvero.

Tre sonde toccano il mondo reale, e stanno **fuori da `ctest` di proposito**: la suite deve girare
su una macchina senza cache e senza rete. `hf_probe` legge il cache HF di questa macchina (sola
lettura), `hf_fetch_probe` scarica, `hf_manifest_probe` chiede al server la lista dei file. La loro
ragione di esistere è che un parser testato solo su fixture sintetiche può essere coerente con se
stesso e sbagliato col mondo — è esattamente ciò che è successo alla tabella dei tipi (vedi sopra),
e il cache reale di questa macchina ha due radici diverse (`HF_HOME` e `~/.cache`) che il probe
distingue da solo.

La suite (`tests/gguf/test_gguf.c`) **non scarica nulla**: i file validi si scrivono col writer
di fixture del progetto stesso, i file malati si costruiscono byte per byte. Copre i casi (a)–(g)
del piano §8 più undici corruzioni distinte, ciascuna con il proprio codice atteso.
`tests/gguf/test_quant.c` prova invece i valori noti (IEEE half, blocchi costruiti a mano) e — la
parte che conta — che **un rifiuto non lasci valori in `dst`**.

Il **gate differenziale** non è un target CMake di proposito: confronta il dequant con quattro
implementazioni che non vivono in questo repo — due già in esercizio su GMBasic (A, B),
`ggml`/llama.cpp (C), che è anche l'autorità sui NUMERI di enum perché i file GGUF li scrive, e
**gguflib (D)** — aggiunta perché è il SECONDO MOTORE NUMERICO che MLX userà davvero
(`mlx/io/gguf.cpp:228-237` smista a `gguf_load_quantized` solo Q4_0/Q4_1/Q8_0; ogni altro
quantizzato passa a `extract_tensor_data` → `gguf_tensor_to_f16` di gguflib). Ha bisogno di quei
sorgenti e della libreria già costruita su questa macchina. Si lancia così (7 GGUF reali di questa
macchina + blocchi pseudo-casuali a seed fisso):

```sh
G=~/Documents/Projects/GMBasic
L=~/llamacpp
U="$G/native/gmb_host_ai_mlx/build/_deps/gguflib-src"
cc -Wall -Wextra -Werror -std=gnu17 -I include -I src/lane_gguf -I "$G/native/gmb_host_ai_mlx" \
   -I "$G/native/gmb_moe_runtime/include" -I "$G/native/gmb_moe_runtime/src" -I "$U" \
   -I "$L/ggml/src" -I "$L/ggml/include" \
   tools/quant_diff.c tools/quant_diff_ggml.c tools/quant_diff_gguflib.c \
   "$U/gguflib.c" "$U/fp16.c" src/lane_gguf/fmlx_gguf.c \
   src/lane_gguf/fmlx_quant.c src/lane_gguf/fmlx_quant_ext.c \
   src/lane_gguf/fmlx_quant_iq.c \
   "$G/native/gmb_moe_runtime/src/gmb_moe_ggml.c" \
   -L "$L/build/bin" -lggml-base -Wl,-rpath,"$L/build/bin" -o build/quant_diff
./build/quant_diff ~/ai/Models/gguf/*.gguf ~/ds4/ds4flash.gguf
./build/quant_diff --synthetic   # blocchi pseudo-casuali + sweep griglie + ESAUSTIVO f16->f32
```

`fp16.c` vicino a `gguflib.c` non è opzionale: `from_half`/`to_half` vivono lì e il linker lo
chiede per nome. `tools/quant_diff_gguflib.c` è il nostro modulo (non codice di terze parti):
contiene i `_Static_assert` che bloccano i numeri di enum di gguflib contro quelli di ggml — senza
di quelli il gate interpellerebbe la LORO tavola dei blocchi con un indice LORO, e un'accusa
costruita su indici non verificati è peggio del bug che vorrebbe aprire.

Ogni invocazione esegue **tutto**: tensori dei file passati (se dati), blocco sintetico per ogni
tipo coperto, `grid_sweep` (ogni indice di ogni codebook), `f16_exhaustive` (65536 pattern) e
`coverage_audit`. Esito a **quattro** valori, volutamente, perché "rosso" non dice chi deve lavorare:

| codice | significato | di chi è il lavoro |
|---|---|---|
| `0` | nessun bit divergente su nessuno degli assi | nessuno |
| `1` | **i ns.** float divergono da un testimone | questo repo |
| `2` | almeno un file non si è aperto | **non è un verdetto sui bit**, mai letta come verde |
| `3` | noi esatti, ma **due codici altrove divergono tra loro** | il repo dove vivono |

Il `3` esiste per una ragione precisa: `tipo 1` (F16) è bit-esatto con ggml e DIVERSO da A e B per
il solo payload degli NaN segnalati, e su quella riga il gate era **sempre rosso da tre sessioni** —
un rosso permanente non è una prova, è un allarme che addestra a ignorarlo. La divergenza non è
assorbita e non è zittita: è una voce aperta nel vault (`Bugs/(open) -
GGUF-F16-Nan-Segnalandi-Non-Quietato.md`), e qui si distingue solo di chi è la colpa. Esito di oggi
sui 7 GGUF locali: `nessun ns. bit divergente su 250 confronti, 23 disaccordi TRA CODICI CHE VIVONO
ALTROVE`.

**E il gate stesso aveva un difetto, trovato misurando D**: il ciclo che percorre gli argomenti
inizializzava `i = 2`, cioè **saltava il primo file**. Con un solo `.gguf` non veniva confrontato
*nessun tensore reale* — si vedeva solo il blocco sintetico, e il verdetto aveva esattamente la
faccia di un verdetto che ha provato tutto. È la ragione per cui il conteggio dei confronti è nella
riga di esito (250, non un «tutti»): un numero che non cresce con gli argomenti è un numero che
dice qualcosa.

**Cosa ha trovato D, e perché non è nostro** (tutto misurato, `docs/mlx-quant-bridge.md` §5.2):
(1) il dequant **Q4_K** di gguflib calcola le 8 scale e gli 8 minimi dei sotto-blocchi e poi usa
sempre `scales[b]` anche per la seconda metà del gruppo da 64 — `scales[b+1]` non è mai letto:
metà dei pesi di ogni super-blocco prende la scala del vicino. Sui tensori REALI la divergenza
comincia dove il codice dice «qui serviva la scala dispari»: indice **32** su
`token_embd.weight` di qwen2.5-omni Q4_K_M (max|diff| 0.0521) e su `blk.37.ffn_gate_exps.weight` di
`~/ds4/ds4flash.gguf` (0.0716), indice 2080 — che è `32 mod 64` — su `token_embd.weight` di olmoe
(0.0355); applicare le **due righe** di correzione a una COPIA rende il tipo 12 bit-exact contro A,
B, C e D e fa calare i disaccordi da 11 a 7 nel `--synthetic`. (2) La **tavola** delle dimensioni di blocco
ha tre voci fuori dal mondo — `q2_k` 82 (dove ggml, la tavola di DS4 e la ns. dicono 84), `iq1_s`
110 (50), `iq4_nl` 50 (144) — e non è un dettaglio del dequant: `gguflib.c:309-310` calcola
`bsize` di OGNI tensore da lì, quindi il data section si sposta per tutto ciò che segue. gguflib
si contraddice da sola: il suo `gguf_q2_k_to_float` avanza di `16+64+4` = 84. Il gate le intercetta
perché interpella la tavola **su ogni tipo**, non sui soli sei con un dequant accanto. Nessuna delle
due è nostra: ggml dice come noi su entrambi i punti, quindi sono contate come ESTERNE (`exit 3`,
mai un rosso nostro) e i bug sono aperti nel vault GMBasic, dove quel codice è vendorizzato.

Lo stesso eseguibile fa l'**audit** su tre assi (`34 tipi confrontati sui bit, 42 forme confrontate
con ggml, 0 incoerenze, 0 gap colmabili, 3 byte in disaccordo tra testimoni`):
* **bit** — `is_covered` non può mai asserire una copertura che nessun secondo codice conferma;
  il caso inverso (un testimone copre e noi no) NON è un fallimento ma una **lista di gap
  colmabili**, cioè il lavoro che si può fare restando dentro la regola — ed è stata la lista dei
  tre lotti, ora vuota;
* **forma** — elementi per blocco e byte per riga della ns. tabella contro `ggml_row_size` per
  **tutti e 42** i valori di enum. È l'asse che vale più dei bit per i tipi che nessun file
  locale contiene: senza di quello, cinque tipi spostati di uno restavano invisibili;
* **byte di un lettore** — la stessa domanda fatta a gguflib, voce per voce, con attribuzione
  segue-l'autorità: se ggml coincide con noi l'outlier è LUI e il numero finisce negli esterni.
  Un'incoerenza sempre ascritta a noi, a forza di vederla, diventa un verde bugiardo al contrario.

## Regole che questo repo eredita

* **Niente stub**: una funzione che non esiste non viene dichiarata; una lane che non c'è non ha
  target CMake. Un caso non implementabile deve **fallire con un errore**, mai ritornare 0.
* **File ≤ ~1200 righe, uno per dominio** (il lettore GGUF è già oltre la soglia di attenzione:
  si spezza per area — tabella tipi / parse KV / parse tensor-info — appena cresce).
* **Nessun test massivo** senza richiesta esplicita: si esegue la suite del componente toccato.
* **Niente `|` per combinare effetti** — e niente `int` che diventa `FmlxApiErr` senza rileggere i
  punti di chiamata. `A | B` non garantisce che `A` sia eseguito prima di `B`: dove l'ordine degli
  effetti È il contratto (chiave prima del valore, `arr_begin` prima del suo `arr_end`) si usa
  `||`/`&&`/`?:`/`,` o due statement. E `== 0` cambia significato con il tipo: 1=ok e 0=ok sono
  compatibili col compilatore e incompatibili col mondo.
* **Nessun nome di persona o azienda nei nomi dei file.**
* Licenze e attribuzioni: vedi `LICENSES.md`. **Un'obbligazione esiste già oggi**: le codebook ggml
  (IQ2, le cinque griglie IQ del lotto C, `kvalues_iq4nl`) sono **dati** MIT copiati con
  il generatore `gen_codebooks.py` (poi rimosso), attribuiti nei file generati e in `LICENSES.md`. Le altre (oMLX
  Apache-2.0, mlx/mlx-c, llama.cpp linkato nel prodotto) scattano con la lane MLX.
