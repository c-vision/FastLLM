#!/usr/bin/env python3
"""Trasforma `bad |= A | B;` in `if (A || B) return ...;` nei due adapter.

Perché: `|` NON sequenzia gli operandi, e qui l'ordine È il contratto del writer (una chiave
deve essere valutata prima del suo valore, un `arr_end` dopo l'`arr_begin`). `||` sequenzia.
L'accumulatore `bad` sparisce con i suoi `int bad = 0;` e i suoi `if (bad) return`.
"""
import re
import sys

PREFIX = "return FMLX_API_INTERNAL;"
COL = 100


def wrap(line):
    """Rimanda a capo una `if (A || B) return ...;` troppo lunga: prima l'azione, poi — se
    serve — la catena di `||`."""
    ind = line[: len(line) - len(line.lstrip())]
    m = re.match(r"^if \((.*)\) ((?:return|err =) [^;]+;)$", line.strip())
    if not m:
        return [line]
    cond, act = m.group(1), m.group(2)
    if len(f"{ind}if ({cond}) {act}") <= COL:
        return [f"{ind}if ({cond}) {act}"]
    if len(f"{ind}if ({cond})") <= COL:
        return [f"{ind}if ({cond})", f"{ind}    {act}"]
    parts = cond.split(" || ")
    out = [f"{ind}if ({parts[0]} ||"]
    out += [f"{ind}    {p} ||" for p in parts[1:-1]]
    out.append(f"{ind}    {parts[-1]})")
    out.append(f"{ind}    {act}")
    return out


def split_top(expr):
    """Divide a ` | ` ad alto livello (fuori da parentesi)."""
    parts, depth, cur = [], 0, ""
    i = 0
    while i < len(expr):
        ch = expr[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "|" and depth == 0 and expr[i - 1] not in "|&=" and expr[i + 1: i + 2] != "|":
            parts.append(cur)
            cur = ""
            i += 1
            continue
        cur += ch
        i += 1
    parts.append(cur)
    return [p.strip() for p in parts if p.strip()]


def transform(path):
    src = open(path).read().split("\n")
    out, i = [], 0
    while i < len(src):
        line = src[i]
        m = re.match(r"^(\s*)bad \|= (.*)$", line)
        if m:
            indent, expr = m.group(1), m.group(2)
            while not expr.rstrip().endswith(";"):  # statement su più righe
                i += 1
                expr += " " + src[i].strip()
            expr = expr.rstrip().rstrip(";").strip()
            parts = split_top(expr)
            cond = " || ".join(parts)
            out.append(f"{indent}if ({cond}) {PREFIX}")
            i += 1
            continue
        m = re.match(r"^(\s*)if \(bad \|\| (.*)\) (.*)$", line)
        if m:
            out.append(f"{m.group(1)}if ({m.group(2)}) {m.group(3)}")
            i += 1
            continue
        if re.match(r"^\s*if \(bad\) return FMLX_API_INTERNAL;\s*$", line):
            i += 1
            continue
        if re.match(r"^\s*int bad = 0;\s*$", line):
            i += 1
            continue
        if re.match(r"^\s*FmlxApiErr bad = FMLX_API_OK;\s*$", line):
            i += 1
            continue
        out.append(line)
        i += 1
    open(path, "w").write("\n".join(out))
    print(f"{path}: righe {len(src)} -> {len(out)}")


def rewrap(path):
    out = []
    for line in open(path).read().split("\n"):
        out += wrap(line)
    open(path, "w").write("\n".join(out))
    print(f"{path}: a capo, {len(out)} righe")


if sys.argv[1] == "--wrap":
    for p in sys.argv[2:]:
        rewrap(p)
else:
    for p in sys.argv[1:]:
        transform(p)
