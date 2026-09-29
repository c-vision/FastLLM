#!/usr/bin/env python3
"""spec_gate.py — Promotion Gate per lo spec self-speculativo (metodo Gufo).

Verifica, per un modello+prompt, i gate di promozione di una modifica al decode speculativo:
  1. CORRETTEZZA: output BIT-IDENTICO col decode senza spec (parita' greedy);
  2. WORKLOAD: throughput (tok/s) con/senza spec + speedup;
  3. MEMORIA: maxrss con/senza spec (niente regressioni oltre la soglia).
Emette un artefatto JSON (machine-readable) su stdout, e opzionalmente su file (--out).
Exit 0 se i gate passano (parita' + memoria entro soglia), 1 altrimenti.

Uso:
  python3 tools/spec_gate.py [--bin <fastllm>] [--order N] [--repeat R] [--mem-tol 0.10]
                             [--out <file.json>] <modello> "<prompt>" [max_tokens]

Default: --bin ../build-release/fastllm, --order 3, --repeat 1, --mem-tol 0.10, max_tokens 96.
Nessuna env: parametri solo da argomenti.
"""
import argparse, hashlib, json, os, re, statistics, subprocess, sys, time

TOK = re.compile(r"([0-9.]+)\s+tok/s")
RSS = re.compile(r"(\d+)\s+maximum resident set size")

def run(bin, model, prompt, n, spec):
    """Esegue una generazione greedy. Ritorna (stdout, tok_s, maxrss_bytes)."""
    cmd = [bin, "run", "--opt", "temp=0", "--opt", f"spec_ngram={spec}", model, prompt,
           "--max-tokens", str(n)]
    t = "/usr/bin/time"
    if os.path.exists(t):
        cmd = [t, "-l"] + cmd
    p = subprocess.run(cmd, capture_output=True, text=True)
    err = p.stderr or ""
    m = TOK.search(err)
    r = RSS.search(err)
    return p.stdout, (float(m.group(1)) if m else None), (int(r.group(1)) if r else None)

def med(xs):
    v = [x for x in xs if x is not None]
    return statistics.median(v) if v else None

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(here, "..", "build-release", "fastllm"))
    ap.add_argument("--order", type=int, default=3)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--mem-tol", type=float, default=0.10)  # regressione maxrss ammessa (frazione)
    ap.add_argument("--out", default=None)
    ap.add_argument("model"); ap.add_argument("prompt")
    ap.add_argument("max_tokens", nargs="?", type=int, default=96)
    a = ap.parse_args()

    out0, tok0, rss0 = [], [], []
    out1, tok1, rss1 = [], [], []
    for _ in range(a.repeat):
        o, t, r = run(a.bin, a.model, a.prompt, a.max_tokens, 0)
        out0.append(o); tok0.append(t); rss0.append(r)
        o, t, r = run(a.bin, a.model, a.prompt, a.max_tokens, a.order)
        out1.append(o); tok1.append(t); rss1.append(r)

    base = out0[0]
    parity = all(x == base for x in out0) and all(x == base for x in out1)
    b, s = med(tok0), med(tok1)
    m0, m1 = med(rss0), med(rss1)
    mem_delta = (round((m1 - m0) / m0, 4) if m0 and m1 else None)
    mem_ok = (mem_delta is None) or (mem_delta <= a.mem_tol)

    art = {
        "model": os.path.basename(os.path.normpath(a.model)),
        "prompt_sha256": hashlib.sha256(a.prompt.encode()).hexdigest(),
        "max_tokens": a.max_tokens, "spec_order": a.order, "repeat": a.repeat,
        "spec_off_tok_s": b, "spec_on_tok_s": s,
        "speedup": (round(s / b, 3) if b and s else None),
        "spec_off_maxrss_mb": (round(m0 / 1048576, 1) if m0 else None),
        "spec_on_maxrss_mb": (round(m1 / 1048576, 1) if m1 else None),
        "mem_delta_pct": (round(mem_delta * 100, 2) if mem_delta is not None else None),
        "parity_bit_identical": parity,
        "output_sha256": hashlib.sha256(base.encode()).hexdigest(),
        "gate": "PASS" if (parity and mem_ok) else "FAIL",
    }
    txt = json.dumps(art, indent=1)
    print(txt)
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, "w") as f:
            f.write(txt + "\n")
    return 0 if (parity and mem_ok) else 1

if __name__ == "__main__":
    sys.exit(main())
