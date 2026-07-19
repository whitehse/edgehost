/**
 * @file edge_config_hup.h
 * @brief SIGHUP reload entry points (P1.3 / ADR-005).
 *
 * Signal handler only sets a flag (async-signal-safe). The host loop (or tests)
 * call edgehost_hup_take() then edgehost_reload_config() — same apply path as
 * startup load (Decision X2).
 */
#ifndef EDGE_CONFIG_HUP_H
#define EDGE_CONFIG_HUP_H

#include "edgecore.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Install SIGHUP handler (idempotent). */
void edgehost_hup_install(void);

/**
 * If SIGHUP was received, clear the flag and return 1; else 0.
 * Not async-signal-safe to call from the handler itself.
 */
int edgehost_hup_take(void);

/**
 * Load YAML from @p path into a shadow config, validate, then
 * edgecore_apply_config. On load failure, emits CONFIG_REJECTED and keeps
 * the previous applied config.
 *
 * @return 0 applied, -1 rejected or load error.
 */
int edgehost_reload_config(edgecore_t *core, const char *path, char *err,
                           size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_CONFIG_HUP_H */
