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

# --- lane_gguf: il lettore GGUF di riferimento (nessun cap, allineamento onorato) ----
add_library(fmlx_gguf STATIC src/lane_gguf/fmlx_gguf.c src/lane_gguf/fmlx_quant.c)
target_include_directories(fmlx_gguf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

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

# --- lane_hf: cache Hugging Face letto senza rete (il download non ha ancora codice) ---------
add_library(fmlx_hf STATIC src/hf/fmlx_hf_cache.c src/hf/fmlx_hf_hash.c)
target_include_directories(fmlx_hf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hf)

add_executable(test_hf_cache tests/hf/test_hf_cache.c)
target_link_libraries(test_hf_cache PRIVATE fmlx_hf)
add_test(NAME hf_cache COMMAND test_hf_cache ${CMAKE_CURRENT_BINARY_DIR}/work)

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

# Le altre lane (engine MLX, il `serve` esposto su TCP) non hanno ancora codice: si aggiungono
# QUI quando esistono, non come target vuoti. La CLI c'è ed è in fondo a questo file.

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

# --- lane_cli: il binario `fastmlx` (piano §10.3, §13) --------------------------------------
# Due file, non uno: `pull` è una lane con un report e delle regole sui path, l'argv è un'altra
# cosa che cambia con motivi diversi (una bandiera nuova contro un ordine di download). Il `main`
# sta da solo perché è l'unica parte NON provabile in-process.
add_library(fmlx_cli STATIC src/cli/fmlx_pull.c src/cli/fmlx_cli.c)
target_include_directories(fmlx_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fmlx_cli PUBLIC fmlx_hf_fetch)

add_executable(fastmlx src/cli/fastmlx_main.c)
target_link_libraries(fastmlx PRIVATE fmlx_cli)

# Il `pull` contro un server finto: cache reale su disco, byte finti, zero rete.
add_executable(test_pull tests/cli/test_pull.c)
target_link_libraries(test_pull PRIVATE fmlx_cli)
add_test(NAME pull COMMAND test_pull ${CMAKE_CURRENT_BINARY_DIR}/work-cli-pull)

# L'argv e il dispatch: canali, bandiere rifiutate, verbi non ancora cablati.
add_executable(test_cli tests/cli/test_cli.c)
target_link_libraries(test_cli PRIVATE fmlx_cli)
add_test(NAME cli COMMAND test_cli)
