/**
 * @file e7_config_xml.c
 * @brief E7 config XML → JSON + ONT inventory extract.
 */

#include "edge_e7_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int e7_stristr(const char *hay, const char *needle)
{
    size_t nlen;
    size_t i;
    if (!hay || !needle || !needle[0]) {
        return 0;
    }
    nlen = strlen(needle);
    for (i = 0; hay[i]; i++) {
        size_t j = 0;
        while (j < nlen && hay[i + j] &&
               tolower((unsigned char)hay[i + j]) ==
                   tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

/* ---- growing buffer ---- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} e7_sb_t;

static int sb_reserve(e7_sb_t *sb, size_t need)
{
    size_t ncap;
    char *n;
    if (!sb) {
        return -1;
    }
    if (sb->len + need + 1 <= sb->cap) {
        return 0;
    }
    ncap = sb->cap ? sb->cap : 4096;
    while (sb->len + need + 1 > ncap) {
        ncap *= 2;
    }
    n = (char *)realloc(sb->data, ncap);
    if (!n) {
        return -1;
    }
    sb->data = n;
    sb->cap = ncap;
    return 0;
}

static int sb_append(e7_sb_t *sb, const char *s, size_t n)
{
    if (!s || n == 0) {
        return 0;
    }
    if (sb_reserve(sb, n) != 0) {
        return -1;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

static int sb_append_str(e7_sb_t *sb, const char *s)
{
    return sb_append(sb, s, s ? strlen(s) : 0);
}

static int sb_append_ch(e7_sb_t *sb, char c)
{
    return sb_append(sb, &c, 1);
}

static int sb_append_json_str(e7_sb_t *sb, const char *s, size_t n)
{
    size_t i;
    if (sb_append_ch(sb, '"') != 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (sb_append_ch(sb, '\\') != 0 || sb_append_ch(sb, (char)c) != 0) {
                return -1;
            }
        } else if (c < 0x20) {
            char esc[8];
            int en = snprintf(esc, sizeof(esc), "\\u%04x", c);
            if (en < 0 || sb_append(sb, esc, (size_t)en) != 0) {
                return -1;
            }
        } else {
            if (sb_append_ch(sb, (char)c) != 0) {
                return -1;
            }
        }
    }
    return sb_append_ch(sb, '"');
}

static void sb_free(e7_sb_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ---- XML helpers ---- */

static const char *local_name(const char *tag, size_t tag_len, size_t *out_len)
{
    size_t i;
    for (i = 0; i < tag_len; i++) {
        if (tag[i] == ':') {
            *out_len = tag_len - i - 1;
            return tag + i + 1;
        }
    }
    *out_len = tag_len;
    return tag;
}

static int tag_eq(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    size_t ln;
    const char *loc = local_name(a, alen, &ln);
    return ln == blen && memcmp(loc, b, blen) == 0;
}

static void trim_ws(const char **s, size_t *n)
{
    while (*n && isspace((unsigned char)**s)) {
        (*s)++;
        (*n)--;
    }
    while (*n && isspace((unsigned char)(*s)[*n - 1])) {
        (*n)--;
    }
}

static void copy_trim(char *dst, size_t dst_sz, const char *s, size_t n)
{
    size_t i = 0;
    if (!dst || dst_sz == 0) {
        return;
    }
    trim_ws(&s, &n);
    while (i + 1 < dst_sz && i < n) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---- XML → JSON (stack of open elements) ---- */

#define E7_JSON_STACK 64

typedef struct {
    char   name[64];
    size_t name_len;
    int    is_array;     /* 1 if this element became an array of children */
    int    child_count;  /* number of child elements emitted */
    int    has_text;
    int    has_attrs;
    int    started_obj;  /* '{' already written */
    e7_sb_t text;
} e7_json_frame_t;

static int json_start_obj_if_needed(e7_sb_t *out, e7_json_frame_t *fr)
{
    if (fr->started_obj) {
        return 0;
    }
    if (sb_append_ch(out, '{') != 0) {
        return -1;
    }
    fr->started_obj = 1;
    return 0;
}

static int json_sep(e7_sb_t *out, e7_json_frame_t *fr)
{
    if (fr->child_count > 0 || fr->has_text || fr->has_attrs) {
        return sb_append_ch(out, ',');
    }
    return 0;
}

int edge_e7_xml_to_json(const char *xml, size_t xml_len, char **out,
                        size_t *out_len)
{
    e7_sb_t sb;
    e7_json_frame_t stack[E7_JSON_STACK];
    int sp = -1;
    size_t i = 0;
    int rc = -1;

    if (!xml || !out) {
        return -1;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }
    memset(&sb, 0, sizeof(sb));
    memset(stack, 0, sizeof(stack));

    /* Root object wrapper */
    if (sb_append_str(&sb, "{\"config\":") != 0) {
        goto done;
    }

    while (i < xml_len) {
        if (xml[i] != '<') {
            /* text under current element */
            size_t start = i;
            while (i < xml_len && xml[i] != '<') {
                i++;
            }
            if (sp >= 0) {
                const char *t = xml + start;
                size_t tn = i - start;
                trim_ws(&t, &tn);
                if (tn > 0) {
                    if (sb_append(&stack[sp].text, t, tn) != 0) {
                        goto done;
                    }
                    stack[sp].has_text = 1;
                }
            }
            continue;
        }
        /* tag */
        i++; /* skip < */
        if (i < xml_len && xml[i] == '?') {
            /* PI / xml decl */
            while (i + 1 < xml_len && !(xml[i] == '?' && xml[i + 1] == '>')) {
                i++;
            }
            if (i + 1 < xml_len) {
                i += 2;
            }
            continue;
        }
        if (i < xml_len && xml[i] == '!') {
            /* comment or CDATA skip */
            if (i + 2 < xml_len && xml[i + 1] == '-' && xml[i + 2] == '-') {
                i += 3;
                while (i + 2 < xml_len &&
                       !(xml[i] == '-' && xml[i + 1] == '-' && xml[i + 2] == '>')) {
                    i++;
                }
                if (i + 2 < xml_len) {
                    i += 3;
                }
            } else {
                while (i < xml_len && xml[i] != '>') {
                    i++;
                }
                if (i < xml_len) {
                    i++;
                }
            }
            continue;
        }
        {
            int closing = 0;
            int self_close = 0;
            size_t name_start, name_end, tag_end;
            const char *name;
            size_t name_len;

            if (i < xml_len && xml[i] == '/') {
                closing = 1;
                i++;
            }
            name_start = i;
            while (i < xml_len && xml[i] != '>' && xml[i] != '/' &&
                   !isspace((unsigned char)xml[i])) {
                i++;
            }
            name_end = i;
            /* skip attrs to > */
            while (i < xml_len && xml[i] != '>') {
                if (xml[i] == '/' && i + 1 < xml_len && xml[i + 1] == '>') {
                    self_close = 1;
                }
                i++;
            }
            tag_end = i;
            if (i < xml_len) {
                i++; /* > */
            }
            (void)tag_end;
            name = xml + name_start;
            name_len = name_end - name_start;
            if (name_len == 0 || name_len >= 64) {
                continue;
            }

            if (closing) {
                e7_json_frame_t *fr;
                if (sp < 0) {
                    continue;
                }
                fr = &stack[sp];
                /* emit element value */
                if (!fr->started_obj && fr->has_text && fr->child_count == 0 &&
                    !fr->has_attrs) {
                    /* leaf string */
                    if (sb_append_json_str(&sb, fr->text.data ? fr->text.data : "",
                                           fr->text.len) != 0) {
                        goto done;
                    }
                } else {
                    if (json_start_obj_if_needed(&sb, fr) != 0) {
                        goto done;
                    }
                    if (fr->has_text && fr->text.len > 0) {
                        if (json_sep(&sb, fr) != 0) {
                            goto done;
                        }
                        if (sb_append_str(&sb, "\"#text\":") != 0 ||
                            sb_append_json_str(&sb, fr->text.data, fr->text.len) !=
                                0) {
                            goto done;
                        }
                        fr->has_text = 1; /* already counted for sep */
                    }
                    if (sb_append_ch(&sb, '}') != 0) {
                        goto done;
                    }
                }
                sb_free(&fr->text);
                sp--;
                if (sp >= 0) {
                    stack[sp].child_count++;
                }
            } else {
                e7_json_frame_t *parent;
                e7_json_frame_t *fr;
                size_t ln;
                const char *loc = local_name(name, name_len, &ln);

                if (sp + 1 >= E7_JSON_STACK) {
                    goto done;
                }
                parent = sp >= 0 ? &stack[sp] : NULL;
                if (parent) {
                    if (json_start_obj_if_needed(&sb, parent) != 0) {
                        goto done;
                    }
                    if (json_sep(&sb, parent) != 0) {
                        goto done;
                    }
                    if (sb_append_json_str(&sb, loc, ln) != 0 ||
                        sb_append_ch(&sb, ':') != 0) {
                        goto done;
                    }
                } else {
                    /* first top-level under our wrapper: if rpc-reply/data, keep */
                    if (sb.len > 0 && sb.data[sb.len - 1] == ':') {
                        /* after {"config": already — replace approach: open raw */
                    }
                }
                sp++;
                fr = &stack[sp];
                memset(fr, 0, sizeof(*fr));
                memcpy(fr->name, loc, ln);
                fr->name[ln] = '\0';
                fr->name_len = ln;
                if (self_close) {
                    if (sb_append_str(&sb, "null") != 0) {
                        goto done;
                    }
                    sb_free(&fr->text);
                    sp--;
                    if (sp >= 0) {
                        stack[sp].child_count++;
                    }
                }
            }
        }
    }

    /* close any unclosed with null */
    while (sp >= 0) {
        e7_json_frame_t *fr = &stack[sp];
        if (!fr->started_obj && fr->has_text && fr->child_count == 0) {
            (void)sb_append_json_str(&sb, fr->text.data ? fr->text.data : "",
                                     fr->text.len);
        } else {
            (void)json_start_obj_if_needed(&sb, fr);
            (void)sb_append_ch(&sb, '}');
        }
        sb_free(&fr->text);
        sp--;
    }

    if (sb_append_ch(&sb, '}') != 0) {
        goto done;
    }
    *out = sb.data;
    if (out_len) {
        *out_len = sb.len;
    }
    sb.data = NULL; /* ownership transferred */
    rc = 0;

done:
    while (sp >= 0) {
        sb_free(&stack[sp].text);
        sp--;
    }
    if (rc != 0) {
        sb_free(&sb);
    }
    return rc;
}

/* ---- inventory extract (AXOS + synthetic EXA) ---- */

#define E7_INV_STACK 64

typedef enum {
    INV_NONE = 0,
    INV_ONT,
    INV_PORT,
    INV_VLAN,   /* AXOS outer <vlan> under eth/ua → service */
    INV_SVC,    /* legacy eth-svc / data-svc */
    INV_POLMAP  /* AXOS <policy-map> (name = profile) */
} inv_ctx_t;

typedef struct {
    inv_ctx_t kind;
    char      tag[48];
} inv_frame_t;

static void inv_compose_fsan(edge_e7_cfg_ont_t *cur, const char *vendor,
                             size_t vendor_len, const char *serial,
                             size_t serial_len)
{
    char v[32];
    char s[32];
    if (!cur) {
        return;
    }
    copy_trim(v, sizeof(v), vendor, vendor_len);
    copy_trim(s, sizeof(s), serial, serial_len);
    if (v[0] && s[0]) {
        /* Calix FSAN = vendor-id + serial-number (e.g. CXNK + DFB749). */
        size_t vi = 0, si = 0, o = 0;
        while (v[vi] && o + 1 < sizeof(cur->fsan)) {
            cur->fsan[o++] = v[vi++];
        }
        while (s[si] && o + 1 < sizeof(cur->fsan)) {
            cur->fsan[o++] = s[si++];
        }
        cur->fsan[o] = '\0';
    } else if (s[0] && cur->fsan[0] == '\0') {
        copy_trim(cur->fsan, sizeof(cur->fsan), s, strlen(s));
    }
}

static void inv_infer_svc_kind(edge_e7_cfg_service_t *svc, const char *hint_tag)
{
    const char *p;
    if (!svc) {
        return;
    }
    if (svc->kind[0]) {
        return;
    }
    p = svc->profile[0] ? svc->profile : svc->name;
    if (p[0]) {
        if (e7_stristr(p, "voice") || e7_stristr(p, "sip") ||
            e7_stristr(p, "pots")) {
            snprintf(svc->kind, sizeof(svc->kind), "voice");
            return;
        }
        if (e7_stristr(p, "video") || e7_stristr(p, "iptv") ||
            e7_stristr(p, "mcast")) {
            snprintf(svc->kind, sizeof(svc->kind), "video");
            return;
        }
        /* AXOS HSI bandwidth profiles: 100M_100M, 1G_1G, 250M_250M */
        if (strchr(p, 'M') || strchr(p, 'G') || e7_stristr(p, "hsi") ||
            e7_stristr(p, "data")) {
            snprintf(svc->kind, sizeof(svc->kind), "hsi");
            return;
        }
    }
    if (hint_tag) {
        if (strcmp(hint_tag, "pots") == 0 || strcmp(hint_tag, "pots-svc") == 0 ||
            strcmp(hint_tag, "sip-service") == 0) {
            snprintf(svc->kind, sizeof(svc->kind), "voice");
            return;
        }
        if (strcmp(hint_tag, "rf-video") == 0 || strcmp(hint_tag, "video") == 0) {
            snprintf(svc->kind, sizeof(svc->kind), "video");
            return;
        }
        if (strcmp(hint_tag, "hsi") == 0 || strcmp(hint_tag, "data-svc") == 0) {
            snprintf(svc->kind, sizeof(svc->kind), "hsi");
            return;
        }
    }
    snprintf(svc->kind, sizeof(svc->kind), "eth");
}

static void inv_finish_ont(edge_e7_cfg_inventory_t *inv, edge_e7_cfg_ont_t *cur,
                           int *have, char *vendor_buf, char *serial_buf)
{
    int i;
    if (!*have) {
        return;
    }
    /* Prefer composed FSAN when vendor+serial collected separately (AXOS). */
    if (vendor_buf && serial_buf && vendor_buf[0] && serial_buf[0]) {
        inv_compose_fsan(cur, vendor_buf, strlen(vendor_buf), serial_buf,
                         strlen(serial_buf));
    }
    if (cur->ont_id[0] == '\0' && cur->fsan[0] == '\0') {
        *have = 0;
        memset(cur, 0, sizeof(*cur));
        if (vendor_buf) {
            vendor_buf[0] = '\0';
        }
        if (serial_buf) {
            serial_buf[0] = '\0';
        }
        return;
    }
    if (inv->ont_count >= EDGE_E7_CFG_ONTS_MAX) {
        inv->truncated = 1;
        *have = 0;
        memset(cur, 0, sizeof(*cur));
        if (vendor_buf) {
            vendor_buf[0] = '\0';
        }
        if (serial_buf) {
            serial_buf[0] = '\0';
        }
        return;
    }
    if (cur->account[0]) {
        inv->account_count++;
    }
    for (i = 0; i < cur->port_count; i++) {
        int s;
        /* Fill default port id for UA when only nested id was missed. */
        if (cur->ports[i].port[0] == '\0' &&
            strcmp(cur->ports[i].type, "ua") == 0) {
            snprintf(cur->ports[i].port, sizeof(cur->ports[i].port), "ua");
        }
        for (s = 0; s < cur->ports[i].service_count; s++) {
            inv_infer_svc_kind(&cur->ports[i].services[s],
                               cur->ports[i].type);
        }
        inv->service_count += cur->ports[i].service_count;
    }
    inv->onts[inv->ont_count++] = *cur;
    *have = 0;
    memset(cur, 0, sizeof(*cur));
    if (vendor_buf) {
        vendor_buf[0] = '\0';
    }
    if (serial_buf) {
        serial_buf[0] = '\0';
    }
}

static int inv_in_ont(const inv_frame_t *stack, int sp)
{
    int i;
    for (i = 0; i <= sp; i++) {
        if (stack[i].kind == INV_ONT) {
            return 1;
        }
    }
    return 0;
}

static int inv_port_depth(const inv_frame_t *stack, int sp)
{
    int i;
    for (i = sp; i >= 0; i--) {
        if (stack[i].kind == INV_PORT) {
            return i;
        }
    }
    return -1;
}

static int inv_is_port_tag(const char *loc, size_t ln)
{
    static const char *const tags[] = {
        "ont-ethernet", "ont-port", "eth-port", "ontport", "ont-ua", "pots",
        "rg", "rf-video", NULL};
    size_t i;
    for (i = 0; tags[i]; i++) {
        size_t tlen = strlen(tags[i]);
        if (ln == tlen && memcmp(loc, tags[i], tlen) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *inv_port_type_for_tag(const char *tag)
{
    if (strcmp(tag, "pots") == 0) {
        return "pots";
    }
    if (strcmp(tag, "ont-ua") == 0) {
        return "ua";
    }
    if (strcmp(tag, "rg") == 0) {
        return "rg";
    }
    if (strcmp(tag, "rf-video") == 0) {
        return "video";
    }
    return "eth";
}

static int inv_is_legacy_svc_tag(const char *loc, size_t ln)
{
    static const char *const tags[] = {"eth-svc", "data-svc", "hsi", "video",
                                       "pots-svc", "service", "ethsvc",
                                       "sip-service", NULL};
    size_t i;
    for (i = 0; tags[i]; i++) {
        size_t tlen = strlen(tags[i]);
        if (ln == tlen && memcmp(loc, tags[i], tlen) == 0) {
            return 1;
        }
    }
    return 0;
}

int edge_e7_config_extract_inventory(const char *xml, size_t xml_len,
                                     edge_e7_cfg_inventory_t *inv)
{
    inv_frame_t stack[E7_INV_STACK];
    int sp = -1;
    size_t i = 0;
    edge_e7_cfg_ont_t cur;
    int have_ont = 0;
    edge_e7_cfg_port_t *cur_port = NULL;
    edge_e7_cfg_service_t *cur_svc = NULL;
    char vendor[32];
    char serial_only[32];
    char text_tag[48];
    size_t text_tag_len = 0;
    int collecting_text = 0;
    int in_policy_map = 0;
    e7_sb_t text;

    if (!xml || !inv) {
        return -1;
    }
    memset(inv, 0, sizeof(*inv));
    memset(&cur, 0, sizeof(cur));
    memset(stack, 0, sizeof(stack));
    memset(&text, 0, sizeof(text));
    vendor[0] = '\0';
    serial_only[0] = '\0';
    text_tag[0] = '\0';

    while (i < xml_len) {
        if (xml[i] != '<') {
            size_t start = i;
            while (i < xml_len && xml[i] != '<') {
                i++;
            }
            if (collecting_text) {
                const char *t = xml + start;
                size_t tn = i - start;
                trim_ws(&t, &tn);
                if (tn > 0) {
                    (void)sb_append(&text, t, tn);
                }
            }
            continue;
        }
        i++;
        if (i < xml_len && (xml[i] == '?' || xml[i] == '!')) {
            if (xml[i] == '!' && i + 2 < xml_len && xml[i + 1] == '-' &&
                xml[i + 2] == '-') {
                i += 3;
                while (i + 2 < xml_len &&
                       !(xml[i] == '-' && xml[i + 1] == '-' &&
                         xml[i + 2] == '>')) {
                    i++;
                }
                if (i + 2 < xml_len) {
                    i += 3;
                }
            } else {
                while (i < xml_len && xml[i] != '>') {
                    i++;
                }
                if (i < xml_len) {
                    i++;
                }
            }
            continue;
        }
        {
            int closing = 0;
            int self_close = 0;
            size_t name_start, name_end;
            const char *name;
            size_t name_len;
            size_t ln;
            const char *loc;

            if (i < xml_len && xml[i] == '/') {
                closing = 1;
                i++;
            }
            name_start = i;
            while (i < xml_len && xml[i] != '>' && xml[i] != '/' &&
                   !isspace((unsigned char)xml[i])) {
                i++;
            }
            name_end = i;
            while (i < xml_len && xml[i] != '>') {
                if (xml[i] == '/' && i + 1 < xml_len && xml[i + 1] == '>') {
                    self_close = 1;
                }
                i++;
            }
            if (i < xml_len) {
                i++;
            }
            name = xml + name_start;
            name_len = name_end - name_start;
            if (name_len == 0) {
                continue;
            }
            loc = local_name(name, name_len, &ln);
            if (ln >= sizeof(text_tag)) {
                ln = sizeof(text_tag) - 1;
            }

            if (closing) {
                if (collecting_text && text.len > 0 && have_ont) {
                    int ont_depth = 0;
                    int pdepth = inv_port_depth(stack, sp);
                    int j;
                    for (j = 0; j <= sp; j++) {
                        if (stack[j].kind == INV_ONT) {
                            ont_depth = j;
                            break;
                        }
                    }
                    /* Leaf assignment depends on path: ONT-level vs port/svc. */
                    if (tag_eq(text_tag, text_tag_len, "ont-id") &&
                        cur.ont_id[0] == '\0') {
                        copy_trim(cur.ont_id, sizeof(cur.ont_id), text.data,
                                  text.len);
                    } else if (tag_eq(text_tag, text_tag_len, "serial-number") ||
                               tag_eq(text_tag, text_tag_len, "fsan") ||
                               tag_eq(text_tag, text_tag_len, "ont-sn")) {
                        copy_trim(serial_only, sizeof(serial_only), text.data,
                                  text.len);
                        if (cur.fsan[0] == '\0') {
                            copy_trim(cur.fsan, sizeof(cur.fsan), text.data,
                                      text.len);
                        }
                    } else if (tag_eq(text_tag, text_tag_len, "vendor-id")) {
                        copy_trim(vendor, sizeof(vendor), text.data, text.len);
                    } else if ((tag_eq(text_tag, text_tag_len, "subscriber-id") ||
                                tag_eq(text_tag, text_tag_len, "account") ||
                                tag_eq(text_tag, text_tag_len,
                                       "subscriber-location-id")) &&
                               cur.account[0] == '\0') {
                        /* Prefer ONT-level subscriber-id (not only port). */
                        if (pdepth < 0 || sp <= ont_depth + 2) {
                            copy_trim(cur.account, sizeof(cur.account), text.data,
                                      text.len);
                        } else if (cur.account[0] == '\0') {
                            copy_trim(cur.account, sizeof(cur.account), text.data,
                                      text.len);
                        }
                    } else if ((tag_eq(text_tag, text_tag_len, "profile-id") ||
                                tag_eq(text_tag, text_tag_len, "ont-type") ||
                                tag_eq(text_tag, text_tag_len, "model")) &&
                               cur.model[0] == '\0') {
                        copy_trim(cur.model, sizeof(cur.model), text.data,
                                  text.len);
                    } else if ((tag_eq(text_tag, text_tag_len, "linked-pon") ||
                                tag_eq(text_tag, text_tag_len, "pon-id")) &&
                               cur.pon_id[0] == '\0') {
                        copy_trim(cur.pon_id, sizeof(cur.pon_id), text.data,
                                  text.len);
                    } else if ((tag_eq(text_tag, text_tag_len, "admin-state") ||
                                tag_eq(text_tag, text_tag_len, "enabled")) &&
                               cur.admin_state[0] == '\0') {
                        copy_trim(cur.admin_state, sizeof(cur.admin_state),
                                  text.data, text.len);
                    } else if (cur_port && cur_port->port[0] == '\0' &&
                               (tag_eq(text_tag, text_tag_len, "port") ||
                                (strcmp(cur_port->type, "ua") == 0 &&
                                 tag_eq(text_tag, text_tag_len, "id")))) {
                        copy_trim(cur_port->port, sizeof(cur_port->port),
                                  text.data, text.len);
                    } else if (cur_svc &&
                               tag_eq(text_tag, text_tag_len, "vlan-id")) {
                        /* Outer vlan-id first; c-vlan id into name if set. */
                        if (cur_svc->vlan[0] == '\0') {
                            copy_trim(cur_svc->vlan, sizeof(cur_svc->vlan),
                                      text.data, text.len);
                        } else if (cur_svc->name[0] == '\0') {
                            /* secondary id (c-vlan) → name as c-vlan:N */
                            char tmp[32];
                            copy_trim(tmp, sizeof(tmp), text.data, text.len);
                            snprintf(cur_svc->name, sizeof(cur_svc->name),
                                     "c-vlan:%s", tmp);
                        }
                    } else if (cur_svc && in_policy_map &&
                               tag_eq(text_tag, text_tag_len, "name") &&
                               cur_svc->profile[0] == '\0') {
                        copy_trim(cur_svc->profile, sizeof(cur_svc->profile),
                                  text.data, text.len);
                        if (cur_svc->name[0] == '\0' ||
                            strncmp(cur_svc->name, "c-vlan:", 7) == 0) {
                            /* keep c-vlan name; profile is bandwidth map */
                        }
                    } else if (cur_svc &&
                               (tag_eq(text_tag, text_tag_len, "name") ||
                                tag_eq(text_tag, text_tag_len, "profile") ||
                                tag_eq(text_tag, text_tag_len, "kind")) &&
                               !in_policy_map) {
                        if (tag_eq(text_tag, text_tag_len, "kind") &&
                            cur_svc->kind[0] == '\0') {
                            copy_trim(cur_svc->kind, sizeof(cur_svc->kind),
                                      text.data, text.len);
                        } else if (tag_eq(text_tag, text_tag_len, "profile") &&
                                   cur_svc->profile[0] == '\0') {
                            copy_trim(cur_svc->profile, sizeof(cur_svc->profile),
                                      text.data, text.len);
                        } else if (tag_eq(text_tag, text_tag_len, "name") &&
                                   cur_svc->name[0] == '\0') {
                            copy_trim(cur_svc->name, sizeof(cur_svc->name),
                                      text.data, text.len);
                        }
                    }
                }
                collecting_text = 0;
                sb_free(&text);
                text_tag[0] = '\0';
                text_tag_len = 0;

                if (sp >= 0 && stack[sp].kind == INV_POLMAP &&
                    tag_eq(stack[sp].tag, strlen(stack[sp].tag), loc)) {
                    in_policy_map = 0;
                    sp--;
                } else if (sp >= 0 && stack[sp].kind == INV_VLAN &&
                           tag_eq(stack[sp].tag, strlen(stack[sp].tag), loc)) {
                    if (cur_svc) {
                        inv_infer_svc_kind(cur_svc, "ont-ethernet");
                        /* drop empty services (no vlan/profile) */
                        if (cur_svc->vlan[0] == '\0' &&
                            cur_svc->profile[0] == '\0' &&
                            cur_svc->name[0] == '\0' && cur_port &&
                            cur_port->service_count > 0) {
                            cur_port->service_count--;
                        }
                    }
                    cur_svc = NULL;
                    sp--;
                } else if (sp >= 0 && stack[sp].kind == INV_SVC &&
                           tag_eq(stack[sp].tag, strlen(stack[sp].tag), loc)) {
                    if (cur_svc) {
                        inv_infer_svc_kind(cur_svc, stack[sp].tag);
                    }
                    cur_svc = NULL;
                    sp--;
                } else if (sp >= 0 && stack[sp].kind == INV_PORT &&
                           tag_eq(stack[sp].tag, strlen(stack[sp].tag), loc)) {
                    /* pots with no explicit service still counts as a port */
                    cur_port = NULL;
                    cur_svc = NULL;
                    sp--;
                } else if (sp >= 0 && stack[sp].kind == INV_ONT &&
                           tag_eq(stack[sp].tag, strlen(stack[sp].tag), loc)) {
                    inv_finish_ont(inv, &cur, &have_ont, vendor, serial_only);
                    cur_port = NULL;
                    cur_svc = NULL;
                    in_policy_map = 0;
                    sp--;
                } else if (sp >= 0 &&
                           tag_eq(stack[sp].tag, strlen(stack[sp].tag), loc)) {
                    if (stack[sp].kind == INV_POLMAP) {
                        in_policy_map = 0;
                    }
                    sp--;
                }
            } else {
                /* open tag */
                if (ln == 3 && memcmp(loc, "ont", 3) == 0) {
                    /* Match bare <ont>, not ont-ethernet / ont-profile / ont-ua */
                    inv_finish_ont(inv, &cur, &have_ont, vendor, serial_only);
                    have_ont = 1;
                    memset(&cur, 0, sizeof(cur));
                    cur_port = NULL;
                    cur_svc = NULL;
                    in_policy_map = 0;
                    if (sp + 1 < E7_INV_STACK) {
                        sp++;
                        stack[sp].kind = INV_ONT;
                        memcpy(stack[sp].tag, loc, ln);
                        stack[sp].tag[ln] = '\0';
                    }
                } else if (have_ont && inv_is_port_tag(loc, ln)) {
                    char ptag[48];
                    memcpy(ptag, loc, ln);
                    ptag[ln] = '\0';
                    if (cur.port_count < EDGE_E7_CFG_PORTS_MAX) {
                        cur_port = &cur.ports[cur.port_count++];
                        memset(cur_port, 0, sizeof(*cur_port));
                        snprintf(cur_port->type, sizeof(cur_port->type), "%s",
                                 inv_port_type_for_tag(ptag));
                    } else {
                        inv->truncated = 1;
                        cur_port = NULL;
                    }
                    cur_svc = NULL;
                    if (sp + 1 < E7_INV_STACK) {
                        sp++;
                        stack[sp].kind = INV_PORT;
                        memcpy(stack[sp].tag, loc, ln);
                        stack[sp].tag[ln] = '\0';
                    }
                } else if (have_ont && cur_port && ln == 4 &&
                           memcmp(loc, "vlan", 4) == 0 &&
                           inv_port_depth(stack, sp) >= 0) {
                    /* AXOS service: <vlan> under ont-ethernet / ont-ua */
                    if (cur_port->service_count < EDGE_E7_CFG_SVCS_MAX) {
                        cur_svc = &cur_port->services[cur_port->service_count++];
                        memset(cur_svc, 0, sizeof(*cur_svc));
                    } else {
                        inv->truncated = 1;
                        cur_svc = NULL;
                    }
                    if (sp + 1 < E7_INV_STACK) {
                        sp++;
                        stack[sp].kind = INV_VLAN;
                        memcpy(stack[sp].tag, loc, ln);
                        stack[sp].tag[ln] = '\0';
                    }
                } else if (have_ont && cur_port &&
                           inv_is_legacy_svc_tag(loc, ln)) {
                    if (cur_port->service_count < EDGE_E7_CFG_SVCS_MAX) {
                        cur_svc = &cur_port->services[cur_port->service_count++];
                        memset(cur_svc, 0, sizeof(*cur_svc));
                    } else {
                        inv->truncated = 1;
                        cur_svc = NULL;
                    }
                    if (sp + 1 < E7_INV_STACK) {
                        sp++;
                        stack[sp].kind = INV_SVC;
                        memcpy(stack[sp].tag, loc, ln);
                        stack[sp].tag[ln] = '\0';
                    }
                } else if (have_ont && cur_svc && ln == 10 &&
                           memcmp(loc, "policy-map", 10) == 0) {
                    in_policy_map = 1;
                    if (sp + 1 < E7_INV_STACK) {
                        sp++;
                        stack[sp].kind = INV_POLMAP;
                        memcpy(stack[sp].tag, loc, ln);
                        stack[sp].tag[ln] = '\0';
                    }
                } else if (have_ont && !self_close) {
                    collecting_text = 1;
                    sb_free(&text);
                    memcpy(text_tag, loc, ln);
                    text_tag[ln] = '\0';
                    text_tag_len = ln;
                    if (sp + 1 < E7_INV_STACK) {
                        sp++;
                        stack[sp].kind = INV_NONE;
                        memcpy(stack[sp].tag, loc, ln);
                        stack[sp].tag[ln] = '\0';
                    }
                }
                if (self_close) {
                    collecting_text = 0;
                    if (sp >= 0 && tag_eq(stack[sp].tag, strlen(stack[sp].tag),
                                          loc)) {
                        if (stack[sp].kind == INV_PORT) {
                            cur_port = NULL;
                        }
                        sp--;
                    }
                }
            }
        }
    }
    inv_finish_ont(inv, &cur, &have_ont, vendor, serial_only);
    sb_free(&text);
    (void)inv_in_ont;
    return 0;
}

int edge_e7_cfg_ont_json(const edge_e7_cfg_ont_t *ont, const char *shelf_id,
                         const char *captured_at, char *buf, size_t buf_sz)
{
    e7_sb_t sb;
    int p, s;
    int rc = -1;

    if (!ont || !buf || buf_sz < 8) {
        return -1;
    }
    memset(&sb, 0, sizeof(sb));
    if (sb_append_str(&sb, "{\"v\":1") != 0) {
        goto done;
    }
    if (shelf_id && shelf_id[0]) {
        if (sb_append_str(&sb, ",\"shelf_id\":") != 0 ||
            sb_append_json_str(&sb, shelf_id, strlen(shelf_id)) != 0) {
            goto done;
        }
    }
    if (sb_append_str(&sb, ",\"ont_id\":") != 0 ||
        sb_append_json_str(&sb, ont->ont_id, strlen(ont->ont_id)) != 0 ||
        sb_append_str(&sb, ",\"fsan\":") != 0 ||
        sb_append_json_str(&sb, ont->fsan, strlen(ont->fsan)) != 0 ||
        sb_append_str(&sb, ",\"account\":") != 0 ||
        sb_append_json_str(&sb, ont->account, strlen(ont->account)) != 0 ||
        sb_append_str(&sb, ",\"pon_id\":") != 0 ||
        sb_append_json_str(&sb, ont->pon_id, strlen(ont->pon_id)) != 0 ||
        sb_append_str(&sb, ",\"model\":") != 0 ||
        sb_append_json_str(&sb, ont->model, strlen(ont->model)) != 0 ||
        sb_append_str(&sb, ",\"admin_state\":") != 0 ||
        sb_append_json_str(&sb, ont->admin_state, strlen(ont->admin_state)) !=
            0 ||
        sb_append_str(&sb, ",\"ports\":[") != 0) {
        goto done;
    }
    for (p = 0; p < ont->port_count; p++) {
        const edge_e7_cfg_port_t *pt = &ont->ports[p];
        if (p > 0 && sb_append_ch(&sb, ',') != 0) {
            goto done;
        }
        if (sb_append_str(&sb, "{\"port\":") != 0 ||
            sb_append_json_str(&sb, pt->port, strlen(pt->port)) != 0 ||
            sb_append_str(&sb, ",\"type\":") != 0 ||
            sb_append_json_str(&sb, pt->type, strlen(pt->type)) != 0 ||
            sb_append_str(&sb, ",\"services\":[") != 0) {
            goto done;
        }
        for (s = 0; s < pt->service_count; s++) {
            const edge_e7_cfg_service_t *sv = &pt->services[s];
            if (s > 0 && sb_append_ch(&sb, ',') != 0) {
                goto done;
            }
            if (sb_append_str(&sb, "{\"kind\":") != 0 ||
                sb_append_json_str(&sb, sv->kind, strlen(sv->kind)) != 0 ||
                sb_append_str(&sb, ",\"name\":") != 0 ||
                sb_append_json_str(&sb, sv->name, strlen(sv->name)) != 0 ||
                sb_append_str(&sb, ",\"vlan\":") != 0 ||
                sb_append_json_str(&sb, sv->vlan, strlen(sv->vlan)) != 0 ||
                sb_append_str(&sb, ",\"profile\":") != 0 ||
                sb_append_json_str(&sb, sv->profile, strlen(sv->profile)) != 0 ||
                sb_append_ch(&sb, '}') != 0) {
                goto done;
            }
        }
        if (sb_append_str(&sb, "]}") != 0) {
            goto done;
        }
    }
    if (sb_append_ch(&sb, ']') != 0) {
        goto done;
    }
    if (captured_at && captured_at[0]) {
        if (sb_append_str(&sb, ",\"captured_at\":") != 0 ||
            sb_append_json_str(&sb, captured_at, strlen(captured_at)) != 0) {
            goto done;
        }
    }
    if (sb_append_ch(&sb, '}') != 0) {
        goto done;
    }
    if (sb.len + 1 > buf_sz) {
        goto done;
    }
    memcpy(buf, sb.data, sb.len + 1);
    rc = (int)sb.len;

done:
    sb_free(&sb);
    return rc;
}

int edge_e7_cfg_summary_json(const edge_e7_cfg_inventory_t *inv, char *buf,
                             size_t buf_sz)
{
    int n;
    if (!inv || !buf || buf_sz < 16) {
        return -1;
    }
    n = snprintf(buf, buf_sz,
                 "{\"ont_count\":%d,\"service_count\":%d,\"account_count\":%d,"
                 "\"truncated\":%s,\"warnings\":%s%s%s}",
                 inv->ont_count, inv->service_count, inv->account_count,
                 inv->truncated ? "true" : "false",
                 inv->warnings[0] ? "\"" : "null",
                 inv->warnings[0] ? inv->warnings : "",
                 inv->warnings[0] ? "\"" : "");
    if (n < 0 || (size_t)n >= buf_sz) {
        return -1;
    }
    return n;
}

int edge_e7_cfg_inventory_json(const edge_e7_cfg_inventory_t *inv,
                               const char *shelf_id, const char *captured_at,
                               char *buf, size_t buf_sz)
{
    e7_sb_t sb;
    int i;
    int rc = -1;
    char one[2048];

    if (!inv || !buf || buf_sz < 4) {
        return -1;
    }
    memset(&sb, 0, sizeof(sb));
    if (sb_append_ch(&sb, '[') != 0) {
        goto done;
    }
    for (i = 0; i < inv->ont_count; i++) {
        int n = edge_e7_cfg_ont_json(&inv->onts[i], shelf_id, captured_at, one,
                                     sizeof(one));
        if (n < 0) {
            goto done;
        }
        if (i > 0 && sb_append_ch(&sb, ',') != 0) {
            goto done;
        }
        if (sb_append(&sb, one, (size_t)n) != 0) {
            goto done;
        }
    }
    if (sb_append_ch(&sb, ']') != 0) {
        goto done;
    }
    if (sb.len + 1 > buf_sz) {
        goto done;
    }
    memcpy(buf, sb.data, sb.len + 1);
    rc = (int)sb.len;

done:
    sb_free(&sb);
    return rc;
}
