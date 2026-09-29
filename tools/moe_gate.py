#!/usr/bin/env python3
"""moe_gate.py — Promotion Gate per il fix del forward batch MoE (n=2..7).

Compila ed esegue `tq_moe_batch_repro.c` (LLMEnv) contro un modello MoE: verifica che l'append
(prefill) con n=1..8,14 ritorni rc=0 (prima del fix `df73a23`/`fe3b190` falliva per n=2..7).
Emette un artefatto JSON. Exit 0 se tutti rc=0.

Uso:
  python3 tools/moe_gate.py [--dylib D] [--repro-src S] [--ids "1,2"] [--out f.json] <modello>
Default dylib: ../build-release/llmengine_mlx/libllm_ai_mlx.dylib
Default repro: ../../LLMEnv/tools/tq_moe_batch_repro.c
Nessuna env: parametri solo da argomenti.
"""
import argparse, json, os, re, subprocess, sys, tempfile

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dylib", default=os.path.join(here, "..", "build-release", "llmengine_mlx",
                                                    "libllm_ai_mlx.dylib"))
    ap.add_argument("--repro-src", default=os.path.join(here, "..", "..", "LLMEnv", "tools",
                                                        "tq_moe_batch_repro.c"))
    ap.add_argument("--ids", default="100,200")
    ap.add_argument("--out", default=None)
    ap.add_argument("model")
    a = ap.parse_args()

    exe = os.path.join(tempfile.gettempdir(), "moe_gate_repro")
    cc = subprocess.run(["clang", "-O2", os.path.abspath(a.repro_src), "-o", exe],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        print(json.dumps({"error": "compile failed", "stderr": cc.stderr[-400:]})); return 2

    # cwd = dir superiore alla dylib (build-release, dove sta mlx.metallib colocated per l'engine)
    cwd = os.path.dirname(os.path.dirname(os.path.abspath(a.dylib)))
    p = subprocess.run([exe, os.path.abspath(a.dylib), a.model, a.ids],
                       capture_output=True, text=True, cwd=cwd)
    rc_rows = [(int(n), int(rc)) for n, rc in
               re.findall(r"append n=(\d+)\s+rc=(-?\d+)", p.stdout or "")]
    fails = [n for n, rc in rc_rows if rc != 0]
    art = {
        "model": os.path.basename(os.path.normpath(a.model)),
        "fix": "MoE forward batch n=2..7 (qwen3_5 df73a23 / qwen3_moe fe3b190)",
        "repro": os.path.basename(a.repro_src),
        "n_tested": [n for n, _ in rc_rows],
        "rc": {str(n): rc for n, rc in rc_rows},
        "failures": len(fails),
        "gate": "PASS" if (rc_rows and not fails) else "FAIL",
    }
    txt = json.dumps(art, indent=1)
    print(txt)
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        open(a.out, "w").write(txt + "\n")
    return 0 if (rc_rows and not fails) else 1

if __name__ == "__main__":
    sys.exit(main())
