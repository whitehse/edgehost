/**
 * @file state_store.c
 * @brief Multi-namespace key/value store (P1.7a). Create-time alloc only.
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
    edge_state_entry_t *entries;
    size_t capacity;
    size_t count;
} edge_state_ns_t;

struct edge_state_store {
    edge_state_ns_t ns[EDGE_STATE_NS_MAX];
    size_t          n_ns;
    size_t          max_keys;
    size_t          max_value;
};

edge_state_config_t edge_state_default_config(void)
{
    edge_state_config_t c;
    c.max_keys_per_ns = EDGE_STATE_KEYS_DEFAULT;
    c.max_value_bytes = EDGE_STATE_VALUE_DEFAULT;
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

static int register_ns(edge_state_store_t *st, const char *name, int enabled)
{
    edge_state_ns_t *n;
    size_t i;

    if (!st || !name || !name[0] || strlen(name) >= 64) {
        return -1;
    }
    n = find_ns(st, name);
    if (n) {
        n->enabled = enabled ? 1 : 0;
        return 0;
    }
    if (st->n_ns >= EDGE_STATE_NS_MAX) {
        return -1;
    }
    n = &st->ns[st->n_ns];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->enabled = enabled ? 1 : 0;
    n->registered = 1;
    n->capacity = st->max_keys;
    n->entries = (edge_state_entry_t *)calloc(n->capacity, sizeof(*n->entries));
    if (!n->entries) {
        return -1;
    }
    for (i = 0; i < n->capacity; i++) {
        n->entries[i].value = (char *)calloc(1, st->max_value + 1);
        if (!n->entries[i].value) {
            size_t j;
            for (j = 0; j < i; j++) {
                free(n->entries[j].value);
            }
            free(n->entries);
            n->entries = NULL;
            return -1;
        }
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

    /* v1 fully enabled namespaces */
    if (register_ns(st, "net.core", 1) != 0 ||
        register_ns(st, "map.dynamic", 1) != 0) {
        edge_state_destroy(st);
        return NULL;
    }
    /* Registered disabled hooks */
    (void)register_ns(st, "net.pon", 0);
    (void)register_ns(st, "net.home", 0);
    (void)register_ns(st, "electric", 0);
    (void)register_ns(st, "inventory", 0);

    return st;
}

void edge_state_destroy(edge_state_store_t *st)
{
    size_t i, j;
    if (!st) {
        return;
    }
    for (i = 0; i < st->n_ns; i++) {
        if (st->ns[i].entries) {
            for (j = 0; j < st->ns[i].capacity; j++) {
                free(st->ns[i].entries[j].value);
            }
            free(st->ns[i].entries);
        }
    }
    free(st);
}

int edge_state_ns_set_enabled(edge_state_store_t *st, const char *ns, int enabled)
{
    edge_state_ns_t *n = find_ns(st, ns);
    if (!n) {
        return register_ns(st, ns, enabled);
    }
    n->enabled = enabled ? 1 : 0;
    return 0;
}

int edge_state_ns_enabled(const edge_state_store_t *st, const char *ns)
{
    const edge_state_ns_t *n = find_ns_const(st, ns);
    return n && n->enabled;
}

static edge_state_entry_t *find_entry(edge_state_ns_t *n, const char *key)
{
    size_t i;
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
