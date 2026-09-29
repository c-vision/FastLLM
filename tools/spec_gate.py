#!/usr/bin/env python3
"""spec_gate.py — Promotion Gate per il decode speculativo (metodo Gufo), generale.

Verifica, per un modello+prompt, i gate di promozione di una modifica al decode:
  1. CORRETTEZZA: output BIT-IDENTICO tra le due configurazioni (parita' greedy);
  2. WORKLOAD: throughput (tok/s) + speedup;
  3. MEMORIA: maxrss (niente regressioni oltre soglia).
Il "toggle" tra le due configurazioni e' una coppia di opzioni di registro (--off-opts/--on-opts):
  - n-gram (default):  --off-opts spec_ngram=0  --on-opts spec_ngram=3
  - MTP (MTPLX):       --off-opts LLM_MTP_AUTO=0 --on-opts ""        (default del modello = MTP on)
Emette un artefatto JSON (stdout + --out). Exit 0 se PASS (parita' + memoria), 1 altrimenti.

Uso:
  python3 tools/spec_gate.py [--bin B] [--off-opts a=b,c=d] [--on-opts ...] [--repeat R]
                             [--mem-tol 0.10] [--out f.json] <modello> "<prompt>" [max_tokens]
Nessuna env: parametri solo da argomenti.
"""
import argparse, hashlib, json, os, re, statistics, subprocess, sys

TOK = re.compile(r"([0-9.]+)\s+tok/s")
RSS = re.compile(r"(\d+)\s+maximum resident set size")

def parse_opts(s):
    return [x for x in (p.strip() for p in (s or "").split(",")) if x]

def run(bin, model, prompt, n, opts):
    cmd = [bin, "run", "--opt", "temp=0"] + [v for o in opts for v in ("--opt", o)] + \
          [model, prompt, "--max-tokens", str(n)]
    if os.path.exists("/usr/bin/time"):
        cmd = ["/usr/bin/time", "-l"] + cmd
    p = subprocess.run(cmd, capture_output=True, text=True)
    err = p.stderr or ""
    m = TOK.search(err); r = RSS.search(err)
    return p.stdout, (float(m.group(1)) if m else None), (int(r.group(1)) if r else None)

def med(xs):
    v = [x for x in xs if x is not None]
    return statistics.median(v) if v else None

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(here, "..", "build-release", "fastllm"))
    ap.add_argument("--off-opts", default="spec_ngram=0")
    ap.add_argument("--on-opts", default="spec_ngram=3")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--mem-tol", type=float, default=0.10)
    ap.add_argument("--out", default=None)
    ap.add_argument("model"); ap.add_argument("prompt")
    ap.add_argument("max_tokens", nargs="?", type=int, default=96)
    a = ap.parse_args()
    off, on = parse_opts(a.off_opts), parse_opts(a.on_opts)

    o0, t0, r0, o1, t1, r1 = [], [], [], [], [], []
    for _ in range(a.repeat):
        o, t, r = run(a.bin, a.model, a.prompt, a.max_tokens, off); o0.append(o); t0.append(t); r0.append(r)
        o, t, r = run(a.bin, a.model, a.prompt, a.max_tokens, on);  o1.append(o); t1.append(t); r1.append(r)

    base = o0[0]
    parity = all(x == base for x in o0) and all(x == base for x in o1)
    b, s = med(t0), med(t1)
    m0, m1 = med(r0), med(r1)
    mem_delta = (round((m1 - m0) / m0, 4) if m0 and m1 else None)
    mem_ok = (mem_delta is None) or (mem_delta <= a.mem_tol)

    art = {
        "model": os.path.basename(os.path.normpath(a.model)),
        "prompt_sha256": hashlib.sha256(a.prompt.encode()).hexdigest(),
        "max_tokens": a.max_tokens, "repeat": a.repeat,
        "off_opts": a.off_opts, "on_opts": a.on_opts,
        "off_tok_s": b, "on_tok_s": s,
        "speedup": (round(s / b, 3) if b and s else None),
        "off_maxrss_mb": (round(m0 / 1048576, 1) if m0 else None),
        "on_maxrss_mb": (round(m1 / 1048576, 1) if m1 else None),
        "mem_delta_pct": (round(mem_delta * 100, 2) if mem_delta is not None else None),
        "parity_bit_identical": parity,
        "output_sha256": hashlib.sha256(base.encode()).hexdigest(),
        "gate": "PASS" if (parity and mem_ok) else "FAIL",
    }
    txt = json.dumps(art, indent=1)
    print(txt)
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        open(a.out, "w").write(txt + "\n")
    return 0 if (parity and mem_ok) else 1

if __name__ == "__main__":
    sys.exit(main())
