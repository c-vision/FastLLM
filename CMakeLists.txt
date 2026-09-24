cmake_minimum_required(VERSION 3.20)
project(FastMLX LANGUAGES C)

# Le estensioni GNU/POSIX servono: mmap/open/lseek non sono visibili con -std=c17 puro.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)

add_compile_options(-Wall -Wextra)
option(FASTMLX_WERROR "Tratta gli avvisi come errori" ON)
if(FASTMLX_WERROR)
  add_compile_options(-Werror)
endif()

# Le lane statiche finiscono dentro la libreria condivisa `mlxengine`, e una libreria condivisa
# vuole object position-independent: si dichiara qui, una volta, non con una proprietà per target
# (che è il modo in cui una lane nuova nasce linkabile a metà).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- lane_gguf: il lettore GGUF di riferimento (nessun cap, allineamento onorato) ----
# Tre file perché due contratti diversi: `quant.c` smista e conosce il rifiuto, `quant_ext.c`
# è SOLA aritmetica, `quant_iq.c` ha bisogno delle codebook generate da tools/gen_codebooks.py
# (e per questo legge un header accanto a sé, da cui il include dir qui sotto).
add_library(fmlx_gguf STATIC src/lane_gguf/fmlx_gguf.c src/lane_gguf/fmlx_quant.c
                             src/lane_gguf/fmlx_quant_ext.c src/lane_gguf/fmlx_quant_iq.c)
target_include_directories(fmlx_gguf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(fmlx_gguf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lane_gguf)

enable_testing()

add_executable(test_gguf tests/gguf/test_gguf.c)
target_link_libraries(test_gguf PRIVATE fmlx_gguf)
add_test(NAME gguf COMMAND test_gguf)

add_executable(test_quant tests/gguf/test_quant.c)
target_link_libraries(test_quant PRIVATE fmlx_gguf)
add_test(NAME quant COMMAND test_quant)

# Ispeziona la testata di GGUF reali (stampa tipi, allineamento, codici di rifiuto).
add_executable(fmlx_gguf_inspect tools/gguf_inspect.c)
target_link_libraries(fmlx_gguf_inspect PRIVATE fmlx_gguf)

# --- MLXEngine: il deliverable ① — la libreria condivisa, non una lane interna ----------------
# `SHARED` è il punto, non un dettaglio di gusti: una lane interna la si può linkare statica e
# cambiarle la forma a ogni commit, un motore che DOMANI GMBasic userà via P/Invoke no. Qui
# dentro vive l'ABI (`include/mlxengine.h`), che è l'unica cosa del progetto pensata per durare
# più della sua implementazione: il numero di versione sale solo quando una firma cambia.
#
# Il primo blocco è il lettore GGUF + il dequant. E non c'è niente da nascondere dietro un
# target vuoto: `mlxengine.c` non implementa un byte di aritmetica, si appoggia a `fmlx_gguf` (che
# è già bit-exact contro tre implementazioni indipendenti). Quando entreranno i pesi packed, la
# cache KV e i kernel, entreranno QUI con lo stesso numero d'ABI e un gate di parità proprio.
add_library(mlxengine SHARED src/engine/mlxengine.c)
set_target_properties(mlxengine PROPERTIES VERSION 1.0.0 SOVERSION 1)
target_include_directories(mlxengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mlxengine PUBLIC fmlx_gguf)

# L'ABI si prova DA FUORI: gli stessi byte letti attraverso il confine e dalla parte di dentro
# devono dare gli STESSI bit, e nessun altro tipo di asserzione lo dice.
add_executable(test_engine tests/engine/test_engine.c)
target_link_libraries(test_engine PRIVATE mlxengine)
add_test(NAME engine COMMAND test_engine)

# --- lane_hf: cache Hugging Face letto senza rete (il download non ha ancora codice) ---------
# `fmlx_hf_catalog.c` sta QUI e non nella lane download perché non tocca la rete e non scrive:
# guarda cosa c'è sul disco. Le sue regole di conteggio vengono dal cache REALE di questa
# macchina (34 directory, due radici), non dall'idea di come dovrebbe essere.
add_library(fmlx_hf STATIC src/hf/fmlx_hf_cache.c src/hf/fmlx_hf_hash.c src/hf/fmlx_hf_catalog.c
                           src/hf/fmlx_hf_audit.c)
target_include_directories(fmlx_hf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hf)

add_executable(test_hf_cache tests/hf/test_hf_cache.c)
target_link_libraries(test_hf_cache PRIVATE fmlx_hf)
add_test(NAME hf_cache COMMAND test_hf_cache ${CMAKE_CURRENT_BINARY_DIR}/work)

# Il catalogo locale: fixture costruite su disco (nessun transport da falsificare) e, in fondo, il
# cache REALE della macchina. Il file si passa il work directory: scrive e legge con lo stesso
# helper, così "radice sbagliata" non può passare per disallineamento di path.
add_executable(test_hf_catalog tests/hf/test_hf_catalog.c)
target_link_libraries(test_hf_catalog PRIVATE fmlx_hf)
add_test(NAME hf_catalog COMMAND test_hf_catalog ${CMAKE_CURRENT_BINARY_DIR}/work-catalog)

# L'audit hashà DAVVERO i byte: le fixture sono cache finti ma con gli hash veri (vettori da
# `git hash-object` / `shasum`, li stessi di `test_hf_cache.c`), perché un verificatore provato su
# nomi casuali non ha mai verificato niente.
add_executable(test_hf_audit tests/hf/test_hf_audit.c)
target_link_libraries(test_hf_audit PRIVATE fmlx_hf)
add_test(NAME hf_audit COMMAND test_hf_audit ${CMAKE_CURRENT_BINARY_DIR}/work-audit)

# Guarda il cache HF reale (sola lettura, nessuna rete): il controllo sul mondo vero.
add_executable(hf_probe tools/hf_probe.c)
target_link_libraries(hf_probe PRIVATE fmlx_hf)

# --- lane_hf/download: la parte di cache del download + il trasporto libcurl ----------------
# `find_package(CURL)` qui trova la libcurl DI SISTEMA (SDK, SecureTransport): nessuna copia di
# terze parti nel repo, nessuna CA bundle nostra. È una scelta misurata, non di comodità:
# vedi l'intestazione di src/hf/fmlx_hf_curl.c e la nota in LICENSES.md.
find_package(CURL REQUIRED)

add_library(fmlx_hf_fetch STATIC src/hf/fmlx_hf_fetch.c src/hf/fmlx_hf_curl.c
                                        src/hf/fmlx_hf_manifest.c)
target_include_directories(fmlx_hf_fetch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hf)
target_link_libraries(fmlx_hf_fetch PUBLIC fmlx_hf PRIVATE CURL::libcurl)

add_executable(test_hf_fetch tests/hf/test_hf_fetch.c)
target_link_libraries(test_hf_fetch PRIVATE fmlx_hf_fetch)
add_test(NAME hf_fetch COMMAND test_hf_fetch ${CMAKE_CURRENT_BINARY_DIR}/work)

# Il manifesto non tocca il disco: nessun work directory da passare, nessun cache da pulire.
add_executable(test_hf_manifest tests/hf/test_hf_manifest.c)
target_link_libraries(test_hf_manifest PRIVATE fmlx_hf_fetch)
add_test(NAME hf_manifest COMMAND test_hf_manifest)

# Sonda di rete VERA, non un test: due righe di README spiegano perché non sta in ctest.
add_executable(hf_fetch_probe tools/hf_fetch_probe.c)
target_link_libraries(hf_fetch_probe PRIVATE fmlx_hf_fetch)

# Anche questa è una sonda, non un test: una richiesta di metadati al server vero, zero byte di
# modello. Serve a provare la FORMA della risposta (che il test offline non può provare).
add_executable(hf_manifest_probe tools/hf_manifest_probe.c)
target_link_libraries(hf_manifest_probe PRIVATE fmlx_hf_fetch)

# Terza sonda, e la sola che parli con un server CHE SI FERMA: huggingface.co non lo si può far
# stare zitto a comando, e il watchdog di stall è esattamente la guardia che serve a un `pull` da
# 30 GB ripreso all'90%. Sta nel tempo (pochi secondi), quindi NON in ctest — vedi l'intestazione
# del file. Il path privato serve per il git blob id: calcolarsene uno proprio avrebbe significato
# avere due autorità sullo stesso nome di blob.
add_executable(hf_stall_probe tools/hf_stall_probe.c)
target_include_directories(hf_stall_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hf)
target_link_libraries(hf_stall_probe PRIVATE fmlx_hf_fetch)

# --- lane_json: il codec che tocca ogni byte in entrata e in uscita dalle API -------------
# Due file separati perché parser e writer hanno contratti diversi: il parser RIFIUTA con
# un codice per causa e un offset, il writer NON DEVE MAI produrre output a metà.
add_library(fmlx_json STATIC src/json/fmlx_json.c src/json/fmlx_jsonw.c)
target_include_directories(fmlx_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Il manifesto è JSON: la lane download dipende dal codec, e il codec non dipende da nessuno.
# (Qui e non nel blocco `fmlx_hf_fetch` sopra: un target si può linkare solo DOVE esiste.)
target_link_libraries(fmlx_hf_fetch PUBLIC fmlx_json)

add_executable(test_json tests/json/test_json.c)
target_link_libraries(test_json PRIVATE fmlx_json)
add_test(NAME json COMMAND test_json)

add_executable(test_jsonw tests/json/test_jsonw.c)
target_link_libraries(test_jsonw PRIVATE fmlx_json)
add_test(NAME jsonw COMMAND test_jsonw)

# Verdetto del parser su un input: è lo strumento con cui il parser è stato confrontato con
# `json.loads` (tools/json_oracle.py). Da solo NON è un test — stampa e basta.
add_executable(json_diff tools/json_diff.c)
target_link_libraries(json_diff PRIVATE fmlx_json)

# Il differenziale gira in ctest SOLO DOVE Python c'è: `find_package`Quiet, e il test si
# registra solo se l'interprete esiste. La macchina senza Python NON lo vede verde per
# assenza: semplicemente non c'è, e il numero di test in output lo dice.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME json_oracle
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/json_oracle.py diff
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/json/corpus.hex $<TARGET_FILE:json_diff>)
    set_tests_properties(json_oracle PROPERTIES
        PASS_REGULAR_EXPRESSION "divergenze dure 8, round-trip 0")
endif()

# Il motore (lane engine + la libreria condivisa `mlxengine`) è più in su, insieme al suo test.
# Le lane che non hanno ancora codice si aggiungono QUI quando esistono, non come target vuoti:
# oggi resta `lane_mlx` (i pesi packed, la cache KV, l'attenzione, il MoE, la generazione). La
# CLI c'è ed è in fondo a questo file.

# --- lane_http: il codec che legge i byte da internet PRIMA che arrivino al JSON ---------
# Separate dal JSON perché il livello è un altro: qui ciò che deve essere corretto è il
# FRAMING (dove finisce la richiesta), non il contenuto. Un server con questo codec e senza
# JSON regge connessioni; il contrario no.
add_library(fmlx_http STATIC src/server/fmlx_http.c)
target_include_directories(fmlx_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(test_http tests/http/test_http.c)
target_link_libraries(test_http PRIVATE fmlx_http)
add_test(NAME http COMMAND test_http)

# --- lane_api: il layer ② — router, catalogo errori, adapter OpenAI/Anthropic -----------
# Tre file perché due DIALETTI non stanno nello stesso file: il confine è quello che rende
# leggibile "questa regola è del protocollo" versus "questa è del nostro motore". Il quarto
# (fmlx_api.c) è ciò che i due dialetti DEVONO condividere, e che se duplicato li fa
# divergere: il catalogo errori, la rotta, l'accumulatore, il ponte arguments<->input.
add_library(fmlx_api STATIC
    src/server/fmlx_api.c
    src/server/fmlx_api_openai.c
    src/server/fmlx_api_anthropic.c)
target_include_directories(fmlx_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fmlx_api PUBLIC fmlx_json fmlx_http)

add_executable(test_api tests/api/test_api.c)
target_link_libraries(test_api PRIVATE fmlx_api)
add_test(NAME api COMMAND test_api)

# --- lane_serve: il layer ③ — le risposte e il loop TCP --------------------------------------
# DUE file, non uno, perché hanno due contratti diversi: `fmlx_serve_respond.c` è PURA (una
# richiesta già incorniciata entra, una risposta completa esce — si prova in memoria, senza socket,
# senza orologio, senza malloc), `fmlx_serve.c` è il LOOP (socket, segnali, byte fin dove la testata
# promette). Nello stesso file, l'unica parte provabile in-process dipenderebbe dalla build di
# quella che chiede una porta libera: e un test di framing diventerebbe un test di rete.
add_library(fmlx_serve STATIC src/server/fmlx_serve_respond.c src/server/fmlx_serve.c)
target_include_directories(fmlx_serve PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fmlx_serve PUBLIC fmlx_api fmlx_hf)

# Il work directory qui serve DAVVERO: `/v1/models` e `/health` leggono il cache da disco, quindi
# o il test costruisce un cache finto su disco, o sta provando un cammino che non ha mai corso.
add_executable(test_serve tests/server/test_serve.c)
target_link_libraries(test_serve PRIVATE fmlx_serve)
add_test(NAME serve COMMAND test_serve ${CMAKE_CURRENT_BINARY_DIR}/work-serve)

# --- lane_cli: il binario `fastmlx` (piano §10.3, §13) --------------------------------------
# Un file per verbo, non uno: `pull` è una lane con un report e delle regole sui path, `models` è
# una tabella letta da disco, l'argv è una terza cosa che cambia con motivi diversi (una bandiera
# nuova contro un ordine di download). Il `main` sta da solo perché è l'unica parte NON provabile
# in-process. `fmlx_pull_progress.c` è il QUARTO motivo: il disegno di una riga che si riscrive ha
# una sua severità (orologio, throttling, che cosa non stampare su una pipe) che non c'entra niente
# con l'ordine dei byte nel cache, e sta in un file che si prova da solo. `fmlx_doctor.c` è il
# QUINTO: `models` legge i NOMI, `doctor` verifica gli hash sui byte, e la differenza è un ordine di
# grandezza di costo — due verbi nello stesso file avrebbero indotto l'uno a pagare il lavoro
# dell'altro.
add_library(fmlx_cli STATIC src/cli/fmlx_pull.c src/cli/fmlx_models.c src/cli/fmlx_cli.c
                            src/cli/fmlx_pull_progress.c src/cli/fmlx_serve.c src/cli/fmlx_doctor.c)
target_include_directories(fmlx_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# `fmlx_serve` tra le dipendenze PUBBLICHE: il verbo c'è solo se il loop c'è, e un `serve` che
# compilasse senza il socket sarebbe il "comando che risponde di sì" che il piano vieta.
target_link_libraries(fmlx_cli PUBLIC fmlx_hf_fetch fmlx_serve)

add_executable(fastmlx src/cli/fastmlx_main.c)
target_link_libraries(fastmlx PRIVATE fmlx_cli)

# Il `pull` contro un server finto: cache reale su disco, byte finti, zero rete.
add_executable(test_pull tests/cli/test_pull.c)
target_link_libraries(test_pull PRIVATE fmlx_cli)
# `src/cli` in più sul path: il renderer di avanzamento si prova anche da solo, e il suo header è
# INTERNO — non è API della libreria, ed è esattamente la ragione per cui sta in `src` e non in
# `include`.
target_include_directories(test_pull PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
add_test(NAME pull COMMAND test_pull ${CMAKE_CURRENT_BINARY_DIR}/work-cli-pull)

# L'argv, il dispatch e la tabella di `models`: canali, bandiere rifiutate, verbi non ancora cablati.
# Il work directory serve solo a `models`, che legge un cache VERO costruito dal test (non c'è una
# rete da falsificare, qui: o si scrive su disco, o non si prova).
add_executable(test_cli tests/cli/test_cli.c)
target_link_libraries(test_cli PRIVATE fmlx_cli)
add_test(NAME cli COMMAND test_cli ${CMAKE_CURRENT_BINARY_DIR}/work-cli)

# Il `doctor` da CLI: i CANALI e l'uscita, non l'algoritmo (che è di `test_hf_audit`). Serve un cache
# con hash VERI, quindi anche qui il work directory è su disco: `fmlx_hf_verify` apre file, e un
# transport finto non ha niente da dire a un verbo che non tocca la rete.
add_executable(test_doctor tests/cli/test_doctor.c)
target_link_libraries(test_doctor PRIVATE fmlx_cli)
add_test(NAME doctor COMMAND test_doctor ${CMAKE_CURRENT_BINARY_DIR}/work-cli-doctor)
