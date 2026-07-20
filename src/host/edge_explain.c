/**
 * @file edge_explain.c
 * @brief Template fill + anim_load_plan validation for /api/v1/explain.
 */

#include "edge_explain.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if EDGEHOST_HAVE_LIBANIM
#include "anim.h"
#endif

static int json_escape(const char *in, char *out, size_t out_cap)
{
    size_t o = 0;
    size_t i;
    if (!in || !out || out_cap < 3) {
        return -1;
    }
    out[o++] = '"';
    for (i = 0; in[i]; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= out_cap) {
                return -1;
            }
            out[o++] = '\\';
            out[o++] = c;
        } else if (c == '\n') {
            if (o + 3 >= out_cap) {
                return -1;
            }
            out[o++] = '\\';
            out[o++] = 'n';
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            if (o + 2 >= out_cap) {
                return -1;
            }
            out[o++] = c;
        }
    }
    if (o + 2 >= out_cap) {
        return -1;
    }
    out[o++] = '"';
    out[o] = '\0';
    return (int)o;
}

/** Extract "key":"value" string field (first occurrence). */
static int json_str_field(const char *json, const char *key, char *out,
                          size_t out_sz)
{
    char pat[96];
    const char *p;
    const char *q;
    size_t n;
    if (!json || !key || !out || out_sz == 0) {
        return -1;
    }
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return -1;
    }
    p = strchr(p + strlen(pat), '"');
    if (!p) {
        return -1;
    }
    p++;
    q = p;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1]) {
            q += 2;
            continue;
        }
        q++;
    }
    if (*q != '"') {
        return -1;
    }
    n = (size_t)(q - p);
    if (n >= out_sz) {
        n = out_sz - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

/**
 * Convert "params":{ "a":"b", "c":"d" } into key=value\\n map.
 * Minimal scanner — no nested objects.
 */
static int params_to_kv(const char *json, char *kv, size_t kv_cap)
{
    const char *p = strstr(json, "\"params\"");
    const char *end;
    size_t o = 0;
    if (!p) {
        if (kv_cap) {
            kv[0] = '\0';
        }
        return 0;
    }
    p = strchr(p, '{');
    if (!p) {
        return -1;
    }
    end = strchr(p, '}');
    if (!end) {
        return -1;
    }
    p++;
    while (p < end) {
        const char *k0, *k1, *v0, *v1;
        size_t klen, vlen;
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',')) {
            p++;
        }
        if (p >= end || *p != '"') {
            break;
        }
        k0 = ++p;
        while (p < end && *p != '"') {
            p++;
        }
        if (p >= end) {
            break;
        }
        k1 = p++;
        while (p < end && *p != '"') {
            p++;
        }
        if (p >= end) {
            break;
        }
        v0 = ++p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            p++;
        }
        if (p >= end) {
            break;
        }
        v1 = p++;
        klen = (size_t)(k1 - k0);
        vlen = (size_t)(v1 - v0);
        if (o + klen + vlen + 3 >= kv_cap) {
            return -1;
        }
        memcpy(kv + o, k0, klen);
        o += klen;
        kv[o++] = '=';
        memcpy(kv + o, v0, vlen);
        o += vlen;
        kv[o++] = '\n';
    }
    kv[o] = '\0';
    return 0;
}

static int safe_template_name(const char *name)
{
    size_t i;
    if (!name || !name[0] || strlen(name) > 64) {
        return 0;
    }
    for (i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

int edge_explain_list_templates_json(const char *templates_root, char *out,
                                     size_t out_cap)
{
    DIR *d;
    struct dirent *de;
    size_t o = 0;
    int first = 1;

    if (!templates_root || !out || out_cap < 8) {
        return -1;
    }
    d = opendir(templates_root);
    if (!d) {
        return snprintf(out, out_cap, "{\"templates\":[]}");
    }
    o = (size_t)snprintf(out, out_cap, "{\"templates\":[");
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        char base[80];
        char esc[160];
        int el;
        if (n < 6 || strcmp(de->d_name + n - 5, ".tmpl") != 0) {
            continue;
        }
        if (n - 5 >= sizeof(base)) {
            continue;
        }
        memcpy(base, de->d_name, n - 5);
        base[n - 5] = '\0';
        if (!safe_template_name(base)) {
            continue;
        }
        el = json_escape(base, esc, sizeof(esc));
        if (el < 0) {
            continue;
        }
        if (o + (size_t)el + 2 >= out_cap) {
            closedir(d);
            return -1;
        }
        if (!first) {
            out[o++] = ',';
        }
        memcpy(out + o, esc, (size_t)el);
        o += (size_t)el;
        first = 0;
    }
    closedir(d);
    if (o + 3 >= out_cap) {
        return -1;
    }
    out[o++] = ']';
    out[o++] = '}';
    out[o] = '\0';
    return (int)o;
}

int edge_explain_render_json(const char *templates_root, const char *body,
                             size_t body_len, char *out, size_t out_cap)
{
#if !EDGEHOST_HAVE_LIBANIM
    (void)templates_root;
    (void)body;
    (void)body_len;
    return snprintf(out, out_cap,
                    "{\"ok\":false,\"error\":\"LIBANIM_UNAVAILABLE\"}");
#else
    char tmpl_name[72];
    char path[512];
    char *tmpl_buf = NULL;
    char kv[4096];
    char *plan = NULL;
    size_t plan_cap = 65536;
    size_t plan_len = 0;
    FILE *f;
    long flen;
    size_t nread;
    anim_ctx *ctx;
    int jn;
    char *plan_esc = NULL;
    size_t plan_esc_cap = 0;
    int el;
    uint32_t duration = 0;
    uint32_t objs = 0;

    if (!templates_root || !body || !out || out_cap < 64) {
        return -1;
    }
    {
        char *tmp = (char *)malloc(body_len + 1);
        if (!tmp) {
            return -1;
        }
        memcpy(tmp, body, body_len);
        tmp[body_len] = '\0';
        if (json_str_field(tmp, "template", tmpl_name, sizeof(tmpl_name)) != 0) {
            free(tmp);
            return snprintf(out, out_cap,
                            "{\"ok\":false,\"error\":\"missing template\"}");
        }
        if (params_to_kv(tmp, kv, sizeof(kv)) != 0) {
            free(tmp);
            return snprintf(out, out_cap,
                            "{\"ok\":false,\"error\":\"bad params\"}");
        }
        free(tmp);
    }

    if (!safe_template_name(tmpl_name)) {
        return snprintf(out, out_cap,
                        "{\"ok\":false,\"error\":\"bad template name\"}");
    }
    jn = snprintf(path, sizeof(path), "%s/%s.tmpl", templates_root, tmpl_name);
    if (jn < 0 || (size_t)jn >= sizeof(path)) {
        return -1;
    }

    f = fopen(path, "rb");
    if (!f) {
        return snprintf(out, out_cap,
                        "{\"ok\":false,\"error\":\"template not found\"}");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    flen = ftell(f);
    if (flen < 0 || flen > 256 * 1024) {
        fclose(f);
        return snprintf(out, out_cap, "{\"ok\":false,\"error\":\"template too large\"}");
    }
    rewind(f);
    tmpl_buf = (char *)malloc((size_t)flen + 1);
    if (!tmpl_buf) {
        fclose(f);
        return -1;
    }
    nread = fread(tmpl_buf, 1, (size_t)flen, f);
    fclose(f);
    tmpl_buf[nread] = '\0';

    plan = (char *)malloc(plan_cap);
    if (!plan) {
        free(tmpl_buf);
        return -1;
    }
    if (anim_template_fill(tmpl_buf, nread, kv, plan, plan_cap, &plan_len) !=
        0) {
        free(tmpl_buf);
        free(plan);
        return snprintf(out, out_cap,
                        "{\"ok\":false,\"error\":\"template fill failed\"}");
    }
    free(tmpl_buf);

    ctx = anim_create();
    if (!ctx) {
        free(plan);
        return -1;
    }
    if (anim_load_plan(ctx, plan, plan_len) != 0) {
        anim_destroy(ctx);
        free(plan);
        return snprintf(out, out_cap,
                        "{\"ok\":false,\"error\":\"plan validation failed\"}");
    }
    duration = anim_duration_ms(ctx);
    objs = anim_object_count(ctx);
    anim_destroy(ctx);

    plan_esc_cap = plan_len * 2 + 64;
    plan_esc = (char *)malloc(plan_esc_cap);
    if (!plan_esc) {
        free(plan);
        return -1;
    }
    el = json_escape(plan, plan_esc, plan_esc_cap);
    free(plan);
    if (el < 0) {
        free(plan_esc);
        return snprintf(out, out_cap,
                        "{\"ok\":false,\"error\":\"plan escape failed\"}");
    }
    {
        char te[160];
        int rc;
        if (json_escape(tmpl_name, te, sizeof(te)) < 0) {
            free(plan_esc);
            return -1;
        }
        rc = snprintf(out, out_cap,
                      "{\"ok\":true,\"template\":%s,\"duration_ms\":%u,"
                      "\"object_count\":%u,\"plan\":%s}",
                      te, (unsigned)duration, (unsigned)objs, plan_esc);
        free(plan_esc);
        return rc;
    }
#endif
}
