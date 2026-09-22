#!/usr/bin/env python3
"""gen_codebooks.py — genera (e verifica) le tabelle codebook ggml di FastMLX.

Perché uno script e non una trascrizione a mano: una voce sbagliata su 256 cambia il peso
di UN elemento e non si vede — è esattamente il difetto che i test di questa lane esistono
per evitare. Qui ogni valore viene da ALMENO DUE copie indipendenti già su disco che
dicono la STESSA cosa; se divergono, lo script rifiuta di scrivere e nomina i file.

Le copie note (tutte locali, nessun download):
  * native/gmb_host_ai_mlx/gmb_iq2_tables.h  (duplicato nel vault GMBasic, lane MLX)
  * native/gmb_moe_runtime/src/gmb_moe_iq2_tables.h  (gemello, lane MoE)
  * <ds4>/ds4.c  (sorgente da cui i primi due dichiarano di essere generati)

Uso:
    python3 tools/gen_codebooks.py --check [--ref GMBasic=<path>] [--ref ds4=<path>]
    python3 tools/gen_codebooks.py --write src/lane_gguf/fmlx_iq2_tables.h
"""
import argparse
import os
import re
import sys

# nome canonico FastMLX -> [(etichetta sorgente, nome nella sorgente), ...]
TABLES = {
    "iq2xxs_grid":   {"ctype": "uint64_t", "src": [("gmb_mlx", "gmb_iq2_grid"),
                                                    ("gmb_moe", "gmb_iq2_grid"),
                                                    ("ds4",     "iq2xxs_grid")]},
    "iq2_ksigns":    {"ctype": "uint8_t",  "src": [("gmb_mlx", "gmb_iq2_ksigns"),
                                                    ("gmb_moe", "gmb_iq2_ksigns"),
                                                    ("ds4",     "ksigns_iq2xs")]},
}

DEF_RELS = {
    "gmb_mlx": "native/gmb_host_ai_mlx/gmb_iq2_tables.h",
    "gmb_moe": "native/gmb_moe_runtime/src/gmb_moe_iq2_tables.h",
    "ds4":     "ds4.c",
}
# Le etichette NON di questo progetto vivono accanto, su questa macchina: i default sono
# percorsi candidati, non dipendenze hard (si possono sempre scavalcare con --ref).
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIBLINGS = [os.path.join(os.path.dirname(REPO_ROOT), "GMBasic"),
            os.path.join(os.path.dirname(REPO_ROOT), "ds4"),
            os.path.expanduser("~/ds4")]


def default_ref(label):
    rel = DEF_RELS[label]
    for base in [os.getcwd(), REPO_ROOT] + SIBLINGS:
        p = os.path.join(base, rel)
        if os.path.isfile(p):
            return p
    env = {"gmb_mlx": "GMBASIC_REPO", "gmb_moe": "GMBASIC_REPO", "ds4": "DS4_REPO"}[label]
    root = os.environ.get(env)
    if root:
        p = os.path.join(os.path.expanduser(root), rel)
        if os.path.isfile(p):
            return p
    return None

ARRAY_RE = r"static const\s+\w+\s+%s\[(\d+)\]\s*=\s*\{(.*?)\n\};"


def parse_table(path, name):
    """Ritorna (dichiarati, valori) oppure None se il nome non c'è."""
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(ARRAY_RE % re.escape(name), text, re.S)
    if not m:
        return None
    decl = int(m.group(1))
    vals = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", m.group(2))]
    return decl, vals


def collect(refs):
    """-> {nome_canonico: {etichetta: (decl, vals)}} e una lista di problemi."""
    out, problems = {}, []
    for canon, spec in TABLES.items():
        out[canon] = {}
        for label, srcname in spec["src"]:
            if label not in refs:
                problems.append("%-14s: sorgente %s non indicata (--ref %s=<file>)"
                                % (canon, label, label))
                continue
            got = parse_table(refs[label], srcname)
            if got is None:
                problems.append("%-14s: \"%s\" non trovata in %s" % (canon, srcname, refs[label]))
                continue
            decl, vals = got
            if decl != len(vals):
                problems.append("%-14s/%s: dichiarati %d, parsed %d in %s"
                                % (canon, label, decl, len(vals), refs[label]))
                continue
            if len(vals) < 8:
                problems.append("%-14s/%s: %d valori, troppo pochi per essere reale"
                                % (canon, label, len(vals)))
                continue
            out[canon][label] = got
    return out, problems


def cross_check(collected):
    """Le copie devono coincidere: numero di voci E valori, voce per voce."""
    bad = []
    for canon, copies in collected.items():
        items = sorted(copies.items())
        if len(items) < 2:
            bad.append("%s: una sola copia (%s) — non è un dato verificabile, serve di più"
                       % (canon, items[0][0] if items else "nessuna"))
            continue
        base_label, (base_n, base_v) = items[0]
        for label, (n, v) in items[1:]:
            if n != base_n:
                bad.append("%s: %s ha %d voci, %s ne ha %d" % (canon, label, n, base_label, base_n))
                continue
            diff = [i for i in range(base_n) if v[i] != base_v[i]]
            if diff:
                i = diff[0]
                bad.append("%s: %s e %s divergono su %d/%d voci (prima: [%d] 0x%x vs 0x%x)"
                           % (canon, base_label, label, len(diff), base_n, i, base_v[i], v[i]))
    return bad


def fmt_rows(vals, ctype, per_line):
    width = 18 if ctype == "uint64_t" else 5
    lines, row = [], []
    for i, v in enumerate(vals):
        s = ("0x%016x" % v) if ctype == "uint64_t" else ("%3u" % v)
        row.append(s.rjust(width) + ("," if i + 1 < len(vals) else ""))
        if len(row) == per_line:
            lines.append("    " + " ".join(row))
            row = []
    if row:
        lines.append("    " + " ".join(row))
    return "\n".join(lines)


def emit(canon, spec, vals):
    per = 4 if spec["ctype"] == "uint64_t" else 8
    return ("static const %s fmlx_%s[%d] = {\n%s\n};\n"
            % (spec["ctype"], canon, len(vals), fmt_rows(vals, spec["ctype"], per)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="confronta le copie, non scrive")
    ap.add_argument("--write", metavar="FILE", help="scrive l'header generato")
    ap.add_argument("--ref", action="append", default=[], metavar="ETICHETTA=FILE",
                    help="percorso di una sorgente nota (ripetibile)")
    a = ap.parse_args()
    if not a.check and not a.write:
        ap.error("indicare --check e/o --write")

    refs = {}
    for r in a.ref:
        if "=" not in r:
            ap.error("--ref vuole ETICHETTA=FILE")
        k, v = r.split("=", 1)
        refs[k] = os.path.expanduser(v)
    for k in DEF_RELS:
        if k not in refs:
            p = default_ref(k)
            if p:
                refs[k] = p
    missing = [k for k, spec in TABLES.items()
               for lbl, _ in spec["src"] if lbl not in refs]
    if missing:
        print("mancano le sorgenti: %s" % ", ".join(sorted(set(missing))))
        print("passare --ref ETICHETTA=FILE (i default cercano GMBasic/ds4 accanto al repo)")
        return 2

    collected, problems = collect(refs)
    problems += cross_check(collected)
    for canon, copies in sorted(collected.items()):
        for lbl, (n, _) in sorted(copies.items()):
            print("  %-14s <- %-8s %4d voci in %s" % (canon, lbl, n, refs[lbl]))
    if problems:
        print("RIFIUTATO (nessuna tabella inventata):")
        for p in problems:
            print("  - " + p)
        return 1
    print("tutte le copie concordano, voce per voce")

    if a.write:
        parts = ["/* GENERATO da tools/gen_codebooks.py — NON modificare a mano.",
                 " * Ogni voce è confermata voce-per-voce da più copie indipendenti già su",
                 " * questa macchina (vedi --check); se una copia diverge, lo script rifiuta",
                 " * di scrivere. Provenienza: tabelle codebook ggml (licenza MIT, vedi",
                 " * LICENSES.md) quali sono in ds4.c e nei duplicati del vault GMBasic.",
                 " */",
                 "#ifndef FMLX_IQ2_TABLES_H",
                 "#define FMLX_IQ2_TABLES_H",
                 "#include <stdint.h>",
                 "",
                 "/* kmask NON è una tabella: è 1<<j, e riscriverlo come dati sarebbe un",
                 " * secondo posto dove il valore può divergere. */",
                 ""]
        for canon in sorted(TABLES):
            spec = TABLES[canon]
            label, (_, vals) = sorted(collected[canon].items())[0]
            parts.append("/* %s: %d voci (confermata da %s) */"
                         % (canon, len(vals),
                            ", ".join(sorted(collected[canon]))))
            parts.append(emit(canon, spec, vals))
        parts.append("#endif /* FMLX_IQ2_TABLES_H */")
        with open(a.write, "w", encoding="utf-8") as f:
            f.write("\n".join(parts) + "\n")
        print("scritto %s" % a.write)
    return 0


if __name__ == "__main__":
    sys.exit(main())
