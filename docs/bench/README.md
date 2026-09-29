# Benchmark & Promotion Gates — FastLLM

Metodo di **promozione delle modifiche al decode** (adattato da `gufo-org/gufo`,
`docs/PERFORMANCE.md`): una modifica si **tiene** solo se passa i gate, con **evidenza
versionata** (artefatti JSON), non con un numero isolato.

## I gate
1. **Correttezza** — l'output deve restare **bit-identico** al decode di riferimento (greedy senza
   spec): `output_sha256` uguale con/senza la modifica.
2. **Workload** — il throughput (tok/s) sul carico dichiarato deve **migliorare fuori dal rumore**
   (`--repeat` per la mediana; A/B alternati).
3. **Memoria/tail** — nessuna regressione oltre la soglia (`--mem-tol`, default +10%).
4. **Stato** — rollback/cancellazione/cleanup corretti (test dedicati dove esiste stato, es.
   `tools/tq_moe_batch_repro.c`, `tools/dflash_parity.c`).

## Tool
- `tools/spec_gate.py` — gate per lo **spec self-speculativo**:
  ```
  python3 tools/spec_gate.py --repeat 3 --order 3 \
      --out docs/bench/<model>-<case>.json <modello> "<prompt>" [max_tokens]
  ```
  Verifica parità (gate 1), throughput/speedup (gate 2) e maxrss (gate 3); emette l'artefatto JSON
  e **exit 0 se PASS**. Parametri **solo da argomenti** (nessuna env).
- `tools/dflash_parity.c` — parità+speed DFlash2 vs AR (unità-first, ABI via dlopen).
- `tools/tq_moe_batch_repro.c` — riproducibile del forward batch MoE per n=1..14 (fix `n=2..7`).

## Artefatti
- Un file per modello+caso in `docs/bench/`, **versionato** (evidenza del gate al momento della
  promozione). Campi principali: `model`, `prompt_sha256`, `spec_off_tok_s`, `spec_on_tok_s`,
  `speedup`, `spec_{off,on}_maxrss_mb`, `mem_delta_pct`, `parity_bit_identical`, `output_sha256`,
  `gate`.
- Esempio (Llama-1B, copia): `docs/bench/llama1b-copy.json` → speedup 1.63×, memoria +0%, **PASS**.

## Trabocchetti misurati da riusare (da Gufo)
- **"Size the working set like the model does"**: un solo shape in loop resta in cache e riporta
  banda falsata; per il **decode** contano i **byte letti**, non il picco GEMM.
- `__builtin_nontemporal_load` **danneggia** lo streaming (verificato su AMD; vale come sospetto).
- Misure A/B **nello stesso run** e **mediane**, non il campione migliore; macchina condivisa ⇒
  attenzione alla contesa (se la varianza supera l'effetto: "non misurabile ora").
