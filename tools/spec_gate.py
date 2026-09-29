#!/usr/bin/env python3
"""spec_gate.py — Promotion Gate per lo spec self-speculativo (metodo Gufo).

Verifica, per un modello+prompt, i gate di promozione di una modifica al decode speculativo:
  1. CORRETTEZZA: output BIT-IDENTICO col decode senza spec (parita' greedy);
  2. WORKLOAD: throughput (tok/s) con/senza spec, e speedup.
Emette un artefatto JSON (machine-readable) su stdout. Exit 0 se i gate passano, 1 altrimenti.

Uso:
  python3 tools/spec_gate.py [--bin <fastllm>] [--order N] [--repeat R] <modello> "<prompt>" [max_tokens]

Default: --bin ../build-release/fastllm, --order 3, --repeat 1, max_tokens 96.
Nessuna env: parametri solo da argomenti.
"""
import argparse, hashlib, json, os, re, statistics, subprocess, sys, time

TOK = re.compile(r"([0-9.]+)\s+tok/s")

def run(bin, model, prompt, n, spec):
    cmd = [bin, "run", "--opt", "temp=0", "--opt", f"spec_ngram={spec}", model, prompt,
           "--max-tokens", str(n)]
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.time() - t0
    m = TOK.search(p.stderr or "")
    return p.stdout, (float(m.group(1)) if m else None), dt

def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--bin", default=os.path.join(here, "..", "build-release", "fastllm"))
    ap.add_argument("--order", type=int, default=3)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("model"); ap.add_argument("prompt")
    ap.add_argument("max_tokens", nargs="?", type=int, default=96)
    a = ap.parse_args()

    a0, t0s, t0w = [], [], []
    a1, t1s, t1w = [], [], []
    for _ in range(a.repeat):
        o, t, w = run(a.bin, a.model, a.prompt, a.max_tokens, 0); a0.append(o); t0s.append(t); t0w.append(w)
        o, t, w = run(a.bin, a.model, a.prompt, a.max_tokens, a.order); a1.append(o); t1s.append(t); t1w.append(w)
    base, spec = a0[0], a1[0]
    parity = all(x == base for x in a0) and all(x == base for x in a1)
    med = lambda xs: statistics.median([x for x in xs if x is not None]) if any(x is not None for x in xs) else None
    b, s = med(t0s), med(t1s)
    art = {
        "model": os.path.basename(os.path.normpath(a.model)),
        "prompt_sha256": hashlib.sha256(a.prompt.encode()).hexdigest(),
        "max_tokens": a.max_tokens, "spec_order": a.order, "repeat": a.repeat,
        "spec_off_tok_s": b, "spec_on_tok_s": s,
        "speedup": (round(s / b, 3) if b and s else None),
        "parity_bit_identical": parity,
        "output_sha256": hashlib.sha256(base.encode()).hexdigest(),
        "gate": "PASS" if parity else "FAIL",
    }
    print(json.dumps(art, indent=1))
    return 0 if parity else 1

if __name__ == "__main__":
    sys.exit(main())
