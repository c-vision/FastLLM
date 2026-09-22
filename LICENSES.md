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
| contratto del layout del cache (catena `HF_HUB_CACHE`/`HF_HOME`/`XDG_CACHE_HOME`, `REPO_ID_SEPARATOR = "--"`, template URL, `{1,ON,YES,TRUE}`, `.no_exist`) | **letto** in `huggingface_hub/constants.py:10,67-68,99,129-155,165` (venv locale `~/ai/env-vllmlx`) e confermato su due cache reali su disco | **Apache-2.0 (huggingface_hub)** — qui è *specifica di comportamento*, non codice: nessun file Python copiato o generato. Se in futuro si linka o si trasporta codice di huggingface_hub, la licenza va venduta e dichiarata |
| layout dei nomi-blob (40 hex = git blob id, 64 hex = SHA256) | **misurato su questa macchina** con `git hash-object` e `shasum -a 256` sui blob reali (6/6 e 2/2) — non preso da un sorgente | nessuna (proprietà del formato git/LFS) |
| SHA1/SHA256 del verificatore | `CommonCrypto` (`CC_SHA1`/`CC_SHA256`), header nell'SDK di sistema (`MacOSX.sdk/usr/include/CommonCrypto/CommonDigest.h`) | framework Apple, parte del SO: nessuna attribuzione da vendere, nessun file da includere |
| libcurl, il trasporto del download | `MacOSX.sdk/usr/include/curl/curl.h` + `-lcurl` (probe compilato: `libcurl/8.7.1 SecureTransport`) | **curl license** (MIT-consentente): link dinamico a libreria di sistema, nulla da vendere. **Nota: questa misura smentisce ciò che avevo scritto prima** — le intestazioni di curl *ci sono*; la decisione di trasporto è cambiata di conseguenza (README, "Trasporto del download") |
| contratto del download (HEAD + `Accept-Encoding: identity`, redirect relativo rifatto a mano, `url_to_download = metadata.location`, token tolto su host terzo, etag **e** lunghezza obbligatori, GET saltata sul temporaneo completo, 200-a-Range che azzera, `.incomplete` determinato, ref scritto dopo) | **letto** in `huggingface_hub/file_download.py:282-302,391,642-660,1176,1470,1478-1489,1567,1569-1576,1588,1601-1603` + `constants.py:68,74-76` | **Apache-2.0 (huggingface_hub)** — *specifica di comportamento*, non codice: nessun file Python copiato o generato da quei file |
| semantica del lock (`flock(LOCK_EX\|LOCK_NB)`, rilascio che NON cancella il file) | **letto** in `filelock/_unix.py:76,103-108` e confermato sui `.lock` rimasti nei due cache reali di questa macchina | **MIT (filelock)** — stessa categoria: comportamento misurato, non codice. Il lock è un *file condiviso sul disco*, non una dipendenza linkata |
| `percent-encoding` dell'URL (`quote(revision, safe="")`, `quote(filename)`) | **letto** in `file_download.py:278-280` | **Apache-2.0 (huggingface_hub)** — è il template di `constants.py:68` applicato allo stesso modo; la nostra implementazione (`pct_encode`) è scritta qui |

Nessun modello, peso o file di cache è copiato dentro questo repo: la lane legge ciò che l'utente
ha già scaricato, e i test costruiscono un cache finto sotto `build/work/`.

## Obbligazioni che scattano con la lane MLX (non ancora presa)

| cosa | da dove | licenza | obbligo |
|---|---|---|---|
| kernel Metal `oq_a8*` (già usati da GMBasic) | `GMBasic/native/gmb_metal_oq_a8/` → a monte **oMLX** | **Apache-2.0** (`LICENSE-oMLX-Apache-2.0.txt`, `THIRD_PARTY_NOTICES.md`) | §4(a)(b)(c): copia della licenza, notice di modifica, conservazione degli attributi |
| host/engine MLX | `mlx` e `mlx-c` (Apple) | Apache-2.0 | link dinamico a runtime; notice se si redistribuisce |
| link a ggml/llama.cpp per i tipi IQ non ancora coperti (invece di riscriverli) | llama.cpp / ggml | MIT | attribuzione se si linka statico o si copia |
| motore ds4 (`ds4_infer.c`) | `/Users/gaetano/ds4/` | MIT | attribuzione |

La decisione **D3** del piano (distribuzione: binario privato vs pacchetto) determina *come*
queste obbligazioni si adempiono — non *se*. Fino a D3 questo repo resta privato e non
redistribuisce nulla.
