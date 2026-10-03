cmake_minimum_required(VERSION 3.20)

# NAX (tensor unit M5) è la leva del prefill: MLX costruisce quei kernel SOLO con deployment
# target >= 26.2, e li salta (definendo MLX_METAL_NO_NAX) sotto. Pinnare >=26.2 è la scelta giusta
# PER PRESTAZIONI: la metallib include i kernel NAX e il prefill usa le tensor unit. Il fix
# Metal-4.1 dell'era migrazione (MLX patch #3963, `libs/mlx-deps`) rende i kernel NAX compilabili
# con il toolchain corrente, quindi 26.2 ORA compila (verificato: `build-gmbasic` NAX OK).
# Storicamente era "15.0" per SALTARE NAX quando non compilava: NON farlo più (prefill ~5x).
# Un valore passato sulla riga di comando vince (il blocco è condizionale).
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.2" CACHE STRING "macOS deployment target (NAX abilitato)")
endif()

project(FastLLM LANGUAGES C)

# Il codec GGUF è UNA SOLA SORGENTE in LLMEnv: FastLLM lo CONSUMA. Progetto CMake autonomo
# fratello (nessuna copia nel repo). Si aggancia QUI, PRIMA di `add_compile_options(-Werror)`,
# così i sorgenti di LLMCodec non ereditano la politica di warning di FastLLM.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMCodec ${CMAKE_BINARY_DIR}/llmcodec)

# --- motore di inferenza: il tokenizer e l'engine MLX di LLMEnv -------------------------------
# L'engine vive in LLMEnv/LLMEngine/mlx (target `llm_ai_mlx`), che a sua volta aggancia LLMCodec
# (già presente) e LLMEngine/moe. Si aggancia QUI, PRIMA di `add_compile_options(-Werror)`, così
# i sorgenti del motore non ereditano la politica di warning di FastLLM (stesso motivo del codec).
# Il `set(LLMENV_BUILD_ENGINE ON)` documenta l'intenzione del superbuild di LLMEnv; l'aggancio è
# DIRETTO sulla sottocartella perché il superbuild (plugin+moe+llama+mlx) colliderebbe sul target
# `llmengine_plugin` (LLMModelIO lo aggiunge prima del blocco engine) e tirerebbe dentro anche
# llama.cpp, che questa tappa non usa. Il target del motore resta UNO: `llm_ai_mlx`.
set(LLMENV_BUILD_ENGINE ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMToken ${CMAKE_BINARY_DIR}/llmtoken)
# Motore Jinja per i chat-template (`LLMJinja`: port di GMBasic/native/gmb_jinja). Layer
# autonomo di LLMEnv; FastLLM lo consuma per rendere il `chat_template` reale del modello.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMJinja ${CMAKE_BINARY_DIR}/llmjinja)

# MLX sceglie DOVE generare `mlx.metallib` con la variabile `MLX_METAL_PATH` (default:
# `<mlx-build>/mlx/backend/metal/kernels/`). A runtime MLX prova PRIMA il metallib COLOCATED
# accanto al binario e solo dopo il path assoluto `METAL_PATH` (= `${MLX_METAL_PATH}/mlx.metallib`).
# Col default, se in `build-release/` resta un `mlx.metallib` vecchio (es. pre-NAX, senza i kernel
# `steel_attention_*_bq64_*`) quel file SHADOWA il metallib giusto e OGNI forward fallisce con
# "function not found". Fissando `MLX_METAL_PATH` alla radice del build, MLX genera il metallib
# COMPLETO direttamente accanto al binario: colocated e `METAL_PATH` sono lo STESSO file, quindi
# non può esistere un doppio disallineato. Va impostato PRIMA di add_subdirectory(LLMEngine/mlx),
# così `FetchContent` di mlx lo vede (variabile CACHE = globale).
set(MLX_METAL_PATH "${CMAKE_BINARY_DIR}" CACHE PATH
    "Dir dove MLX genera e carica mlx.metallib (colocated col binario)")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMEngine/mlx ${CMAKE_BINARY_DIR}/llmengine_mlx)
# Lane VISION/OCR: il motore llama.cpp/mtmd di LLMEnv (`llm_ai_llamacpp`). L'engine MLX ha
# `vision_* = NULL`, quindi i modelli multimodali (VLM/OCR/ASR) passano da QUI. Nessuna collisione
# di target con `llm_ai_mlx` (llama.cpp non dipende dal codec).
if(APPLE)
  # `LLMEngine/llama` contiene sorgenti Objective-C (Apple Vision): senza OBJC la generazione
  # fallisce con `CMAKE_OBJC_COMPILE_OBJECT` non definito.
  enable_language(OBJC)
endif()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMEngine/llama ${CMAKE_BINARY_DIR}/llmengine_llama)

# MLX carica `mlx.metallib` a runtime dal path ASSOLUTO compilato in libmlx (target `mlx-metallib`
# di MLX). Senza il file, OGNI forward Metal fallisce. Il target esiste, ma MLX lo lascia
# SCOLLEGATO di proposito (vedi il commento in kernels/CMakeLists.txt): qui lo riagganciamo,
# perché per FastLLM "il motore compila" deve significare "il motore parte". Grazie a
# `MLX_METAL_PATH=${CMAKE_BINARY_DIR}` (sopra) il file generato è COLOCATED col binario.
add_dependencies(llm_ai_mlx mlx-metallib)


# Le estensioni GNU/POSIX servono: mmap/open/lseek non sono visibili con -std=c17 puro.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)

add_compile_options(-Wall -Wextra)
option(FASTLLM_WERROR "Tratta gli avvisi come errori" ON)
if(FASTLLM_WERROR)
  add_compile_options(-Werror)
endif()

# Le lane statiche finiscono dentro le librerie linkate ai binari/test di FastLLM e nei plugin
# del codec condiviso: object position-independent, dichiarato una volta qui e non con una
# proprietà per target (che è il modo in cui una lane nuova nasce linkabile a metà).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- codec GGUF: FastLLM CONSUMA `llmcodec` (LLMEnv), non ne tiene una copia ----------------
# Il parsing GGUF, la TASSONOMIA d'ERRORE e il dequant dei tipi ggml sono UNA SOLA SORGENTE in
# `LLMEnv/LLMCodec` (agganciato come subdirectory in cima a questo file). La vecchia lane
# `src/lane_gguf/` e l'ABI `mlxengine_*` sono state ELIMINATE. `llmcodec` è il core (contenitori,
# parser, rolemap); `llmcodec_ggml` è il plugin che implementa i codec ggml (dequant/encode).

enable_testing()

# Il consumo del codec: scrive un GGUF col writer di LLMCodec, lo rilegge col parser (con la
# tassonomia d'errore) e dequantizza un blocco. Se il link a `llmcodec` si rompe, questo test
# non compila: è la prova che FastLLM non ha più un codec proprio.
add_executable(test_codec tests/gguf/test_codec.c)
target_link_libraries(test_codec PRIVATE llmcodec llmcodec_ggml)
add_test(NAME codec COMMAND test_codec)

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

# Il codec GGUF è più in su: FastLLM CONSUMA `llmcodec` (LLMEnv) col suo test (`codec`).
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
    src/server/fmlx_api_openai_text.c
    src/server/fmlx_api_openai_embeddings.c
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
add_library(fmlx_serve STATIC src/server/fmlx_serve_respond.c src/server/fmlx_serve_admin.c
                             src/server/fmlx_serve_metrics.c
                             src/server/fmlx_serve.c src/engine/fmlx_engine.c
                             src/engine/fmlx_engine_gen.c src/engine/fmlx_tools.c
                             src/engine/fmlx_vision.c src/server/fmlx_serve_vision.c)
target_include_directories(fmlx_serve PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                              PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                      ${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMEngine/include)
# Il motore è una dipendenza PUBBLICA: chi linka `fmlx_serve` linka anche `llm_ai_mlx` e
# `llmtoken` (dylib nel build tree). La generazione non è opzionale in questa lane: un
# `fastllm serve` che non potesse generare sarebbe il comando-che-risponde-di-sì.
target_link_libraries(fmlx_serve PUBLIC fmlx_api fmlx_hf llm_ai_mlx llmtoken llmjinja llm_ai_llamacpp)

# Il work directory qui serve DAVVERO: `/v1/models` e `/health` leggono il cache da disco, quindi
# o il test costruisce un cache finto su disco, o sta provando un cammino che non ha mai corso.
add_executable(test_serve tests/server/test_serve.c)
target_link_libraries(test_serve PRIVATE fmlx_serve)
add_test(NAME serve COMMAND test_serve ${CMAKE_CURRENT_BINARY_DIR}/work-serve)

# Lane VISION/OCR: punto d'ingresso testabile. Carica un VLM (GGUF testo + GGUF `mmproj`) e legge
# un'immagine (o un audio) via il motore llama.cpp/mtmd. Linka `fmlx_serve` che a sua volta porta
# `llm_ai_llamacpp` (la lane vision) e `llm_ai_mlx`.
add_executable(vision_ocr tools/vision_ocr.c)
target_include_directories(vision_ocr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vision_ocr PRIVATE fmlx_serve)
add_dependencies(vision_ocr llm_ai_llamacpp)

# --- lane_tools: il riconoscitore delle chiamate di tool (PURO) ------------------------------
# Testo -> chiamate, senza motore e senza rete: è il pezzo che si prova quando il modello non
# c'è. Il caso che vale il file è il marcatore spezzato fra due delta di token, che non deve
# mai uscire come testo.
add_executable(test_tools tests/engine/test_tools.c src/engine/fmlx_tools.c)
target_include_directories(test_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_tools PRIVATE fmlx_json)
add_test(NAME tools COMMAND test_tools)

# --- lane_cli: il binario `fastllm` (piano §10.3, §13) --------------------------------------
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
                            src/cli/fmlx_pull_progress.c src/cli/fmlx_serve.c src/cli/fmlx_doctor.c
                            src/cli/fmlx_gen.c src/cli/fmlx_run.c src/cli/fmlx_chat.c
                            src/cli/fmlx_ps.c src/cli/fmlx_banner.c src/cli/fmlx_registry.c)
target_include_directories(fmlx_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                      ${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMEngine/mlx
                                      ${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/LLMEngine/Options
                                      ${CMAKE_CURRENT_SOURCE_DIR}/../LLMEnv/libs/mlx-deps/mlx-c-src
                            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# `fmlx_serve` tra le dipendenze PUBBLICHE: il verbo c'è solo se il loop c'è, e un `serve` che
# compilasse senza il socket sarebbe il "comando che risponde di sì" che il piano vieta. Da lì
# arrivano anche `llm_ai_mlx`/`llmtoken`/`llmjinja`, cioè il motore di cui `run`/`chat` si servono.
target_link_libraries(fmlx_cli PUBLIC fmlx_hf_fetch fmlx_serve)

add_executable(fastllm src/cli/fastllm_main.c)
target_link_libraries(fastllm PRIVATE fmlx_cli)

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
