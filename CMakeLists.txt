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

# Le altre lane (engine MLX, server OpenAI/Anthropic, CLI, download HF) non hanno
# ancora codice: si aggiungono QUI quando esistono, non come target vuoti.
