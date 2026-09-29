#!/usr/bin/env python3
"""kernel_tune.py — harness di autotuning dei kernel (model-free, metodo Gufo #6).

Chiama l'API del motore `llm_ai_mlx_core_bench` (nessun modello caricato) per misurare le
primitive core (rms_norm / qlinear / sdpa) e le confronta col path MLX diretto; emette un
artefatto JSON (baseline di tuning, hardware-specific) da usare per le decisioni di dispatch.

Nota: il dispatch di `quantized_linear_cfg` è GIÀ shape/quant-aware (`llm_mlx_derive_bits` +
condizioni di eleggibilità: niente forzatura g32->g64, fallback sicuro). Un dispatch-TABLE
data-driven (chiave `(mode,bits,group,M-bin,context)`) + wiring nel motore è il passo successivo.

Uso:
  python3 tools/kernel_tune.py [--dylib D] [--op all|rms_norm|qlinear|sdpa] [--iters N] [--out f.json]
Default dylib: ../build-release/llmengine_mlx/libllm_ai_mlx.dylib. Nessuna env.
"""
import argparse, ctypes, json, os, sys

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dylib", default=os.path.join(here, "..", "build-release", "llmengine_mlx",
                                                    "libllm_ai_mlx.dylib"))
    ap.add_argument("--op", default="all")
    ap.add_argument("--iters", type=int, default=200)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    dylib = os.path.abspath(a.dylib)
    out_abs = os.path.abspath(a.out) if a.out else None
    # cwd = build-release (mlx.metallib colocated per il runtime MLX)
    try:
        os.chdir(os.path.dirname(os.path.dirname(dylib)))
    except OSError:
        pass
    lib = ctypes.CDLL(dylib)
    f = lib.llm_ai_mlx_core_bench
    f.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
    f.restype = ctypes.c_int
    buf = ctypes.create_string_buffer(1 << 16)
    n = f(a.op.encode(), a.iters, buf, len(buf))
    tsv = buf.value.decode(errors="replace")

    rows = []
    for line in tsv.splitlines():
        if not line.startswith("OP "):
            continue
        d = {"line": line.strip()}
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1); d[k] = v
        rows.append(d)
    art = {"op": a.op, "iters": a.iters, "n_ops": n,
           "dylib": os.path.basename(dylib), "rows": rows}
    txt = json.dumps(art, indent=1)
    print(txt)
    if out_abs:
        os.makedirs(os.path.dirname(out_abs), exist_ok=True)
        open(out_abs, "w").write(txt + "\n")
    return 0 if n > 0 else 1

if __name__ == "__main__":
    sys.exit(main())
