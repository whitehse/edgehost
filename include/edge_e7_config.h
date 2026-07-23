/**
 * @file edge_e7_config.h
 * @brief E7 running-config XML → JSON + ONT provision inventory extract.
 *
 * Host helpers (no TCP). Used after NETCONF get-config to store jsonb in
 * Postgres and present account / FSAN / eth-port services in the SPA.
 *
 * Tag matching is namespace-tolerant (local-name only) and alias-friendly
 * until a live EXA dump locks the field map.
 */
#ifndef EDGE_E7_CONFIG_H
#define EDGE_E7_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_E7_CFG_ID_MAX       48
#define EDGE_E7_CFG_FSAN_MAX     40
#define EDGE_E7_CFG_ACCOUNT_MAX  48
#define EDGE_E7_CFG_MODEL_MAX    40
#define EDGE_E7_CFG_PORT_MAX     16
#define EDGE_E7_CFG_PORTS_MAX    12
#define EDGE_E7_CFG_SVCS_MAX     8
#define EDGE_E7_CFG_SVC_NAME_MAX 48
/** Max ONTs in one inventory snapshot (heap-allocate the inventory struct). */
#define EDGE_E7_CFG_ONTS_MAX     256

typedef struct {
    char kind[32];       /* hsi | video | voice | eth | data | other */
    char name[EDGE_E7_CFG_SVC_NAME_MAX];
    char vlan[16];
    char profile[EDGE_E7_CFG_SVC_NAME_MAX];
} edge_e7_cfg_service_t;

typedef struct {
    char port[EDGE_E7_CFG_PORT_MAX]; /* g1, eth 1, 1, … */
    char type[16];                   /* eth | pots | veip | other */
    edge_e7_cfg_service_t services[EDGE_E7_CFG_SVCS_MAX];
    int  service_count;
} edge_e7_cfg_port_t;

typedef struct {
    char ont_id[EDGE_E7_CFG_ID_MAX];
    char fsan[EDGE_E7_CFG_FSAN_MAX];
    char account[EDGE_E7_CFG_ACCOUNT_MAX];
    char pon_id[EDGE_E7_CFG_ID_MAX];
    char model[EDGE_E7_CFG_MODEL_MAX];
    char admin_state[32];
    edge_e7_cfg_port_t ports[EDGE_E7_CFG_PORTS_MAX];
    int  port_count;
} edge_e7_cfg_ont_t;

typedef struct {
    edge_e7_cfg_ont_t onts[EDGE_E7_CFG_ONTS_MAX];
    int               ont_count;
    int               service_count; /* total services across ports */
    int               account_count; /* ONTs with non-empty account */
    int               truncated;     /* 1 if caps hit */
    char              warnings[256];
} edge_e7_cfg_inventory_t;

/**
 * Convert XML document (typically rpc-reply/data/config) to JSON.
 * Heuristic: elements → objects; repeated siblings → arrays; attrs as @k;
 * leaf text collapses to string.
 *
 * @p out is malloc'd (caller free). @p out_len excl NUL.
 * @return 0 ok, -1 error.
 */
int edge_e7_xml_to_json(const char *xml, size_t xml_len, char **out,
                        size_t *out_len);

/**
 * Extract provisioned ONT inventory from config XML (get-config reply or
 * bare &lt;config&gt;). Tolerant of EXA-style tags:
 *   ont-id / id, serial-number / fsan / ont-sn,
 *   subscriber-id / account / descr,
 *   ont-port / eth-port + eth-svc / data-svc / hsi / video / pots-svc.
 *
 * @return 0 ok (maybe 0 ONTs), -1 bad args.
 */
int edge_e7_config_extract_inventory(const char *xml, size_t xml_len,
                                     edge_e7_cfg_inventory_t *inv);

/**
 * Serialize one ONT inventory row to compact JSON (SPA / state).
 * @return bytes written excl NUL, or -1.
 */
int edge_e7_cfg_ont_json(const edge_e7_cfg_ont_t *ont, const char *shelf_id,
                         const char *captured_at, char *buf, size_t buf_sz);

/**
 * Serialize inventory summary JSON:
 * {ont_count,service_count,account_count,truncated,warnings}
 */
int edge_e7_cfg_summary_json(const edge_e7_cfg_inventory_t *inv, char *buf,
                             size_t buf_sz);

/**
 * Write inventory table JSON array into @p buf.
 * @return bytes excl NUL, or -1.
 */
int edge_e7_cfg_inventory_json(const edge_e7_cfg_inventory_t *inv,
                               const char *shelf_id, const char *captured_at,
                               char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_E7_CONFIG_H */
