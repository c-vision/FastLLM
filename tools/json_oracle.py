#!/usr/bin/env python3
# json_oracle.py — il confronto che rende i test della lane json qualcosa di più di
# un'autocertificazione: la STESSA codifica canonica di `tools/json_diff.c`, prodotta però
# da `json.loads` di CPython, un codice che non abbiamo scritto noi.
#
#   json_oracle.py hex   <spec> [out]     genera il corpus esadecimale dalla specifica
#   json_oracle.py emit  <corpus-hex>     stampa la codifica canonica dell'oracolo
#   json_oracle.py diff  <corpus-hex> <bin-json_diff>
#                                         esegue entrambi e riporta le divergenze
#
# Due livelli di confronto, perché sono due domande diverse:
#  - DURO: accettare o rifiutare, e con quale VALORE. Qui ogni differenza è un bug o una
#    divergenza voluta e va motivata in docs/json-contract.md.
#  - MORBIDO: QUALE codice di errore, quando entrambi rifiutano. `json.loads` ha una sua
#    tassonomia (quasi tutto "Expecting value") che non possiamo ricalcare senza indovinare
#    i suoi messaggi: le differenze qui sono segnalate, non contate come fallimenti.
#    (Un tool che "classifica" gli errori dell'oracolo a mano sta scrivendo il risultato.)
#
# La codifica canonica è descritta in testa a tools/json_diff.c: N B I F S [ ] { } K E #W.
# I punti che la rendono confrontabile: i reali passano dal PATTERN DI BIT (8 byte
# big-endian), non dal testo; le stringhe passano dai byte UTF-8 in hex, non dal testo.
r"""La specifica è una riga per record con questi soli escape: \xNN \n \r \t \\ \e .
Qualunque altro carattere è letterale. `\\n` è quindi la sequenza JSON backslash-n,
`\n` è il byte 0x0A nudo: il caso che serve per distinguere un escape da un controllo.
`\e` non produce nessun byte: è il documento VUOTO, che una riga bianca non può essere
perché le righe bianche sono ignorate (come i commenti)."""

import json
import re
import struct
import subprocess
import sys

MAX_DEPTH = 64  # FMLX_JSON_MAX_DEPTH: il nostro contratto, non quello dell'oracolo


def read_spec(path):
    """Una riga = un documento. Gli escape della specifica decidono i byte ESATTI."""
    out = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        for raw in f:
            line = raw.rstrip("\n").rstrip("\r")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            out.append(unescape_spec(line))
    return out


def unescape_spec(s):
    b = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            b += s[i].encode("utf-8")
            i += 1
            continue
        if i + 1 >= len(s):
            raise SystemExit("json_oracle: spec finisce con backslash: %r" % s)
        e = s[i + 1]
        if e == "x":
            if i + 3 >= len(s):
                raise SystemExit("json_oracle: \\x troncato")
            b.append(int(s[i + 2:i + 4], 16))
            i += 4
        elif e == "n":
            b.append(0x0A)
            i += 2
        elif e == "r":
            b.append(0x0D)
            i += 2
        elif e == "t":
            b.append(0x09)
            i += 2
        elif e == "\\":
            b.append(0x5C)
            i += 2
        elif e == "e":
            i += 2  # il documento VUOTO: zero byte, che non sono la stessa cosa di " "
        else:
            raise SystemExit("json_oracle: escape di spec sconosciuto: \\%s" % e)
    return bytes(b)


def mode_hex(argv):
    spec = argv[0]
    dst = argv[1] if len(argv) > 1 else None
    lines = []
    for rec in read_spec(spec):
        lines.append(rec.hex())
    text = "\n".join(lines) + "\n"
    if dst:
        with open(dst, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


class OracleError(Exception):
    def __init__(self, code):
        Exception.__init__(self)
        self.code = code


class Pairs(dict):
    """Lista di coppie IN ORDINE DI SORGENTE + la vista dict, per dire quale membro vince
    in un oggetto con chiavi duplicate (Python: l'ULTIMO; noi: il PRIMO)."""

    def __init__(self, pairs):
        dict.__init__(self, pairs)
        self.pairs = pairs


def reject_const(name):
    raise OracleError("NUMBER")  # NaN / Infinity: per noi non sono JSON, per Python sì


def decode(doc):
    """Il valore Python del documento, o OracleError con il nostro CODICE più vicino.
    Ritorna Pairs per ogni oggetto: è l'unico modo di vedere i membri in ordine di sorgente
    e le chiavi duplicate, che un dict normale ha già buttato via."""
    try:
        text = doc.decode("utf-8")
    except UnicodeDecodeError:
        raise OracleError("UTF8")
    if text.strip() == "":
        raise OracleError("EMPTY")
    try:
        return json.loads(text, parse_constant=reject_const, object_pairs_hook=Pairs)
    except OracleError:
        raise
    except RecursionError:
        raise OracleError("ORACLE_RECURSION")
    except UnicodeEncodeError:
        raise OracleError("UTF8")
    except ValueError as e:
        raise OracleError(classify(text, e))


# `json.loads` non distingue il "perché" come facciamo noi: questa tabella copre i soli
# messaggi che le nostre prove realmente producono. Ciò che non è coperto resta SYNTAX e
# la differenza la si vede nel confronto MORBIDO, mai nascosta.
def classify(text, err):
    msg = getattr(err, "msg", "") or ""
    pos = getattr(err, "pos", 0) or 0
    if msg.startswith("Invalid control character"):
        return "CONTROL"
    if msg == "Extra data":
        return "TRAILING"
    if msg.startswith("Unterminated string"):
        return "SYNTAX"
    if msg.startswith("Invalid \\escape") or msg.startswith("Invalid \\u"):
        return "SYNTAX"
    tok = re.match(r"[-+.0-9eE]+", text[pos:pos + 32])
    if msg.startswith("Expecting value") and tok and re.match(r"^[+.]", tok.group(0)):
        return "NUMBER"  # `.5`, `+1`: un numero malformato, non "valore mancante"
    if msg.startswith("Expecting ',' delimiter") or msg.startswith("Expecting property name"):
        return "SYNTAX"
    return "SYNTAX"


def enc_str(v):
    # surrogatepass: una surrogate isolata (Python l'accetta da `\ud800`) non ha UTF-8, e
    # qui l'oracolo la scrive come CESU-8. Il confronto si ferma PRIMA: noi rifiutiamo.
    b = v.encode("utf-8", "surrogatepass")
    return "S%d %s" % (len(b), b.hex())


def emit(v, depth, out):
    # Il livello è dei CONTENITORI, non delle chiamate: la radice è livello 1 e lo scalare
    # più interno di un documento da 64 contenitori arriva a depth 65. Controllare prima
    # significava rifiutare ciò che sia Python sia il nostro parser accettano.
    if isinstance(v, (list, dict, Pairs)) and depth > MAX_DEPTH:
        raise OracleError("TOO_DEEP")
    if v is None:
        out.append("N")
    elif v is True:
        out.append("B1")
    elif v is False:
        out.append("B0")
    elif isinstance(v, int):
        out.append("I%d" % v)
    elif isinstance(v, float):
        out.append("F%s" % struct.pack(">d", v).hex())
    elif isinstance(v, str):
        out.append(enc_str(v))
    elif isinstance(v, list):
        out.append("[")
        for x in v:
            emit(x, depth + 1, out)
        out.append("]")
    elif isinstance(v, Pairs):
        out.append("{")
        for k, val in v.pairs:
            out.append("K" + enc_str(k)[1:])
            emit(val, depth + 1, out)
        for k in {k for k, _ in v.pairs}:
            if sum(1 for kk, _ in v.pairs if kk == k) > 1:
                # vince l'ULTIMO, per costruzione del dict di Python (noi: il PRIMO)
                idx = max(i for i, (kk, _) in enumerate(v.pairs) if kk == k)
                out.append("G" + enc_str(k)[1:] + " %d" % idx)
        out.append("}")
    else:
        raise OracleError("SYNTAX")


def encode(doc):
    """(codice, righe) — `codice` None se accettato."""
    try:
        v = decode(doc)
    except OracleError as e:
        return e.code, []
    out = []
    try:
        emit(v, 1, out)
    except OracleError as e:
        return e.code, []
    return None, out


def mode_emit(argv):
    for n, ln in enumerate(read_corpus(argv[0])):
        print("R %d" % n)
        code, rows = encode(bytes.fromhex(ln))
        if code:
            print("E %s" % code)
        else:
            for r in rows:
                print(r)
    return 0


def read_corpus(path):
    with open(path, "r", encoding="utf-8") as f:
        return [ln.strip() for ln in f if ln.strip() and not ln.lstrip().startswith("#")]


def partition_w(rows):
    """`#W` separa la codifica del documento da quella del suo round-trip."""
    for i, r in enumerate(rows):
        if r == "#W":
            return rows[:i], rows[i], rows[i + 1:]
    return rows, None, []


def split_records(text):
    """Il testo di un tool in blocchi `R <n>` ... fino al prossimo `R`."""
    recs, cur = {}, None
    for ln in text.splitlines():
        m = re.match(r"^R (\d+)$", ln)
        if m:
            cur = int(m.group(1))
            recs[cur] = []
        elif cur is not None:
            recs[cur].append(ln)
    return recs


def hard(rows):
    """La parte DURA: accettato/rifiutato e i VALORI. Il codice di errore (quando entrambi
    rifiutano) e le righe G (chi decide in un OBJ con chiavi duplicate) sono MORBIDE: le
    righe G in particolare descrivono due comportamenti documentati e DIVERSI per design."""
    out, soft = [], []
    for r in rows:
        (soft if (r.startswith("E ") or r.startswith("G")) else out).append(r)
    return out, soft


def mode_diff(argv):
    corpus, binpath = argv[0], argv[1]
    docs = [bytes.fromhex(ln) for ln in read_corpus(corpus)]
    ours = subprocess.run([binpath, corpus], capture_output=True, text=True)
    if ours.returncode != 0:
        sys.stderr.write(ours.stderr)
        print("json_diff è uscito con %d" % ours.returncode)
        return 1
    blocks = split_records(ours.stdout)
    hard_n = writer_n = soft_n = 0
    for i, doc in enumerate(docs):
        rows = blocks.get(i)
        if rows is None:
            print("== MANCANTE record %d: json_diff non ne parla" % i)
            hard_n += 1
            continue
        head, _, tail = partition_w(rows)
        code, orows = encode(doc)
        if code:
            orows = ["E %s" % code]
        mh, ms = hard(head)
        th, ts = hard(orows)
        if mh != th:
            hard_n += 1
            print("== DURO record %d ==" % i)
            print("  nostro : %s" % " | ".join(mh[:14]))
            print("  oracolo: %s" % " | ".join(th[:14]))
        elif ms != ts:
            soft_n += 1
            print("-- morbido record %d --" % i)
            print("  nostro : %s" % " | ".join(ms[:8]))
            print("  oracolo: %s" % " | ".join(ts[:8]))
        # Il round-trip non si confronta con Python: deve eguagliare NOI STESSI, ed è il
        # controllo che il writer non menta (ordine dei membri, escaping, reali).
        if head and not head[0].startswith("E ") and hard(tail)[0] != hard(head)[0]:
            writer_n += 1
            print("== ROUND-TRIP record %d ==" % i)
            print("  originario: %s" % " | ".join(hard(head)[0][:14]))
            print("  riscritto : %s" % " | ".join(hard(tail)[0][:14]))
    print("record %d, divergenze dure %d, round-trip %d, morbide %d"
          % (len(docs), hard_n, writer_n, soft_n))
    return 1 if (hard_n or writer_n) else 0


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    mode, argv = sys.argv[1], sys.argv[2:]
    if mode == "hex":
        return mode_hex(argv)
    if mode == "emit":
        return mode_emit(argv)
    if mode == "diff":
        return mode_diff(argv)
    sys.stderr.write("modalità sconosciuta: %s\n" % mode)
    return 2


if __name__ == "__main__":
    sys.exit(main())
