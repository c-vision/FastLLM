# Contratto JSON di FastLLM — dove divergiamo da CPython, con la misura accanto

> Scritto perché `include/fmlx/fmlx_json.h` lo prometteva. Ogni numero qui sotto è stato
> **misurato** il 2026-09-22 su questo repo (`tests/json/corpus.hex`, 122 record), non ricordato.

## Come si riproduce

```
python3 tools/json_oracle.py hex  tests/json/corpus.spec tests/json/corpus.hex   # rigenera il corpus
python3 tools/json_oracle.py diff tests/json/corpus.hex  build/json_diff         # il differenziale
ctest --test-dir build -R json                                                    # incluso il gate json_oracle
```

`ctest` fissa il verdetto atteso con `PASS_REGULAR_EXPRESSION`: una divergenza dura in più, o un
round-trip che si rompe, **rendono il test rosso**, non "una nota in più". È il motivo per cui il
contatore è nell'output e non solo in questo file.

Stato misurato oggi: **122 record, 8 divergenze dure, 0 round-trip, 14 morbide** — più
`test_json` 327 verifiche / 0 fallimenti e `test_jsonw` 148 verifiche / 0 fallimenti.

## I tre livelli del confronto

| Livello | Cosa confronta | Esito |
|---|---|---|
| **DURO** | accettato/rifiutato e **valori** | fallimento |
| **ROUND-TRIP** | il nostro writer riletto dal nostro parser deve eguagliare noi stessi | fallimento |
| **MORBIDO** | **quale** codice di errore quando entrambi rifiutano; indice del membro vincente su chiavi duplicate | riportato, non contato |

Il confronto è su una **codifica canonica** (float per bit IEEE-754, stringhe per `len + hex` UTF-8),
mai su testo formattato: confrontare il testo di `%.17g` avrebbe messo il risultato nella domanda.
I record si passano in **hex**, uno per riga — i record che definiscono il contratto sono proprio
quelli ambigui nei byte (control byte nudi, NUL, `\r\n`, solo-whitespace), e un separatore testuale
avrebbe spostato il bug nel driver.

## Divergenze dure: 8, tutte volute

| Documento | Noi | CPython | Perché stiamo zitti sulla risposta giusta |
|---|---|---|---|
| `1e309` | `NUMBER` | `+Inf` | JSON **non ha** Infinity: chi lo accetta produce un valore che non può rimandare |
| `-1e309` | `NUMBER` | `-Inf` | idem |
| `9223372036854775808` | `NUMBER` | int arbitrario | il DOM ha `int64_t`: un INT che non c'entra sarebbe una menzogna sul tipo |
| `-9223372036854775809` | `NUMBER` | int arbitrario | idem |
| `"\ud800"` | `UTF8` | `S3 eda080` | surrogato isolato: non è un codice Unicode, e lo codificherebbe in CESU-8 |
| `"\udc00"` | `UTF8` | `S3 edb080` | idem |
| `"\ud800A"` | `UTF8` | `S4 eda08041` | idem |
| `"\ud800\ud800"` | `UTF8` | `S6 eda080eda080` | idem |

Rifiutare gli interi oltre `int64` **senza** convertirli in double è la scelta che evita il caso
peggiore: `9223372036854775808` come double perde l'unità, e un ID che cambia valore passando dal
parser è il bug che non si vede nei log.

**Sottotrappola misurata mentre si scriveva questa tabella:** `ERANGE` copre overflow **e**
denormalizzazione. Il parser rifiutava entrambi, cioè rifiutava `5e-324`, `1e-320` e
`2.2250738585072009e-308` — che sono double **esatti** e che Python accetta. Ora si rifiuta solo
`ERANGE` con risultato non finito (`isfinite`), e i tre subnormali concordano. `1e-999` dà 0.0 da
entrambe le parti: **non** è una divergenza, ed era il caso che l'errno da solo non sapeva dire.

## Morbide: 14, per due ragioni diverse

**Chi vince su chiave duplicata (4 record).** Noi il **PRIMO** (`G1 61 0`), Python il **ULTIMO**
(`G1 61 1`) — misurato su `{"a":1,"a":2}`, `{"max_tokens":8,"max_tokens":4096}`,
`{"a":1,"b":{"a":2},"a":3}`, `{"a":{"x":1},"a":{"y":2}}`. `had_dup_keys` è segnalato nel documento e
`fmlx_json_obj_get` è deterministico. Non è un "miglioramento": è una scelta, e il round-trip la
mantiene (`G1 61 0` ricompare identico dopo render→riparse).

**Quale codice di errore (10 record).** Stesso rifiuto, classificazione diversa: `01`, `00`, `5.`,
`1.`, `1e`, `1e+`, `-`, `--1` (noi `NUMBER`, Python `TRAILING`/`SYNTAX` perché il suo scanner
consuma il numero e inciampa dopo) e `NaN`, `Infinity` (noi `SYNTAX`). **Nota di onestà:** per
`NaN`/`Infinity` il verdetto "oracolo" non è di CPython — che li accetta — ma del filtro che ho
messo nel mio lato Python. Un codice d'errore non è un valore: contarlo come divergenza dura
avrebbe significato classificare a mano i messaggi di Python, cioè authorare il risultato.

## Cosa il differenziale ha trovato e le 475 asserzioni di casa no

Tre bug, tutti in percorsi che i test scritti a mano non toccavano:

1. **`kids` dei contenitori annidati corrotto** — `[[1],[2]]` emetteva `KEY_INVALORE`: l'indice del
   contenitore si perdeva su due rami di uscita. Fix: scratch per livello e **un solo**
   `push_node` per contenitore.
2. **`-0.0` round-trippava come INT** `I0` — il tipo del valore cambiava passando dal writer. Fix:
   se la forma più corta non ha né punto né esponente, riemettere con `%.1f`.
3. **`raw` rifiutato non avvelenava il writer** — il membro *successivo* prendeva il posto di
   quello rifiutato. Fix: `w->err = e`.
4. **Cap di profondità diviso in due numeri** — il parser a 64, il writer a 32 (`sizeof
   need_comma`): un documento **legale** da 33–64 livelli falliva il proprio round-trip con
   `TOO_DEEP`. Ora i tre array del writer sono lunghi `FMLX_JSON_MAX_DEPTH`, e `test_jsonw` lo
   deriva dalla costante invece di scrivere "40" (che era un 32 travestito).

Il quarto ha anche una **lezione sullo strumento**: per come lo avevo scritto, il "65 livelli
accettati dal parser" che sembrava un bug gravissimo **non esisteva** — era una riga vuota a metà
corpus che il tool contava come record e l'oracolo saltava, quindi ogni verdetto era attribuito al
vicino. Prima di correggere il parser, la curva di profondità misurata a mano diceva già 64 sì / 65
no. Un differenziale che sposta gli indici è peggio di nessun differenziale: finge misure.

## Cosa NON è coperto

- `ORACLE_RECURSION`: se il lato Python esplode, il record è marcato come **non confrontato**, mai
  come "d'accordo".
- Confronto con **altri** parser (RapidJSON, simdjson, `json-parser`): niente. Il contratto è con
  CPython `json.loads`, che è lo standard di fatto per ciò che i client mandano davvero.
- Streaming/incrementale: il parser è tutto-in-una-volta. Un SSE rotto a metà è lavoro della corsia
  HTTP, non di questa.
