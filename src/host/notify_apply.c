/**
 * @file notify_apply.c
 * @brief PG NOTIFY payload schema → state put/delete + WS (P1.12).
 */

#include "edge_notify.h"
#include "edge_state_notify.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *edge_notify_err_name(edge_notify_err_t e)
{
    switch (e) {
    case EDGE_NOTIFY_OK:          return "OK";
    case EDGE_NOTIFY_BAD_JSON:    return "BAD_JSON";
    case EDGE_NOTIFY_BAD_SCHEMA:  return "BAD_SCHEMA";
    case EDGE_NOTIFY_STATE_ERR:   return "STATE_ERR";
    case EDGE_NOTIFY_TOO_LARGE:   return "TOO_LARGE";
    default:                      return "UNKNOWN";
    }
}

int edge_notify_json_string_field(const char *json, size_t json_len,
                                  const char *name, char *out, size_t out_sz)
{
    char key[80];
    const char *p;
    const char *end;
    size_t i = 0;
    char tmp[EDGE_NOTIFY_MAX_PAYLOAD + 1];

    if (!json || !name || !out || out_sz == 0 || json_len == 0 ||
        json_len > EDGE_NOTIFY_MAX_PAYLOAD) {
        return -1;
    }
    memcpy(tmp, json, json_len);
    tmp[json_len] = '\0';
    snprintf(key, sizeof(key), "\"%s\"", name);
    p = strstr(tmp, key);
    if (!p) {
        return -1;
    }
    /* Skip past key "name" (includes quotes), then find value opening quote. */
    p = strchr(p + strlen(key), '"');
    if (!p) {
        return -1;
    }
    p++; /* first char of string value */
    end = p;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) {
            end += 2;
            continue;
        }
        end++;
    }
    if (*end != '"') {
        return -1;
    }
    while (p < end && i + 1 < out_sz) {
        if (*p == '\\' && p + 1 < end) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int extract_raw_value(const char *json, size_t json_len, char *out,
                             size_t out_sz, size_t *out_len)
{
    const char *p;
    const char *start;
    int depth;
    char tmp[EDGE_NOTIFY_MAX_PAYLOAD + 1];

    if (json_len > EDGE_NOTIFY_MAX_PAYLOAD) {
        return -1;
    }
    memcpy(tmp, json, json_len);
    tmp[json_len] = '\0';
    p = strstr(tmp, "\"value\"");
    if (!p) {
        return -1;
    }
    p = strchr(p, ':');
    if (!p) {
        return -1;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        return -1;
    }
    if (strncmp(p, "null", 4) == 0) {
        if (out_sz < 5) {
            return -1;
        }
        memcpy(out, "null", 5);
        if (out_len) {
            *out_len = 4;
        }
        return 0;
    }
    start = p;
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) {
                        p += 2;
                    } else {
                        p++;
                    }
                }
                if (*p == '"') {
                    p++;
                }
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
            }
            p++;
        }
    } else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p += 2;
            } else {
                p++;
            }
        }
        if (*p == '"') {
            p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) {
            p++;
        }
    }
    {
        size_t n = (size_t)(p - start);
        if (n + 1 > out_sz) {
            return -1;
        }
        memcpy(out, start, n);
        out[n] = '\0';
        if (out_len) {
            *out_len = n;
        }
    }
    return 0;
}

edge_notify_err_t edge_notify_apply(edge_state_store_t *st, edge_ws_hub_t *hub,
                                    const char *json, size_t json_len)
{
    char ns[64];
    char key[EDGE_STATE_KEY_MAX];
    char op[16];
    char value[EDGE_STATE_VALUE_MAX + 1];
    char rid[EDGE_WS_REQUEST_ID];
    size_t vlen = 0;
    edge_state_err_t er;

    if (!st || !json) {
        return EDGE_NOTIFY_BAD_JSON;
    }
    if (json_len == 0 || json_len > EDGE_NOTIFY_MAX_PAYLOAD) {
        return EDGE_NOTIFY_TOO_LARGE;
    }
    if (!edge_state_json_ok(json, json_len)) {
        return EDGE_NOTIFY_BAD_JSON;
    }
    if (edge_notify_json_string_field(json, json_len, "ns", ns, sizeof(ns)) !=
            0 ||
        edge_notify_json_string_field(json, json_len, "key", key, sizeof(key)) !=
            0 ||
        edge_notify_json_string_field(json, json_len, "op", op, sizeof(op)) !=
            0) {
        return EDGE_NOTIFY_BAD_SCHEMA;
    }
    if (!edge_state_key_valid(key)) {
        return EDGE_NOTIFY_BAD_SCHEMA;
    }

    if (edge_notify_json_string_field(json, json_len, "request_id", rid,
                                      sizeof(rid)) != 0) {
        if (hub) {
            edge_ws_hub_mint_request_id(hub, NULL, rid, sizeof(rid));
        } else {
            snprintf(rid, sizeof(rid), "notify");
        }
    }

    if (strcmp(op, "delete") == 0) {
        er = edge_state_delete_and_notify(st, hub, ns, key, rid, 0);
        if (er != EDGE_STATE_OK && er != EDGE_STATE_NOT_FOUND) {
            return EDGE_NOTIFY_STATE_ERR;
        }
        return EDGE_NOTIFY_OK;
    }
    if (strcmp(op, "put") != 0) {
        return EDGE_NOTIFY_BAD_SCHEMA;
    }
    if (extract_raw_value(json, json_len, value, sizeof(value), &vlen) != 0) {
        return EDGE_NOTIFY_BAD_SCHEMA;
    }
    if (strcmp(value, "null") == 0) {
        return EDGE_NOTIFY_BAD_SCHEMA;
    }
    er = edge_state_put_and_notify(st, hub, ns, key, value, vlen, rid, 0);
    if (er != EDGE_STATE_OK) {
        return EDGE_NOTIFY_STATE_ERR;
    }
    return EDGE_NOTIFY_OK;
}
