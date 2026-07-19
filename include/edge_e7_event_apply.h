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
/** AID / key segment (e.g. 1/1/3/12 or 1-1-3-12) + NUL. */
#define EDGE_E7_AID_MAX 64
/** ISO-8601 eventTime + NUL. */
#define EDGE_E7_EVENT_TIME_MAX 40
/** Alarm / severity / oper-state short tokens. */
#define EDGE_E7_TOKEN_MAX 32

/**
 * Calix-shaped Call Home identity preamble fields (pre-SSH; not NETCONF hello).
 * mac is required and stored normalized (lowercase colon form).
 */
typedef struct {
    char mac[EDGE_E7_MAC_MAX];
    char serial[EDGE_E7_SERIAL_MAX];
    char model[EDGE_E7_MODEL_MAX];
    char source_ip[EDGE_E7_SOURCE_IP_MAX];
    int  identity_ok; /* 1 if mac parsed and normalized */
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
 * Parse lab.v1 NETCONF notification XML and put ONT/PON state under net.pon.
 *
 * Recognized bodies (namespace urn:edgehost:lab:e7:1.0):
 *   ont-event:  ont-id, pon-id, oper-state → key e7/{mac_key}/ont/{ont_key}
 *   pon-alarm:  pon-id, alarm, severity    → key e7/{mac_key}/pon/{pon_key}
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

#ifdef __cplusplus
}
#endif

#endif /* EDGE_E7_EVENT_APPLY_H */
