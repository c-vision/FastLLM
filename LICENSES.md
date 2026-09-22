# Licenze e attribuzioni

Aggiornato: 2026-09-22. Questo file elenca **cosa è stato usato** e **cosa si userà**, tenendo
separate le due cose: un'attribuzione per codice non ancora preso è falsa tanto quanto un'ombra.

## Oggi (lane_gguf)

**Nessun file sorgente di terze parti è incluso in questo repo.** Il lettore GGUF
(`src/lane_gguf/fmlx_gguf.c`) è scritto da zero qui. Ciò che è stato *letto* per scrivere i
numeri della tabella dei tipi, con la fonte citata nel codice e nel test di parità:

| cosa | da dove | licenza |
|---|---|---|
| valori di enum e dimensioni dei blocchi (42 tipi) | `/Users/gaetano/ds4/gguf-tools/quants.h:20-54` e `quants.c:39-74` (`ds4q_type_traits`) — **verificati poi contro l'autorità**: `llamacpp/ggml/include/ggml.h:389-433`. La prima trascrizione aveva spostato cinque tipi di uno comprimendo i buchi dell'enum: `docs/quant-coverage.md` §5 | MIT — `/Users/gaetano/ds4/LICENSE`, "The ds4.c authors", "The ggml authors", "DeepSeek" |
| **testimone C del gate**: `dequantize_row_*` (25) e `ggml_row_size`, letti E LINKATI a solo tempo-di-sviluppo dalla libreria già costruita su questa macchina (`llamacpp/build/bin/libggml-base.0.15.3.dylib`) | llama.cpp / ggml | **MIT** — **nessun codice copiato, nulla distribuito**: l'obbligo scatta solo se la lane engine linkerà ggml nel prodotto (riga sotto) |
| struttura del record GGUF (magic, versioni 2/3/4, KV, tensor-info, `general.alignment`) | formato pubblico GGUF, ricopiato dai tre lettori già su questa macchina e verificato sui file a disco | — (specifica di formato) |
| **tabelle codebook IQ2** (`src/lane_gguf/fmlx_iq2_tables.h`: `iq2xxs_grid` 256×u64, `ksigns` 128×u8) | **dati copiati**, generati da `tools/gen_codebooks.py` contro tre copie concordi su questa macchina: `/Users/gaetano/ds4/ds4.c` e i due duplicati `GMBasic/native/{gmb_host_ai_mlx,gmb_moe_runtime/src}/gmb_iq2_tables.h` (che a monte sono le tabelle ggml) | **MIT (ggml)** — attribuzione dovuta qui e nel file generato |

**Derivazione reale, oggi:** le codebook IQ2 sono *data copying*, non lettura: per questo esistono
(a) nel file generato, con la provenienza, (b) con il comando che le rigenera e le incrocia,
(c) qui. Il testo integro della licenza (il file `LICENSE` MIT di ggml/llama.cpp) **va venduto in
questo repo prima di qualunque distribuzione**: ora il repo è privato e non redistribuisce nulla,
quindi l'obbligo è registrato e non ancora adempiuto — è la decisione **D3** che lo apre.
Il resto della lane dequant (`fmlx_quant.c`) è **scritto da zero qui**, e le sue formule sono
*verificate* contro tre implementazioni indipendenti — le due di casa più `ggml`
(`tools/quant_diff.c` + `tools/quant_diff_ggml.c`) — **senza copiarne nessuna delle tre**.

Se in futuro si incolla una funzione di `quants.c` o di ggml, la testata MIT va riportata qui
e nel file che la contiene.

## Oggi (lane_hf: cache in sola lettura + download)

| cosa | da dove | licenza / obbligo |
|---|---|---|
| contratto del layout del cache (catena `HF_HUB_CACHE`/`HF_HOME`/`XDG_CACHE_HOME`, `REPO_ID_SEPARATOR = "--"`, template URL, `{1,ON,YES,TRUE}`, `.no_exist`) | **letto** in `huggingface_hub/constants.py` — **copia `~/ai/env-vllmlx`**: righe `10,67-68,99,129-155,165`; la STESSA copia `~/ai/env-mflux` dice `11,68-69,118,166-189,202` | **Apache-2.0 (huggingface_hub)** — qui è *specifica di comportamento*, non codice: nessun file Python copiato o generato. Se in futuro si linka o si trasporta codice di huggingface_hub, la licenza va venduta e dichiarata |
| layout dei nomi-blob (40 hex = git blob id, 64 hex = SHA256) | **misurato su questa macchina** con `git hash-object` e `shasum -a 256` sui blob reali (6/6 e 2/2) — non preso da un sorgente | nessuna (proprietà del formato git/LFS) |
| SHA1/SHA256 del verificatore | `CommonCrypto` (`CC_SHA1`/`CC_SHA256`), header nell'SDK di sistema (`MacOSX.sdk/usr/include/CommonCrypto/CommonDigest.h`) | framework Apple, parte del SO: nessuna attribuzione da vendere, nessun file da includere |
| libcurl, il trasporto del download | `MacOSX.sdk/usr/include/curl/curl.h` + `-lcurl` (probe compilato: `libcurl/8.7.1 SecureTransport`) | **curl license** (MIT-consentente): link dinamico a libreria di sistema, nulla da vendere. **Nota: questa misura smentisce ciò che avevo scritto prima** — le intestazioni di curl *ci sono*; la decisione di trasporto è cambiata di conseguenza (README, "Trasporto del download") |
| contratto del download (HEAD + `Accept-Encoding: identity`, redirect relativo, `url_to_download = metadata.location`, token tolto su host terzo, etag **e** lunghezza obbligatori, GET saltata sul temporaneo completo, 200-a-Range che azzera, `.incomplete`, ref scritto dopo) | **letto su ENTRAMBE le copie** (tabella sotto). Copia `env-mflux` → `file_download.py:359-361,384-396,596,1058,1203,1237,1612,1615-1617,1623-1632,1733-1737,1752,1760-1766,1900-1908,1944-1947` + `utils/_http.py:691`; copia `env-vllmlx` → `file_download.py:261-302,384-386,428-433,642-661,1057,1141,1176,1464,1471,1569-1576,1589,1600-1603,1700-1711`; `278-280` (percent-encoding) vale in **entrambe** | **Apache-2.0 (huggingface_hub)** — *specifica di comportamento*, non codice: nessun file Python copiato o generato da quei file. **Il difetto che questa riga corregge non era "numeri sbagliati": era non dire quale delle DUE 0.36.2 installate si era aperta.** Un numero di riga senza la copia è una scommessa, non una prova |
| semantica del lock (`flock(LOCK_EX\|LOCK_NB)`, rilascio che NON cancella il file) | **letto** in `filelock/_unix.py:76,103-108` e confermato sui `.lock` rimasti nei due cache reali di questa macchina | **MIT (filelock)** — stessa categoria: comportamento misurato, non codice. Il lock è un *file condiviso sul disco*, non una dipendenza linkata |
| `percent-encoding` dell'URL (`quote(revision, safe="")`, `quote(filename)`) | **letto** in `file_download.py:278-280` | **Apache-2.0 (huggingface_hub)** — è il template di `constants.py:69` applicato allo stesso modo; la nostra implementazione (`pct_encode`) è scritta qui |

Nessun modello, peso o file di cache è copiato dentro questo repo: la lane legge ciò che l'utente
ha già scaricato, e i test costruiscono un cache finto sotto `build/work/`.

### Le DUE `huggingface_hub` 0.36.2 di questa macchina (perché ogni riga qui sopra dice quale copia)

`~/ai/env-vllmlx` e `~/ai/env-mflux` contengono **due directory `huggingface_hub` entrambe dichiarate
0.36.2 che non sono lo stesso file** (`cmp` sui `constants.py` dice NO). È la migrazione
`requests` → `httpx` accaduta *dentro* lo stesso numero di versione, quindi **il numero non
identifica nulla**: identifica la riga solo il percorso della copia aperta.

| regola | copia `env-vllmlx` (serie `requests`) | copia `env-mflux` (serie `httpx`) | quale seguiamo |
|---|---|---|---|
| redirect relativi rifatti a mano | `_request_wrapper(follow_relative_redirects=True)` `:261-302` | `_httpx_follow_relative_redirects_with_backoff` `:1615-1617` (+ `utils/_http.py:691`) | **entrambi**: `allow_redirects=False` + Location relativo risolto da noi |
| HEAD con `allow_redirects=False` | `:1471` | (inside the helper) | idem |
| `Accept-Encoding: identity` | `:1464` | `:1612` | sì |
| GET saltata se il temp è già completo | `:384-386` | `:359-361` | sì |
| 200 a una Range ⇒ `seek(0)`+`truncate()` | `:428-433` | `:384-396` | sì |
| `_normalize_etag` = `lstrip("W/").strip('"')` | def `:642`, regola `:661` | def `:577`, regola `:596` | sì |
| `locks_dir` | `:1057` | `:1058` | sì |
| `lock_path` | `:1141` | `:1203` | sì |
| temp logico `<etag>.incomplete` | `:1176` | `:1237` | sì |
| temp **reale** unico per processo | **non esiste** | `:1908` | **NO** — vedi il costo nel README |
| partial mantenuto per il resume | **sì** `:1700-1704` | **no**, si cancella `:1944-1947` | **SÌ** (serietà vecchia) |
| commit obbligatorio dalla risposta | `:1569-1576` | `:1733-1737` | sì (più morbidi: ripieghiamo sul ref, e lo diciamo) |
| lunghezza obbligatoria | `:1589` | `:1752` | sì |
| token tolto su host diverso | `:1600-1603` | `:1760-1766` | sì |
| template URL, `REPO_ID_SEPARATOR`, `ENV_VARS_TRUE_VALUES`, offline | `constants.py:67-68,99,10,165` | `constants.py:68-69,118,11,202` | entrambi = stessa regola |

**Lezione registrata (vale anche fuori da qui):** *una citazione `file:riga` senza il percorso
completo del file non è una prova.* Con due installazioni della stessa libreria a lo stesso numero
di versione, la riga giusta letta sulla copia sbagliata sembra smentire se stessa — ed è quello che
è successo in questa sessione: prima ho "corretto" numeri giusti con altri numeri giusti, poi ho
dovuto misurare le due copie per accorgermi che non c'era nessun numero sbagliato.

## Obbligazioni che scattano con la lane MLX (non ancora presa)

| cosa | da dove | licenza | obbligo |
|---|---|---|---|
| kernel Metal `oq_a8*` (già usati da GMBasic) | `GMBasic/native/gmb_metal_oq_a8/` → a monte **oMLX** | **Apache-2.0** (`LICENSE-oMLX-Apache-2.0.txt`, `THIRD_PARTY_NOTICES.md`) | §4(a)(b)(c): copia della licenza, notice di modifica, conservazione degli attributi |
| host/engine MLX | `mlx` (Apple/ml-explore) e `mlx-c` (ml-explore) — sorgenti letti su questa macchina in `GMBasic/libs/mlx-deps/{mlx-src,mlx-c-src}` | **MIT** — **misurato sui due `LICENSE`** (2026-09-22), non assunto. La riga che stava qui diceva "Apache-2.0": falso |
| ↳ come si linkano | `libmlx.a` (31 MB) + `libmlxc.a` (1,0 MB) in `libs/mlx-deps/precompiled/darwin-arm64`; GMBasic forza `BUILD_SHARED_LIBS OFF` (`native/gmb_host_ai_mlx/CMakeLists.txt:24`) | **link STATICO**, non dinamico: l'obbligo MIT (testo integrale + copyright nel bundle) **scatta alla distribuzione**, non è opzionale "se si redistribuisce" |
| link a ggml/llama.cpp per i tipi IQ non ancora coperti (invece di riscriverli) | llama.cpp / ggml | MIT | attribuzione se si linka statico o si copia |
| motore ds4 (`ds4_infer.c`) | `/Users/gaetano/ds4/` | MIT | attribuzione |

La decisione **D3** del piano (distribuzione: binario privato vs pacchetto) determina *come*
queste obbligazioni si adempiono — non *se*. Fino a D3 questo repo resta privato e non
redistribuisce nulla.
