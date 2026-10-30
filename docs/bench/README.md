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
- `tools/spec_gate.c` (`build-release/spec_gate`) — gate per lo **spec** (n-gram/MTP/DFlash2), **generale**: il toggle è una
  coppia di opzioni di registro (`--off-opts`/`--on-opts`).
  ```
  # n-gram (default)
  build-release/spec_gate --repeat 3 --out docs/bench/<model>-copy.json <modello> "<prompt>" 96
  # MTP (MTPLX: AR vs MTP)
  build-release/spec_gate --off-opts LLM_MTP_AUTO=0 --on-opts "" --out docs/bench/mtplx-mtp.json <MTPLX> "..." 48
  ```
  Verifica parità (gate 1), throughput/speedup (gate 2) e maxrss (gate 3); exit 0 se PASS.
- `tools/moe_gate.c` (`build-release/moe_gate`) — gate per il **fix del forward batch MoE** (n=2..7): compila ed esegue
  `tq_moe_batch_repro.c` e verifica che l'append n=1..8,14 dia rc=0; emette l'artefatto JSON.
  ```
  build-release/moe_gate --out docs/bench/<model>-moe-batch.json <modello_moe>
  ```
- `tools/dflash_parity.c` — parità+speed DFlash2 vs AR (unità-first, ABI via dlopen).
- `tools/tq_moe_batch_repro.c` — riproducibile del forward batch MoE per n=1..14 (fix `n=2..7`).
- `tools/kernel_tune.c` (`build-release/kernel_tune`) — **autotuning kernel model-free**: chiama `llm_ai_mlx_core_bench` e misura
  le primitive core (`rms_norm`/`qlinear`/`sdpa`/`swiglu`) vs il path MLX diretto (`ratio`, `maxdiff`);
  emette `docs/bench/kernel-core-bench.json`. Il dispatch di `quantized_linear_cfg` è **già
  shape/quant-aware** (`llm_mlx_derive_bits` + condizioni di eleggibilità: **g32 non forzato in g64**,
  fallback sicuro); un **dispatch-table data-driven** `(mode,bits,group,M-bin,context)` + wiring nel
  motore è il passo successivo (kernel registry).

Tutti: parametri **solo da argomenti** (nessuna env).

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
