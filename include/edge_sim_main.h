/**
 * @file edge_sim_main.h
 * @brief Class-A sim host: libsim (incl. uring) + edgecore + HTTP/1 (P1.5).
 *
 * No real sockets or liburing. Safe under adversarial byte streams.
 */
#ifndef EDGE_SIM_MAIN_H
#define EDGE_SIM_MAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Drive one fuzz/unit iteration:
 *  1. sim_fuzz_drive_a (clock/timer/net/uring)
 *  2. edgecore create/apply + NEED_ALLOC provide path
 *  3. sim_net listen/connect + sim_uring accept/recv/send of HTTP bytes
 *  4. direct edge_http1_serve_feed of remaining/payload fragments
 *
 * Always returns 0 (libFuzzer uses process crash as failure).
 */
int edge_sim_drive(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_SIM_MAIN_H */
