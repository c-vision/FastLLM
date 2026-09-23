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
  * due checkout di llama.cpp su questa macchina, a VERSIONI DIVERSE (~/llamacpp e il
    vendor dentro GMBasic): per le tabelle che esistono solo in ggml — kvalues_iq4nl e
    kvalues_fp4 — due versioni dello stesso autore escludono la deriva di versione, NON
    l'errore di conception. Per `kvalues_fp4` c'è di più: la tabella è RICAVATA qui sotto
    dai bit E2M1 della specifica OCP MX, che è una prova indipendente dall'autore. Se il
    letterale di ggml divergesse dalla derivazione, lo script rifiuta di scrivere.

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
    # Le due kvalues sono le uniche tabelle di cui vive il dequant dei tipi 20/23/39/40:
    # iq4_nl e iq4_xs condividono la prima, mxfp4 e nvfp4 condividono la seconda.
    "kvalues_iq4nl": {"ctype": "int8_t",   "src": [("ggml_main",     "kvalues_iq4nl"),
                                                    ("ggml_vendored", "kvalues_iq4nl")]},
    "kvalues_fp4":   {"ctype": "int8_t",   "src": [("ggml_main",     "kvalues_fp4"),
                                                    ("ggml_vendored", "kvalues_fp4")],
                      "derive": "e2m1_doubled"},
}

# Valori attesi derivati dai BIT, non da una copia: e2m1 (1 segno, 2 esponente bias 1,
# 1 mantissa) moltiplicato per 2, che è la convenzione di ggml ("e2m1 values (doubled)").
# L'indice 8 è -0.0 e si scrive 0: è il motivo per cui la tabella NON è antisimmetrica.
def derived_e2m1_doubled():
    out = []
    for code in range(16):
        sgn, i = code >> 3, code & 7
        e, m = i >> 1, i & 1
        v = 0.5 * m if e == 0 else (2.0 ** (e - 1)) * (1.0 + 0.5 * m)
        v *= -1 if sgn else 1
        out.append(int(round(v * 2)))
    return [0 if v == 0 else v for v in out]      # -0 -> 0, come (int8_t)

DERIVATIONS = {"e2m1_doubled": derived_e2m1_doubled}

DEF_RELS = {
    "gmb_mlx": "native/gmb_host_ai_mlx/gmb_iq2_tables.h",
    "gmb_moe": "native/gmb_moe_runtime/src/gmb_moe_iq2_tables.h",
    "ds4":     "ds4.c",
}
# I due checkout ggml sono cercati per path assoluto noto: sono FORI repo, non dipendenze.
GGML_RELS = {
    "ggml_main":     "ggml/src/ggml-common.h",
    "ggml_vendored": "native/gmb_host_ai/build/_deps/llama_cpp-src/ggml/src/ggml-common.h",
}
GGML_ENV = {"ggml_main": "LLAMACPP_REPO", "ggml_vendored": "GMBASIC_REPO"}
# Le etichette NON di questo progetto vivono accanto, su questa macchina: i default sono
# percorsi candidati, non dipendenze hard (si possono sempre scavalcare con --ref).
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIBLINGS = [os.path.join(os.path.dirname(REPO_ROOT), "GMBasic"),
            os.path.join(os.path.dirname(REPO_ROOT), "ds4"),
            os.path.expanduser("~/ds4")]


ENV_FOR = {"gmb_mlx": "GMBASIC_REPO", "gmb_moe": "GMBASIC_REPO", "ds4": "DS4_REPO"}


def default_ref(label):
    """I percorsi qui sono CANDIDATI, non dipendenze: --ref scavalca sempre.

    `ggml_main` e `ggml_vendored` non hanno una relazione con questo repo (non è un
    submodule, non è un download): sono due checkout che esistono SU QUESTA MACCHINA a
    versioni diverse. Se uno dei due manca, il controllo di deriva di versione non può
    essere fatto e lo script rifiuta, invece di fidarsi di una copia sola.
    """
    if label in GGML_RELS:
        env = os.environ.get(GGML_ENV[label])
        cands = [os.path.join(os.path.expanduser(env), GGML_RELS[label])] if env else []
        if label == "ggml_main":
            cands += [os.path.join(p, GGML_RELS[label])
                      for p in ("~/llamacpp", "~/llama.cpp", "~/ai/llamacpp")]
        else:
            cands += [os.path.join(b, GGML_RELS[label]) for b in SIBLINGS]
        for p in cands:
            p = os.path.expanduser(p)
            if os.path.isfile(p):
                return p
        return None
    rel = DEF_RELS[label]
    for base in [os.getcwd(), REPO_ROOT] + SIBLINGS:
        p = os.path.join(base, rel)
        if os.path.isfile(p):
            return p
    env = os.environ.get(ENV_FOR[label])
    if env:
        p = os.path.join(os.path.expanduser(env), rel)
        if os.path.isfile(p):
            return p
    return None

# Le tabelle ggml moderne non sono `static const`: sono aperte da una macro il cui corpo
# cambia a seconda del target (C, Metal, CUDA, `constant`), quindi il pattern è DUE.
ARRAY_RE = r"static const\s+\w+\s+%s\[(\d+)\]\s*=\s*\{(.*?)\n\};"
MACRO_RE = r"GGML_TABLE_BEGIN\(\s*\w+\s*,\s*%s\s*,\s*(\d+)\s*\)\s*(.*?)\n\s*GGML_TABLE_END"


def parse_table(path, name):
    """Ritorna (dichiarati, valori) oppure None se il nome non c'è.

    Il conteggio dichiarato nella macro NON è decorative: è il numero che ggml stessa
    asserisce, e se i valori parsati sono di meno il `\n};` finale è stato mangiato da una
    formattazione diversa. In quel caso si rifiuta, non si prende `len(vals)`.
    """
    text = open(path, encoding="utf-8", errors="replace").read()
    for rx in (ARRAY_RE, MACRO_RE):
        m = re.search(rx % re.escape(name), text, re.S)
        if m:
            break
    if not m:
        return None
    decl = int(m.group(1))
    body = m.group(2)
    vals = [int(v, 0) if not v.startswith("-") else -int(v[1:], 0)
            for v in re.findall(r"-?0x[0-9a-fA-F]+|-?\b\d+\b", body)]
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


def derivation_check(collected):
    """Dove la tabella È derivabile dai bit, la copia non basta: si confronta coi bit.

    Questo è l'unico controllo del file che non dipende da quello che qualcuno ha scritto:
    i 16 valori `kvalues_fp4` sono funzione della sola codifica E2M1 (specifica OCP MX),
    quindi se ggml e il nostro derivatore concordano, la concordanza non è ereditata.
    """
    bad = []
    for canon, spec in TABLES.items():
        how = spec.get("derive")
        if not how:
            continue
        expect = DERIVATIONS[how]()
        for label, (decl, vals) in sorted(collected[canon].items()):
            if len(vals) != len(expect):
                bad.append("%s/%s: %d valori, la derivazione ne produce %d"
                           % (canon, label, len(vals), len(expect)))
                continue
            diff = [i for i in range(len(vals)) if vals[i] != expect[i]]
            if diff:
                i = diff[0]
                bad.append("%s/%s: DIVERGE dalla derivazione dai bit su %d/%d voci "
                           "(prima: [%d] copia %d, bit %d)"
                           % (canon, label, len(diff), len(vals), i, vals[i], expect[i]))
    return bad


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
    for k in list(DEF_RELS) + list(GGML_RELS):
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
    problems += derivation_check(collected)
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
                 " *",
                 " * Asimmetria da conoscere, perché cambia come si legge questo file:",
                 " * iq2xxs_grid/ksigns hanno DUE AUTORI distinti (ds4 e la linea ggml di",
                 " * GMBasic) che dicono gli stessi valori; kvalues_iq4nl ha invece DUE",
                 " * VERSIONI di ggml, che esclude la deriva di versione ma non un errore di",
                 " * conception. A kvalues_fp4 questo non è bastato: i suoi 16 valori sono",
                 " * anche derivati qui dai bit E2M1 della specifica OCP MX, che è l'unico",
                 " * testimone che non è la copia di qualcuno.",
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
