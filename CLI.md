# fastllm — guida della CLI

`fastllm` è il binario C di FastLLM: **hub** Hugging Face (download e cache), **server**
OpenAI/Anthropic, **CLI** in stile Ollama per scegliere, ispezionare ed eseguire i modelli.
Il motore non è in questo repo: è [LLMEnv](https://github.com/c-vision/LLMEnv) (`LLMEngine`,
`LLMCodec`, `LLMToken`, `LLMJinja`), consumato a sorgente.

> **Regola d'oro.** I parametri del motore **non sono variabili d'ambiente**: si passano dal
> **registro opzioni** con `--opt <nome>=<valore>` (ripetibile, prima del load). L'elenco sempre
> aggiornato è `fastllm options`. Le uniche variabili lette sono quelle di **servizio** (§9).

---

## 1. Build e installazione

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target fastllm -j8
export PATH="$PWD/build-release:$PATH"
fastllm help
```

Prerequisiti: macOS su Apple Silicon (Metal), un compilatore C11 e — per la lane MLX — il toolchain
che LLMEnv richiede. Il binario esce in `build-release/fastllm` e carica le dylib `libllm_ai_mlx`,
`libllm_ai_llamacpp`, `libllmcodec_ggml`, `libllmtoken` dalla stessa cartella (`@rpath`).

---

## 2. Come `fastllm` trova un modello

Per `<nome|#n>` (usato da `run`, `chat`, `test`, `show`, `rm`) l'ordine è:

1. **path esplicito** (`./modello.gguf`, `~/ai/Modelli/…`);
2. **registro locale** — alias e indice `#n` di `fastllm list` (`$FASTLLM_HOME/models.tsv`);
3. **cache Hugging Face** — repo `models--org--nome` già scaricati;
4. **directory di ricerca** — `$FASTLLM_HOME/paths`, una per riga (vedi `fastllm paths`).

Nessun default implicito legato alla macchina: le directory di ricerca le decidi tu.

| file | cosa contiene | lo gestisce |
|---|---|---|
| `$FASTLLM_HOME/paths` | directory di ricerca dei modelli | `fastllm paths` |
| `$FASTLLM_HOME/models.tsv` | registro locale alias → path | `fastllm add/rm/show` |
| cache HF (`HF_HUB_CACHE`/`HF_HOME`) | repo scaricati | `fastllm pull/models/doctor/serve` |

`$FASTLLM_HOME` vale `~/.fastllm` se non impostata.

---

## 3. Gestione dei modelli

### `pull <repo>` — scaricare un repo (o alcuni file)

```sh
fastllm pull mlx-community/Qwen3-8B-4bit
fastllm pull antirez/deepseek-v4-gguf --file '*IQ2XXS*imatrix-0731.gguf'
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q2.gguf'
fastllm pull org/modello --revision main --cache ~/.cache/huggingface \
    --dest ~/ai/Models --max-bytes 5000000000 --stall-timeout 60 --stall-retries 3 --repair
```

| bandiera | effetto |
|---|---|
| `--revision <ref>` | ramo o commit (default `main`) |
| `--cache <dir>` | radice del cache (default: quella di Hugging Face) |
| `--endpoint <url>` | server diverso da `huggingface.co` |
| `--dest <dir>` | specchia in `<dir>/<nome-repo>` con **hardlink** (mai symlink) |
| `--file <glob>` | scarica **solo** i file che corrispondono: `*` = qualsiasi sequenza (anche `/`), `?` = un carattere. **Ripetibile**, max 16. Senza `--file` si scarica **tutto** il repo |
| `--max-bytes <n>` | tetto di byte per **un** file |
| `--stall-timeout <s>` | secondi senza byte prima di riprovare (default 60) |
| `--stall-retries <n>` | riprese con `Range` dopo uno stall (default 3) |
| `--offline` | mai rete: se manca un file, errore |
| `--repair` | permesso di sostituire un blob che non verifica |
| `--quiet` | niente righe di progresso |

Note che contano:

- il commit è **uno solo** (quello del manifesto): un pull non produce mai uno snapshot misto;
- il download è **riprendibile**: i byte parziali restano in `<etag>.incomplete` e ripartono da lì;
- un `--file` che non seleziona **nulla** è un **errore** (`nessun file corrisponde…`), mai un
  successo con zero file: il messaggio dice anche quanti file ha il repo;
- `--dest` è un hardlink quando cache e destinazione sono sullo stesso volume, una copia atomica
  altrimenti. Il modello è specchiato **solo** dopo che il cache è completo.

I comandi per-modello (ds4, GLM, Qwen, MLX, OCR, ASR) sono in [MODELS.md](MODELS.md).

### `models [<filtro>]` — cosa c'è già nel cache (senza rete)

```sh
fastllm models
fastllm models qwen
fastllm models mlx-community/* --json
fastllm models --quiet            # solo gli id, uno per riga
```

Il filtro ha **tre forme**: `org/nome`, `nome` (ultima parte dell'id), `org/*`. Zero match = uscita
1 con stdout vuoto (convenzione di `grep`).

### `list` (`ls`) — i modelli disponibili, numerati

Unisce **registro locale** e **cache HF**; il numero `#n` è quello usato da `run`/`chat`/`rm`/`show`.

```sh
fastllm list
fastllm list --cache ~/.cache/huggingface --json
```

### `add <nome> <path>` / `rm` / `show` — registro locale

```sh
fastllm add llama1b ~/ai/Models/Llama-3.2-1B-Instruct-4bit
fastllm add ds4 ~/ds4/ds4flash.gguf --modality chat
fastllm add doodle-ocr ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF.gguf \
    --mmproj ~/ai/Models/gguf/PaddleOCR-VL-1.6-GGUF-mmproj.gguf --modality ocr

fastllm show ds4          # dettagli della voce
fastllm rm ds4            # toglie la voce, NON i file
fastllm rm 2
fastllm rm doodle-ocr --purge   # cancella anche i file
```

`--modality chat|vlm|ocr|audio` decide come `run` interpreta il secondo argomento (testo, immagine,
file audio). Per `vlm`/`ocr` serve `--mmproj`.

---

## 4. Trovare modelli su disco

### `paths` — dove cercare

```sh
fastllm paths                      # elenco NUMERATO (il progressivo serve a rm)
fastllm paths add ~/ai/Models
fastllm paths add /Volumes/disco/ai/Models
fastllm paths rm 2
fastllm paths scan --json          # inventario: dove ESISTONO modelli
```

### `search` — trovare i modelli

```sh
fastllm search                          # tutti i dischi accessibili
fastllm search ~/ai --max-depth 4
fastllm search ~/ai --roots --max-depth 4   # albero delle CARTELLE RADICE
fastllm search /Volumes/disco --type gguf --json
fastllm search ~/ai --walk              # forza il walk (niente Spotlight)
```

Default: lista **numerata e alfabetica** dei modelli, nome su una riga e percorso sotto, senza
accorciature; i duplicati si mostrano **tutti**. Un «modello» è: una dir con `config.json` + pesi
(`*.safetensors`/`.bin`/`.npz`), un file `*.gguf` (≥ 1 MB, non `ggml-vocab-*`; N gguf in una
cartella = N modelli) oppure un repo-cache HF.

### `test` — il modello è completo e non corrotto?

```sh
fastllm test ds4
fastllm test 2
fastllm test /Volumes/disco/ai/models/Qwen3.8-27B-DFlash2
```

Verifica **senza caricare il modello**: header GGUF + tensori che ci stanno nel file; per una dir
MLX/HF `config.json`, pesi presenti, header safetensors; per un repo-cache blob e snapshot (nessun
symlink rotto); in tutti i casi i marker di download incompleto
(`*.part`/`*.incomplete`/`*.download`/`*.tmp`). **Uscita 0 = completo, 1 = problema col motivo.**

---

## 5. Generazione

### `run <nome|#n> [prompt]`

```sh
fastllm run llama1b "Ciao, presentati in una frase."
fastllm run 0 --cache ~/.cache/huggingface --system "Sei conciso." \
    --max-tokens 256 --think --offload-experts
fastllm run ds4 "Spiega il MoE in due righe." --context 32k
```

Il testo esce **token-live** su stdout. Senza `[prompt]` legge una riga per volta da stdin (una
generazione per riga).

### `chat <nome|#n>`

```sh
fastllm chat llama1b
fastllm chat 0 --system "Sei un assistente." --max-tokens 512 --think
```

Comandi REPL: `/exit` `/quit` `/clear` `/new` `/system` `/thinking` `/prompt` `/compact` `/help`.

| bandiera (run/chat) | effetto |
|---|---|
| `--system <testo>` | messaggio di sistema (per `ocr`/`vlm`/`audio`: l'istruzione al modello) |
| `--max-tokens <n>` | tetto sui token generati |
| `--context <n>` | contesto in token (`32k`, `1m`, numero); default: quello del modello |
| `--think` | abilita e mostra il thinking (default: **disattivato**) |
| `--offload-experts` | streama gli esperti MoE da disco (modelli più grandi della RAM) |
| `--cache <dir>` | radice da cui risolvere il modello |

Con `--modality vlm|ocr|audio` il secondo argomento è un **file** (immagine/audio) e `--system` è
l'istruzione (`"OCR:"`, `"Table Recognition:"`).

> Se il modello supera metà della memoria libera+reclamabile e manca `--offload-experts`,
> `run`/`chat` chiedono `[Y/n]` sul terminale. La domanda si disabilita con
> `FASTLLM_NO_OFFLOAD_PROMPT=1` (l'opzione resta `--offload-experts`).

---

## 6. Server

### `serve` — OpenAI + Anthropic

```sh
fastllm serve
fastllm serve --host 0.0.0.0 --port 8000 --cache ~/.cache/huggingface \
    --max-body 64000000 --offload-experts --quiet
fastllm serve --once          # serve UNA richiesta e esce (smoke test)
```

Rotte: `/v1/models`, `/v1/chat/completions`, `/v1/completions`, `/v1/responses`,
`/v1/messages` (Anthropic), `/v1/embeddings`, `/health`, più le rotte di gestione
`/v1/models/load`, `/v1/models/status`, `/v1/models/unload`. Ogni rotta che non può essere
servita risponde con il **suo** codice e la **sua** ragione (mai un 200 dal corpo vuoto).

| bandiera | effetto |
|---|---|
| `--host <if>` | interfaccia di ascolto (default `127.0.0.1`) |
| `--port <n>` | porta (default 8000) |
| `--max-body <n>` | tetto di byte di un corpo di richiesta |
| `--parallel <n>` | pool I/O + worker (default 1) |
| `--batch` | lotto continuo: N richieste decodificano insieme (implica `--parallel`) |
| `--vision-model` / `--vision-mmproj` | VLM per input immagini (servono entrambi) |
| `--once` | una richiesta e esce |

### `ps` — c'è un server in ascolto?

```sh
fastllm ps
fastllm ps --host 127.0.0.1 --port 8000
```

---

## 7. Ispezione e media

### `compare <a.gguf> <b.gguf>`

Confronto **numerico per tensore** (dequantizza i tensori comuni):

```sh
fastllm compare base.gguf quant.gguf
fastllm compare a.gguf b.gguf --glob 'blk.*.ffn_*'
```

`rel_l2` e `avg_diff%` (0 = identico, ~100 = casuale): serve per il **lineage** (i layer congelati
hanno `avg_diff ≈ 0`) e per il breakdown per-ruolo dopo quantizzazione o surgery.

### `tensors <model.gguf>`

```sh
fastllm tensors modello.gguf
fastllm tensors modello.gguf --diffable   # output deterministico: due file identici danno lo stesso testo
```

Metadati (key-value) e tabella tensori (tipo, nome, dims; offset e byte senza `--diffable`).

### `gmbarch <url|org/nome|cartella>`

```sh
fastllm gmbarch mlx-community/Mixtral-8x22B-4bit
fastllm gmbarch ~/ai/Models/Qwen3-8B-4bit
```

Crea `<model_type>.gmbarch` nella cartella **ABI di LLMEnv** (ruoli dei tensori, mappa dei campi di
`config.json`, strutture attenzione/MoE/vision) **solo se l'architettura non ha già un descrittore**:
uno esistente non viene mai sovrascritto. Lo stesso controllo parte da solo dopo `pull` e `add`.

### `asr <model_dir|model.gguf> <file>`

```sh
fastllm asr ~/ai/Models/Qwen3-ASR-0.6B discorso.wav
fastllm asr parakeet-tdt.gguf audio.mp3
```

Due famiglie: **Qwen3-ASR** (safetensors HF, 0.6B/1.7B) e **Parakeet-TDT** in GGUF (25 lingue
europee; su macOS accetta anche mp3/m4a/aiff). Il testo esce **token-live** (chunk 2 s + cache delle
finestre encoder + prefix rollback).

---

## 8. Diagnostica

### `doctor [<filtro>]`

```sh
fastllm doctor
fastllm doctor qwen
fastllm doctor mlx-community/* --quiet     # solo i PROBLEMA
```

Hasha **davvero** i byte del cache e li confronta con il nome del blob (la stessa verifica usata da
`pull`). Due ordini: i PROBLEMA muovono l'uscita, le note no. Il danno non si tocca: la cura è
`pull --repair`. Zero match = fallimento.

### `options`

```sh
fastllm options
fastllm options --json
```

Elenca **tutte** le opzioni del motore (registro, senza env) con valore corrente ed esempio. Non
carica alcun modello: è la fonte di verità per `--opt`.

---

## 9. Opzioni globali, ambiente, uscite

| opzione | effetto |
|---|---|
| `--opt <nome>=<valore>` | parametro del motore (ripetibile, prima del load) |
| `--context <n>` | contesto in token (`32k`, `1m`), alias dell'opzione di registro `context` |
| `--offload-experts` | streama gli esperti MoE da disco |
| `--cache <dir>` | radice del cache (`pull`, `models`, `list`, `serve`, `doctor`) |
| `--quiet` | niente progresso/riepilogo |
| `-h`, `--help` | aiuto del comando (uscita 0) |

Una bandiera che il verbo **non legge** è un **errore**, non un'opzione ignorata.

Variabili d'ambiente (solo **servizio**, mai parametri del motore):

| variabile | uso |
|---|---|
| `HF_TOKEN` | token Hugging Face per repo privati/gated (mai in file, log o commit) |
| `FASTLLM_HOME` | radice dei file di stato (default `~/.fastllm`) |
| `FASTLLM_PORT` | porta di default per `ps` (poi 8000) |
| `FASTLLM_NO_OFFLOAD_PROMPT=1` | disabilita la domanda `[Y/n]` sull'offload |
| `HF_HUB_CACHE` / `HF_HOME` | posizione del cache Hugging Face |

Uscite: `0` successo · `1` uso errato/errore di lane (il codice preciso è nel messaggio) ·
`2` verbo non ancora cablato (dice quale pezzo manca).

**Scelta del motore DS4.** Dal 2026-10-30 c'è **un solo** motore DS4: il **vendor**
(`LLMEngine/ds4`), che copre tutte le famiglie. Il motore nativo (ex ds4-slow) è **ritirato** —
era più lento e usava il doppio della RAM sullo stesso GGUF. L'opzione resta per compatibilità:

```sh
fastllm --opt ds4_engine=auto   run modello.gguf   # default: vendor
fastllm --opt ds4_engine=vendor run modello.gguf   # idem
fastllm --opt ds4_engine=native run modello.gguf   # accettato, avvisa che il nativo è ritirato
```

`auto` instrada `deepseek4`, `deepseek41`, `glm5-next` e `qwen4exp` sul vendor, `glm-dsa` su
llama.cpp (il vendor si può forzare con `vendor`). Il valore è nel registro
(`fastllm options`), non in una variabile d'ambiente.

**MTP/DSpark del vendor.** Opzioni del registro, lette all'apertura del motore:

```sh
fastllm --opt ds4_mtp=/percorso/DSpark-support.gguf --opt ds4_dspark=1 run modello.gguf
```

`ds4_dspark=1` richiede `ds4_mtp` (errore esplicito se manca); un path non leggibile viene nominato
prima di aprire.

**Visione: resta su llama.cpp.** I motori MLX e ds4 **non** espongono `vision_*` nell'ABI: VLM e OCR
passano da `--vision-model`/`--vision-mmproj` con l'engine llama.cpp/mtmd. Non esiste un
`--opt ds4_vision` — sarebbe una promessa che il motore non mantiene.

---

## 10. Ricette

```sh
# Dal repo al primo token, in cinque righe
fastllm pull mlx-community/Qwen3-8B-4bit
fastllm list
fastllm test mlx-community/Qwen3-8B-4bit
fastllm run mlx-community/Qwen3-8B-4bit "Ciao"

# Un GGUF enorme, con gli esperti su disco
fastllm pull antirez/glm-5.3-flash-gguf --file 'GLM-5.3-Flash-Q2.gguf'
fastllm add glm53 ~/.cache/huggingface/hub/models--antirez--glm-5.3-flash-gguf/snapshots/*/GLM-5.3-Flash-Q2.gguf
fastllm run glm53 --offload-experts --context 32k "Riassumi in tre punti."

# Server in rete locale
fastllm serve --host 0.0.0.0 --port 8000 --cache ~/.cache/huggingface --batch --parallel 4

# Un modello locale su disco esterno
fastllm paths add /Volumes/disco/ai/Models
fastllm search /Volumes/disco/ai/Models --type mlx
fastllm add locale 1
fastllm chat locale
```

---

## 11. Problemi comuni

| sintomo | causa | rimedio |
|---|---|---|
| `pull` scarica troppo | il repo contiene più varianti | `--file '<glob>'` (vedi [MODELS.md](MODELS.md)) |
| `test` esce 1 | download incompleto o blob corrotto | riprovare `pull`; se il blob è corrotto `pull --repair` |
| `doctor` segnala PROBLEMA | byte diversi dal nome del blob | `pull --repair` sul repo indicato |
| «motore non disponibile» | dylib LLMEnv non trovata accanto al binario | ricostruire `fastllm` (le dylib escono nel build) |
| il modello non entra in RAM | residente troppo grande | `--offload-experts` e `--context` più corto |
| `run` chiede `[Y/n]` | modello oltre metà della memoria libera | rispondere, o `FASTLLM_NO_OFFLOAD_PROMPT=1` |

---

## 12. Riferimenti

- [MODELS.md](MODELS.md) — comandi di download per ogni modello gestito.
- [README.md](README.md) — stato del codice e confini.
- `fastllm help <comando>` — l'aiuto vivo di ogni verbo (questa guida lo segue).
- `docs/quant-coverage.md`, `docs/json-contract.md`, `docs/mlx-quant-bridge.md` — contratti interni.
