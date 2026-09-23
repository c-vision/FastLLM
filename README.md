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
| `src/hf/fmlx_hf_fetch.c` + `fmlx_hf_curl.c` — download, lock, resume, watchdog di stall, backend classico su repo xet | **esiste, compilato, testato** (199 verifiche senza rete + probe sul server vero; il watchdog su rete reale NON è ancora misurato) |
| `src/hf/fmlx_hf_manifest.c` — lista dei file di un repo (ciò che serve a `pull` per sapere COSA scaricare) | **esiste, compilato, testato** (51 verifiche senza rete + sonda sul server vero: `bert-base-uncased` 16 file, commit `86b5e09…`, 7 con `lfs`) |
| `src/json/fmlx_json.c` + `fmlx_jsonw.c` — parser e writer JSON | **esiste, compilato, testato** (327 + 148 verifiche, **e** differenziale contro CPython su 122 record: 8 divergenze dure volute, 0 round-trip — `docs/json-contract.md`) |
| `src/server/fmlx_http.c` — codec HTTP/1.1: framing richiesta, testata risposta, SSE | **esiste, compilato, testato** (127 verifiche byte-esatte) |
| `src/server/fmlx_api*.c` — router + adapter OpenAI/Anthropic sopra il codec | **esiste, compilato, testato** (151 verifiche). Non c'è ancora un motore sotto: la generazione è attaccata al confine `FmlxApiNext`, che il test copre con un doppio scriptato |
| `src/server/fmlx_serve_respond.c` + `fmlx_serve.c` — il server: `/v1/models`, `/health`, il 501 per ogni rotta che genera, e il loop TCP | **esiste, compilato, testato, provato su socket vero** (150 verifiche + uno smoke del binario con `curl` vero). Il motore non c'è e **non serve per provarlo** |
| `src/lane_mlx/` — motore MLX estratto da GMBasic | non scritto. **D2 non blocca il "come"**: i prerequisiti per compilarlo sono già su questa macchina (verificato 2026-09-22: 28 header in `GMBasic/libs/mlx-deps/mlx-c-src/mlx/c/*.h`, più `libmlx.a` 31 MB e `libmlxc.a` 1,0 MB in `libs/mlx-deps/precompiled/darwin-arm64`). D2 decide solo **chi possiede** il motore |
| `src/cli/fmlx_pull.c` + `fmlx_cli.c` — `fastmlx pull`, il dispatch e gli esiti | **esiste, compilato, testato, provato sul server vero** (48 + 156 verifiche; pull reale `sshleifer/tiny-gpt2`: 9/9 file, 9 blob il cui nome è il loro hash, snapshot riletto da `hf_probe`) |
| `src/cli/fmlx_models.c` + `src/hf/fmlx_hf_catalog.c` — `fastmlx models` / `list` | **esiste, compilato, testato, provato sul cache vero di questa macchina** (103 + 156 verifiche; 23 repo letti in 0,01 s, 3.675.292.998 byte che coincidono con un conteggio di inode fatto a parte con `find`+`stat`) |
| `src/cli/fmlx_serve.c` — `fastmlx serve` | **esiste, compilato, testato, provato**: risolve host/porta/chiave (flag > ambiente > default, piano §14) e passa tutto al loop. `--cache` vale anche qui: `pull`, `models` e `serve` guardano LO STESSO disco |
| `src/cli/` — `run\|chat\|ps\|cp\|rm\|doctor` | **non collegati, e lo dicono**: `fmlx_cli_exec` ritorna `NOT_WIRED` nominando il pezzo che manca (motore / registro delle sessioni / chi scrive nel cache) ed esce 2. Il parser delle bandiere invece **esiste**, ed è provato per i 6 verbi rimasti — e una bandiera che il verbo non legge è un errore, non un'opzione ignorata |

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
non in una scelta cablata del transport; `CURLOPT_XFERINFOFUNCTION` tronca la richiesta quando non
arriva **un byte** per quel tempo (`LOW_SPEED_TIME`, che misura la media, da solo non lo prenderebbe
mai) e la ripartenza chiede `Range: bytes=N-` sui byte già scritti nel `.incomplete`, con
`stall_retries` ripartenze e backoff 250→2000 ms. Un 404, un hash che non torna o un lock in mano ad
altri risale subito: ripeterebbe la stessa risposta.

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

**Ciò che non è onorato viene DETTO.** `logit_bias`, `logprobs`, `top_logprobs`,
`response_format`, `user` sono accettati per nome e dichiarati in `ignored_params[]` (piano:474);
`n:2` è un 400 con `param:"n"`, non una risposta singola silenziosa; `max_tokens: 1.5` è un errore
di tipo, non un troncamento. Due `max_tokens`/`max_completion_tokens` discordi: vince il più
recente, l'altro finisce negli ignored.

**`|` non sequenzia gli operandi, e qui l'ordine È il contratto.** La prima versione degli adapter
accumulava gli esiti del writer così: `bad |= fmlx_jsonw_key(w, "id") | fmlx_jsonw_cstr(w, id);`.
Il C garantisce che i due valori vengano prodotti, non **in quale ordine**: un compilatore può
calcolare `id` prima di emettere la chiave, o un `arr_end` prima del suo `arr_begin`. La forma
corretta è `if (A || B) return …;` — `||` sequenzia. L'accumulatore `bad` è stato rimosso del tutto
(tutti i punti migrati con `tools/gmb_seq_unbad.py`, che ha anche la modalità `--wrap` per il limite
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
2. **quel che manca lo dice** — ogni rotta che genera (`chat/completions`, `completions`,
   `responses`, `messages`, `models/load|unload|status`, `embeddings`, `metrics`) risponde **501**
   con UNA frase sua e un `code` machine-readable per causa (`engine_not_wired`, …). Il piano
   (§11.2) vieta il risultato che sembra quello che non è: un 200 con `content: ""` sarebbe il
   peggiore esito possibile — coerente, vuoto, e falso;
3. **quel che NON richiede il motore viene già validato oggi** — auth, incorniciatura, tetto del
   corpo, JSON, `anthropic-version`. Un 400 che arriva oggi arriverà anche quando il motore ci
   sarà: il comportamento del server non deve cambiare all'arrivo della lane ①.

**`degraded` (200) contro `unavailable` (503)** è la scelta fra «sono limitato» e «sono rotto».
`/health` è `degraded` quando fa le due cose che dichiara e non può generare — dire `ok` a chi
chiede «qui si lavora?» sarebbe bugiardo per omissione, e 503 mentirebbe dall'altra parte. È
`unavailable` quando **nemmeno il catalogo si legge**: il cache non c'è o `HF_HOME` punta nel posto
sbagliato. I contatori (`repos_total`/`repos_listed`/`repos_refs_only`/…) arrivano dagli stessi
campi che stampa `fastmlx models`, non da un secondo conteggio.

**L'auth viene SOLO da `FASTMLX_API_KEY`**, e non è una bandiera per lo stesso motivo per cui
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
fuori da `ctest`, il binario: `fastmlx serve --cache <fixture> --port 8733` + `curl` →
`{"object":"list","data":[{"id":"org/demo","object":"model","created":…,"owned_by":"local"}]}`,
501 su `POST /v1/chat/completions`, e due richieste sulla STESSA connessione keep-alive contate
`#2` e `#3` nel log. `--once` esiste per questo: serve UNA richiesta e esce, così uno smoke non
lascia un processo appeso.

**Cosa NON è misurato qui**: il cammino dei segnali (`SIGINT`/`SIGTERM` → `g_stop` → il loop esce e
`restore_handlers` rimette i precedenti) non è eseguito da nessun test — si può provare solo
uccidendo un processo vero, e un test che ammazza processi è un test che si rompe da solo. Non è
misurato il `bind` su una porta già occupata (sarebbe un fixture dipendente dallo stato della
macchina). **`Expect: 100-continue` invece SÌ**, ed è il caso che ha nominato da sé la sua ragione
d'essere: la prima versione mandava un corpo con `messages: []`, e il layer ② rispondeva 400 **prima**
di arrivare al motore — il test passava sul via libera e falliva sull'esito, che è il modo giusto
per fallire. Ora il caso prova tre cose in ordine: i 25 byte del via libera arrivano **prima** che il
corpo sia scritto (l'ordine dei byte È la prova: senza, il client che rispetta il protocollo non
manda niente e lo stallo sembra lentezza), il 501 che segue è lungo esattamente quanto il suo
`Content-Length`, e un corpo da zero byte **non** ottiene un via libera.

## La lane CLI (`src/cli/fmlx_pull.c` + `fmlx_cli.c`) — il primo pezzo eseguibile

`fastmlx` è il primo binario di questo repo che si può lanciare da terminale. Fa **una** cosa
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

**Un errore ferma tutto, senza lasciare pezzi.** Al primo fallimento: nessun mirror, nessun ref, `FmlxPullReport.error_*` compilato con la causa e il file, esito `FAILED`. Copiare i file già scaricati
prima di sapere se il nono esiste vorrebbe dire consegnare uno snapshot parziale che sembra completo.

**Il dispatch:** `fmlx_cli_parse` non sa nulla dei motori e `fmlx_cli_exec` non sa nulla delle
bandiere; il primo produce `FmlxCliArgs`, il secondo mappa gli esiti su exit code (`OK`→0,
`USAGE`→2, `FAILED`→1, `NOT_WIRED`→1). Il token **non è una bandiera**: si passa solo con
`HF_TOKEN`, perché una `--token` finirebbe nello storico del terminale e in ogni script che la
invoca — e per lo stesso motivo `FASTMLX_API_KEY` non ha la sua bandiera. `--help` non è un errore e
non vuole un verbo; un parse fallito azzera `FmlxCliArgs` (un verbo residuo verrebbe eseguito). I
nomi long si ammettono sia staccati sia `--flag=val`.

**Le bandiere sono un INSIEME DI PAROLE, non un valore.** Ogni opzione ha un bit in una tabella
unica, perché la domanda «questa bandiera è stata data?» non si può fare sul valore: `--stall-retries
0` e `--stall-retries` non dato sono lo stesso numero (0). Da lì due regole che senza quel bit non
esistono: **una bandiera che il verbo non legge è un errore** (`fastmlx serve --dest x` → «--dest non
è un'opzione di serve», exit 2) — accettarla significherebbe eseguire un comando che sembra dire due
cose e ne fa una, e chi legge l'output non ha modo di sapere quale delle due è saltata; e **una
booleana non prende valore** (`--once=0`, `--quiet=1`, `--offline=` → rifiutati), perché `--once=0`
avrebbe lo stesso effetto di `--once=1`: due semantiche con la stessa grafia. Le condivise —
`--quiet` e `--cache` — non stanno in nessun insieme esclusivo: `--cache` alimenta `pull`, `models` e
`serve` con un solo valore, perché i tre verbi guardano LO STESSO disco e una `--cache` ignorata da
`serve` (col server che legge `HF_HOME`) sarebbe un'opzione che mente.

**La prima regola vale solo fra i verbi che hanno una superficie di opzioni** (`pull`, `models/list`,
`serve`). `fastmlx run --port 8000` NON deve sentire «--port non è un'opzione di run»: `run` non ha
ancora opzioni, e dire quali non ha è un'affermazione sul futuro di un verbo che non è scritto. Il
parse passa, e a parlare è il dispatch con l'unica cosa vera oggi: manca il motore (exit 1).

**Misurato sul server vero** (fuori da `ctest`, come le altre tre sonde):
`build/fastmlx pull sshleifer/tiny-gpt2 --cache <work> --dest <work>` → esito 0, 9/9 file, 10.9 s,
`refs/main` = `5f91d94bd9cd…`, e `HF_HUB_CACHE=<work> build/hf_probe --tree sshleifer/tiny-gpt2 main`
risolve lo snapshot scritto da `pull` con **`verifica=ok` su tutte le entry** — il cache che
scriviamo è leggibile dal lettore con cui abbiamo verificato il cache di `huggingface_hub`.

**Cosa NON è misurato qui**: la via di fallback `copy_stream` (hardlink impossibile) non è mai
stata eseguita — il test gira su un singolo filesystem; la riga di avanzamento in-place (stile
Ollama) non è implementata; i 6 verbi non collegati non hanno ancora un motore da nominare davvero,
nominano il pezzo.

## `fastmlx models` / `list` — il catalogo locale (`src/hf/fmlx_hf_catalog.c` + `src/cli/fmlx_models.c`)

Secondo verbo vivo, e primo che **non tocca la rete**: legge il cache così com'è. Non prende un
`FmlxHfTransport` nemmeno per finta, perché la lane che legge il disco un transport non ce l'ha.

**L'ingombro è dei blob, non degli symlink.** Uno snapshot è un fascio di link a
`../../blobs/<nome>`: sommare anche quelli raddoppierebbe ogni modello, e chi ha 40 GB liberi si
fida di quel numero. Si contano gli **inode distinti** di `blobs/` (due hardlink allo stesso blob
sono un blob), un solo livello di ricorsione perché i blob sono piatti — e ciò che fosse annidato
NON è un nostro blob. Misurato: la colonna `DIM` della tabella e un
`find models--*/blobs -type f | xargs stat -f "%z %i" | sort -u -k2` indipendente coincidono a
**3.675.292.998 byte** su 23 repo.

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

**Due difetti trovati solo girando sul campo**, non sul fixture: la prima `parse_dirname` produceva
`org/-due` al posto di `org/due` (dopo aver scritto `/` per la coppia copiava anche il secondo `-`)
e il messaggio di rifiuto dell'argomento posizionale stampava `fastmlx: :` con il verbo **vuoto**
(`fail()` azzera `FmlxCliArgs` prima che il `%s` legga `out->verb`). Il secondo è passato inosservato
al test perché l'asserzione guardava solo il frammento finale: ora si asserisce anche il nome del
verbo, per esteso.

## Build e test

```sh
cmake -S . -B build
cmake --build build          # -Wall -Wextra -Werror
ctest --test-dir build --output-on-failure   # 14 suite, nessuna tocca la rete (1,4 s)
./build/fmlx_gguf_inspect <file.gguf>   # testata, tipi, allineamento, coda di byte
./build/fastmlx pull <org>/<repo> [--revision R] [--dest D] [--cache C]   # lo snapshot nel cache
./build/fastmlx models [--cache C] [--quiet]   # cosa c'è già nel cache, senza rete (`list` = alias)
./build/fastmlx serve [--host I] [--port N] [--cache C] [--max-body N] [--once]
                                         # /v1/models + /health VERI, 501 su tutto il resto;
                                         # l'auth è SOLO FASTMLX_API_KEY (vuota = nessuna)
./build/hf_probe --tree <org>/<repo> [<rev>]   # cache HF reale: locate + verifica, sola lettura
HF_HUB_CACHE=work/probe-cache ./build/hf_fetch_probe <org>/<repo> [<rev>] [<file>] [<max byte>]
                                         # download VERO, in una cache di lavoro: mai la reale
./build/hf_manifest_probe <org>/<repo> [<rev>]
                                         # UNA richiesta di metadati: lista i file, zero byte di modello
```

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
* **Niente `|` per combinare effetti** — e niente `int` che diventa `FmlxApiErr` senza rileggere i
  punti di chiamata. `A | B` non garantisce che `A` sia eseguito prima di `B`: dove l'ordine degli
  effetti È il contratto (chiave prima del valore, `arr_begin` prima del suo `arr_end`) si usa
  `||`/`&&`/`?:`/`,` o due statement. E `== 0` cambia significato con il tipo: 1=ok e 0=ok sono
  compatibili col compilatore e incompatibili col mondo.
* **Nessun nome di persona o azienda nei nomi dei file.**
* Licenze e attribuzioni: vedi `LICENSES.md`. **Un'obbligazione esiste già oggi**: le codebook
  IQ2 sono dati ggml (MIT) copiati con `tools/gen_codebooks.py`, attribuiti nel file generato e
  in `LICENSES.md`. Le altre (oMLX Apache-2.0, mlx/mlx-c, llama.cpp) scattano con la lane MLX.
