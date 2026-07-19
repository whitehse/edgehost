/**
 * @file edge_state.h
 * @brief In-memory multi-namespace state store (P1.7a / P1.14 / ADR-007 / K10).
 *
 * Syscall-free after create/enable. Default: net.core + map.dynamic enabled;
 * net.pon, net.home, electric, inventory registered disabled. Config may
 * enable hooks when producers exist (P1.14). Values are UTF-8 JSON strings
 * (opaque to the store beyond size/JSON brace check).
 *
 * K10 / PR-2a: entry tables + value buffers allocate at namespace enable
 * (or create for default-enabled ns), not on put. Per-namespace max_keys;
 * disabled ns hold a name stub only (entries NULL, capacity 0 allocated).
 */
#ifndef EDGE_STATE_H
#define EDGE_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "edge_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_STATE_NS_MAX        32
#define EDGE_STATE_KEY_MAX       128
#define EDGE_STATE_VALUE_DEFAULT 4096
#define EDGE_STATE_VALUE_MAX     65536
#define EDGE_STATE_KEYS_DEFAULT  1024
#define EDGE_STATE_LIST_CAP      1000

typedef enum {
    EDGE_STATE_OK = 0,
    EDGE_STATE_NOT_FOUND,
    EDGE_STATE_DENIED,
    EDGE_STATE_TOO_LARGE,
    EDGE_STATE_NS_FULL,
    EDGE_STATE_NS_DISABLED,
    EDGE_STATE_BAD_KEY,
    EDGE_STATE_BAD_JSON,
    EDGE_STATE_BAD_NS,
    EDGE_STATE_NOMEM
} edge_state_err_t;

typedef struct edge_state_store edge_state_store_t;

typedef struct {
    size_t max_keys_per_ns;   /* 0 → EDGE_STATE_KEYS_DEFAULT (store default) */
    size_t max_value_bytes;   /* 0 → EDGE_STATE_VALUE_DEFAULT */
} edge_state_config_t;

edge_state_config_t edge_state_default_config(void);

edge_state_store_t *edge_state_create(void);
edge_state_store_t *edge_state_create_with_config(const edge_state_config_t *cfg);
void                edge_state_destroy(edge_state_store_t *st);

/**
 * Enable/disable namespace (unknown ns names still registered as disabled stub).
 * Enable allocates the entry table (eager value buffers) for that ns using its
 * configured capacity. Disable frees the table (RSS reclaim; keys discarded).
 * @return 0 ok, -1 on unknown error / OOM.
 */
int edge_state_ns_set_enabled(edge_state_store_t *st, const char *ns, int enabled);
int edge_state_ns_enabled(const edge_state_store_t *st, const char *ns);

/**
 * Set per-namespace key capacity (eager table size when next enabled).
 * @p max_keys 0 → store default (max_keys_per_ns from create).
 * Safe before enable; if the table is already allocated and size differs,
 * frees and re-allocates (data discarded). Call before/at enable ideally.
 * Unknown ns is registered disabled with the given capacity.
 * @return 0 ok, -1 on error / OOM.
 */
int edge_state_ns_set_capacity(edge_state_store_t *st, const char *ns,
                               size_t max_keys);

/**
 * Configured max keys for @p ns (store default if ns unknown or unset).
 * Does not imply the table is allocated.
 */
size_t edge_state_ns_max_keys(const edge_state_store_t *st, const char *ns);

/**
 * Allocated entry slots for @p ns (0 if table not allocated — disabled stub).
 */
size_t edge_state_ns_capacity(const edge_state_store_t *st, const char *ns);

/**
 * Apply namespace enable flags + per-ns max_keys from @p cfg onto @p st (P1.14 /
 * K10). Safe to call for owned or external stores; no-op if either arg is NULL.
 * Capacities are applied before enable so enable-time alloc uses the right size.
 * Also usable for `ext.*` namespaces via edge_state_ns_set_enabled directly.
 */
void edge_state_apply_config(edge_state_store_t *st, const edge_config_t *cfg);

/**
 * Build edge_state_config_t from edge_config_t (max_keys_default /
 * max_value_bytes). Use with edge_state_create_with_config before apply_config.
 */
edge_state_config_t edge_state_config_from_edge_config(const edge_config_t *cfg);

edge_state_err_t edge_state_put(edge_state_store_t *st, const char *ns,
                                const char *key, const char *value,
                                size_t value_len);

/**
 * Get value. Writes value_len bytes into out (not necessarily NUL-terminated
 * unless value was). Sets *out_len.
 */
edge_state_err_t edge_state_get(const edge_state_store_t *st, const char *ns,
                                const char *key, char *out, size_t out_cap,
                                size_t *out_len);

edge_state_err_t edge_state_delete(edge_state_store_t *st, const char *ns,
                                   const char *key);

/**
 * List keys with optional prefix. Writes up to max_keys into keys[][] .
 * @return number of keys written, or -1 on bad ns.
 */
int edge_state_list(const edge_state_store_t *st, const char *ns,
                    const char *prefix, char keys[][EDGE_STATE_KEY_MAX],
                    size_t max_keys);

size_t edge_state_count(const edge_state_store_t *st, const char *ns);

const char *edge_state_err_name(edge_state_err_t err);

/** Validate key charset [a-z0-9_./:-]{1,128}. */
int edge_state_key_valid(const char *key);

/** Light JSON object/array/primitive check for v1. */
int edge_state_json_ok(const char *value, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_STATE_H */
