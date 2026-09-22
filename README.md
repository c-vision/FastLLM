# FastMLX

Motore LLM in C puro: due programmi, **`MLXEngine`** (i motori MLX e GGUF veri, con swap
su disco) e **`FastMLX`** (server OpenAI/Anthropic + CLI in stile Ollama + download e cache
da Hugging Face). Piano completo: `AiWiki/01-Projects/FastMLX/Piani/(open) - FastMLX-Piano-Fondativo.md`
(vault, non versionato qui).

## Stato reale del codice (non quello del piano)

| componente | stato |
|---|---|
| `src/lane_gguf/fmlx_gguf.c` — lettore GGUF di riferimento | **esiste, compilato, testato** (259 verifiche) |
| `src/lane_gguf/fmlx_quant.c` — dequant ggml → F32 (13 tipi) | **esiste, compilato, testato** (461 verifiche + gate differenziale) |
| `src/hf/fmlx_hf_cache.c` + `fmlx_hf_hash.c` — cache HF letto, senza rete | **esiste, compilato, testato** (131 verifiche + sonda sul cache reale) |
| `src/hf/fmlx_hf_fetch.c` + `fmlx_hf_curl.c` — download, lock, resume, verifica | **esiste, compilato, testato** (172 verifiche senza rete + probe sul server vero) |
| `src/lane_mlx/` — motore MLX estratto da GMBasic | non scritto. **D2 non blocca il "come"**: i prerequisiti per compilarlo sono già su questa macchina (verificato 2026-09-22: 28 header in `GMBasic/libs/mlx-deps/mlx-c-src/mlx/c/*.h`, più `libmlx.a` 31 MB e `libmlxc.a` 1,0 MB in `libs/mlx-deps/precompiled/darwin-arm64`). D2 decide solo **chi possiede** il motore |
| `src/server/` (OpenAI + Anthropic), `src/cli/` (stile Ollama) | non scritti |

Qui sotto c'è solo ciò che compila davvero. Nessuna cartella vuota "perché il piano la prevede".
L'enumerazione di ciò che MLX/caricatore loader GGUF sanno davvero fare, con i `file:riga` della
lettura, è in **`docs/quant-coverage.md`** (ed è lì che è finito il bug dei cinque tipi spostati).

## La lane GGUF oggi

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

`include/fmlx/fmlx_quant.h` + `src/lane_gguf/fmlx_quant.c`: `fmlx_ggml_dequant_row(type, src, n,
dst)` converte una riga di `n` elementi in F32. Copre **13** tipi: F32, F16, BF16, Q4_0, Q8_0,
Q2_K, Q4_K, Q5_K, Q6_K, **IQ2_XXS**, I8, I16, I32. Non è una scelta di comodo: ogni tipo è entrato
perché esiste un **secondo codice** con cui confrontarlo bit a bit — oggi i codici indipendenti
confrontati dal gate sono tre (due di casa + `ggml` di llama.cpp) e i 13 sono esatti su tutti e
tre. Tutto il resto (Q4_1, Q5_0/1, Q8_1, Q3_K, Q8_K, gli altri IQ, TQ*, MXFP4, NVFP4, Q1_0, I64,
F64) ritorna `FMLX_GGUF_ERR_NOT_COVERED` e **non tocca `dst`**: gap dichiarato, non uno stub che
rimette zeri. `fmlx_ggml_type_is_covered` permette di saperlo prima di copiare byte.

**18 di quei gap sono già colmabili** — il gate li enumera uno per uno con i testimoni pronti
(Q4_1, Q5_0, Q5_1, Q3_K, Q8_K, IQ2_XS, IQ3_XXS, IQ1_S, IQ4_NL, IQ3_S, IQ2_S, IQ4_XS, IQ1_M,
TQ1_0, TQ2_0, MXFP4, NVFP4, Q1_0: tutti con `ggml` come secondo codice). Sono il lavoro di
dequant successivo, non una promessa: un tipo entra solo quando la SUA riga passa, non prima.

Conseguenza misurata: **ogni tensore dei 7 GGUF presenti su questa macchina è di un tipo
coperto** (istogrammi: `F32 F16 Q8_0 Q2_K Q4_K Q6_K IQ2_XXS I32 BF16`), e il gate sotto lo
conferma bit a bit su quei tensori reali.

Le codebook IQ2 (`src/lane_gguf/fmlx_iq2_tables.h`) sono **generate**, non trascritte a mano:
`python3 tools/gen_codebooks.py --check` confronta voce per voce le tre copie indipendenti su
disco (ds4 + i due duplicati del vault GMBasic) e rifiuta di scrivere se divergono anche di uno
solo dei 256×64 + 128×8 bit. Licenza MIT ggml: vedi `LICENSES.md`.

## La lane HF (cache) oggi

`include/fmlx/fmlx_hf.h` + `src/hf/fmlx_hf_cache.c` + `src/hf/fmlx_hf_hash.c`: legge il cache di
Hugging Face **senza toccare la rete**. Ogni regola è misurata da una fonte, non ricordata:

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

Cosa **non** fa ancora: nessun protocollo xet (esiste nel cache come staging, l'ho misurato ma non
l'ho implementato), nessuna API `delete`/`upload`.

## Lane download (`src/hf/fmlx_hf_fetch.c` + `fmlx_hf_curl.c`) — esiste, compilato, provato

La logica di cache e il trasporto sono due file separati perché la prima si prova **senza rete**:
`tests/hf/test_hf_fetch.c` usa un transport finto (20 casi, 172 verifiche) e scopre i bug di
DOVE-finiscono-i-byte, che sono gli unici che corrompono un cache condiviso.

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

  FastMLX sta sulla semantica di **sinistra** — nome determinato, partial mantenuto — perché il caso
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

## Build e test

```sh
cmake -S . -B build
cmake --build build          # -Wall -Wextra -Werror
ctest --test-dir build --output-on-failure   # 4 suite, nessuna tocca la rete
./build/fmlx_gguf_inspect <file.gguf>   # testata, tipi, allineamento, coda di byte
./build/hf_probe --tree <org>/<repo> [<rev>]   # cache HF reale: locate + verifica, sola lettura
HF_HUB_CACHE=work/probe-cache ./build/hf_fetch_probe <org>/<repo> [<rev>] [<file>] [<max byte>]
                                         # download VERO, in una cache di lavoro: mai la reale
```

`hf_probe` è l'unico contatto con i dati reali ed è **in fuori da `ctest` di proposito**: la suite
devve girare su una macchina senza cache e senza rete. La sua ragione di esistere è che un parser
testato solo su fixture sintetiche può essere coerente con se stesso e sbagliato col mondo — è
esattamente ciò che è successo alla tabella dei tipi (vedi sopra), e il cache reale di questa
macchina ha due radici diverse (`HF_HOME` e `~/.cache`) che il probe distingue da solo.

La suite (`tests/gguf/test_gguf.c`) **non scarica nulla**: i file validi si scrivono col writer
di fixture del progetto stesso, i file malati si costruiscono byte per byte. Copre i casi (a)–(g)
del piano §8 più undici corruzioni distinte, ciascuna con il proprio codice atteso.
`tests/gguf/test_quant.c` prova invece i valori noti (IEEE half, blocchi costruiti a mano) e — la
parte che conta — che **un rifiuto non lasci valori in `dst`**.

Il **gate differenziale** non è un target CMake di proposito: confronta il dequant con tre
implementazioni che non vivono in questo repo — due già in esercizio su GMBasic (A, B) e
`ggml`/llama.cpp (C), che è anche l'autorità sui NUMERI di enum perché i file GGUF li scrive. Ha
bisogno di quei sorgenti e della libreria già costruita su questa macchina. Si lancia così
(7 GGUF reali di questa macchina + blocchi pseudo-casuali a seed fisso):

```sh
G=~/Documents/Projects/GMBasic
L=~/llamacpp
cc -Wall -Wextra -Werror -std=gnu17 -I include -I "$G/native/gmb_host_ai_mlx" \
   -I "$G/native/gmb_moe_runtime/include" -I "$G/native/gmb_moe_runtime/src" \
   -I "$L/ggml/src" -I "$L/ggml/include" \
   tools/quant_diff.c tools/quant_diff_ggml.c src/lane_gguf/fmlx_gguf.c \
   src/lane_gguf/fmlx_quant.c "$G/native/gmb_moe_runtime/src/gmb_moe_ggml.c" \
   -L "$L/build/bin" -lggml-base -Wl,-rpath,"$L/build/bin" -o build/quant_diff
./build/quant_diff ~/ai/Models/gguf/*.gguf ~/ds4/ds4flash.gguf
```

Esito a tre valori, volutamente: `0` nessun bit divergente, `1` divergenze, `2` **almeno un file
non si è aperto** — che non è un verdetto sui bit e non va mai stampato come se lo fosse. Il gate
segnala anche il caso "due testimoni divergono tra loro", che è un bug da aprire dove quei codici
vivono (vault GMBasic per A e B), non una scelta di questo progetto.

Lo stesso eseguibile fa l'**audit** su due assi (`34 tipi confrontati sui bit, 42 forme confrontate
con ggml, 0 incoerenze, 18 gap colmabili`):
* **bit** — `is_covered` non può mai asserire una copertura che nessun secondo codice conferma;
  il caso inverso (un testimone copre e noi no) NON è un fallimento ma una **lista di gap
  colmabili**, cioè il lavoro che si può fare restando dentro la regola;
* **forma** — elementi per blocco e byte per riga della ns. tabella contro `ggml_row_size` per
  **tutti e 42** i valori di enum. È l'asse che vale più dei bit per i tipi che nessun file
  locale contiene: senza di quello, cinque tipi spostati di uno restavano invisibili.

## Regole che questo repo eredita

* **Niente stub**: una funzione che non esiste non viene dichiarata; una lane che non c'è non ha
  target CMake. Un caso non implementabile deve **fallire con un errore**, mai ritornare 0.
* **File ≤ ~1200 righe, uno per dominio** (il lettore GGUF è già oltre la soglia di attenzione:
  si spezza per area — tabella tipi / parse KV / parse tensor-info — appena cresce).
* **Nessun test massivo** senza richiesta esplicita: si esegue la suite del componente toccato.
* **Nessun nome di persona o azienda nei nomi dei file.**
* Licenze e attribuzioni: vedi `LICENSES.md`. **Un'obbligazione esiste già oggi**: le codebook
  IQ2 sono dati ggml (MIT) copiati con `tools/gen_codebooks.py`, attribuiti nel file generato e
  in `LICENSES.md`. Le altre (oMLX Apache-2.0, mlx/mlx-c, llama.cpp) scattano con la lane MLX.
