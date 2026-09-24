#!/usr/bin/env python3
"""gen_codebooks.py — genera (e verifica) le tabelle codebook ggml di FastLLM.

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
    python3 tools/gen_codebooks.py --check [--group iq2|iqgrid|iq1s] \
                                    [--ref GMBasic=<path>] [--ref ds4=<path>]
    python3 tools/gen_codebooks.py --write --group iqgrid      # path dal gruppo

Il `--group` esiste perché i tre insiemi NON hanno la stessa forza di prova: `iq2` ha copie
di autori diversi (ds4 contro la linea ggml), `iqgrid`/`iq1s` hanno solo due VERSIONI di ggml.
Il banner che esce nel file lo dice, perché un lettore che crede di avere due autori dove c'è
un autore con due checkout trae conclusioni sbagliate su quanto fidarsi dei dati.
"""
import argparse
import os
import re
import sys

# nome canonico FastLLM -> [(etichetta sorgente, nome nella sorgente), ...]
# `group` = quale header esce (--write ... --group <nome>): i file generati restano sotto
# le 1200 righe, e il gruppo è anche l'unità con cui si ragiona («le griglie IQ2/IQ3» contro
# «la griglia IQ1_S», che da sola è 2048 voci).
# `bytes`/`alphabet` sono la SORVEGLIANZA sul dato, non la sua prova: vedi shape_check.
TABLES = {
    "iq2xxs_grid":   {"ctype": "uint64_t", "group": "iq2", "bytes": 8,
                      "alphabet": [8, 25, 43],
                      "src": [("gmb_mlx", "gmb_iq2_grid"),
                              ("gmb_moe", "gmb_iq2_grid"),
                              ("ds4",     "iq2xxs_grid")]},
    "iq2_ksigns":    {"ctype": "uint8_t",  "group": "iq2",
                      "src": [("gmb_mlx", "gmb_iq2_ksigns"),
                              ("gmb_moe", "gmb_iq2_ksigns"),
                              ("ds4",     "ksigns_iq2xs")]},
    # Le due kvalues sono le uniche tabelle di cui vive il dequant dei tipi 20/23/39/40:
    # iq4_nl e iq4_xs condividono la prima, mxfp4 e nvfp4 condividono la seconda.
    "kvalues_iq4nl": {"ctype": "int8_t",   "group": "iq2",
                      "src": [("ggml_main",     "kvalues_iq4nl"),
                              ("ggml_vendored", "kvalues_iq4nl")]},
    "kvalues_fp4":   {"ctype": "int8_t",   "group": "iq2",
                      "src": [("ggml_main",     "kvalues_fp4"),
                              ("ggml_vendored", "kvalues_fp4")],
                      "derive": "e2m1_doubled"},
    # --- griglie dei tipi IQ2_XS / IQ2_S / IQ3_XXS / IQ3_S / IQ1_S / IQ1_M (batch C) ---
    # Testimone: SOLO le due versioni di ggml. ds4 non le ha (porta solo iq2xxs_grid) e i
    # duplicati GMBasic pure: se una di queste cinque entra in `fmlx_ggml_dequant_row`, ci
    # entra con la prova debole, e va battuta indice-per-indice contro la dylib (vedi
    # grid_sweep in tools/quant_diff.c) — non basta che due testi coincidano.
    "iq2xs_grid":    {"ctype": "uint64_t", "group": "iqgrid", "bytes": 8,
                      "alphabet": [8, 25, 43],
                      "src": [("ggml_main", "iq2xs_grid"), ("ggml_vendored", "iq2xs_grid")]},
    "iq2s_grid":     {"ctype": "uint64_t", "group": "iqgrid", "bytes": 8,
                      "alphabet": [8, 25, 43],
                      "src": [("ggml_main", "iq2s_grid"), ("ggml_vendored", "iq2s_grid")]},
    "iq3xxs_grid":   {"ctype": "uint32_t", "group": "iqgrid", "bytes": 4,
                      "alphabet": [4, 12, 20, 28, 36, 44, 52, 62],
                      "src": [("ggml_main", "iq3xxs_grid"),
                              ("ggml_vendored", "iq3xxs_grid")]},
    "iq3s_grid":     {"ctype": "uint32_t", "group": "iqgrid", "bytes": 4,
                      "alphabet": [1, 3, 5, 7, 9, 11, 13, 15],
                      "src": [("ggml_main", "iq3s_grid"), ("ggml_vendored", "iq3s_grid")]},
    "iq1s_grid":     {"ctype": "uint64_t", "group": "iq1s", "bytes": 8,
                      "alphabet": [0, 1, 255],
                      "src": [("ggml_main", "iq1s_grid"), ("ggml_vendored", "iq1s_grid")]},
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
# Il conteggio può essere un MACRO (`NGRID_IQ1S` = 2048): `(\w+)` e non `(\d+)`, altrimenti
# la tabella più grande del set non verrebbe letta AFFATTO e il buco sarebbe silenzioso.
ARRAY_RE = r"static const\s+\w+\s+%s\[(\w+)\]\s*=\s*\{(.*?)\n\};"
MACRO_RE = r"GGML_TABLE_BEGIN\(\s*\w+\s*,\s*%s\s*,\s*(\w+)\s*\)\s*(.*?)\n\s*GGML_TABLE_END"


def _decl_count(text, token):
    """Quante voci dichiara la tabella: una cifra, o un macro #definito nello stesso file."""
    if token.isdigit():
        return int(token)
    m = re.search(r"#define\s+%s\s+(\d+)" % re.escape(token), text)
    return int(m.group(1)) if m else None


def parse_table(path, name):
    """Ritorna (dichiarati, valori), (None, None) se il conteggio non è risolvibile,
    None se il nome non c'è.

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
    decl = _decl_count(text, m.group(1))
    if decl is None:
        return (None, None)
    body = m.group(2)
    vals = [int(v, 0) if not v.startswith("-") else -int(v[1:], 0)
            for v in re.findall(r"-?0x[0-9a-fA-F]+|-?\b\d+\b", body)]
    return decl, vals


def collect(refs, tables=None):
    """-> {nome_canonico: {etichetta: (decl, vals)}} e una lista di problemi."""
    tables = TABLES if tables is None else tables
    out, problems = {}, []
    for canon, spec in tables.items():
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
            if decl is None:
                problems.append("%-14s/%s: conteggio dichiarato come macro non risolvibile "
                                "in %s (rifiutato: senza numero non si può verificare nulla)"
                                % (canon, label, refs[label]))
                continue
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


def shape_check(collected, tables):
    """Sorveglia la FORMA del dato, non il suo essere copiato bene: due copie dello stesso
    upstream che dicono la stessa cosa non escludono che quel che leggiamo non sia più la
    tabella che abbiamo verificato (una riga duplicata o un dito su un carattere esadecimale
    sono esattamente i difetti che il confronto tra copie NON vede).

    Due asserzioni, entrambe falsificabili e independientesi dalle copie:
      * **codici distinti**: una codebook con due voci identiche spreca un indice, quindi
        `distinct == dichiarato` è una proprietà del formato, non dell'autore;
      * **alfabeto dei byte**: ogni elemento di una griglia IQ prende uno di pochi valori
        (IQ2: 8/25/43; IQ1_S: 0/±1; IQ3_S: i dispari 1..15). Un dito su UNA cifra esadecimale
        che cambia un byte lo sposta fuori dall'alfabeto con probabilità alta — e per la
        famiglia IQ2 i tre valori differiscono in almeno due nibble, quindi lì l'errore di
        copia è preso SEMPRE.
    L'alfabeto qui è OSSERVATO, non derivato da una specifica: è una rete, non una prova. La
    prova di queste cinque griglie è un' altra, e si esegue — grid_sweep() in
    tools/quant_diff.c batte ogni indice contro la copia COMPILATA dentro la dylib di ggml.

    COSA NON VEDE, e va detto perché il contrario è falso per costruzione: `--check`
    confronta le COPIE SORGENTE tra loro e NON rilegge l'header già scritto. Una PERMUTAZIONE
    introdotta nel ns. file — un taglio sbagliato nell'emissione, un copia-incolla di due
    righe — passa `--check` e passa la forma (le voci restano 1024, distinte, in alfabeto).
    Misurato il 2026-09-23: scambiando `iq2s_grid[504]` e `[505]` nello header generato,
    `--check --group iqgrid` ha detto "tutte le copie concordano" (exit 0) mentre
    `grid_sweep` ha risposto `DIVERGENZA C (ggml) su "iq2s_grid"[160]: 11 vs 34.375` e il
    gate è uscito 1. È l'unica ragione per cui quello sweep esiste.
    """
    bad = []
    for canon, copies in collected.items():
        spec = tables[canon]
        nbytes, alphabet = spec.get("bytes"), spec.get("alphabet")
        if not (nbytes and alphabet):
            continue
        allowed = set(alphabet)
        for label, (decl, vals) in sorted(copies.items()):
            if len(set(vals)) != decl:
                bad.append("%s/%s: %d voci ma %d distinte — una codebook non può avere due "
                           "codici identici" % (canon, label, decl, len(set(vals))))
            first = None
            for i, v in enumerate(vals):
                for j in range(nbytes):
                    b = (v >> (8 * j)) & 0xff
                    if b not in allowed:
                        first = (i, j, b)
                        break
                if first:
                    break
            if first:
                bad.append("%s/%s: [%d] byte %d = 0x%02x fuori dall'alfabeto %s"
                           % (canon, label, first[0], first[1], first[2],
                              ", ".join(sorted(hex(x) for x in allowed))))
    return bad


def fmt_rows(vals, ctype, per_line):
    width = {"uint64_t": 18, "uint32_t": 10}.get(ctype, 5)
    hexw = {"uint64_t": 16, "uint32_t": 8}.get(ctype)
    lines, row = [], []
    for i, v in enumerate(vals):
        s = ("0x%0*x" % (hexw, v)) if hexw else ("%3u" % v)
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
    for canon, copies in collected.items():
        how = TABLES[canon].get("derive")
        if not how:
            continue
        expect = DERIVATIONS[how]()
        for label, (decl, vals) in sorted(copies.items()):
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
    per = {"uint64_t": 4}.get(spec["ctype"], 8)
    return ("static const %s fmlx_%s[%d] = {\n%s\n};\n"
            % (spec["ctype"], canon, len(vals), fmt_rows(vals, spec["ctype"], per)))


# Un banner per gruppo, perché la provenienza NON è la stessa: scrivere «confermata da più
# copie indipendenti» su una griglia che ha solo due versioni dello stesso file sarebbe
# esattamente il soffitto verde che questo repo ha già imparato a odiare.
GROUPS = {
    "iq2": {
        "out": "src/lane_gguf/fmlx_iq2_tables.h",
        "banner": [
            " * Asimmetria da conoscere, perché cambia come si legge questo file:",
            " * iq2xxs_grid/ksigns hanno DUE AUTORI distinti (ds4 e la linea ggml di",
            " * GMBasic) che dicono gli stessi valori; kvalues_iq4nl ha invece DUE",
            " * VERSIONI di ggml, che esclude la deriva di versione ma non un errore di",
            " * conception. A kvalues_fp4 questo non è bastato: i suoi 16 valori sono",
            " * anche derivati qui dai bit E2M1 della specifica OCP MX, che è l'unico",
            " * testimone che non è la copia di qualcuno.",
        ],
    },
    "iqgrid": {
        "out": "src/lane_gguf/fmlx_iqgrid_tables.h",
        "banner": [
            " * Provenienza DEBOLE, detta come tale: qui non c'è un secondo autore. Ogni",
            " * voce è confermata da DUE VERSIONI di ggml-common.h su questa macchina, che",
            " * esclude la deriva di versione ma non un errore di conception — e queste",
            " * griglie non sono derivabili da una specifica: sono codebook ALLENATI, e",
            " * quindi non esiste il testimone «i bit lo impongono» che c'è per fp4.",
            " * La prova vera di questi dati si esegue, non si legge: grid_sweep() in",
            " * tools/quant_diff.c batte OGNI indice di griglia contro la copia COMPILATA",
            " * dentro la dylib di ggml e confronta i bit del risultato. Se una voce di",
            " * questo header fosse sbagliata, quell'indice dequantizza un float diverso.",
            " * Qui restano due reti: «tutte le voci sono distinte» (una codebook con due",
            " * codici identici spreca un indice) e l'alfabeto dei byte (0x08/0x19/0x2b",
            " * per IQ2, dispari 1..15 per IQ3_S), che prende un dito su una cifra.",
        ],
    },
    "iq1s": {
        "out": "src/lane_gguf/fmlx_iq1s_tables.h",
        "banner": [
            " * 2048 voci per i tipi IQ1_S e IQ1_M: lette come int8, ogni byte è 0, +1 o -1",
            " * (l'alfabeto è controllato qui, e un dito su una cifra lo rompe). Vale la",
            " * stessa avvertenza del gruppo iqgrid — DUE VERSIONI di ggml, nessun secondo",
            " * autore, e la prova è grid_sweep() di tools/quant_diff.c che batte ogni",
            " * indice contro la copia compilata nella dylib, non questo testo.",
            " * Il delta (±IQ1S_DELTA = 0.125) NON sta nella tabella: è un bit di qh e lo",
            " * aggiunge il dequant, per lo stesso motivo per cui kmask non è un dato.",
        ],
    },
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="confronta le copie, non scrive")
    ap.add_argument("--write", metavar="FILE", nargs="?", const="", default=None,
                    help="scrive l'header del --group (senza valore, il path del gruppo)")
    ap.add_argument("--group", default="iq2", choices=sorted(GROUPS),
                    help="quale insieme di tabelle (default iq2, come in passato)")
    ap.add_argument("--ref", action="append", default=[], metavar="ETICHETTA=FILE",
                    help="percorso di una sorgente nota (ripetibile)")
    a = ap.parse_args()
    if not a.check and a.write is None:
        ap.error("indicare --check e/o --write")

    tables = {k: v for k, v in TABLES.items() if v["group"] == a.group}
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
    missing = [k for k, spec in tables.items()
               for lbl, _ in spec["src"] if lbl not in refs]
    if missing:
        print("mancano le sorgenti: %s" % ", ".join(sorted(set(missing))))
        print("passare --ref ETICHETTA=FILE (i default cercano GMBasic/ds4 accanto al repo)")
        return 2

    collected, problems = collect(refs, tables)
    problems += cross_check(collected)
    problems += shape_check(collected, tables)
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

    if a.write is not None:
        grp = GROUPS[a.group]
        out = a.write or grp["out"]
        guard = re.sub(r"\W+", "_", os.path.basename(out).upper()).rstrip("_")
        parts = ["/* GENERATO da tools/gen_codebooks.py --group %s — NON modificare a mano."
                 % a.group,
                 " * Ogni voce è confermata voce-per-voce da più copie già su questa macchina",
                 " * (vedi --check); se una copia diverge, lo script rifiuta di scrivere.",
                 " * Provenienza: tabelle codebook ggml (licenza MIT, vedi LICENSES.md).",
                 " *"]
        parts += grp["banner"]
        parts += [" */",
                  "#ifndef %s" % guard,
                  "#define %s" % guard,
                  "#include <stdint.h>",
                  "",
                  "/* kmask NON è una tabella: è 1<<j, e riscriverlo come dati sarebbe un",
                  " * secondo posto dove il valore può divergere. */",
                  ""]
        for canon in sorted(tables):
            spec = tables[canon]
            label, (_, vals) = sorted(collected[canon].items())[0]
            parts.append("/* %s: %d voci (confermata da %s) */"
                         % (canon, len(vals),
                            ", ".join(sorted(collected[canon]))))
            parts.append(emit(canon, spec, vals))
        parts.append("#endif /* %s */" % guard)
        with open(out, "w", encoding="utf-8") as f:
            f.write("\n".join(parts) + "\n")
        print("scritto %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
