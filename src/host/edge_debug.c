/**
 * @file edge_debug.c
 * @brief Hierarchical memory report + SIGPROF/perf_event_open CPU sampler.
 */

#define _GNU_SOURCE

#include "edge_debug.h"

#include "host_alloc.h"

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- HTTP live snap ---------------------------------------------------- */

static edge_debug_http_snap_t g_http;

void edge_debug_http_publish(const edge_debug_http_snap_t *snap)
{
    if (!snap) {
        memset(&g_http, 0, sizeof(g_http));
        return;
    }
    g_http = *snap;
    g_http.valid = 1;
}

void edge_debug_http_get(edge_debug_http_snap_t *out)
{
    if (!out) {
        return;
    }
    *out = g_http;
}

/* ---- JSON helpers ------------------------------------------------------ */

static int appendf(char *buf, size_t buf_sz, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int appendf(char *buf, size_t buf_sz, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!buf || !off || *off >= buf_sz) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, buf_sz - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= buf_sz - *off) {
        return -1;
    }
    *off += (size_t)n;
    return 0;
}

/* ---- Memory report ----------------------------------------------------- */

int edge_debug_memory_json(const edge_metrics_t *metrics,
                           const edge_state_store_t *state,
                           const edge_e7_callhome_t *e7, char *buf,
                           size_t buf_sz)
{
    size_t off = 0;
    uint64_t rss_kb = 0, hwm_kb = 0, size_kb = 0, peak_kb = 0;
    char e7_mod[24576];
    char st_mod[4096];
    edge_debug_http_snap_t http;
    uint64_t host_total;
    uint64_t state_bytes = 0;
    uint64_t e7_host = 0;
    uint64_t http_host = 0;
    uint64_t ws_host = 0;
    uint64_t edgecore_host = 0;
    uint64_t plugin_host = 0;
    uint64_t other_host = 0;
    uint64_t accounted = 0;
    uint64_t rss_bytes;
    long uptime = 0;
    int en = -1;
    int sn = -1;

    if (!buf || buf_sz < 256) {
        return -1;
    }

    (void)host_mem_process_kb(&rss_kb, &hwm_kb, &size_kb, &peak_kb);

    host_total = host_bytes_outstanding();
    e7_host = host_bytes_outstanding_kind(EDGE_MEM_E7);
    http_host = host_bytes_outstanding_kind(EDGE_MEM_HTTP);
    ws_host = host_bytes_outstanding_kind(EDGE_MEM_WS);
    edgecore_host = host_bytes_outstanding_kind(EDGE_MEM_EDGECORE);
    plugin_host = host_bytes_outstanding_kind(EDGE_MEM_PLUGIN);
    other_host = host_bytes_outstanding_kind(EDGE_MEM_OTHER);
    if (state) {
        state_bytes = edge_state_rss_bytes(state);
    }
    edge_debug_http_get(&http);

    if (metrics && metrics->started_at) {
        time_t now = time(NULL);
        uptime = (now >= metrics->started_at)
                     ? (long)(now - metrics->started_at)
                     : 0;
    }

    e7_mod[0] = '\0';
    if (e7) {
        en = edge_e7_callhome_memory_json(e7, e7_mod, sizeof(e7_mod));
    }
    st_mod[0] = '\0';
    if (state) {
        sn = edge_state_memory_json(state, st_mod, sizeof(st_mod));
    }

    if (appendf(buf, buf_sz, &off,
                "{\"v\":1,\"uptime_s\":%ld,"
                "\"process\":{"
                "\"vm_rss_kb\":%llu,\"vm_hwm_kb\":%llu,"
                "\"vm_size_kb\":%llu,\"vm_peak_kb\":%llu"
                "},"
                "\"host_alloc\":{"
                "\"bytes\":%llu,\"peak_bytes\":%llu,"
                "\"allocs\":%llu,\"reallocs\":%llu,\"frees\":%llu,"
                "\"by_kind\":{",
                uptime, (unsigned long long)rss_kb,
                (unsigned long long)hwm_kb, (unsigned long long)size_kb,
                (unsigned long long)peak_kb, (unsigned long long)host_total,
                (unsigned long long)host_bytes_peak(),
                (unsigned long long)host_alloc_count(),
                (unsigned long long)host_realloc_count(),
                (unsigned long long)host_free_count()) != 0) {
        return -1;
    }
    {
        int i;
        for (i = 0; i < (int)EDGE_MEM_KIND_COUNT; i++) {
            if (appendf(buf, buf_sz, &off,
                        "%s\"%s\":{\"bytes\":%llu,\"peak_bytes\":%llu,"
                        "\"allocs\":%llu,\"frees\":%llu}",
                        i ? "," : "", edge_mem_kind_name((edge_mem_kind_t)i),
                        (unsigned long long)host_bytes_outstanding_kind(
                            (edge_mem_kind_t)i),
                        (unsigned long long)host_bytes_peak_kind(
                            (edge_mem_kind_t)i),
                        (unsigned long long)host_alloc_count_kind(
                            (edge_mem_kind_t)i),
                        (unsigned long long)host_free_count_kind(
                            (edge_mem_kind_t)i)) != 0) {
                return -1;
            }
        }
    }
    if (appendf(buf, buf_sz, &off, "}},\"modules\":[") != 0) {
        return -1;
    }

    /* --- module: process (kernel view) --- */
    rss_bytes = rss_kb * 1024ull;
    if (appendf(buf, buf_sz, &off,
                "{\"id\":\"process\",\"name\":\"Process (kernel)\","
                "\"bytes\":%llu,\"kind\":\"rss\","
                "\"items\":["
                "{\"id\":\"vm_rss\",\"label\":\"Resident set (VmRSS)\","
                "\"bytes\":%llu},"
                "{\"id\":\"vm_hwm\",\"label\":\"High-water (VmHWM)\","
                "\"bytes\":%llu},"
                "{\"id\":\"vm_size\",\"label\":\"Virtual size (VmSize)\","
                "\"bytes\":%llu}"
                "]}",
                (unsigned long long)rss_bytes, (unsigned long long)rss_bytes,
                (unsigned long long)(hwm_kb * 1024ull),
                (unsigned long long)(size_kb * 1024ull)) != 0) {
        return -1;
    }
    accounted = 0; /* process is ground truth, not double-counted */

    /* --- module: http --- */
    {
        uint64_t table_b = http.valid ? (uint64_t)http.conn_table_bytes
                                      : http_host;
        uint64_t send_b = http.valid ? (uint64_t)http.send_bufs_live : 0;
        uint64_t recv_b = http.valid ? (uint64_t)http.recv_bufs_embedded : 0;
        uint64_t mod_b = http_host;
        if (mod_b == 0 && http.valid) {
            mod_b = table_b + send_b;
        }
        if (appendf(buf, buf_sz, &off,
                    ",{\"id\":\"http\",\"name\":\"HTTP / io_uring webserver\","
                    "\"bytes\":%llu,\"host_alloc_bytes\":%llu,"
                    "\"items\":["
                    "{\"id\":\"conn_table\",\"label\":\"Connection table\","
                    "\"bytes\":%llu,\"count\":%d},"
                    "{\"id\":\"active_conns\",\"label\":\"Active connections\","
                    "\"bytes\":0,\"count\":%d},"
                    "{\"id\":\"ws_conns\",\"label\":\"WebSocket connections\","
                    "\"bytes\":0,\"count\":%d},"
                    "{\"id\":\"send_buffers\",\"label\":\"Send buffers (live)\","
                    "\"bytes\":%llu,\"count\":%d,\"per_item_bytes\":%zu},"
                    "{\"id\":\"recv_buffers\",\"label\":\"Recv buffers (embedded)\","
                    "\"bytes\":%llu,\"count\":%d},"
                    "{\"id\":\"host_alloc_http\",\"label\":\"host_alloc[http]\","
                    "\"bytes\":%llu}"
                    "],\"metrics\":{\"accepts\":%llu,\"requests\":%llu,"
                    "\"max_conns\":%d,\"send_cap\":%zu}}",
                    (unsigned long long)mod_b, (unsigned long long)http_host,
                    (unsigned long long)table_b,
                    http.valid ? http.max_conns : 0,
                    http.valid ? http.active_conns : 0,
                    http.valid ? http.ws_conns : 0,
                    (unsigned long long)send_b,
                    http.valid ? http.active_conns : 0,
                    http.valid ? http.send_cap : (size_t)0,
                    (unsigned long long)recv_b,
                    http.valid ? http.active_conns : 0,
                    (unsigned long long)http_host,
                    (unsigned long long)(http.valid ? http.accepts : 0),
                    (unsigned long long)(http.valid ? http.requests : 0),
                    http.valid ? http.max_conns : 0,
                    http.valid ? http.send_cap : (size_t)0) != 0) {
            return -1;
        }
        accounted += http_host;
    }

    /* --- module: ws --- */
    if (appendf(buf, buf_sz, &off,
                ",{\"id\":\"ws\",\"name\":\"WebSocket hub\","
                "\"bytes\":%llu,\"host_alloc_bytes\":%llu,"
                "\"items\":["
                "{\"id\":\"hub_and_queues\",\"label\":\"Hub + pending messages\","
                "\"bytes\":%llu}"
                "]}",
                (unsigned long long)ws_host, (unsigned long long)ws_host,
                (unsigned long long)ws_host) != 0) {
        return -1;
    }
    accounted += ws_host;

    /* --- module: e7 --- */
    if (en > 0) {
        if (appendf(buf, buf_sz, &off, ",%s", e7_mod) != 0) {
            return -1;
        }
        accounted += e7_host;
    } else {
        if (appendf(buf, buf_sz, &off,
                    ",{\"id\":\"e7\",\"name\":\"E7 Call Home\","
                    "\"bytes\":%llu,\"host_alloc_bytes\":%llu,"
                    "\"enabled\":false,\"items\":[]}",
                    (unsigned long long)e7_host,
                    (unsigned long long)e7_host) != 0) {
            return -1;
        }
        accounted += e7_host;
    }

    /* --- module: state --- */
    if (sn > 0) {
        if (appendf(buf, buf_sz, &off, ",%s", st_mod) != 0) {
            return -1;
        }
        /* state is create-time calloc, not in host_alloc */
    } else {
        if (appendf(buf, buf_sz, &off,
                    ",{\"id\":\"state\",\"name\":\"State store\","
                    "\"bytes\":%llu,\"kind\":\"eager_calloc\",\"items\":[]}",
                    (unsigned long long)state_bytes) != 0) {
            return -1;
        }
    }

    /* --- module: edgecore --- */
    if (appendf(buf, buf_sz, &off,
                ",{\"id\":\"edgecore\",\"name\":\"edgecore buffers\","
                "\"bytes\":%llu,\"host_alloc_bytes\":%llu,"
                "\"items\":[{\"id\":\"need_alloc\",\"label\":\"NEED_ALLOC slabs\","
                "\"bytes\":%llu}]}",
                (unsigned long long)edgecore_host,
                (unsigned long long)edgecore_host,
                (unsigned long long)edgecore_host) != 0) {
        return -1;
    }
    accounted += edgecore_host;

    /* --- module: plugins --- */
    if (appendf(buf, buf_sz, &off,
                ",{\"id\":\"plugin\",\"name\":\"Plugins / outbound\","
                "\"bytes\":%llu,\"host_alloc_bytes\":%llu,\"items\":["
                "{\"id\":\"plugin_heap\",\"label\":\"host_alloc[plugin]\","
                "\"bytes\":%llu}]}",
                (unsigned long long)plugin_host,
                (unsigned long long)plugin_host,
                (unsigned long long)plugin_host) != 0) {
        return -1;
    }
    accounted += plugin_host;

    if (other_host) {
        if (appendf(buf, buf_sz, &off,
                    ",{\"id\":\"other\",\"name\":\"Other host_alloc\","
                    "\"bytes\":%llu,\"items\":[]}",
                    (unsigned long long)other_host) != 0) {
            return -1;
        }
        accounted += other_host;
    }

    /* Close modules; unaccounted vs RSS */
    {
        int64_t unacct = (int64_t)rss_bytes - (int64_t)accounted -
                         (int64_t)state_bytes;
        if (appendf(buf, buf_sz, &off,
                    "],\"summary\":{"
                    "\"rss_bytes\":%llu,"
                    "\"host_alloc_bytes\":%llu,"
                    "\"state_eager_bytes\":%llu,"
                    "\"accounted_host_alloc\":%llu,"
                    "\"unaccounted_vs_rss\":%lld,"
                    "\"note\":\"unaccounted includes libc arenas, OpenSSL, "
                    "libnetconf, sibling libs, stacks, and page fragmentation\""
                    "}}",
                    (unsigned long long)rss_bytes,
                    (unsigned long long)host_total,
                    (unsigned long long)state_bytes,
                    (unsigned long long)accounted,
                    (long long)unacct) != 0) {
            return -1;
        }
    }
    return (int)off;
}

/* ---- CPU profiler ------------------------------------------------------ */

#define EDGE_CPU_MAX_DEPTH   48
#define EDGE_CPU_MAX_SAMPLES 25000
#define EDGE_CPU_MAX_NODES   8192
#define EDGE_CPU_NAME_MAX    96

typedef struct {
    void *frames[EDGE_CPU_MAX_DEPTH];
    int   depth;
} edge_cpu_sample_t;

typedef struct {
    char     name[EDGE_CPU_NAME_MAX];
    uint32_t count;
    int      parent; /* index or -1 */
    int      first_child;
    int      next_sibling;
} edge_cpu_node_t;

typedef enum {
    EDGE_CPU_IDLE = 0,
    EDGE_CPU_RUNNING,
    EDGE_CPU_DONE,
    EDGE_CPU_ERROR
} edge_cpu_state_t;

static edge_cpu_state_t g_cpu_state = EDGE_CPU_IDLE;
static char             g_cpu_err[128];
static char             g_cpu_mode[32];
static uint64_t         g_cpu_start_ms;
static uint64_t         g_cpu_end_ms;
static int              g_cpu_seconds;
static uint64_t         g_cpu_deadline_ms;
static uint64_t         g_cpu_last_sample_ms;
static edge_cpu_sample_t *g_cpu_samples;
static int              g_cpu_nsamples;
static int              g_cpu_sample_cap;
static edge_cpu_node_t  g_cpu_nodes[EDGE_CPU_MAX_NODES];
static int              g_cpu_nnodes;
static uint32_t         g_cpu_total;
static int              g_cpu_perf_fd = -1;

/** Take one stack sample from normal (non-signal) context. */
static void edge_cpu_take_sample(void)
{
    edge_cpu_sample_t *s;
    int n;

    if (!g_cpu_samples || g_cpu_nsamples >= g_cpu_sample_cap) {
        return;
    }
    s = &g_cpu_samples[g_cpu_nsamples];
    n = backtrace(s->frames, EDGE_CPU_MAX_DEPTH);
    if (n < 1) {
        return;
    }
    s->depth = n;
    g_cpu_nsamples++;
}

static void edge_cpu_stop_extras(void)
{
    if (g_cpu_perf_fd >= 0) {
        ioctl(g_cpu_perf_fd, PERF_EVENT_IOC_DISABLE, 0);
        close(g_cpu_perf_fd);
        g_cpu_perf_fd = -1;
    }
}

static void edge_cpu_resolve(void *addr, char *out, size_t out_sz)
{
    Dl_info info;
    if (dladdr(addr, &info) && info.dli_sname) {
        snprintf(out, out_sz, "%s", info.dli_sname);
        return;
    }
    snprintf(out, out_sz, "%p", addr);
}

static int edge_cpu_find_child(int parent, const char *name)
{
    int i;
    if (parent < 0) {
        for (i = 0; i < g_cpu_nnodes; i++) {
            if (g_cpu_nodes[i].parent < 0 &&
                strcmp(g_cpu_nodes[i].name, name) == 0) {
                return i;
            }
        }
        return -1;
    }
    for (i = g_cpu_nodes[parent].first_child; i >= 0;
         i = g_cpu_nodes[i].next_sibling) {
        if (strcmp(g_cpu_nodes[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int edge_cpu_add_node(int parent, const char *name)
{
    int idx;
    if (g_cpu_nnodes >= EDGE_CPU_MAX_NODES) {
        return -1;
    }
    idx = g_cpu_nnodes++;
    memset(&g_cpu_nodes[idx], 0, sizeof(g_cpu_nodes[idx]));
    snprintf(g_cpu_nodes[idx].name, sizeof(g_cpu_nodes[idx].name), "%s", name);
    g_cpu_nodes[idx].parent = parent;
    g_cpu_nodes[idx].first_child = -1;
    g_cpu_nodes[idx].next_sibling = -1;
    if (parent >= 0) {
        g_cpu_nodes[idx].next_sibling = g_cpu_nodes[parent].first_child;
        g_cpu_nodes[parent].first_child = idx;
    }
    return idx;
}

static void edge_cpu_fold_samples(void)
{
    int s, d;
    g_cpu_nnodes = 0;
    g_cpu_total = 0;
    for (s = 0; s < g_cpu_nsamples; s++) {
        int parent = -1;
        /* Walk root→leaf: reverse backtrace order (leaf at [0]) */
        for (d = g_cpu_samples[s].depth - 1; d >= 0; d--) {
            char name[EDGE_CPU_NAME_MAX];
            int child;
            edge_cpu_resolve(g_cpu_samples[s].frames[d], name, sizeof(name));
            /* skip noise */
            if (strcmp(name, "edge_cpu_take_sample") == 0 ||
                strcmp(name, "edge_debug_cpu_on_tick") == 0 ||
                strcmp(name, "edge_cpu_sample_handler") == 0) {
                continue;
            }
            child = edge_cpu_find_child(parent, name);
            if (child < 0) {
                child = edge_cpu_add_node(parent, name);
                if (child < 0) {
                    break;
                }
            }
            g_cpu_nodes[child].count++;
            parent = child;
        }
        g_cpu_total++;
    }
}

static void edge_cpu_finish(void)
{
    edge_cpu_stop_extras();
    edge_cpu_fold_samples();
    g_cpu_state = EDGE_CPU_DONE;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_cpu_end_ms =
            (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    }
}

void edge_debug_cpu_on_tick(uint64_t mono_ms)
{
    if (g_cpu_state != EDGE_CPU_RUNNING) {
        return;
    }
    /* ~100 Hz wall samples from the host loop (safe backtrace context). */
    if (mono_ms - g_cpu_last_sample_ms >= 10ull) {
        edge_cpu_take_sample();
        g_cpu_last_sample_ms = mono_ms;
    }
    if (g_cpu_deadline_ms && mono_ms >= g_cpu_deadline_ms) {
        edge_cpu_finish();
    }
}

int edge_debug_cpu_profiling(void)
{
    return g_cpu_state == EDGE_CPU_RUNNING;
}

static int edge_cpu_path_exists(const char *p)
{
    return p && access(p, X_OK) == 0;
}

int edge_debug_cpu_capabilities_json(char *buf, size_t buf_sz)
{
    int n;
    int has_perf = edge_cpu_path_exists("/usr/bin/perf") ||
                   edge_cpu_path_exists("/bin/perf");
    int has_bpftrace = edge_cpu_path_exists("/usr/bin/bpftrace");
    int has_bpftool = edge_cpu_path_exists("/usr/sbin/bpftool") ||
                      edge_cpu_path_exists("/usr/bin/bpftool");
    int paranoid = 3;
    FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (f) {
        if (fscanf(f, "%d", &paranoid) != 1) {
            paranoid = 3;
        }
        fclose(f);
    }
    n = snprintf(buf, buf_sz,
                 "{"
                 "\"v\":1,"
                 "\"samplers\":{"
                 "\"host_tick\":{\"available\":true,"
                 "\"desc\":\"host loop tick + backtrace(); wall samples ~100Hz while profiling\"},"
                 "\"perf_event_open\":{\"available\":true,"
                 "\"desc\":\"kernel HW/SW sampling when paranoid allows\","
                 "\"perf_event_paranoid\":%d},"
                 "\"perf_cli\":{\"available\":%s,"
                 "\"desc\":\"external: perf record -g -p $PID\"},"
                 "\"bpftrace\":{\"available\":%s,"
                 "\"desc\":\"external eBPF: profile:/profile { @[ustack]=count(); }\"},"
                 "\"bpftool\":{\"available\":%s}"
                 "},"
                 "\"recommended\":\"auto (in-process host_tick; use scripts/cpu-flame-perf.sh for eBPF/perf)\","
                 "\"state\":\"%s\","
                 "\"scripts\":{\"offline_perf\":\"scripts/cpu-flame-perf.sh\"}"
                 "}",
                 paranoid, has_perf ? "true" : "false",
                 has_bpftrace ? "true" : "false",
                 has_bpftool ? "true" : "false",
                 g_cpu_state == EDGE_CPU_RUNNING
                     ? "running"
                     : (g_cpu_state == EDGE_CPU_DONE
                            ? "done"
                            : (g_cpu_state == EDGE_CPU_ERROR ? "error"
                                                            : "idle")));
    if (n < 0 || (size_t)n >= buf_sz) {
        return -1;
    }
    return n;
}

static long perf_event_open_wrap(struct perf_event_attr *attr, pid_t pid,
                                 int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int edge_debug_cpu_profile_start(int seconds, const char *mode, char *err,
                                 size_t err_sz)
{
    struct timespec ts;
    const char *m = mode && mode[0] ? mode : "auto";
    int use_perf = 0;

    if (err && err_sz) {
        err[0] = '\0';
    }
    if (g_cpu_state == EDGE_CPU_RUNNING) {
        if (err && err_sz) {
            snprintf(err, err_sz, "profile already running");
        }
        return -1;
    }
    if (seconds < 1) {
        seconds = 1;
    }
    if (seconds > 60) {
        seconds = 60;
    }

    if (g_cpu_samples) {
        host_free(g_cpu_samples);
        g_cpu_samples = NULL;
    }
    g_cpu_sample_cap = EDGE_CPU_MAX_SAMPLES;
    g_cpu_samples = (edge_cpu_sample_t *)host_alloc_kind(
        EDGE_MEM_OTHER, (size_t)g_cpu_sample_cap * sizeof(edge_cpu_sample_t));
    if (!g_cpu_samples) {
        if (err && err_sz) {
            snprintf(err, err_sz, "OOM allocating sample buffer");
        }
        g_cpu_state = EDGE_CPU_ERROR;
        return -1;
    }
    g_cpu_nsamples = 0;
    g_cpu_nnodes = 0;
    g_cpu_total = 0;
    g_cpu_seconds = seconds;
    g_cpu_err[0] = '\0';
    g_cpu_last_sample_ms = 0;

    if (strcmp(m, "perf") == 0 || strcmp(m, "auto") == 0) {
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.type = PERF_TYPE_SOFTWARE;
        pe.size = sizeof(pe);
        pe.config = PERF_COUNT_SW_CPU_CLOCK;
        pe.disabled = 1;
        pe.exclude_kernel = 1;
        pe.freq = 1;
        pe.sample_freq = 99;
        pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN;
        g_cpu_perf_fd = (int)perf_event_open_wrap(&pe, 0, -1, -1, 0);
        if (g_cpu_perf_fd >= 0) {
            use_perf = 1;
            ioctl(g_cpu_perf_fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(g_cpu_perf_fd, PERF_EVENT_IOC_ENABLE, 0);
        } else if (strcmp(m, "perf") == 0) {
            if (err && err_sz) {
                snprintf(err, err_sz,
                         "perf_event_open failed (errno=%d, paranoid?); "
                         "try mode=auto (host_tick)",
                         errno);
            }
            host_free(g_cpu_samples);
            g_cpu_samples = NULL;
            g_cpu_state = EDGE_CPU_ERROR;
            return -1;
        }
    }

    snprintf(g_cpu_mode, sizeof(g_cpu_mode), "%s",
             use_perf ? "host_tick+perf_event" : "host_tick");

    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_cpu_start_ms =
        (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    g_cpu_deadline_ms = g_cpu_start_ms + (uint64_t)seconds * 1000ull;
    g_cpu_last_sample_ms = g_cpu_start_ms;
    g_cpu_end_ms = 0;
    g_cpu_state = EDGE_CPU_RUNNING;
    /* Immediate sample so idle captures at least the start stack. */
    edge_cpu_take_sample();
    return 0;
}

int edge_debug_cpu_profile_status_json(char *buf, size_t buf_sz)
{
    const char *st;
    int n;
    uint64_t now = 0;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    if (g_cpu_state == EDGE_CPU_RUNNING && g_cpu_deadline_ms &&
        now >= g_cpu_deadline_ms) {
        edge_cpu_finish();
    }

    st = g_cpu_state == EDGE_CPU_RUNNING
             ? "running"
             : (g_cpu_state == EDGE_CPU_DONE
                    ? "done"
                    : (g_cpu_state == EDGE_CPU_ERROR ? "error" : "idle"));
    n = snprintf(buf, buf_sz,
                 "{"
                 "\"v\":1,"
                 "\"state\":\"%s\","
                 "\"mode\":\"%s\","
                 "\"seconds\":%d,"
                 "\"samples\":%d,"
                 "\"sample_cap\":%d,"
                 "\"total_folded\":%u,"
                 "\"nodes\":%d,"
                 "\"elapsed_ms\":%llu,"
                 "\"error\":%s%s%s"
                 "}",
                 st, g_cpu_mode[0] ? g_cpu_mode : "none", g_cpu_seconds,
                 g_cpu_nsamples, g_cpu_sample_cap, g_cpu_total, g_cpu_nnodes,
                 (unsigned long long)(g_cpu_state == EDGE_CPU_RUNNING
                                          ? (now - g_cpu_start_ms)
                                          : (g_cpu_end_ms
                                                 ? (g_cpu_end_ms - g_cpu_start_ms)
                                                 : 0)),
                 g_cpu_err[0] ? "\"" : "null", g_cpu_err[0] ? g_cpu_err : "",
                 g_cpu_err[0] ? "\"" : "");
    if (n < 0 || (size_t)n >= buf_sz) {
        return -1;
    }
    return n;
}

static int edge_cpu_emit_folded_rec(int idx, char *path, size_t path_len,
                                    char *buf, size_t buf_sz, size_t *off)
{
    int c;
    char name[EDGE_CPU_NAME_MAX];
    size_t nlen;
    size_t saved;

    if (idx < 0) {
        return 0;
    }
    snprintf(name, sizeof(name), "%s", g_cpu_nodes[idx].name);
    nlen = strlen(name);
    saved = path_len;
    if (path_len + nlen + 2 >= 1024) {
        return 0;
    }
    if (path_len) {
        path[path_len++] = ';';
    }
    memcpy(path + path_len, name, nlen);
    path_len += nlen;
    path[path_len] = '\0';

    /* leaf contribution = count - sum(children) approximated by printing
     * self when no children, else each full path uses node count at leaves
     * only if first_child < 0 */
    if (g_cpu_nodes[idx].first_child < 0) {
        int w = snprintf(buf + *off, buf_sz - *off, "%s %u\n", path,
                         g_cpu_nodes[idx].count);
        if (w < 0 || (size_t)w >= buf_sz - *off) {
            return -1;
        }
        *off += (size_t)w;
    }
    for (c = g_cpu_nodes[idx].first_child; c >= 0;
         c = g_cpu_nodes[c].next_sibling) {
        if (edge_cpu_emit_folded_rec(c, path, path_len, buf, buf_sz, off) !=
            0) {
            return -1;
        }
    }
    path[saved] = '\0';
    return 0;
}

int edge_debug_cpu_profile_folded(char *buf, size_t buf_sz)
{
    size_t off = 0;
    int i;
    char path[1024];

    if (!buf || buf_sz < 8) {
        return -1;
    }
    if (g_cpu_state != EDGE_CPU_DONE || g_cpu_nnodes == 0) {
        buf[0] = '\0';
        return 0;
    }
    path[0] = '\0';
    for (i = 0; i < g_cpu_nnodes; i++) {
        if (g_cpu_nodes[i].parent < 0) {
            if (edge_cpu_emit_folded_rec(i, path, 0, buf, buf_sz, &off) != 0) {
                return -1;
            }
        }
    }
    buf[off] = '\0';
    return (int)off;
}

static int json_esc_name(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    size_t i;
    if (!in || !out || out_sz < 3) {
        return -1;
    }
    out[o++] = '"';
    for (i = 0; in[i] && o + 2 < out_sz; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            out[o++] = '?';
        } else {
            out[o++] = c;
        }
    }
    if (o + 1 >= out_sz) {
        return -1;
    }
    out[o++] = '"';
    out[o] = '\0';
    return 0;
}

static int edge_cpu_flame_node(int idx, char *buf, size_t buf_sz, size_t *off)
{
    char esc[EDGE_CPU_NAME_MAX * 2];
    int c;
    int first = 1;

    if (json_esc_name(g_cpu_nodes[idx].name, esc, sizeof(esc)) != 0) {
        return -1;
    }
    if (appendf(buf, buf_sz, off, "{\"name\":%s,\"value\":%u,\"children\":[",
                esc, g_cpu_nodes[idx].count) != 0) {
        return -1;
    }
    for (c = g_cpu_nodes[idx].first_child; c >= 0;
         c = g_cpu_nodes[c].next_sibling) {
        if (!first) {
            if (appendf(buf, buf_sz, off, ",") != 0) {
                return -1;
            }
        }
        first = 0;
        if (edge_cpu_flame_node(c, buf, buf_sz, off) != 0) {
            return -1;
        }
    }
    if (appendf(buf, buf_sz, off, "]}") != 0) {
        return -1;
    }
    return 0;
}

int edge_debug_cpu_profile_flame_json(char *buf, size_t buf_sz)
{
    size_t off = 0;
    int i;
    int first = 1;

    if (!buf || buf_sz < 16) {
        return -1;
    }
    if (g_cpu_state != EDGE_CPU_DONE) {
        return snprintf(buf, buf_sz,
                        "{\"name\":\"root\",\"value\":0,\"children\":[],"
                        "\"state\":\"%s\"}",
                        g_cpu_state == EDGE_CPU_RUNNING ? "running" : "idle");
    }
    if (appendf(buf, buf_sz, &off,
                "{\"name\":\"root\",\"value\":%u,\"children\":[",
                g_cpu_total) != 0) {
        return -1;
    }
    for (i = 0; i < g_cpu_nnodes; i++) {
        if (g_cpu_nodes[i].parent < 0) {
            if (!first && appendf(buf, buf_sz, &off, ",") != 0) {
                return -1;
            }
            first = 0;
            if (edge_cpu_flame_node(i, buf, buf_sz, &off) != 0) {
                return -1;
            }
        }
    }
    if (appendf(buf, buf_sz, &off, "],\"samples\":%d,\"mode\":\"%s\"}",
                g_cpu_nsamples, g_cpu_mode) != 0) {
        return -1;
    }
    return (int)off;
}
