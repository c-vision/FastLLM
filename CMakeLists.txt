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

add_library(fmlx_hf_fetch STATIC src/hf/fmlx_hf_fetch.c src/hf/fmlx_hf_curl.c)
target_include_directories(fmlx_hf_fetch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hf)
target_link_libraries(fmlx_hf_fetch PUBLIC fmlx_hf PRIVATE CURL::libcurl)

add_executable(test_hf_fetch tests/hf/test_hf_fetch.c)
target_link_libraries(test_hf_fetch PRIVATE fmlx_hf_fetch)
add_test(NAME hf_fetch COMMAND test_hf_fetch ${CMAKE_CURRENT_BINARY_DIR}/work)

# Sonda di rete VERA, non un test: due righe di README spiegano perché non sta in ctest.
add_executable(hf_fetch_probe tools/hf_fetch_probe.c)
target_link_libraries(hf_fetch_probe PRIVATE fmlx_hf_fetch)

# Le altre lane (engine MLX, server OpenAI/Anthropic, CLI) non hanno
# ancora codice: si aggiungono QUI quando esistono, non come target vuoti.
