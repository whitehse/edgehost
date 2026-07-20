/**
 * @file edge_explain.h
 * @brief Employee fiber explain: template fill + plan validation (libanim).
 *
 * Host owns paths and HTTP. Optional when EDGEHOST_HAVE_LIBANIM=1.
 */
#ifndef EDGE_EXPLAIN_H
#define EDGE_EXPLAIN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Render a template under templates_root.
 *
 * body JSON (minimal):
 *   {"template":"optical_path","params":{"olt_label":"OLT","..." : "..."}}
 *
 * Writes JSON response into out (NUL-terminated). Returns length or -1.
 */
int edge_explain_render_json(const char *templates_root, const char *body,
                             size_t body_len, char *out, size_t out_cap);

/** List *.tmpl basenames as JSON array. Returns length or -1. */
int edge_explain_list_templates_json(const char *templates_root, char *out,
                                     size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_EXPLAIN_H */
