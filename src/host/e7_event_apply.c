/**
 * @file e7_event_apply.c
 * @brief lab.v1 ONT/PON apply + MAC/identity helpers (PR-3).
 *
 * lab.v1 is not a Calix wire format — synthetic fixtures only until field
 * samples exist (see docs/designs/e7-netconf-callhome.md).
 */

#include "edge_e7_event_apply.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *edge_e7_apply_err_name(edge_e7_apply_err_t e)
{
    switch (e) {
    case EDGE_E7_APPLY_OK:             return "OK";
    case EDGE_E7_APPLY_BAD_ARG:        return "BAD_ARG";
    case EDGE_E7_APPLY_BAD_MAC:        return "BAD_MAC";
    case EDGE_E7_APPLY_BAD_XML:        return "BAD_XML";
    case EDGE_E7_APPLY_UNKNOWN_EVENT:  return "UNKNOWN_EVENT";
    case EDGE_E7_APPLY_STATE_ERR:      return "STATE_ERR";
    default:                           return "UNKNOWN";
    }
}

int edge_e7_mac_normalize(const char *in, char *out, size_t out_sz)
{
    unsigned char hex[12];
    size_t n = 0;
    size_t i;

    if (!in || !out || out_sz < EDGE_E7_MAC_MAX) {
        return -1;
    }
    for (i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ':' || c == '-' || c == '.' || c == ' ' || c == '\t') {
            continue;
        }
        if (c >= '0' && c <= '9') {
            if (n >= 12) {
                return -1;
            }
            hex[n++] = (unsigned char)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            if (n >= 12) {
                return -1;
            }
            hex[n++] = (unsigned char)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            if (n >= 12) {
                return -1;
            }
            hex[n++] = (unsigned char)(c - 'A' + 10);
        } else {
            return -1;
        }
    }
    if (n != 12) {
        return -1;
    }
    /* aa:bb:cc:dd:ee:ff */
    for (i = 0; i < 6; i++) {
        unsigned v = (unsigned)((hex[i * 2] << 4) | hex[i * 2 + 1]);
        static const char *digits = "0123456789abcdef";
        out[i * 3] = digits[(v >> 4) & 0xf];
        out[i * 3 + 1] = digits[v & 0xf];
        out[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
    return 0;
}

int edge_e7_mac_to_key_seg(const char *mac, char *out, size_t out_sz)
{
    char norm[EDGE_E7_MAC_MAX];
    size_t i;

    if (!mac || !out || out_sz < EDGE_E7_MAC_MAX) {
        return -1;
    }
    /* Accept either already-normalized colon form or any normalizeable MAC. */
    if (edge_e7_mac_normalize(mac, norm, sizeof(norm)) != 0) {
        return -1;
    }
    for (i = 0; norm[i] != '\0'; i++) {
        out[i] = (norm[i] == ':') ? '-' : norm[i];
    }
    out[i] = '\0';
    return 0;
}

int edge_e7_aid_to_key_seg(const char *aid, char *out, size_t out_sz)
{
    size_t i = 0;

    if (!aid || !out || out_sz == 0 || aid[0] == '\0') {
        return -1;
    }
    for (; aid[i] != '\0'; i++) {
        char c = aid[i];
        if (i + 1 >= out_sz) {
            return -1;
        }
        if (c == '/') {
            c = '-';
        } else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[i] = '\0';
    return 0;
}

/**
 * Extract text content of first element named @p local (no namespace prefix
 * required on the open tag). Handles attributes on the open tag.
 * Case-sensitive local name. Copies into out (NUL-terminated), trims leading
 * and trailing ASCII whitespace.
 * @return 0 ok, -1 missing/invalid/overflow.
 */
static int xml_elem_text(const char *xml, size_t len, const char *local,
                         char *out, size_t out_sz)
{
    char close[96];
    size_t local_len;
    const char *end = xml + len;
    const char *p;
    const char *gt;
    const char *close_p;
    const char *text;
    size_t tlen;
    size_t i;

    if (!xml || !local || !out || out_sz == 0 || len == 0) {
        return -1;
    }
    local_len = strlen(local);
    if (local_len == 0 || local_len + 3 >= sizeof(close)) {
        return -1;
    }
    /* Build "</local>" for close match. */
    if ((size_t)snprintf(close, sizeof(close), "</%s>", local) >= sizeof(close)) {
        return -1;
    }

    p = xml;
    while (p < end) {
        const char *hit = NULL;
        size_t remain = (size_t)(end - p);
        size_t j;

        /* Manual search so we do not require a NUL-terminated xml buffer. */
        if (remain < local_len + 1) {
            break;
        }
        for (j = 0; j + local_len + 1 <= remain; j++) {
            if (p[j] == '<' &&
                memcmp(p + j + 1, local, local_len) == 0) {
                char after = p[j + 1 + local_len];
                if (after == '>' || after == '/' || after == ' ' ||
                    after == '\t' || after == '\n' || after == '\r') {
                    hit = p + j;
                    break;
                }
            }
        }
        if (!hit) {
            return -1;
        }
        /* Open tag ends at '>'. */
        gt = hit + 1 + local_len;
        while (gt < end && *gt != '>') {
            gt++;
        }
        if (gt >= end) {
            return -1;
        }
        /* Self-closing <local ... /> → empty text. */
        if (gt > hit && gt[-1] == '/') {
            if (out_sz < 1) {
                return -1;
            }
            out[0] = '\0';
            return 0;
        }
        text = gt + 1;
        /* Find close tag within remaining buffer. */
        close_p = NULL;
        {
            size_t clen = strlen(close);
            size_t k;
            size_t rem2 = (size_t)(end - text);
            for (k = 0; k + clen <= rem2; k++) {
                if (memcmp(text + k, close, clen) == 0) {
                    close_p = text + k;
                    break;
                }
            }
        }
        if (!close_p) {
            /* Try next candidate open tag. */
            p = hit + 1;
            continue;
        }
        /* Trim whitespace. */
        while (text < close_p &&
               (*text == ' ' || *text == '\t' || *text == '\n' ||
                *text == '\r')) {
            text++;
        }
        tlen = (size_t)(close_p - text);
        while (tlen > 0) {
            char c = text[tlen - 1];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                tlen--;
            } else {
                break;
            }
        }
        if (tlen + 1 > out_sz) {
            return -1;
        }
        for (i = 0; i < tlen; i++) {
            out[i] = text[i];
        }
        out[tlen] = '\0';
        return 0;
    }
    return -1;
}

/** True if buffer contains element open for @p local (boundary-checked). */
static int xml_has_elem(const char *xml, size_t len, const char *local)
{
    size_t local_len = strlen(local);
    size_t j;

    if (!xml || len < local_len + 1) {
        return 0;
    }
    for (j = 0; j + local_len + 1 <= len; j++) {
        if (xml[j] == '<' && memcmp(xml + j + 1, local, local_len) == 0) {
            char after = xml[j + 1 + local_len];
            if (after == '>' || after == '/' || after == ' ' || after == '\t' ||
                after == '\n' || after == '\r') {
                return 1;
            }
        }
    }
    return 0;
}

int edge_e7_identity_parse(const char *xml, size_t len,
                           edge_e7_identity_t *identity)
{
    char mac_raw[EDGE_E7_MAC_MAX + 16];

    if (!xml || !identity || len == 0) {
        return -1;
    }
    memset(identity, 0, sizeof(*identity));

    if (xml_elem_text(xml, len, "mac", mac_raw, sizeof(mac_raw)) != 0) {
        return -1;
    }
    if (edge_e7_mac_normalize(mac_raw, identity->mac, sizeof(identity->mac)) !=
        0) {
        return -1;
    }
    /* Optional attributes — ignore failure. */
    (void)xml_elem_text(xml, len, "serial-number", identity->serial,
                        sizeof(identity->serial));
    (void)xml_elem_text(xml, len, "model-name", identity->model,
                        sizeof(identity->model));
    (void)xml_elem_text(xml, len, "source-ip", identity->source_ip,
                        sizeof(identity->source_ip));
    identity->identity_ok = 1;
    return 0;
}

static edge_e7_apply_err_t apply_state(edge_state_store_t *store,
                                       const char *key, const char *json)
{
    edge_state_err_t e;

    e = edge_state_put(store, "net.pon", key, json, strlen(json));
    if (e != EDGE_STATE_OK) {
        return EDGE_E7_APPLY_STATE_ERR;
    }
    return EDGE_E7_APPLY_OK;
}

/**
 * Parse a coordinate token; rejects empty, trailing junk, NaN/Inf spellings.
 * @return 0 ok, -1 invalid.
 */
static int parse_coord(const char *s, double *out)
{
    char *end = NULL;
    double v;
    size_t i;

    if (!s || !s[0] || !out) {
        return -1;
    }
    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c == 'n' || c == 'N' || c == 'i' || c == 'I') {
            return -1; /* reject nan/inf */
        }
    }
    errno = 0;
    v = strtod(s, &end);
    if (end == s || *end != '\0' || errno == ERANGE) {
        return -1;
    }
    /* Reject non-finite without libm: Inf fails v==v? no NaN; Inf*0 != 0. */
    if (v != v || (v * 0.0) != 0.0) {
        return -1;
    }
    *out = v;
    return 0;
}

/** Map oper-state token → map.dynamic status (up→ok, down→down, else unknown). */
static const char *oper_to_map_status(const char *oper)
{
    if (oper && strcmp(oper, "up") == 0) {
        return "ok";
    }
    if (oper && strcmp(oper, "down") == 0) {
        return "down";
    }
    return "unknown";
}

/**
 * Optional PR-9 map.dynamic ONT mirror when lon/lat present.
 * Ignores put failure (ns disabled) so net.pon success still wins.
 */
static void try_map_dynamic_ont(edge_state_store_t *store, const char *mac,
                                const char *mac_key, const char *ont_id,
                                const char *ont_key, const char *oper,
                                const char *event_time, const char *xml,
                                size_t xml_len)
{
    char lon_s[64];
    char lat_s[64];
    double lon = 0.0;
    double lat = 0.0;
    char key[EDGE_STATE_KEY_MAX];
    char json[768];
    int n;
    edge_state_err_t e;
    const char *status;

    if (!store || !mac || !mac_key || !ont_id || !ont_key || !xml) {
        return;
    }
    lon_s[0] = lat_s[0] = '\0';
    if (xml_elem_text(xml, xml_len, "lon", lon_s, sizeof(lon_s)) != 0) {
        (void)xml_elem_text(xml, xml_len, "longitude", lon_s, sizeof(lon_s));
    }
    if (xml_elem_text(xml, xml_len, "lat", lat_s, sizeof(lat_s)) != 0) {
        (void)xml_elem_text(xml, xml_len, "latitude", lat_s, sizeof(lat_s));
    }
    if (lon_s[0] == '\0' || lat_s[0] == '\0') {
        return;
    }
    if (parse_coord(lon_s, &lon) != 0 || parse_coord(lat_s, &lat) != 0) {
        return;
    }
    n = snprintf(key, sizeof(key), "ont/%s/%s", mac_key, ont_key);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        return;
    }
    status = oper_to_map_status(oper);
    n = snprintf(
        json, sizeof(json),
        "{\"id\":\"%s/%s\",\"class\":\"alert\",\"status\":\"%s\","
        "\"updated_at\":\"%s\",\"mac\":\"%s\",\"ont_id\":\"%s\","
        "\"oper_state\":\"%s\",\"geom\":{\"type\":\"Point\",\"coordinates\":"
        "[%.6f,%.6f]},\"lon\":%.6f,\"lat\":%.6f,\"source\":\"lab.v1\"}",
        mac_key, ont_key, status, event_time ? event_time : "", mac, ont_id,
        oper ? oper : "", lon, lat, lon, lat);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return;
    }
    e = edge_state_put(store, "map.dynamic", key, json, (size_t)n);
    (void)e; /* ignore NS_DISABLED / capacity — net.pon already applied */
}

edge_e7_apply_err_t edge_e7_event_apply_lab_v1(edge_state_store_t *store,
                                               const char *mac_colon,
                                               const char *notification_xml,
                                               size_t xml_len)
{
    char mac[EDGE_E7_MAC_MAX];
    char mac_key[EDGE_E7_MAC_MAX];
    char event_time[EDGE_E7_EVENT_TIME_MAX];
    char key[EDGE_STATE_KEY_MAX];
    char json[512];
    int n;

    if (!store || !mac_colon || !notification_xml || xml_len == 0) {
        return EDGE_E7_APPLY_BAD_ARG;
    }
    if (edge_e7_mac_normalize(mac_colon, mac, sizeof(mac)) != 0) {
        return EDGE_E7_APPLY_BAD_MAC;
    }
    if (edge_e7_mac_to_key_seg(mac, mac_key, sizeof(mac_key)) != 0) {
        return EDGE_E7_APPLY_BAD_MAC;
    }

    event_time[0] = '\0';
    (void)xml_elem_text(notification_xml, xml_len, "eventTime", event_time,
                        sizeof(event_time));

    if (xml_has_elem(notification_xml, xml_len, "ont-event")) {
        char ont_id[EDGE_E7_AID_MAX];
        char pon_id[EDGE_E7_AID_MAX];
        char oper[EDGE_E7_TOKEN_MAX];
        char ont_key[EDGE_E7_AID_MAX];
        edge_e7_apply_err_t ae;

        if (xml_elem_text(notification_xml, xml_len, "ont-id", ont_id,
                          sizeof(ont_id)) != 0 ||
            xml_elem_text(notification_xml, xml_len, "pon-id", pon_id,
                          sizeof(pon_id)) != 0 ||
            xml_elem_text(notification_xml, xml_len, "oper-state", oper,
                          sizeof(oper)) != 0) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        if (edge_e7_aid_to_key_seg(ont_id, ont_key, sizeof(ont_key)) != 0) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        n = snprintf(key, sizeof(key), "e7/%s/ont/%s", mac_key, ont_key);
        if (n < 0 || (size_t)n >= sizeof(key)) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        n = snprintf(json, sizeof(json),
                     "{\"ont_id\":\"%s\",\"pon_id\":\"%s\",\"oper_state\":\"%s\","
                     "\"mac\":\"%s\",\"source\":\"lab.v1\",\"event_time\":\"%s\"}",
                     ont_id, pon_id, oper, mac, event_time);
        if (n < 0 || (size_t)n >= sizeof(json)) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        ae = apply_state(store, key, json);
        if (ae == EDGE_E7_APPLY_OK) {
            try_map_dynamic_ont(store, mac, mac_key, ont_id, ont_key, oper,
                                event_time, notification_xml, xml_len);
        }
        return ae;
    }

    if (xml_has_elem(notification_xml, xml_len, "pon-alarm")) {
        char pon_id[EDGE_E7_AID_MAX];
        char alarm[EDGE_E7_TOKEN_MAX];
        char severity[EDGE_E7_TOKEN_MAX];
        char pon_key[EDGE_E7_AID_MAX];

        if (xml_elem_text(notification_xml, xml_len, "pon-id", pon_id,
                          sizeof(pon_id)) != 0 ||
            xml_elem_text(notification_xml, xml_len, "alarm", alarm,
                          sizeof(alarm)) != 0 ||
            xml_elem_text(notification_xml, xml_len, "severity", severity,
                          sizeof(severity)) != 0) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        if (edge_e7_aid_to_key_seg(pon_id, pon_key, sizeof(pon_key)) != 0) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        n = snprintf(key, sizeof(key), "e7/%s/pon/%s", mac_key, pon_key);
        if (n < 0 || (size_t)n >= sizeof(key)) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        n = snprintf(json, sizeof(json),
                     "{\"pon_id\":\"%s\",\"alarm\":\"%s\",\"severity\":\"%s\","
                     "\"mac\":\"%s\",\"source\":\"lab.v1\",\"event_time\":\"%s\"}",
                     pon_id, alarm, severity, mac, event_time);
        if (n < 0 || (size_t)n >= sizeof(json)) {
            return EDGE_E7_APPLY_BAD_XML;
        }
        return apply_state(store, key, json);
    }

    return EDGE_E7_APPLY_UNKNOWN_EVENT;
}
