/**
 * @file static_files.c
 * @brief SPA / package static file serving helpers (P1.6).
 */

#include "edge_static.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int edge_static_resolve(const char *root, const char *url_path, char *out,
                        size_t out_sz)
{
    char rel[512];
    size_t ri = 0;
    size_t i;
    size_t root_len;
    int slash;

    if (!root || !root[0] || !out || out_sz < 4) {
        return -1;
    }
    if (!url_path) {
        url_path = "/";
    }

    /* Strip query / fragment */
    {
        const char *q = strchr(url_path, '?');
        const char *h = strchr(url_path, '#');
        size_t n = strlen(url_path);
        if (q && (size_t)(q - url_path) < n) {
            n = (size_t)(q - url_path);
        }
        if (h && (size_t)(h - url_path) < n) {
            n = (size_t)(h - url_path);
        }
        if (n >= sizeof(rel)) {
            return -1;
        }
        memcpy(rel, url_path, n);
        rel[n] = '\0';
    }

    /* Leading slashes → relative */
    i = 0;
    while (rel[i] == '/') {
        i++;
    }

    /* Default document */
    if (rel[i] == '\0') {
        snprintf(rel, sizeof(rel), "index.html");
        i = 0;
    }

    /* Walk components; reject . and .. */
    {
        char clean[512];
        size_t ci = 0;
        while (rel[i]) {
            size_t start = i;
            while (rel[i] && rel[i] != '/') {
                i++;
            }
            size_t clen = i - start;
            if (clen == 0) {
                if (rel[i] == '/') {
                    i++;
                }
                continue;
            }
            if ((clen == 1 && rel[start] == '.') ||
                (clen == 2 && rel[start] == '.' && rel[start + 1] == '.')) {
                return -1;
            }
            /* reject backslash / control */
            {
                size_t k;
                for (k = 0; k < clen; k++) {
                    unsigned char ch = (unsigned char)rel[start + k];
                    if (ch < 0x20 || ch == '\\' || ch == 0x7f) {
                        return -1;
                    }
                }
            }
            if (ci > 0) {
                if (ci + 1 >= sizeof(clean)) {
                    return -1;
                }
                clean[ci++] = '/';
            }
            if (ci + clen >= sizeof(clean)) {
                return -1;
            }
            memcpy(clean + ci, rel + start, clen);
            ci += clen;
            if (rel[i] == '/') {
                i++;
            }
        }
        clean[ci] = '\0';
        if (ci == 0) {
            snprintf(clean, sizeof(clean), "index.html");
        }
        snprintf(rel, sizeof(rel), "%s", clean);
    }

    root_len = strlen(root);
    /* root + '/' + rel + NUL */
    if (root_len + 1 + strlen(rel) + 1 > out_sz) {
        return -1;
    }
    slash = (root[root_len - 1] == '/') ? 0 : 1;
    if (slash) {
        snprintf(out, out_sz, "%s/%s", root, rel);
    } else {
        snprintf(out, out_sz, "%s%s", root, rel);
    }
    (void)ri;
    return 0;
}

void edge_static_content_type(const char *path, char *ctype, size_t ctype_sz)
{
    const char *ext;

    if (!ctype || ctype_sz == 0) {
        return;
    }
    ext = path ? strrchr(path, '.') : NULL;
    if (!ext) {
        snprintf(ctype, ctype_sz, "application/octet-stream");
        return;
    }
    ext++;
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) {
        snprintf(ctype, ctype_sz, "text/html; charset=utf-8");
    } else if (strcmp(ext, "js") == 0 || strcmp(ext, "mjs") == 0) {
        snprintf(ctype, ctype_sz, "application/javascript; charset=utf-8");
    } else if (strcmp(ext, "css") == 0) {
        snprintf(ctype, ctype_sz, "text/css; charset=utf-8");
    } else if (strcmp(ext, "json") == 0) {
        snprintf(ctype, ctype_sz, "application/json");
    } else if (strcmp(ext, "svg") == 0) {
        snprintf(ctype, ctype_sz, "image/svg+xml");
    } else if (strcmp(ext, "png") == 0) {
        snprintf(ctype, ctype_sz, "image/png");
    } else if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) {
        snprintf(ctype, ctype_sz, "image/jpeg");
    } else if (strcmp(ext, "wasm") == 0) {
        snprintf(ctype, ctype_sz, "application/wasm");
    } else if (strcmp(ext, "wmap") == 0 || strcmp(ext, "fmap") == 0) {
        snprintf(ctype, ctype_sz, "application/octet-stream");
    } else if (strcmp(ext, "txt") == 0 || strcmp(ext, "map") == 0) {
        snprintf(ctype, ctype_sz, "text/plain; charset=utf-8");
    } else {
        snprintf(ctype, ctype_sz, "application/octet-stream");
    }
}

int edge_static_read_file(const char *path, void *buf, size_t max_bytes,
                          size_t *out_len)
{
    FILE *fp;
    struct stat st;
    size_t nread;

    if (!path || !buf || max_bytes == 0 || !out_len) {
        return -1;
    }
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        return -1;
    }
    if (st.st_size < 0 || (size_t)st.st_size > max_bytes) {
        return -1;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    nread = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (nread != (size_t)st.st_size) {
        return -1;
    }
    *out_len = nread;
    return (int)nread;
}

int edge_static_load(const char *root, const char *url_path, void *buf,
                     size_t max_bytes, size_t *out_len, char *ctype,
                     size_t ctype_sz)
{
    char fspath[1024];
    struct stat st;

    if (edge_static_resolve(root, url_path, fspath, sizeof(fspath)) != 0) {
        return -1;
    }
    /* Directory URL → index.html (e.g. /map/ → spa/map/index.html). */
    if (stat(fspath, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t n = strlen(fspath);
        if (n + 12 >= sizeof(fspath)) {
            return -1;
        }
        if (fspath[n - 1] != '/') {
            fspath[n++] = '/';
            fspath[n] = '\0';
        }
        memcpy(fspath + n, "index.html", 11);
    }
    if (edge_static_read_file(fspath, buf, max_bytes, out_len) < 0) {
        return -1;
    }
    edge_static_content_type(fspath, ctype, ctype_sz);
    return 0;
}
