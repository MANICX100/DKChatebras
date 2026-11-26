#define _POSIX_C_SOURCE 200809L

#include "net.h"
#include "json.h"
#include "str.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define CEREBRAS_HOST "api.cerebras.ai"
#define CEREBRAS_PORT "443"
#define CEREBRAS_PATH "/v1/chat/completions"

/* OpenSSL ABI constants stable across 1.1 / 3.x / 4.x */
#define DK_SSL_VERIFY_PEER             0x01
#define DK_SSL_ERROR_WANT_READ         2
#define DK_SSL_ERROR_WANT_WRITE        3
#define DK_SSL_ERROR_ZERO_RETURN       6
#define DK_SSL_CTRL_SET_TLSEXT_HOST    55
#define DK_TLSEXT_NAMETYPE_HOST_NAME   0

typedef void SSL_CTX;
typedef void SSL;
typedef void SSL_METHOD;

typedef struct {
    void *handle;
    const SSL_METHOD *(*TLS_client_method)(void);
    SSL_CTX *(*SSL_CTX_new)(const SSL_METHOD *);
    void (*SSL_CTX_free)(SSL_CTX *);
    int (*SSL_CTX_set_default_verify_paths)(SSL_CTX *);
    void (*SSL_CTX_set_verify)(SSL_CTX *, int, void *);
    SSL *(*SSL_new)(SSL_CTX *);
    void (*SSL_free)(SSL *);
    int (*SSL_set_fd)(SSL *, int);
    int (*SSL_set1_host)(SSL *, const char *);
    long (*SSL_ctrl)(SSL *, int, long, void *);
    int (*SSL_connect)(SSL *);
    int (*SSL_read)(SSL *, void *, int);
    int (*SSL_write)(SSL *, const void *, int);
    int (*SSL_shutdown)(SSL *);
    int (*SSL_get_error)(const SSL *, int);
    bool loaded;
} TlsApi;

static pthread_mutex_t g_tls_mtx = PTHREAD_MUTEX_INITIALIZER;
static TlsApi g_tls;

static bool tls_load(char *err, size_t err_cap) {
    pthread_mutex_lock(&g_tls_mtx);
    if (g_tls.loaded) {
        pthread_mutex_unlock(&g_tls_mtx);
        return true;
    }

    static const char *const candidates[] = {
        "libssl.so.4",
        "libssl.so.3",
        "libssl.so.1.1",
        "libssl.so",
    };

    void *h = NULL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        h = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (h) break;
    }
    if (!h) {
        pthread_mutex_unlock(&g_tls_mtx);
        snprintf(err, err_cap, "OpenSSL (libssl) not found on this system");
        return false;
    }

    #define LOAD_SYM(name) do { \
        *(void **)(&g_tls.name) = dlsym(h, #name); \
        if (!g_tls.name) { \
            dlclose(h); \
            memset(&g_tls, 0, sizeof(g_tls)); \
            pthread_mutex_unlock(&g_tls_mtx); \
            snprintf(err, err_cap, "Missing TLS symbol: " #name); \
            return false; \
        } \
    } while (0)

    LOAD_SYM(TLS_client_method);
    LOAD_SYM(SSL_CTX_new);
    LOAD_SYM(SSL_CTX_free);
    LOAD_SYM(SSL_CTX_set_default_verify_paths);
    LOAD_SYM(SSL_CTX_set_verify);
    LOAD_SYM(SSL_new);
    LOAD_SYM(SSL_free);
    LOAD_SYM(SSL_set_fd);
    LOAD_SYM(SSL_set1_host);
    LOAD_SYM(SSL_ctrl);
    LOAD_SYM(SSL_connect);
    LOAD_SYM(SSL_read);
    LOAD_SYM(SSL_write);
    LOAD_SYM(SSL_shutdown);
    LOAD_SYM(SSL_get_error);
    #undef LOAD_SYM

    g_tls.handle = h;
    g_tls.loaded = true;
    pthread_mutex_unlock(&g_tls_mtx);
    return true;
}

struct DkNetClient {
    int wake_pipe[2];
    int cancel_pipe[2];
    pthread_mutex_t mtx;
    pthread_t thread;
    bool thread_active;
    bool running;
    bool finished;
    bool cancelled;
    DkStr pending_chunks;
    char error_msg[512];

    /* Request payload owned by worker */
    char *req_http;
    size_t req_len;
};

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void wake_ui(DkNetClient *c) {
    char b = 1;
    ssize_t r = write(c->wake_pipe[1], &b, 1);
    (void)r;
}

static void drain_fd(int fd) {
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
}

static void push_chunk(DkNetClient *c, const char *s, size_t len) {
    if (len == 0) return;
    pthread_mutex_lock(&c->mtx);
    dk_str_append_len(&c->pending_chunks, s, len);
    pthread_mutex_unlock(&c->mtx);
    wake_ui(c);
}

static bool is_cancelled(DkNetClient *c) {
    struct pollfd pfd = { .fd = c->cancel_pipe[0], .events = POLLIN };
    return poll(&pfd, 1, 0) > 0;
}

static int wait_socket_or_cancel(DkNetClient *c, int sock, short events, int timeout_ms) {
    struct pollfd pfds[2] = {
        { .fd = c->cancel_pipe[0], .events = POLLIN },
        { .fd = sock, .events = events },
    };
    for (;;) {
        int r = poll(pfds, 2, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0; /* timeout */
        if (pfds[0].revents & (POLLIN | POLLHUP)) return -2; /* cancelled */
        if (pfds[1].revents & (events | POLLERR | POLLHUP)) return 1;
    }
}

static int tls_write_all(DkNetClient *c, SSL *ssl, int sock, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        if (is_cancelled(c)) return -2;
        int n = g_tls.SSL_write(ssl, buf + off, (int)(len - off));
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        int err = g_tls.SSL_get_error(ssl, n);
        if (err == DK_SSL_ERROR_WANT_READ || err == DK_SSL_ERROR_WANT_WRITE) {
            short ev = (err == DK_SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            int w = wait_socket_or_cancel(c, sock, ev, 30000);
            if (w <= 0) return w == -2 ? -2 : -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int tls_read_some(DkNetClient *c, SSL *ssl, int sock, char *buf, size_t cap) {
    for (;;) {
        if (is_cancelled(c)) return -2;
        int n = g_tls.SSL_read(ssl, buf, (int)cap);
        if (n > 0) return n;
        int err = g_tls.SSL_get_error(ssl, n);
        if (err == DK_SSL_ERROR_ZERO_RETURN) return 0;
        if (err == DK_SSL_ERROR_WANT_READ || err == DK_SSL_ERROR_WANT_WRITE) {
            short ev = (err == DK_SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            int w = wait_socket_or_cancel(c, sock, ev, 60000);
            if (w <= 0) return w == -2 ? -2 : -1;
            continue;
        }
        return (n == 0) ? 0 : -1;
    }
}

typedef struct {
    DkStr sse_line_buf;
    DkStr event_data;
    bool done;
    bool has_error;
    char err[512];
} SseParser;

static bool sse_handle_data(DkNetClient *c, SseParser *sp, const char *data, size_t len) {
    if (len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        sp->done = true;
        return false;
    }
    DkJson *root = dk_json_parse(data, len);
    if (!root || root->type != DK_JSON_OBJECT) {
        dk_json_free(root);
        return true; /* ignore keepalive / malformed chunk */
    }
    DkJson *err_obj = dk_json_object_get(root, "error");
    if (err_obj && err_obj->type == DK_JSON_OBJECT) {
        const char *msg = dk_json_get_str(err_obj, "message", "Cerebras API error");
        snprintf(sp->err, sizeof(sp->err), "%s", msg);
        sp->has_error = true;
        dk_json_free(root);
        return false;
    }
    DkJson *choices = dk_json_object_get(root, "choices");
    if (choices && choices->type == DK_JSON_ARRAY && choices->u.array.len > 0) {
        DkJson *ch = choices->u.array.items[0];
        DkJson *delta = dk_json_object_get(ch, "delta");
        if (delta && delta->type == DK_JSON_OBJECT) {
            DkJson *content = dk_json_object_get(delta, "content");
            if (content && content->type == DK_JSON_STRING && content->u.string.len > 0) {
                push_chunk(c, content->u.string.ptr, content->u.string.len);
            }
        }
    }
    dk_json_free(root);
    return true;
}

static bool sse_dispatch(DkNetClient *c, SseParser *sp) {
    if (sp->event_data.len == 0)
        return true;
    if (sp->event_data.data[sp->event_data.len - 1] == '\n')
        sp->event_data.data[--sp->event_data.len] = '\0';
    bool ok = sse_handle_data(c, sp, sp->event_data.data, sp->event_data.len);
    dk_str_clear(&sp->event_data);
    return ok;
}

static bool sse_feed(DkNetClient *c, SseParser *sp, const char *bytes, size_t len, bool flush) {
    if (len > 0)
        dk_str_append_len(&sp->sse_line_buf, bytes, len);

    while (!sp->done && !sp->has_error) {
        if (!sp->sse_line_buf.data || sp->sse_line_buf.len == 0)
            break;
        char *nl = memchr(sp->sse_line_buf.data, '\n', sp->sse_line_buf.len);
        if (!nl) break;
        size_t consumed = (size_t)(nl - sp->sse_line_buf.data) + 1;
        size_t llen = consumed - 1;
        if (llen > 0 && sp->sse_line_buf.data[llen - 1] == '\r')
            llen--;

        if (llen == 0) {
            dk_str_erase(&sp->sse_line_buf, 0, consumed);
            if (!sse_dispatch(c, sp)) return false;
            continue;
        }
        if (llen >= 5 && memcmp(sp->sse_line_buf.data, "data:", 5) == 0) {
            const char *d = sp->sse_line_buf.data + 5;
            size_t dl = llen - 5;
            if (dl > 0 && *d == ' ') { d++; dl--; }
            dk_str_append_len(&sp->event_data, d, dl);
            dk_str_push(&sp->event_data, '\n');
        }
        dk_str_erase(&sp->sse_line_buf, 0, consumed);
    }

    if (flush && !sp->done && !sp->has_error) {
        if (sp->sse_line_buf.len > 0) {
            size_t llen = sp->sse_line_buf.len;
            if (sp->sse_line_buf.data[llen - 1] == '\r') llen--;
            if (llen >= 5 && memcmp(sp->sse_line_buf.data, "data:", 5) == 0) {
                const char *d = sp->sse_line_buf.data + 5;
                size_t dl = llen - 5;
                if (dl > 0 && *d == ' ') { d++; dl--; }
                dk_str_append_len(&sp->event_data, d, dl);
                dk_str_push(&sp->event_data, '\n');
            }
            dk_str_clear(&sp->sse_line_buf);
        }
        sse_dispatch(c, sp);
    }
    return !sp->done && !sp->has_error;
}

/* Chunked transfer decoder */
typedef struct {
    DkStr buf;
    size_t chunk_rem;
    bool reading_size;
    bool expect_crlf;
    bool eof;
} ChunkedDec;

static bool chunked_feed(ChunkedDec *cd, DkNetClient *c, SseParser *sp,
                         const char *data, size_t len, DkStr *raw_out) {
    if (len > 0)
        dk_str_append_len(&cd->buf, data, len);

    while (!cd->eof) {
        if (cd->expect_crlf) {
            if (cd->buf.len < 2) break;
            if (cd->buf.data[0] == '\r' && cd->buf.data[1] == '\n')
                dk_str_erase(&cd->buf, 0, 2);
            else if (cd->buf.data[0] == '\n')
                dk_str_erase(&cd->buf, 0, 1);
            cd->expect_crlf = false;
            cd->reading_size = true;
        }
        if (cd->reading_size) {
            char *nl = cd->buf.len ? memchr(cd->buf.data, '\n', cd->buf.len) : NULL;
            if (!nl) break;
            size_t line_bytes = (size_t)(nl - cd->buf.data) + 1;
            char hex[32];
            size_t hlen = line_bytes - 1;
            if (hlen > 0 && cd->buf.data[hlen - 1] == '\r') hlen--;
            if (hlen >= sizeof(hex)) hlen = sizeof(hex) - 1;
            memcpy(hex, cd->buf.data, hlen);
            hex[hlen] = '\0';
            char *semi = strchr(hex, ';');
            if (semi) *semi = '\0';
            cd->chunk_rem = (size_t)strtoull(hex, NULL, 16);
            dk_str_erase(&cd->buf, 0, line_bytes);
            if (cd->chunk_rem == 0) {
                cd->eof = true;
                break;
            }
            cd->reading_size = false;
        }
        if (cd->chunk_rem > 0) {
            if (cd->buf.len == 0) break;
            size_t take = cd->buf.len < cd->chunk_rem ? cd->buf.len : cd->chunk_rem;
            if (raw_out) {
                dk_str_append_len(raw_out, cd->buf.data, take);
            } else if (!sse_feed(c, sp, cd->buf.data, take, false)) {
                return false;
            }
            dk_str_erase(&cd->buf, 0, take);
            cd->chunk_rem -= take;
            if (cd->chunk_rem == 0)
                cd->expect_crlf = true;
        }
    }
    return true;
}

static void *worker_main(void *arg) {
    DkNetClient *c = (DkNetClient *)arg;
    char err[512] = {0};
    bool cancelled = false;
    int sock = -1;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;

    if (!tls_load(err, sizeof(err)))
        goto finish;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(CEREBRAS_HOST, CEREBRAS_PORT, &hints, &res);
    if (gai != 0 || !res) {
        snprintf(err, sizeof(err), "DNS lookup failed: %s", gai_strerror(gai));
        goto finish;
    }

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        if (is_cancelled(c)) {
            cancelled = true;
            break;
        }
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        set_nonblock(sock);
        int rc = connect(sock, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) break;
        if (errno == EINPROGRESS) {
            int w = wait_socket_or_cancel(c, sock, POLLOUT, 15000);
            if (w == 1) {
                int soerr = 0;
                socklen_t slen = sizeof(soerr);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr == 0)
                    break;
            } else if (w == -2) {
                cancelled = true;
                close(sock);
                sock = -1;
                break;
            }
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (cancelled) goto finish;
    if (sock < 0) {
        snprintf(err, sizeof(err), "Could not connect to %s", CEREBRAS_HOST);
        goto finish;
    }

    ctx = g_tls.SSL_CTX_new(g_tls.TLS_client_method());
    if (!ctx) {
        snprintf(err, sizeof(err), "Failed to initialize TLS context");
        goto finish;
    }
    g_tls.SSL_CTX_set_default_verify_paths(ctx);
    g_tls.SSL_CTX_set_verify(ctx, DK_SSL_VERIFY_PEER, NULL);

    ssl = g_tls.SSL_new(ctx);
    if (!ssl) {
        snprintf(err, sizeof(err), "Failed to create TLS session");
        goto finish;
    }
    g_tls.SSL_set_fd(ssl, sock);
    g_tls.SSL_ctrl(ssl, DK_SSL_CTRL_SET_TLSEXT_HOST, DK_TLSEXT_NAMETYPE_HOST_NAME, (void *)CEREBRAS_HOST);
    g_tls.SSL_set1_host(ssl, CEREBRAS_HOST);

    for (;;) {
        if (is_cancelled(c)) { cancelled = true; goto finish; }
        int rc = g_tls.SSL_connect(ssl);
        if (rc == 1) break;
        int serr = g_tls.SSL_get_error(ssl, rc);
        if (serr == DK_SSL_ERROR_WANT_READ || serr == DK_SSL_ERROR_WANT_WRITE) {
            short ev = (serr == DK_SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            int w = wait_socket_or_cancel(c, sock, ev, 15000);
            if (w == -2) { cancelled = true; goto finish; }
            if (w <= 0) {
                snprintf(err, sizeof(err), "TLS handshake timed out");
                goto finish;
            }
            continue;
        }
        snprintf(err, sizeof(err), "TLS handshake failed");
        goto finish;
    }

    int wr = tls_write_all(c, ssl, sock, c->req_http, c->req_len);
    if (wr == -2) { cancelled = true; goto finish; }
    if (wr < 0) {
        snprintf(err, sizeof(err), "Failed sending request to Cerebras");
        goto finish;
    }

    /* Read HTTP response headers */
    DkStr hdr_buf;
    dk_str_init_cap(&hdr_buf, 1024);
    size_t hdr_end = 0;
    char tmp[8192];

    for (;;) {
        int n = tls_read_some(c, ssl, sock, tmp, sizeof(tmp));
        if (n == -2) { cancelled = true; dk_str_free(&hdr_buf); goto finish; }
        if (n <= 0) {
            snprintf(err, sizeof(err), "Connection closed before HTTP headers");
            dk_str_free(&hdr_buf);
            goto finish;
        }
        dk_str_append_len(&hdr_buf, tmp, (size_t)n);
        char *sep = strstr(hdr_buf.data, "\r\n\r\n");
        if (sep) {
            hdr_end = (size_t)(sep - hdr_buf.data) + 4;
            break;
        }
        if (hdr_buf.len > 65536) {
            snprintf(err, sizeof(err), "HTTP response header too large");
            dk_str_free(&hdr_buf);
            goto finish;
        }
    }

    int status_code = 0;
    char reason[128] = {0};
    if (sscanf(hdr_buf.data, "HTTP/%*d.%*d %d %127[^\r\n]", &status_code, reason) < 1) {
        snprintf(err, sizeof(err), "Malformed HTTP response");
        dk_str_free(&hdr_buf);
        goto finish;
    }

    bool is_chunked = false;
    const char *hp = hdr_buf.data;
    const char *hend = hdr_buf.data + hdr_end;
    while (hp < hend) {
        const char *eol = memchr(hp, '\n', (size_t)(hend - hp));
        if (!eol) break;
        size_t hlen = (size_t)(eol - hp);
        if (hlen >= 18 && dk_ascii_strncasecmp(hp, "Transfer-Encoding:", 18) == 0) {
            for (const char *q = hp + 18; q + 7 <= eol; q++) {
                if (dk_ascii_strncasecmp(q, "chunked", 7) == 0) {
                    is_chunked = true;
                    break;
                }
            }
        }
        hp = eol + 1;
    }

    const char *initial_body = hdr_buf.data + hdr_end;
    size_t initial_len = hdr_buf.len - hdr_end;

    if (status_code < 200 || status_code >= 300) {
        DkStr err_body;
        dk_str_init_cap(&err_body, 512);
        ChunkedDec cd = { .reading_size = true };
        dk_str_init(&cd.buf);
        if (is_chunked) {
            chunked_feed(&cd, c, NULL, initial_body, initial_len, &err_body);
            while (!cd.eof && err_body.len < 8192) {
                int n = tls_read_some(c, ssl, sock, tmp, sizeof(tmp));
                if (n <= 0) break;
                chunked_feed(&cd, c, NULL, tmp, (size_t)n, &err_body);
            }
        } else {
            dk_str_append_len(&err_body, initial_body, initial_len);
            while (err_body.len < 8192) {
                int n = tls_read_some(c, ssl, sock, tmp, sizeof(tmp));
                if (n <= 0) break;
                dk_str_append_len(&err_body, tmp, (size_t)n);
            }
        }
        dk_str_free(&cd.buf);

        bool parsed_msg = false;
        if (err_body.len > 0) {
            DkJson *j = dk_json_parse(err_body.data, err_body.len);
            if (j && j->type == DK_JSON_OBJECT) {
                DkJson *eo = dk_json_object_get(j, "error");
                const char *m = eo ? dk_json_get_str(eo, "message", NULL) : dk_json_get_str(j, "message", NULL);
                if (m && *m) {
                    snprintf(err, sizeof(err), "HTTP %d: %s", status_code, m);
                    parsed_msg = true;
                }
            }
            dk_json_free(j);
        }
        if (!parsed_msg)
            snprintf(err, sizeof(err), "Cerebras returned HTTP %d: %s", status_code, reason[0] ? reason : "Error");

        dk_str_free(&err_body);
        dk_str_free(&hdr_buf);
        goto finish;
    }

    SseParser sp;
    memset(&sp, 0, sizeof(sp));
    dk_str_init_cap(&sp.sse_line_buf, 1024);
    dk_str_init_cap(&sp.event_data, 1024);

    ChunkedDec cd = { .reading_size = true };
    dk_str_init_cap(&cd.buf, 1024);

    bool keep_going = true;
    if (initial_len > 0) {
        if (is_chunked)
            keep_going = chunked_feed(&cd, c, &sp, initial_body, initial_len, NULL);
        else
            keep_going = sse_feed(c, &sp, initial_body, initial_len, false);
    }
    dk_str_free(&hdr_buf);

    while (keep_going && !cd.eof) {
        int n = tls_read_some(c, ssl, sock, tmp, sizeof(tmp));
        if (n == -2) { cancelled = true; break; }
        if (n == 0) break;
        if (n < 0) {
            snprintf(err, sizeof(err), "Network error while streaming response");
            break;
        }
        if (is_chunked)
            keep_going = chunked_feed(&cd, c, &sp, tmp, (size_t)n, NULL);
        else
            keep_going = sse_feed(c, &sp, tmp, (size_t)n, false);
    }

    if (!cancelled && !err[0] && !sp.done && !sp.has_error)
        sse_feed(c, &sp, NULL, 0, true);

    if (sp.has_error && !err[0])
        snprintf(err, sizeof(err), "%s", sp.err);

    dk_str_free(&cd.buf);
    dk_str_free(&sp.sse_line_buf);
    dk_str_free(&sp.event_data);

finish:
    if (ssl) {
        if (!cancelled) g_tls.SSL_shutdown(ssl);
        g_tls.SSL_free(ssl);
    }
    if (ctx) g_tls.SSL_CTX_free(ctx);
    if (sock >= 0) close(sock);

    free(c->req_http);
    c->req_http = NULL;
    c->req_len = 0;

    pthread_mutex_lock(&c->mtx);
    c->running = false;
    c->finished = true;
    c->cancelled = cancelled;
    snprintf(c->error_msg, sizeof(c->error_msg), "%s", err);
    pthread_mutex_unlock(&c->mtx);
    wake_ui(c);
    return NULL;
}

DkNetClient *dk_net_new(void) {
    DkNetClient *c = calloc(1, sizeof(DkNetClient));
    if (!c) return NULL;
    if (pipe(c->wake_pipe) != 0) { free(c); return NULL; }
    if (pipe(c->cancel_pipe) != 0) {
        close(c->wake_pipe[0]); close(c->wake_pipe[1]);
        free(c);
        return NULL;
    }
    set_nonblock(c->wake_pipe[0]);
    set_nonblock(c->wake_pipe[1]);
    set_nonblock(c->cancel_pipe[0]);
    set_nonblock(c->cancel_pipe[1]);
    pthread_mutex_init(&c->mtx, NULL);
    dk_str_init(&c->pending_chunks);
    return c;
}

void dk_net_free(DkNetClient *c) {
    if (!c) return;
    dk_net_stop(c);
    if (c->thread_active) {
        pthread_join(c->thread, NULL);
        c->thread_active = false;
    }
    free(c->req_http);
    dk_str_free(&c->pending_chunks);
    pthread_mutex_destroy(&c->mtx);
    close(c->wake_pipe[0]);
    close(c->wake_pipe[1]);
    close(c->cancel_pipe[0]);
    close(c->cancel_pipe[1]);
    free(c);
}

int dk_net_wake_fd(const DkNetClient *c) {
    return c ? c->wake_pipe[0] : -1;
}

bool dk_net_start_stream(DkNetClient *c, const char *api_key, const char *model,
                         const DkMessage *messages, size_t msg_count) {
    if (!c || !api_key || !model) return false;
    if (c->thread_active) {
        pthread_join(c->thread, NULL);
        c->thread_active = false;
    }
    drain_fd(c->cancel_pipe[0]);
    drain_fd(c->wake_pipe[0]);

    /* Build JSON body */
    DkJson *root = dk_json_new_object();
    dk_json_object_set(root, "model", dk_json_new_string(model));
    dk_json_object_set(root, "stream", dk_json_new_bool(true));
    DkJson *arr = dk_json_new_array();
    for (size_t i = 0; i < msg_count; i++) {
        DkJson *m = dk_json_new_object();
        dk_json_object_set(m, "role", dk_json_new_string(messages[i].role));
        dk_json_object_set(m, "content", dk_json_new_string(messages[i].content));
        dk_json_array_push(arr, m);
    }
    dk_json_object_set(root, "messages", arr);

    DkStr body;
    dk_str_init_cap(&body, 512);
    dk_json_write(root, &body, false);
    dk_json_free(root);

    DkStr req;
    dk_str_init_cap(&req, body.len + 320);
    dk_str_appendf(&req,
                   "POST %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: DKChatebras/2.0\r\n"
                   "Authorization: Bearer %s\r\n"
                   "Content-Type: application/json\r\n"
                   "Accept: text/event-stream\r\n"
                   "Connection: close\r\n"
                   "Content-Length: %zu\r\n\r\n",
                   CEREBRAS_PATH, CEREBRAS_HOST, api_key, body.len);
    dk_str_append_len(&req, body.data, body.len);
    dk_str_free(&body);

    pthread_mutex_lock(&c->mtx);
    dk_str_clear(&c->pending_chunks);
    c->error_msg[0] = '\0';
    c->running = true;
    c->finished = false;
    c->cancelled = false;
    c->req_len = req.len;
    c->req_http = dk_str_steal(&req);
    pthread_mutex_unlock(&c->mtx);

    if (pthread_create(&c->thread, NULL, worker_main, c) != 0) {
        pthread_mutex_lock(&c->mtx);
        c->running = false;
        free(c->req_http);
        c->req_http = NULL;
        pthread_mutex_unlock(&c->mtx);
        return false;
    }
    c->thread_active = true;
    return true;
}

void dk_net_stop(DkNetClient *c) {
    if (!c) return;
    char b = 1;
    ssize_t r = write(c->cancel_pipe[1], &b, 1);
    (void)r;
}

bool dk_net_is_running(const DkNetClient *c) {
    if (!c) return false;
    pthread_mutex_lock((pthread_mutex_t *)&c->mtx);
    bool r = c->running;
    pthread_mutex_unlock((pthread_mutex_t *)&c->mtx);
    return r;
}

bool dk_net_poll(DkNetClient *c, DkStr *out_chunk, bool *out_cancelled,
                 char *err_buf, size_t err_cap) {
    if (!c) return false;
    drain_fd(c->wake_pipe[0]);

    pthread_mutex_lock(&c->mtx);
    if (out_chunk && c->pending_chunks.len > 0) {
        dk_str_append_len(out_chunk, c->pending_chunks.data, c->pending_chunks.len);
        dk_str_clear(&c->pending_chunks);
    }
    bool done = c->finished;
    if (done) {
        c->finished = false;
        if (out_cancelled) *out_cancelled = c->cancelled;
        if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", c->error_msg);
    }
    pthread_mutex_unlock(&c->mtx);

    if (done && c->thread_active) {
        pthread_join(c->thread, NULL);
        c->thread_active = false;
    }
    return done;
}
