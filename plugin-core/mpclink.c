/* mpclink: MPC side of GPP Link. See mpclink.h. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "mpclink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- small helpers ---------- */

static void copy_str(char *dst, size_t n, const char *src) {
    if (!n) return;
    snprintf(dst, n, "%s", src ? src : "");
}

static int valid_device(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 40) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

/* percent-encode for a query value */
static void url_encode(const char *in, char *out, size_t outlen) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < outlen; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static int mkdirs(const char *dir) {
    char tmp[1024];
    copy_str(tmp, sizeof tmp, dir);
    size_t n = strlen(tmp);
    if (n == 0) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* "dir/kick.wav" -> first of kick.wav, kick (2).wav, ... that doesn't exist */
static void free_name(const char *dir, const char *name, char *out, size_t outlen) {
    struct stat st;
    snprintf(out, outlen, "%s/%s", dir, name);
    if (lstat(out, &st) != 0) return;
    const char *dot = strrchr(name, '.');
    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    for (int i = 2; i < 10000; i++) {
        snprintf(out, outlen, "%s/%.*s (%d)%s", dir, stem, name, i, dot ? dot : "");
        if (lstat(out, &st) != 0) return;
    }
}

/* ---------- tiny HTTP/1.0 client (no chunked encoding to handle) ---------- */

typedef struct {
    int fd;
    int timeout_ms;
    char buf[8192];
    size_t len, pos;
} conn_t;

static int wait_fd(int fd, short ev, int ms) {
    struct pollfd p = { fd, ev, 0 };
    int r;
    do { r = poll(&p, 1, ms); } while (r < 0 && errno == EINTR);
    return r > 0 ? 0 : -1;
}

static int conn_open(conn_t *c, const mpclink_ctx *ctx) {
    struct timeval tv;   /* declared up front: C++ forbids a goto past an initialisation */
    memset(c, 0, sizeof *c);
    c->timeout_ms = ctx->timeout_ms;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)ctx->port);
    if (inet_pton(AF_INET, ctx->host, &a.sin_addr) != 1) return MPCLINK_ERR_ARG;
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) return MPCLINK_ERR_NETWORK;
    int fl = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(c->fd, (struct sockaddr *)&a, sizeof a);
    if (r < 0 && errno != EINPROGRESS) goto fail;
    if (r < 0) {
        int err = 0; socklen_t el = sizeof err;
        if (wait_fd(c->fd, POLLOUT, c->timeout_ms) ||
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) || err)
            goto fail;
    }
    fcntl(c->fd, F_SETFL, fl);
    tv.tv_sec = c->timeout_ms / 1000;
    tv.tv_usec = (c->timeout_ms % 1000) * 1000;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return MPCLINK_OK;
fail:
    close(c->fd);
    c->fd = -1;
    return MPCLINK_ERR_NETWORK;
}

static void conn_close(conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int send_all(conn_t *c, const void *data, size_t n) {
    const char *p = (const char *)data;
    while (n) {
        ssize_t w = send(c->fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static ssize_t conn_read(conn_t *c, void *out, size_t n) {
    if (c->pos < c->len) {
        size_t k = c->len - c->pos;
        if (k > n) k = n;
        memcpy(out, c->buf + c->pos, k);
        c->pos += k;
        return (ssize_t)k;
    }
    ssize_t r;
    do { r = recv(c->fd, out, n, 0); } while (r < 0 && errno == EINTR);
    return r;
}

/* Send request line + headers (+ optional body from a file), then read the
 * status and headers. Leaves the connection positioned at the body. */
static int http_request(conn_t *c, const mpclink_ctx *ctx, const char *method, const char *target,
                        FILE *body, long long body_len, int *status, long long *content_len) {
    char *end = NULL;
    int r = conn_open(c, ctx);
    if (r) return r;
    char hdr[2048];
    int n = snprintf(hdr, sizeof hdr,
                     "%s %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: mpclink/0.1\r\n"
                     "X-MPC-Device: %s\r\nConnection: close\r\n",
                     method, target, ctx->host, ctx->port, ctx->device);
    if (body_len >= 0)
        n += snprintf(hdr + n, sizeof hdr - n,
                      "Content-Type: application/octet-stream\r\nContent-Length: %lld\r\n", body_len);
    n += snprintf(hdr + n, sizeof hdr - n, "\r\n");
    if (n >= (int)sizeof hdr || send_all(c, hdr, (size_t)n)) goto netfail;

    if (body) {
        char chunk[16384];
        size_t k;
        long long sent = 0;
        while ((k = fread(chunk, 1, sizeof chunk, body)) > 0) {
            if (ctx->cancel) { conn_close(c); return MPCLINK_ERR_CANCELLED; }
            if (send_all(c, chunk, k)) goto netfail;
            sent += (long long)k;
        }
        if (ferror(body) || sent != body_len) { conn_close(c); return MPCLINK_ERR_IO; }
    }

    /* read until end of headers */
    c->len = 0;
    while (!end) {
        if (c->len >= sizeof c->buf - 1) goto netfail;
        ssize_t got;
        do { got = recv(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len, 0); } while (got < 0 && errno == EINTR);
        if (got <= 0) goto netfail;
        c->len += (size_t)got;
        c->buf[c->len] = 0;
        end = strstr(c->buf, "\r\n\r\n");
    }
    *status = 0;
    if (sscanf(c->buf, "HTTP/%*d.%*d %d", status) != 1) goto netfail;
    *content_len = -1;
    for (char *line = strstr(c->buf, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, "Content-Length:", 15) == 0)
            *content_len = atoll(line + 17);
    }
    c->pos = (size_t)(end + 4 - c->buf);
    return MPCLINK_OK;
netfail:
    conn_close(c);
    return MPCLINK_ERR_NETWORK;
}

/* read the whole (small) body into a malloc'd, NUL-terminated buffer */
static char *read_body(conn_t *c, size_t limit) {
    size_t cap = 4096, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    for (;;) {
        if (n + 4096 > cap) {
            if (cap >= limit) { free(out); return NULL; }
            cap *= 2;
            char *t = (char *)realloc(out, cap);
            if (!t) { free(out); return NULL; }
            out = t;
        }
        ssize_t r = conn_read(c, out + n, cap - n - 1);
        if (r < 0) { free(out); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    out[n] = 0;
    return out;
}

/* GET target, return body (caller frees) */
static int http_get_small(mpclink_ctx *ctx, const char *target, char **body) {
    conn_t c;
    int status; long long clen;
    int r = http_request(&c, ctx, "GET", target, NULL, -1, &status, &clen);
    if (r) return r;
    *body = read_body(&c, 32u << 20);
    conn_close(&c);
    if (!*body) return MPCLINK_ERR_NETWORK;
    if (status != 200) { free(*body); *body = NULL; return MPCLINK_ERR_HTTP; }
    return MPCLINK_OK;
}

/* ---------- public API ---------- */

void mpclink_init(mpclink_ctx *ctx, const char *device) {
    memset(ctx, 0, sizeof *ctx);
    ctx->port = MPCLINK_PORT;
    ctx->timeout_ms = 8000;
    copy_str(ctx->device, sizeof ctx->device, valid_device(device) ? device : "MPC");
}

void mpclink_set_hub(mpclink_ctx *ctx, const char *host, int port) {
    copy_str(ctx->host, sizeof ctx->host, host);
    ctx->port = port > 0 ? port : MPCLINK_PORT;
}

int mpclink_discover(mpclink_ctx *ctx, int wait_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return MPCLINK_ERR_NETWORK;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(MPCLINK_DISCOVERY_PORT);
    const char *msg = "MPCLINK-DISCOVER 1";
    /* broadcast, then also loopback so a hub on the same machine (testing) answers */
    const char *targets[] = { "255.255.255.255", "127.0.0.1" };
    int rc = MPCLINK_ERR_NOT_FOUND;
    for (int attempt = 0; attempt < 3 && rc != MPCLINK_OK; attempt++) {
        for (size_t t = 0; t < 2; t++) {
            inet_pton(AF_INET, targets[t], &to.sin_addr);
            sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof to);
        }
        int slice = wait_ms / 3 > 0 ? wait_ms / 3 : 300;
        if (wait_fd(fd, POLLIN, slice)) continue;
        char buf[256];
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        int port = 0, off = 0;
        if (sscanf(buf, "MPCLINK-HUB 1 %d %n", &port, &off) >= 1 && port > 0 && port < 65536) {
            inet_ntop(AF_INET, &from.sin_addr, ctx->host, sizeof ctx->host);
            ctx->port = port;
            copy_str(ctx->hub_name, sizeof ctx->hub_name, off ? buf + off : "");
            rc = MPCLINK_OK;
        }
    }
    close(fd);
    return rc;
}

int mpclink_ping(mpclink_ctx *ctx) {
    char *body = NULL;
    int r = http_get_small(ctx, "/api/hello", &body);
    if (r == MPCLINK_OK && !strstr(body, "\"mpc-link\"")) r = MPCLINK_ERR_HTTP;
    free(body);
    return r;
}

static int list_tsv(mpclink_ctx *ctx, const char *target, mpclink_entry_cb cb, void *user);

int mpclink_list(mpclink_ctx *ctx, mpclink_entry_cb cb, void *user) {
    return list_tsv(ctx, "/api/list?format=tsv", cb, user);
}

int mpclink_list_stems(mpclink_ctx *ctx, mpclink_entry_cb cb, void *user) {
    return list_tsv(ctx, "/api/stems?format=tsv", cb, user);
}

int mpclink_send_stems(mpclink_ctx *ctx, const char *folder, int *files) {
    if (files) *files = 0;
    if (!folder || !*folder) return MPCLINK_ERR_ARG;
    char enc[1536], target[1700];
    url_encode(folder, enc, sizeof enc);
    snprintf(target, sizeof target, "/api/stems/send?path=%s", enc);
    conn_t c;
    int status; long long clen;
    int r = http_request(&c, ctx, "POST", target, NULL, 0, &status, &clen);
    if (r) return r;
    char *body = read_body(&c, 1u << 16);
    conn_close(&c);
    if (status != 200) { free(body); return MPCLINK_ERR_HTTP; }
    if (files && body) {
        const char *f = strstr(body, "\"files\":");
        if (f) *files = atoi(f + 8);
    }
    free(body);
    return MPCLINK_OK;
}

static int list_tsv(mpclink_ctx *ctx, const char *target, mpclink_entry_cb cb, void *user) {
    char *body = NULL;
    int r = http_get_small(ctx, target, &body);
    if (r) return r;
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        mpclink_entry e;
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        e.size = atoll(line);
        e.mtime = atoll(t1 + 1);
        copy_str(e.path, sizeof e.path, t2 + 1);
        if (cb(&e, user)) break;
    }
    free(body);
    return MPCLINK_OK;
}

int mpclink_upload(mpclink_ctx *ctx, const char *local_file, const char *remote_path) {
    return mpclink_upload_to(ctx, local_file, remote_path, NULL);
}

int mpclink_upload_to(mpclink_ctx *ctx, const char *local_file, const char *remote_path, const char *dest) {
    if (dest && strcmp(dest, "gpp") != 0 && strcmp(dest, "gpp-stems") != 0) return MPCLINK_ERR_ARG;
    FILE *f = fopen(local_file, "rb");
    if (!f) return MPCLINK_ERR_IO;
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode)) { fclose(f); return MPCLINK_ERR_IO; }
    char enc[1536], target[1700];
    url_encode(remote_path ? remote_path : base_name(local_file), enc, sizeof enc);
    snprintf(target, sizeof target, "/api/upload?path=%s%s%s", enc, dest ? "&dest=" : "", dest ? dest : "");
    conn_t c;
    int status; long long clen;
    int r = http_request(&c, ctx, "PUT", target, f, (long long)st.st_size, &status, &clen);
    fclose(f);
    if (r) return r;
    conn_close(&c);
    return status == 200 ? MPCLINK_OK : MPCLINK_ERR_HTTP;
}

int mpclink_download(mpclink_ctx *ctx, const char *path, const char *dest_dir,
                     char *saved_to, size_t saved_to_len) {
    if (!path || !*path || !dest_dir) return MPCLINK_ERR_ARG;
    const char *name = base_name(path);
    if (!*name || name[0] == '.') return MPCLINK_ERR_ARG;
    if (mkdirs(dest_dir)) return MPCLINK_ERR_IO;

    char enc[1536], target[1700];
    url_encode(path, enc, sizeof enc);
    snprintf(target, sizeof target, "/api/file?path=%s", enc);
    conn_t c;
    int status; long long clen;
    int r = http_request(&c, ctx, "GET", target, NULL, -1, &status, &clen);
    if (r) return r;
    if (status != 200) { conn_close(&c); return MPCLINK_ERR_HTTP; }

    /* write to a hidden temp file, rename when complete */
    char part[1100];
    /* pid + context address: unique even with two plugin instances in one MPC process */
    snprintf(part, sizeof part, "%s/.mpclink-%ld-%lx.part", dest_dir, (long)getpid(), (unsigned long)(size_t)ctx);
    FILE *out = fopen(part, "wb");
    if (!out) { conn_close(&c); return MPCLINK_ERR_IO; }
    char chunk[16384];
    long long got = 0;
    ssize_t n;
    while ((n = conn_read(&c, chunk, sizeof chunk)) > 0) {
        if (ctx->cancel) { n = -3; break; }
        if (fwrite(chunk, 1, (size_t)n, out) != (size_t)n) { n = -2; break; }
        got += n;
    }
    conn_close(&c);
    int bad = (n < 0) || (clen >= 0 && got != clen);
    if (fclose(out) || bad) {
        unlink(part);
        return n == -3 ? MPCLINK_ERR_CANCELLED : n == -2 ? MPCLINK_ERR_IO : MPCLINK_ERR_NETWORK;
    }

    char final_path[1100];
    free_name(dest_dir, name, final_path, sizeof final_path);
    if (rename(part, final_path)) { unlink(part); return MPCLINK_ERR_IO; }
    if (saved_to) copy_str(saved_to, saved_to_len, final_path);
    return MPCLINK_OK;
}

int mpclink_fetch_preview(mpclink_ctx *ctx, const char *path, unsigned char **out, size_t *out_len, size_t max_bytes) {
    *out = NULL;
    *out_len = 0;
    if (!path || !*path) return MPCLINK_ERR_ARG;
    char enc[1536], target[1700];
    url_encode(path, enc, sizeof enc);
    snprintf(target, sizeof target, "/api/preview?path=%s", enc);
    conn_t c;
    int status; long long clen;
    int r = http_request(&c, ctx, "GET", target, NULL, -1, &status, &clen);
    if (r) return r;
    if (status != 200) { conn_close(&c); return MPCLINK_ERR_HTTP; }
    if (clen < 44 || (unsigned long long)clen > (unsigned long long)max_bytes) { conn_close(&c); return MPCLINK_ERR_HTTP; }
    unsigned char *buf = (unsigned char *)malloc((size_t)clen);
    if (!buf) { conn_close(&c); return MPCLINK_ERR_IO; }
    size_t got = 0;
    ssize_t n = 0;
    while (got < (size_t)clen && (n = conn_read(&c, buf + got, (size_t)clen - got)) > 0) {
        if (ctx->cancel) { n = -3; break; }
        got += (size_t)n;
    }
    conn_close(&c);
    if (got != (size_t)clen) {
        free(buf);
        return n == -3 ? MPCLINK_ERR_CANCELLED : MPCLINK_ERR_NETWORK;
    }
    *out = buf;
    *out_len = got;
    return MPCLINK_OK;
}

int mpclink_take_cue(mpclink_ctx *ctx, char *buf, size_t buf_len) {
    if (buf_len) buf[0] = 0;
    char *body = NULL;
    int r = http_get_small(ctx, "/api/cue", &body);
    if (r) return r;
    char *nl = strchr(body, '\n');
    if (nl) *nl = 0;
    copy_str(buf, buf_len, body);
    free(body);
    return MPCLINK_OK;
}

int mpclink_poll_inbox(mpclink_ctx *ctx, const char *dest_dir) {
    char *body = NULL;
    int r = http_get_small(ctx, "/api/inbox", &body);
    if (r) return r;
    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        r = mpclink_download(ctx, line, dest_dir, NULL, 0);
        if (r == MPCLINK_ERR_NETWORK || r == MPCLINK_ERR_CANCELLED) break;   /* try the rest next poll */
        /* got it, or the hub can't serve it (deleted/renamed): either way clear it */
        char enc[1536], target[1700];
        url_encode(line, enc, sizeof enc);
        snprintf(target, sizeof target, "/api/inbox/ack?path=%s", enc);
        conn_t c; int status; long long clen;
        if (http_request(&c, ctx, "POST", target, NULL, 0, &status, &clen) == MPCLINK_OK) conn_close(&c);
        if (r == MPCLINK_OK) count++;
    }
    free(body);
    return (r == MPCLINK_ERR_NETWORK || r == MPCLINK_ERR_CANCELLED) ? r : count;
}

const char *mpclink_user_drive(void) {
    const char *env = getenv("MPCLINK_USER_DRIVE");
    if (env && *env) return env;
    static const char *candidates[] = { "/nvme", "/storage", "/sdcard" };
    struct stat root, st;
    if (stat("/", &root) != 0) return NULL;
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode) &&
            st.st_dev != root.st_dev && access(candidates[i], W_OK) == 0)
            return candidates[i];
    }
    return NULL;
}

const char *mpclink_default_dest(void) {
    static char dest[512];
    const char *drive = mpclink_user_drive();
    snprintf(dest, sizeof dest, "%s/From Computer", drive ? drive : "/tmp");
    return dest;
}

const char *mpclink_strerror(int s) {
    switch (s) {
    case MPCLINK_OK: return "OK";
    case MPCLINK_ERR_NETWORK: return "Can't reach the Mac";
    case MPCLINK_ERR_HTTP: return "Mac refused it";
    case MPCLINK_ERR_IO: return "File error on MPC";
    case MPCLINK_ERR_NOT_FOUND: return "No Mac found";
    case MPCLINK_ERR_ARG: return "Bad request";
    case MPCLINK_ERR_CANCELLED: return "Stopped";
    default: return "Error";
    }
}
