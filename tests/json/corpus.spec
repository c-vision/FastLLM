# corpus.spec — i documenti che definiscono il contratto della lane json.
#
# UNA RIGA = UN DOCUMENTO. Escape riconosciuti (il resto è letterale):
#   \xNN   byte esatto          \n \r \t   i byte 0x0A 0x0D 0x09 NUDE
#   \\     un backslash         \e         nessun byte: il documento VUOTO
# Quindi `\\n` è la sequenza JSON backslash-n (due byte nel sorgente), `\n` è il byte di
# controllo 0x0A dentro i delimitatori: il secondo è CONTROL, il primo è una newline.
#
# (il vecchio `tools/json_oracle.py hex corpus.spec`, rimosso, produceva corpus.hex che entrambi i
# programmi leggono. Le righe qui sotto sono raggruppate per che cosa dimostrano; il
# commento `#->` dice cosa CI ASPETTIAMO noi da quell'input, non cosa fa Python.
#
# ---- acceptance: gli scalari, in tutte le forme che un client manda davvero ----
null
true
false
0
-0
-0.0
1
42
-42
1e2
1E2
1e+2
1e-2
-1.5e-3
0.1
3.141592653589793
2.2250738585072014e-308
1.7976931348623157e308
9223372036854775807
-9223372036854775808
#-> il confine dei REALI. `0.0` e `1.0` restano REALI anche nel round-trip: un writer che li
#-> emettesse `0`/`1` cambierebbe il TIPO del valore passando dal DOM (misurato: era `-0` ->
#-> INT). I subnormali sono double ESATTI e si ACCEPTANO; `1e309` trabocca in ±Inf e gli interi
#-> oltre int64 non hanno casa nel DOM: entrambi si RIFIUTANO, e qui divergiamo da Python di
#-> proposito (Python dà Inf, che JSON non ha, e interi arbitrari che noi non rappresentiamo)
0.0
1.0
5e-324
1e-320
2.2250738585072009e-308
1e309
-1e309
1e-999
9223372036854775808
-9223372036854775809
  \r\n\t 42 \n
#-> i whitespace JSON sono SOLO spazio, tab, newline, return: `42\f` e`42\v` NO
42\x0c
42\x0b
#-> contenitori vuoti e annidati: il caso che ha rotto la prima bozza del parser
[]
{}
[[]]
[[],{}]
[[],{},[[]]]
[[1,2],[3]]
{"a":1}
{"":[]}
{"":{"":[]}}
{"a":{"b":{"c":[1,2,3]}}}
[null,true,false,0,-0,"",{},[]]
#-> le stringhe: UTF-8 nudo nel sorgente (è legale) e le forme di escape
"café"
""
"€"
"😀"
"a\\u00e9b"
"\\ud83d\\ude00"
"\\/"
"\\u0000"
"\\"
"\\""
"\\\\"
"\\b\\f\\n\\r\\t"
"\\u001f"
"\\uD7FF\\uE000"
"a\\/b"
#-> chiavi duplicate: ACCEPTED, con comportamento definito (noi: la PRIMA vince)
{"a":1,"a":2}
{"max_tokens":8,"max_tokens":4096}
{"a":1,"b":{"a":2},"a":3}
{"a":{"x":1},"a":{"y":2}}
#
# ---- rifiuti: ognuno con il CODICE SUO, mai un "null" silenzioso ----
\e
#-> EMPTY
\x20
\x09\x0a\x0d\x20
#-> NUMBER: la grammatica dei numeri, non "valore mancante"
01
+1
.5
5.
1e
1e+
-
--1
00
0x10
1.
.
#-> NUMBER: le parole che JS chiama JSON
NaN
Infinity
#-> TRAILING
{} {}
1 2
nulll
[1][2]
{"a":1}{"b":2}
#-> SYNTAX: la forma che non è JSON in nessun dialetto
[1,2,]
[,1]
{"a":}
{"a":1,}
{a:1}
{"a" "b"}
{"a" 1}
[1 2]
"abc
{"a":1
[
]
tru
TRUE
{,}
[,]
""x
#-> CONTROL: i byte di controllo NUDI dentro una stringa (non `\\n`, che è un escape)
"a\nb"
"a\rb"
"a\tb"
"a\x01b"
"a\x1fb"
#-> UTF8: UTF-8 non ben formato nei byte, e i surrogate non appaiati negli escape
"\xff"
"\xc3"
"\xc0\x80"
"\xe0\x80\x80"
"\xed\xa0\x80"
"\xf4\x90\x80\x80"
"\xf0\x9f\x98"
"\\ud800"
"\\udc00"
"\\ud800A"
"\\ud800\\ud800"
#-> il BOM non è whitespace JSON: SYNTAX da entrambe le parti
\xef\xbb\xbf{}
#-> un byte 0x00 nudo fuori da una stringa
\x00
[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]
[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]
{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":{"a":1}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
#-> profondità: 64 è il limite e DEVE passare, 65 no. Sono le due righe più lunghe del
#   corpus per costruzione: `[%d[` e `%d]` con 64/65 parentesi, tenute su una riga sola.

