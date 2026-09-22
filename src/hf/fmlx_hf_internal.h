/* fmlx_hf_internal.h —interno alla lane hf: hash streaming e piccoli helper di path.
 *
 * Questo header NON è superficie pubblica: `include/fmlx/fmlx_hf.h` resta l'unico ABI.
 * Qui c'è il motivo per cui CommonCrypto non entra nell'header pubblico: i contesti di
 * hash sono opachi per chi chiama, così un cambiamento di backend (o una piattaforma
 * senza CommonCrypto) non tocca nessun caller.
 */
#ifndef FMLX_HF_INTERNAL_H
#define FMLX_HF_INTERNAL_H

#include "fmlx/fmlx_hf.h"

#define FMLX_HF_HEX_MAX 65 /* 64 + NUL */

/* Avanza `fd` fino alla fine calcolando l'hash richiesto da `kind`.
 * `git_header_size`: per FMLX_HF_HASH_GIT_BLOB è la lunghezza del contenuto da mettere
 * nell'header `blob <len>\0` (i 40 hex del cache sono il git blob id, non lo SHA1 nudo:
 * misurato, vedi il commento in fmlx_hf.h); per gli altri tipi è ignorato e passa 0.
 * Ritorna 0 e scrive `hex` (minuscole) in FMLX_HF_HEX_MAX, oppure -1 con errno impostato.
 * `*out_bytes` (se non NULL) conta i byte letti DAL FILE, header escluso. */
int fmlx_hf_hash_fd(int fd, FmlxHfHashKind kind, int64_t git_header_size, char *hex,
                    int64_t *out_bytes);

/* Unisce due path con "/", o FMLX_HF_ERR_BUF_SMALL (e azzera `out` in quel caso). */
FmlxHfStatus fmlx_hf_pjoin(char *out, size_t n, const char *a, const char *b);

/* Vero se `name` può essere il nome di un blob: UN solo segmento, non ".", "..", lungo meno di
 * FMLX_HF_BLOB_NAME_MAX, senza caratteri di controllo né backslash. Unica regola per le due
 * fonti ostili: il nome letto da un symlink (locate) e l'etag dato dal server (fetch). */
int fmlx_hf_name_is_safe(const char *name);

/* Come fmlx_hf_url, ma con l'endpoint esplicito: è il template in un posto solo, così
 * FmlxHfFetchOpts.endpoint non introduce mai una seconda copia della formato. */
FmlxHfStatus fmlx_hf_url_with(const char *endpoint, const char *repo_id, const char *revision,
                              const char *relpath, int use_resolve, char *out, size_t out_len);

/* Messaggio d'errore + codice: unico modo per concludere una funzione di questa lane. */
FmlxHfStatus fmlx_hf_fail(FmlxHfStatus s, const char *fmt, ...);

#endif /* FMLX_HF_INTERNAL_H */
