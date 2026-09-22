# Licenze e attribuzioni

Aggiornato: 2026-09-22. Questo file elenca **cosa è stato usato** e **cosa si userà**, tenendo
separate le due cose: un'attribuzione per codice non ancora preso è falsa tanto quanto un'ombra.

## Oggi (solo lane_gguf)

**Nessun file sorgente di terze parti è incluso in questo repo.** Il lettore GGUF
(`src/lane_gguf/fmlx_gguf.c`) è scritto da zero qui. Ciò che è stato *letto* per scrivere i
numeri della tabella dei tipi, con la fonte citata nel codice e nel test di parità:

| cosa | da dove | licenza |
|---|---|---|
| valori di enum e dimensioni dei blocchi (42 tipi) | `/Users/gaetano/ds4/gguf-tools/quants.h:20-54` e `quants.c:39-74` (`ds4q_type_traits`) — **verificati poi contro l'autorità**: `llamacpp/ggml/include/ggml.h:389-433`. La prima trascrizione aveva spostato cinque tipi di uno comprimendo i buchi dell'enum: `docs/quant-coverage.md` §5 | MIT — `/Users/gaetano/ds4/LICENSE`, "The ds4.c authors", "The ggml authors", "DeepSeek" |
| **testimone C del gate**: `dequantize_row_*` (25) e `ggml_row_size`, letti E LINKATI a solo tempo-di-sviluppo dalla libreria già costruita su questa macchina (`llamacpp/build/bin/libggml-base.0.15.3.dylib`) | llama.cpp / ggml | **MIT** | **nessun codice copiato, nulla distribuito**: l'obbligo scatta solo se la lane engine linkerà ggml nel prodotto (riga sotto) |
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
