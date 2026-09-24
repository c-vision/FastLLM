#!/usr/bin/env python3
"""mut_prove.py — prova che un'asserzione ROSSA lo sia davvero.

Un controllo che non è mai stato visto fallire non prova niente: può essere verde perché
l'implementazione è giusta, o perché l'asserzione non guarda nulla. Questo script fa il secondo
passo del metodo usato su ogni caso nuovo di `test_pull`/`test_cli`:

  1. copia il sorgente toccato in `build/probe-<tag>/mut_<nome>.c` (MAI tocca l'originale);
  2. applica UNA sostituzione letterale, che deve combaciare ESATTAMENTE UNA VOLTA — se combacia
     zero o due volte il mutante è costruito a mano e la prova è falsa, quindi si ferma;
  3. compila il mutante e lo mette PRIMA dell'archivio (`libfmlx_cli.a`): il `.o` definisce già tutti
     i simboli di quel membro, così il membro non viene mai prelevato e a girare è il codice mutato;
  4. esegue il binario e riporta QUALI asserzioni sono diventate rosse.

L'ordine dei percorsi è quello del CMake: si lavora dalla root del repo (lo script ci va da sé).

    tools/mut_prove.py --target test_pull --tag dedupe --src src/cli/fmlx_pull_progress.c \
        --sub 'VECCHIO' 'NUOVO' [--sub ... ] [--sub ...]

Con `--list` si stampano i mutanti già definiti per un tag (in `build/probe-<tag>/spec.json`), così
chi riprende il lavoro li riesegue tutti senza riscriverli.
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys

CC = os.environ.get("CC", "/usr/bin/cc")
# I percorsi di `include` sono RELATIVI ALLA ROOT del repo. Lo script ci si porta da solo
# (partendo da dovunque lo si lanci) proprio perché il punto d'appello non deve essere una
# conoscenza nascosta di chi lo esegue.
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
INC = ["-I", "include", "-I", "src/cli", "-I", "src", "-I", "src/hf", "-I", "src/json", "-I", "src/http"]
STD = ["-g", "-std=gnu17", "-Wall", "-Wextra", "-Werror"]


def die(msg):
    sys.stderr.write("mut_prove: %s\n" % msg)
    sys.exit(2)


def link_recipe(target):
    """Ricostruisce la riga di link da build/CMakeFiles/<target>.dir/link.txt, separando flag,
    oggetti e librerie: il mutante si infila fra oggetti e librerie, che è l'unico ordine con cui
    l'archivio originale NON viene consultato."""
    path = os.path.join("build", "CMakeFiles", "%s.dir" % target, "link.txt")
    if not os.path.exists(path):
        die("manca %s: configurare la build prima" % path)
    toks = shlex.split(open(path).read().strip())
    flags, objs, libs = [], [], []
    i = 1  # il PRIMO token è il compilatore che CMake ha scelto: `CC` qui sopra lo rimette già
    while i < len(toks):
        t = toks[i]
        if t == "-o":
            i += 2
            continue
        if t.endswith(".o"):
            objs.append(t)
        elif t.endswith(".a") or t.endswith(".tbd") or t.startswith("-l"):
            libs.append(t)
        else:
            flags.append(t)
        i += 1
    if not objs or not libs:
        die("riga di link non riconosciuta in %s" % path)
    # I percorsi in link.txt sono relativi alla cartella di build, non alla root: si antepone
    # `build/` perché lo script gira dalla root, dove invece `src/...` e `include/` sono già giusti.
    fix = lambda t: t if (t.startswith("-") or os.path.isabs(t)) else os.path.join("build", t)
    return flags, [fix(t) for t in objs], [fix(t) for t in libs]


def build_one(src, name, subs, outdir, flags, objs, libs, exe):
    mut = os.path.join(outdir, "mut_%s.c" % name)
    text = open(src).read()
    for old, new in subs:
        n = text.count(old)
        if n != 1:
            die("la sostituzione per '%s' combacia %d volte in %s (serve 1): il mutante sarebbe "
                "ambiguo e la prova non varrebbe" % (name, n, src))
        text = text.replace(old, new)
    if text == open(src).read():
        die("nessuna sostituzione applicata per '%s'" % name)
    open(mut, "w").write(text)
    obj = os.path.join(outdir, "mut_%s.o" % name)
    cmd = [CC] + STD + INC + ["-c", mut, "-o", obj]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stdout + p.stderr)
        die("il mutante '%s' NON compila: -Werror ha detto qualcosa che il codice vero non dice" % name)
    cmd = [CC] + flags + objs + [obj] + libs + ["-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stdout + p.stderr)
        die("link del mutante '%s' fallita" % name)


def run_one(exe, work, timeout):
    os.makedirs(work, exist_ok=True)
    try:
        p = subprocess.run([exe, work], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out = ((e.stdout or b"") + (e.stderr or b"")).decode("utf-8", "replace")
        return -1, [], out
    out = (p.stdout or "") + (p.stderr or "")
    fails = [ln for ln in out.splitlines() if "FALLIMENTO" in ln or "verifiche" in ln]
    return p.returncode, fails, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True, help="nome del test CMake, es. test_pull")
    ap.add_argument("--tag", required=True, help="cartella di build/probe-<tag>")
    ap.add_argument("--src", help="sorgente da mutare (relativo alla root)")
    ap.add_argument("--name", help="nome del mutante")
    ap.add_argument("--sub", nargs=2, action="append", default=[], metavar=("OLD", "NEW"))
    ap.add_argument("--spec", help="json: [{name, src, sub:[[old,new],..]}, ...]")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--timeout", type=int, default=60,
                    help="secondi per singolo run: uno 0 oltre è un BLOCCO (pty, wait, rete)")
    a = ap.parse_args()

    os.chdir(REPO)
    outdir = os.path.join("build", "probe-%s" % a.tag)
    os.makedirs(outdir, exist_ok=True)
    specfile = os.path.join(outdir, "spec.json")

    mutants = []
    if a.list:
        if not os.path.exists(specfile):
            die("nessun %s per il tag '%s'" % (specfile, a.tag))
        mutants = json.load(open(specfile))
    elif a.spec:
        mutants = json.load(open(a.spec))
        json.dump(mutants, open(specfile, "w"), indent=1, ensure_ascii=False)
    else:
        if not (a.src and a.name and a.sub):
            die("servono --src --name e almeno una --sub (oppure --spec/--list)")
        mutants = [{"name": a.name, "src": a.src, "sub": a.sub}]
        json.dump(mutants, open(specfile, "w"), indent=1, ensure_ascii=False)

    exe = os.path.join("build", "test_%s_mut" % re.sub(r"^test_", "", a.target))
    flags, objs, libs = link_recipe(a.target)
    rc_all = 0
    for m in mutants:
        name, src = m["name"], m["src"]
        subs = [(s[0], s[1]) for s in m["sub"]]
        this_exe = "%s.%s" % (exe, name)
        build_one(src, name, subs, outdir, flags, objs, libs, this_exe)
        work = os.path.join(outdir, "w-%s" % name)
        rc, lines, out = run_one(this_exe, work, a.timeout)
        fired = [ln for ln in lines if "FALLIMENTO" in ln]
        summary = [ln for ln in lines if "FALLIMENTO" not in ln]
        # VERDE-MUTO: un mutante che non fa fallire NIENTE non prova niente, e il caso sotto accusa
        # va riscritto. È il risultato che questo script esiste per scovare.
        if rc == -1:
            tag = "BLOCCATO dopo %ds: il mutante impalla il test (pty piena, wait, rete)" % a.timeout
        else:
            tag = "FALLISCE (prova valida)" if fired else "VERDE-MUTO: nessuna asserzione è cambiata"
        print("== %s: %s" % (name, tag))
        for ln in fired[:12]:
            print("   " + ln.strip())
        for ln in summary:
            print("   " + ln.strip())
        if not fired:
            rc_all = 1
    sys.exit(rc_all)


if __name__ == "__main__":
    main()
