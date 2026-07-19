/**
 * @file edge_static.h
 * @brief Safe static file resolve/read under doc roots (P1.6).
 *
 * Host-only (file I/O). Rejects path traversal. Used for SPA root and
 * libwebmap package path.
 */
#ifndef EDGE_STATIC_H
#define EDGE_STATIC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default max file size served in-process (P1.6). */
#define EDGE_STATIC_MAX_FILE (64u * 1024u)

/**
 * Join @p root and URL @p url_path into a filesystem path in @p out.
 * Rejects empty root, "..", absolute escapes, NUL in path.
 *
 * @p url_path may include a leading '/' and optional ?query (stripped).
 * @return 0 ok, -1 unsafe or overflow.
 */
int edge_static_resolve(const char *root, const char *url_path, char *out,
                        size_t out_sz);

/**
 * Map file extension to Content-Type (static table).
 * Writes into @p ctype (always NUL-terminated on success).
 */
void edge_static_content_type(const char *path, char *ctype, size_t ctype_sz);

/**
 * Read entire file if size <= max_bytes into @p buf.
 * @return bytes read (>=0), -1 on error / too large / not a regular file.
 */
int edge_static_read_file(const char *path, void *buf, size_t max_bytes,
                          size_t *out_len);

/**
 * Resolve + read under root. On success fills @p buf, @p out_len, @p ctype.
 * @return 0 ok, -1 not found / unsafe / too large.
 */
int edge_static_load(const char *root, const char *url_path, void *buf,
                     size_t max_bytes, size_t *out_len, char *ctype,
                     size_t ctype_sz);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_STATIC_H */
