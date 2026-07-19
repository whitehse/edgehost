/**
 * @file state_store.c
 * @brief Multi-namespace key/value store (P1.7a / K10).
 *
 * Create/enable-time alloc only: value buffers are calloc'd when a namespace
 * table is allocated (enable or create for default-enabled ns). Put path never
 * mallocs value buffers (ADR-007). Disabled namespaces are name stubs only.
 */

#include "edge_state.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int  used;
    char key[EDGE_STATE_KEY_MAX];
    char *value;
    size_t value_len;
} edge_state_entry_t;

typedef struct {
    char name[64];
    int  enabled;
    int  registered;
    edge_state_entry_t *entries; /* NULL until enable/create allocates */
    size_t max_keys;             /* configured cap (0 → store default) */
    size_t capacity;             /* allocated slots; 0 if entries == NULL */
    size_t count;
} edge_state_ns_t;

struct edge_state_store {
    edge_state_ns_t ns[EDGE_STATE_NS_MAX];
    size_t          n_ns;
    size_t          max_keys;  /* store-wide default */
    size_t          max_value;
};

edge_state_config_t edge_state_default_config(void)
{
    edge_state_config_t c;
    c.max_keys_per_ns = EDGE_STATE_KEYS_DEFAULT;
    c.max_value_bytes = EDGE_STATE_VALUE_DEFAULT;
    return c;
}

edge_state_config_t edge_state_config_from_edge_config(const edge_config_t *cfg)
{
    edge_state_config_t c = edge_state_default_config();
    if (!cfg) {
        return c;
    }
    if (cfg->state_max_keys_default > 0) {
        c.max_keys_per_ns = cfg->state_max_keys_default;
    }
    if (cfg->state_max_value_bytes > 0) {
        c.max_value_bytes = cfg->state_max_value_bytes;
        if (c.max_value_bytes > EDGE_STATE_VALUE_MAX) {
            c.max_value_bytes = EDGE_STATE_VALUE_MAX;
        }
    }
    return c;
}

const char *edge_state_err_name(edge_state_err_t err)
{
    switch (err) {
    case EDGE_STATE_OK:          return "OK";
    case EDGE_STATE_NOT_FOUND:   return "NOT_FOUND";
    case EDGE_STATE_DENIED:      return "DENIED";
    case EDGE_STATE_TOO_LARGE:   return "TOO_LARGE";
    case EDGE_STATE_NS_FULL:     return "NS_FULL";
    case EDGE_STATE_NS_DISABLED: return "NS_DISABLED";
    case EDGE_STATE_BAD_KEY:     return "BAD_KEY";
    case EDGE_STATE_BAD_JSON:    return "BAD_JSON";
    case EDGE_STATE_BAD_NS:      return "BAD_NS";
    case EDGE_STATE_NOMEM:       return "NOMEM";
    default:                     return "UNKNOWN";
    }
}

int edge_state_key_valid(const char *key)
{
    size_t i, n;
    if (!key || !key[0]) {
        return 0;
    }
    n = strlen(key);
    if (n > EDGE_STATE_KEY_MAX - 1) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)key[i];
        if (!(islower(c) || isdigit(c) || c == '_' || c == '.' || c == '/' ||
              c == ':' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

int edge_state_json_ok(const char *value, size_t len)
{
    size_t i = 0;
    if (!value || len == 0) {
        return 0;
    }
    while (i < len && isspace((unsigned char)value[i])) {
        i++;
    }
    if (i >= len) {
        return 0;
    }
    /* Accept object, array, string, number, true/false/null — cheap check. */
    {
        char c = value[i];
        if (c == '{' || c == '[' || c == '"' || c == '-' ||
            (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
            return 1;
        }
    }
    return 0;
}

static edge_state_ns_t *find_ns(edge_state_store_t *st, const char *ns)
{
    size_t i;
    if (!st || !ns) {
        return NULL;
    }
    for (i = 0; i < st->n_ns; i++) {
        if (st->ns[i].registered && strcmp(st->ns[i].name, ns) == 0) {
            return &st->ns[i];
        }
    }
    return NULL;
}

static edge_state_ns_t *find_ns_const(const edge_state_store_t *st, const char *ns)
{
    return find_ns((edge_state_store_t *)st, ns);
}

static size_t resolve_max_keys(const edge_state_store_t *st, size_t max_keys)
{
    if (max_keys > 0) {
        return max_keys;
    }
    return st->max_keys > 0 ? st->max_keys : EDGE_STATE_KEYS_DEFAULT;
}

/** Free entry table; keep max_keys for re-enable. */
static void ns_free_table(edge_state_ns_t *n)
{
    size_t j;
    if (!n || !n->entries) {
        return;
    }
    for (j = 0; j < n->capacity; j++) {
        free(n->entries[j].value);
        n->entries[j].value = NULL;
    }
    free(n->entries);
    n->entries = NULL;
    n->capacity = 0;
    n->count = 0;
}

/**
 * Eager-allocate entry array + value buffers (enable/create-time only).
 * Uses n->max_keys (0 → store default). No-op if already allocated at same size.
 */
static int ns_alloc_table(edge_state_store_t *st, edge_state_ns_t *n)
{
    size_t i;
    size_t cap;

    if (!st || !n) {
        return -1;
    }
    cap = resolve_max_keys(st, n->max_keys);
    if (n->entries && n->capacity == cap) {
        return 0;
    }
    if (n->entries) {
        ns_free_table(n);
    }
    n->entries = (edge_state_entry_t *)calloc(cap, sizeof(*n->entries));
    if (!n->entries) {
        n->capacity = 0;
        return -1;
    }
    for (i = 0; i < cap; i++) {
        n->entries[i].value = (char *)calloc(1, st->max_value + 1);
        if (!n->entries[i].value) {
            size_t j;
            for (j = 0; j < i; j++) {
                free(n->entries[j].value);
            }
            free(n->entries);
            n->entries = NULL;
            n->capacity = 0;
            return -1;
        }
    }
    n->capacity = cap;
    n->count = 0;
    return 0;
}

static int register_ns(edge_state_store_t *st, const char *name, int enabled,
                       size_t max_keys)
{
    edge_state_ns_t *n;

    if (!st || !name || !name[0] || strlen(name) >= 64) {
        return -1;
    }
    n = find_ns(st, name);
    if (n) {
        n->max_keys = max_keys;
        if (enabled) {
            if (ns_alloc_table(st, n) != 0) {
                return -1;
            }
            n->enabled = 1;
        } else {
            n->enabled = 0;
            ns_free_table(n);
        }
        return 0;
    }
    if (st->n_ns >= EDGE_STATE_NS_MAX) {
        return -1;
    }
    n = &st->ns[st->n_ns];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->registered = 1;
    n->max_keys = max_keys;
    n->entries = NULL;
    n->capacity = 0;
    n->count = 0;
    n->enabled = 0;
    if (enabled) {
        if (ns_alloc_table(st, n) != 0) {
            return -1;
        }
        n->enabled = 1;
    }
    st->n_ns++;
    return 0;
}

edge_state_store_t *edge_state_create(void)
{
    edge_state_config_t c = edge_state_default_config();
    return edge_state_create_with_config(&c);
}

edge_state_store_t *edge_state_create_with_config(const edge_state_config_t *cfg)
{
    edge_state_store_t *st;
    edge_state_config_t c;

    c = edge_state_default_config();
    if (cfg) {
        if (cfg->max_keys_per_ns > 0) {
            c.max_keys_per_ns = cfg->max_keys_per_ns;
        }
        if (cfg->max_value_bytes > 0) {
            c.max_value_bytes = cfg->max_value_bytes;
            if (c.max_value_bytes > EDGE_STATE_VALUE_MAX) {
                c.max_value_bytes = EDGE_STATE_VALUE_MAX;
            }
        }
    }

    st = (edge_state_store_t *)calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    st->max_keys = c.max_keys_per_ns;
    st->max_value = c.max_value_bytes;

    /* v1 fully enabled namespaces — allocate tables now */
    if (register_ns(st, "net.core", 1, 0) != 0 ||
        register_ns(st, "map.dynamic", 1, 0) != 0) {
        edge_state_destroy(st);
        return NULL;
    }
    /* Registered disabled hooks — name stubs only, no entry tables (K10) */
    if (register_ns(st, "net.pon", 0, 0) != 0 ||
        register_ns(st, "net.home", 0, 0) != 0 ||
        register_ns(st, "electric", 0, 0) != 0 ||
        register_ns(st, "inventory", 0, 0) != 0) {
        edge_state_destroy(st);
        return NULL;
    }

    return st;
}

void edge_state_destroy(edge_state_store_t *st)
{
    size_t i;
    if (!st) {
        return;
    }
    for (i = 0; i < st->n_ns; i++) {
        ns_free_table(&st->ns[i]);
    }
    free(st);
}

int edge_state_ns_set_capacity(edge_state_store_t *st, const char *ns,
                               size_t max_keys)
{
    edge_state_ns_t *n;
    size_t cap;

    if (!st || !ns || !ns[0] || strlen(ns) >= 64) {
        return -1;
    }
    cap = max_keys; /* 0 means "use store default" when allocating */
    n = find_ns(st, ns);
    if (!n) {
        return register_ns(st, ns, 0, cap);
    }
    if (n->max_keys == cap && n->entries &&
        n->capacity == resolve_max_keys(st, cap)) {
        return 0;
    }
    n->max_keys = cap;
    if (n->entries) {
        int was_enabled = n->enabled;
        ns_free_table(n);
        if (was_enabled) {
            if (ns_alloc_table(st, n) != 0) {
                n->enabled = 0;
                return -1;
            }
        }
    }
    return 0;
}

int edge_state_ns_set_enabled(edge_state_store_t *st, const char *ns, int enabled)
{
    edge_state_ns_t *n = find_ns(st, ns);
    if (!n) {
        return register_ns(st, ns, enabled, 0);
    }
    if (enabled) {
        if (ns_alloc_table(st, n) != 0) {
            return -1;
        }
        n->enabled = 1;
    } else {
        n->enabled = 0;
        /* Free on disable to reclaim RSS (PR-2a / K10). */
        ns_free_table(n);
    }
    return 0;
}

int edge_state_ns_enabled(const edge_state_store_t *st, const char *ns)
{
    const edge_state_ns_t *n = find_ns_const(st, ns);
    return n && n->enabled;
}

size_t edge_state_ns_max_keys(const edge_state_store_t *st, const char *ns)
{
    const edge_state_ns_t *n = find_ns_const(st, ns);
    if (!st) {
        return 0;
    }
    if (!n) {
        return st->max_keys;
    }
    return resolve_max_keys(st, n->max_keys);
}

size_t edge_state_ns_capacity(const edge_state_store_t *st, const char *ns)
{
    const edge_state_ns_t *n = find_ns_const(st, ns);
    if (!n || !n->entries) {
        return 0;
    }
    return n->capacity;
}

void edge_state_apply_config(edge_state_store_t *st, const edge_config_t *cfg)
{
    if (!st || !cfg) {
        return;
    }
    /* Capacities first so enable-time alloc uses configured sizes (K10). */
    (void)edge_state_ns_set_capacity(st, "net.core", cfg->state_net_core_max_keys);
    (void)edge_state_ns_set_capacity(st, "map.dynamic",
                                     cfg->state_map_dynamic_max_keys);
    (void)edge_state_ns_set_capacity(st, "net.pon", cfg->state_net_pon_max_keys);
    (void)edge_state_ns_set_capacity(st, "net.home",
                                     cfg->state_net_home_max_keys);
    (void)edge_state_ns_set_capacity(st, "electric",
                                     cfg->state_electric_max_keys);
    (void)edge_state_ns_set_capacity(st, "inventory",
                                     cfg->state_inventory_max_keys);

    (void)edge_state_ns_set_enabled(st, "net.core", cfg->state_net_core_enabled);
    (void)edge_state_ns_set_enabled(st, "map.dynamic",
                                    cfg->state_map_dynamic_enabled);
    (void)edge_state_ns_set_enabled(st, "net.pon", cfg->state_net_pon_enabled);
    (void)edge_state_ns_set_enabled(st, "net.home", cfg->state_net_home_enabled);
    (void)edge_state_ns_set_enabled(st, "electric", cfg->state_electric_enabled);
    (void)edge_state_ns_set_enabled(st, "inventory",
                                    cfg->state_inventory_enabled);
}

static edge_state_entry_t *find_entry(edge_state_ns_t *n, const char *key)
{
    size_t i;
    if (!n || !n->entries) {
        return NULL;
    }
    for (i = 0; i < n->capacity; i++) {
        if (n->entries[i].used && strcmp(n->entries[i].key, key) == 0) {
            return &n->entries[i];
        }
    }
    return NULL;
}

static edge_state_entry_t *find_free(edge_state_ns_t *n)
{
    size_t i;
    if (!n || !n->entries) {
        return NULL;
    }
    for (i = 0; i < n->capacity; i++) {
        if (!n->entries[i].used) {
            return &n->entries[i];
        }
    }
    return NULL;
}

edge_state_err_t edge_state_put(edge_state_store_t *st, const char *ns,
                                const char *key, const char *value,
                                size_t value_len)
{
    edge_state_ns_t *n;
    edge_state_entry_t *e;

    if (!st || !ns || !key || !value) {
        return EDGE_STATE_BAD_NS;
    }
    n = find_ns(st, ns);
    if (!n) {
        return EDGE_STATE_BAD_NS;
    }
    if (!n->enabled) {
        return EDGE_STATE_NS_DISABLED;
    }
    if (!n->entries) {
        /* Enabled without table should not happen; treat as OOM, not malloc. */
        return EDGE_STATE_NOMEM;
    }
    if (!edge_state_key_valid(key)) {
        return EDGE_STATE_BAD_KEY;
    }
    if (value_len > st->max_value) {
        return EDGE_STATE_TOO_LARGE;
    }
    if (!edge_state_json_ok(value, value_len)) {
        return EDGE_STATE_BAD_JSON;
    }

    e = find_entry(n, key);
    if (!e) {
        e = find_free(n);
        if (!e) {
            return EDGE_STATE_NS_FULL;
        }
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->used = 1;
        n->count++;
    }
    /* Pre-allocated value buffer — no put-path malloc (ADR-007). */
    memcpy(e->value, value, value_len);
    e->value[value_len] = '\0';
    e->value_len = value_len;
    return EDGE_STATE_OK;
}

edge_state_err_t edge_state_get(const edge_state_store_t *st, const char *ns,
                                const char *key, char *out, size_t out_cap,
                                size_t *out_len)
{
    const edge_state_ns_t *n;
    size_t i;

    if (!st || !ns || !key) {
        return EDGE_STATE_BAD_NS;
    }
    n = find_ns_const(st, ns);
    if (!n) {
        return EDGE_STATE_BAD_NS;
    }
    if (!n->enabled) {
        return EDGE_STATE_NS_DISABLED;
    }
    if (!edge_state_key_valid(key)) {
        return EDGE_STATE_BAD_KEY;
    }
    if (!n->entries) {
        return EDGE_STATE_NOT_FOUND;
    }
    for (i = 0; i < n->capacity; i++) {
        if (n->entries[i].used && strcmp(n->entries[i].key, key) == 0) {
            if (out && out_cap > 0) {
                size_t copy = n->entries[i].value_len;
                if (copy >= out_cap) {
                    copy = out_cap - 1;
                }
                memcpy(out, n->entries[i].value, copy);
                out[copy] = '\0';
            }
            if (out_len) {
                *out_len = n->entries[i].value_len;
            }
            return EDGE_STATE_OK;
        }
    }
    return EDGE_STATE_NOT_FOUND;
}

edge_state_err_t edge_state_delete(edge_state_store_t *st, const char *ns,
                                   const char *key)
{
    edge_state_ns_t *n;
    edge_state_entry_t *e;

    if (!st || !ns || !key) {
        return EDGE_STATE_BAD_NS;
    }
    n = find_ns(st, ns);
    if (!n) {
        return EDGE_STATE_BAD_NS;
    }
    if (!n->enabled) {
        return EDGE_STATE_NS_DISABLED;
    }
    e = find_entry(n, key);
    if (!e) {
        return EDGE_STATE_NOT_FOUND;
    }
    e->used = 0;
    e->value_len = 0;
    e->key[0] = '\0';
    if (n->count > 0) {
        n->count--;
    }
    return EDGE_STATE_OK;
}

int edge_state_list(const edge_state_store_t *st, const char *ns,
                    const char *prefix, char keys[][EDGE_STATE_KEY_MAX],
                    size_t max_keys)
{
    const edge_state_ns_t *n;
    size_t i, written = 0;
    size_t plen = 0;

    if (!st || !ns || !keys || max_keys == 0) {
        return -1;
    }
    n = find_ns_const(st, ns);
    if (!n || !n->enabled) {
        return -1;
    }
    if (!n->entries) {
        return 0;
    }
    if (prefix) {
        plen = strlen(prefix);
    }
    if (max_keys > EDGE_STATE_LIST_CAP) {
        max_keys = EDGE_STATE_LIST_CAP;
    }
    for (i = 0; i < n->capacity && written < max_keys; i++) {
        if (!n->entries[i].used) {
            continue;
        }
        if (prefix && plen > 0 &&
            strncmp(n->entries[i].key, prefix, plen) != 0) {
            continue;
        }
        snprintf(keys[written], EDGE_STATE_KEY_MAX, "%s", n->entries[i].key);
        written++;
    }
    return (int)written;
}

size_t edge_state_count(const edge_state_store_t *st, const char *ns)
{
    const edge_state_ns_t *n = find_ns_const(st, ns);
    return n ? n->count : 0;
}
