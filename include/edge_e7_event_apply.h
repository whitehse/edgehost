/**
 * @file edge_e7_event_apply.h
 * @brief E7 lab.v1 notification → net.pon state apply + identity/MAC helpers.
 *
 * Pure helpers (no TCP / libnetconf). lab.v1 is a synthetic extractor for
 * phase-1 tests — not a Calix wire format. Fixtures use
 * urn:edgehost:lab:e7:1.0 (not forged Calix URIs).
 *
 * Key patterns (namespace net.pon):
 *   e7/{mac_key}/ont/{ont_key}   mac_key = colon→hyphen MAC
 *   e7/{mac_key}/pon/{pon_key}   ont/pon key = slash→hyphen AID
 */
#ifndef EDGE_E7_EVENT_APPLY_H
#define EDGE_E7_EVENT_APPLY_H

#include "edge_state.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Normalized MAC "aa:bb:cc:dd:ee:ff" + NUL. */
#define EDGE_E7_MAC_MAX 18
/** Serial / model / source-ip attribute buffers (identity preamble). */
#define EDGE_E7_SERIAL_MAX 32
#define EDGE_E7_MODEL_MAX 64
#define EDGE_E7_SOURCE_IP_MAX 64
/** Junos DEVICE-ID / shelf key (not necessarily a MAC). */
#define EDGE_E7_DEVICE_ID_MAX 128
/** Shared secret for Junos HOST-KEY HMAC (outbound-ssh secret). */
#define EDGE_E7_SECRET_MAX 128
/** OpenSSH public host key line from HOST-KEY: field. */
#define EDGE_E7_HOST_KEY_MAX 2048
/** Hex HMAC from initiation sequence. */
#define EDGE_E7_HMAC_MAX 128
/** Vendor tag: "calix" | "junos" | "". */
#define EDGE_E7_VENDOR_MAX 16
/** AID / key segment (e.g. 1/1/3/12 or 1-1-3-12) + NUL. */
#define EDGE_E7_AID_MAX 64
/** ISO-8601 eventTime + NUL. */
#define EDGE_E7_EVENT_TIME_MAX 40
/** Alarm / severity / oper-state short tokens. */
#define EDGE_E7_TOKEN_MAX 32

/**
 * Call Home identity preamble (pre-SSH; not NETCONF hello).
 * Calix: mac required (XML identity). Junos: device_id required (DEVICE-CONN-INFO).
 */
typedef struct {
    char mac[EDGE_E7_MAC_MAX];
    char serial[EDGE_E7_SERIAL_MAX];
    char model[EDGE_E7_MODEL_MAX];
    char source_ip[EDGE_E7_SOURCE_IP_MAX];
    char device_id[EDGE_E7_DEVICE_ID_MAX]; /* Junos DEVICE-ID */
    char vendor[EDGE_E7_VENDOR_MAX];       /* "calix" | "junos" */
    char host_key[EDGE_E7_HOST_KEY_MAX];   /* Junos HOST-KEY (optional) */
    char hmac[EDGE_E7_HMAC_MAX];           /* Junos HMAC:… (optional) */
    int  identity_ok; /* 1 if identity complete */
    int  has_host_key;
    int  has_hmac;
    size_t consumed; /* bytes of preamble consumed (SSH may follow) */
} edge_e7_identity_t;

typedef enum {
    EDGE_E7_APPLY_OK = 0,
    EDGE_E7_APPLY_BAD_ARG,
    EDGE_E7_APPLY_BAD_MAC,
    EDGE_E7_APPLY_BAD_XML,
    EDGE_E7_APPLY_UNKNOWN_EVENT,
    EDGE_E7_APPLY_STATE_ERR
} edge_e7_apply_err_t;

const char *edge_e7_apply_err_name(edge_e7_apply_err_t e);

/**
 * Normalize MAC to lowercase colon form "aa:bb:cc:dd:ee:ff".
 * Accepts colon, hyphen, or bare 12-hex input (case-insensitive).
 * @return 0 ok, -1 invalid / buffer too small (need EDGE_E7_MAC_MAX).
 */
int edge_e7_mac_normalize(const char *in, char *out, size_t out_sz);

/**
 * Convert normalized colon MAC to state key segment (':' → '-').
 * @return 0 ok, -1 on error.
 */
int edge_e7_mac_to_key_seg(const char *mac, char *out, size_t out_sz);

/**
 * Convert Calix-style AID (e.g. 1/1/3/12) to key segment (slash → hyphen,
 * lowercase). JSON ont_id/pon_id keep slash form.
 * @return 0 ok, -1 on error.
 */
int edge_e7_aid_to_key_seg(const char *aid, char *out, size_t out_sz);

/**
 * Parse Calix-shaped identity preamble XML:
 *   <version>1</version><identity><mac>…</mac><serial-number>…</serial-number>
 *   <model-name>…</model-name><source-ip>…</source-ip></identity>
 * mac required; others optional. MAC is normalized on success.
 * @return 0 ok (identity_ok set), -1 missing/invalid mac or bad args.
 */
int edge_e7_identity_parse(const char *xml, size_t len,
                           edge_e7_identity_t *identity);

/**
 * True if @p buf looks like a Junos DEVICE-CONN-INFO initiation sequence
 * (MSG-ID: DEVICE-CONN-INFO).
 */
int edge_e7_junos_identity_looks_like(const char *buf, size_t len);

/**
 * Parse Junos NETCONF Call Home initiation sequence (outbound-ssh):
 *   MSG-ID: DEVICE-CONN-INFO\\r\\n
 *   MSG-VER: V1\\r\\n
 *   DEVICE-ID: <device-id>\\r\\n
 *   [HOST-KEY: <public-host-key>\\r\\n
 *    HMAC:<hex>\\r\\n ]   optional when secret is configured on Junos
 *
 * Sets vendor=junos, device_id, optional host_key/hmac, consumed byte count.
 * identity_ok when DEVICE-ID present and (no HOST-KEY or HOST-KEY+HMAC both set).
 * @return 0 complete, 1 incomplete (need more data), -1 invalid.
 */
int edge_e7_junos_identity_parse(const char *buf, size_t len,
                                 edge_e7_identity_t *identity);

/**
 * Verify Junos HOST-KEY HMAC against shared secret (outbound-ssh secret).
 * Juniper docs: SHA1/HMAC derived in part from secret + public host key.
 * Tries HMAC-SHA1(key=secret, data=HOST-KEY) then swapped key/data order
 * (case-insensitive hex compare). Field name is HMAC: even when secret is unset
 * on the NMS (verification skipped).
 * @return 0 match, -1 mismatch / bad args.
 */
int edge_e7_junos_hmac_verify(const char *host_key, const char *hmac_hex,
                              const char *secret);

/**
 * Sanitize device-id / shelf key for state paths (alnum keep; else '-').
 * @return 0 ok, -1 error.
 */
int edge_e7_device_key_seg(const char *device_id, char *out, size_t out_sz);

/**
 * Parse lab.v1 NETCONF notification XML and put ONT/PON state under net.pon.
 *
 * Recognized bodies (namespace urn:edgehost:lab:e7:1.0):
 *   ont-event:  ont-id, pon-id, oper-state → key e7/{mac_key}/ont/{ont_key}
 *   pon-alarm:  pon-id, alarm, severity    → key e7/{mac_key}/pon/{pon_key}
 *
 * Optional ONT geometry (PR-9 partial): if both &lt;lon&gt;/&lt;lat&gt; (or
 * longitude/latitude) are present and finite, also put map.dynamic key
 * `ont/{mac_key}/{ont_key}` with dynamic_feed-shaped JSON. Map put failure
 * (ns disabled) is ignored when net.pon put succeeded.
 *
 * @p mac_colon is the shelf MAC (any accepted normalize form); stored in JSON
 * as lowercase colon MAC. eventTime from the notification is copied when present.
 * @return EDGE_E7_APPLY_OK or error; state put errors map to STATE_ERR
 *         (including NS_DISABLED if net.pon is not enabled).
 */
edge_e7_apply_err_t edge_e7_event_apply_lab_v1(edge_state_store_t *store,
                                               const char *mac_colon,
                                               const char *notification_xml,
                                               size_t xml_len);

/**
 * Apply a Calix EXA notification (xmlns http://www.calix.com/ns/exa/...).
 *
 * Keys under net.pon:
 *   e7/{mac_key}/event/latest
 *   e7/{mac_key}/event/{device-sequence-number}  (when seq present)
 *
 * JSON includes name, id, category, description, event_time, alarm, and
 * common session fields when present (source "calix.exa").
 *
 * @return EDGE_E7_APPLY_OK, UNKNOWN_EVENT if not a Calix EXA body, or STATE_ERR.
 */
edge_e7_apply_err_t edge_e7_event_apply_calix_exa(edge_state_store_t *store,
                                                  const char *mac_colon,
                                                  const char *notification_xml,
                                                  size_t xml_len);

/**
 * Unified apply: try lab.v1 first, then Calix EXA.
 * On success, if @p out_key non-NULL, writes the primary net.pon key for
 * WebSocket notify (event/latest for Calix; ont/pon key for lab.v1).
 * If @p out_source non-NULL, writes "lab.v1" or "calix.exa".
 */
edge_e7_apply_err_t edge_e7_event_apply(edge_state_store_t *store,
                                        const char *mac_colon,
                                        const char *notification_xml,
                                        size_t xml_len, char *out_key,
                                        size_t out_key_sz, char *out_source,
                                        size_t out_source_sz);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_E7_EVENT_APPLY_H */
