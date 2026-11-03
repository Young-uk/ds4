/* Opt-in resumable SSE transport. The inference client talks to a local socket
 * pair, not the transient network connection. Immutable registry entries and
 * atomic publication avoid adding a second lock to the inference scheduler. */
#define ATOMIC_STREAM_LIMIT (16u * 1024u * 1024u)
#define ATOMIC_STREAM_TOTAL (128u * 1024u * 1024u)
#define ATOMIC_STREAM_TTL 86400
typedef struct atomic_stream {
    struct atomic_stream *next;
    server *srv;
    char id[37];
    char path[96];
    char request_path[112];
    char *body;
    size_t body_len;
    size_t length;
    time_t finished;
    int done, cancelled, expired, failed, http_status;
} atomic_stream;
static atomic_stream *atomic_streams;
static size_t atomic_stream_bytes;
static unsigned atomic_stream_count;
static void *client_main(void *arg);

static bool atomic_stream_id_valid(const char *id) {
    if (strlen(id) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!((id[i] >= '0' && id[i] <= '9') ||
                     (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

static void atomic_stream_expire(void) {
    time_t now = time(NULL);
    for (atomic_stream *r = __atomic_load_n(&atomic_streams, __ATOMIC_ACQUIRE);
         r; r = r->next) {
        if (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE) &&
            now - r->finished >= ATOMIC_STREAM_TTL &&
            !__atomic_exchange_n(&r->expired, 1, __ATOMIC_ACQ_REL)) {
            unlink(r->path);
            unlink(r->request_path);
            __atomic_fetch_sub(&atomic_stream_bytes, r->length + r->body_len, __ATOMIC_RELAXED);
        }
    }
}

static atomic_stream *atomic_stream_find(const char *id) {
    for (atomic_stream *r = __atomic_load_n(&atomic_streams, __ATOMIC_ACQUIRE);
         r; r = r->next) if (!strcmp(id, r->id)) return r;
    return NULL;
}

static bool atomic_stream_append(atomic_stream *r, int file, const char *p, size_t n) {
    size_t old = __atomic_fetch_add(&atomic_stream_bytes, n, __ATOMIC_RELAXED);
    if (old + n > ATOMIC_STREAM_TOTAL || r->length + n > ATOMIC_STREAM_LIMIT) {
        __atomic_fetch_sub(&atomic_stream_bytes, n, __ATOMIC_RELAXED);
        return false;
    }
    size_t at = 0;
    while (at < n) {
        ssize_t written = write(file, p + at, n - at);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            __atomic_fetch_sub(&atomic_stream_bytes, n, __ATOMIC_RELAXED);
            return false;
        }
        at += (size_t)written;
    }
    __atomic_store_n(&r->length, r->length + n, __ATOMIC_RELEASE);
    return true;
}

static void *atomic_stream_capture(void *arg) {
    atomic_stream *r = arg;
    int pair[2] = {-1, -1};
    int file = open(r->path, O_WRONLY | O_APPEND);
    bool ok = file >= 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0;
    if (ok) {
        client_arg *ca = xmalloc(sizeof(*ca));
        ca->srv = r->srv; ca->fd = pair[1];
        pthread_t th;
        /* Use the existing client lifetime counter so shutdown drains the
         * internal inference client before releasing the model. */
        pthread_mutex_lock(&r->srv->mu);
        r->srv->clients++;
        pthread_mutex_unlock(&r->srv->mu);
        if (pthread_create(&th, NULL, client_main, ca) != 0) {
            client_done(r->srv); free(ca); close(pair[1]); ok = false;
        } else {
            pthread_detach(th);
            buf h = {0};
            buf_printf(&h, "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\n\r\n", r->body_len);
            ok = send_all(pair[0], h.ptr, h.len) && send_all(pair[0], r->body, r->body_len);
            buf_free(&h);
        }
    }
    free(r->body); r->body = NULL;
    buf head = {0};
    bool headers = false;
    char ending[64] = {0};
    while (ok && !g_stop_requested && !__atomic_load_n(&r->cancelled, __ATOMIC_ACQUIRE)) {
        struct pollfd pfd = {.fd = pair[0], .events = POLLIN};
        int ready = poll(&pfd, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { ok = false; break; }
        if (!ready) continue;
        char data[8192];
        ssize_t n = recv(pair[0], data, sizeof(data), 0);
        if (n == 0) { if (r->http_status == 200 && !strstr(ending, "data: [DONE]\n\n") && !strstr(ending, "data: [DONE]\r\n\r\n")) ok = false; break; }
        if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
        size_t keep = (size_t)n < sizeof(ending) - 1 ? (size_t)n : sizeof(ending) - 1;
        size_t old = strlen(ending);
        if (old + keep >= sizeof(ending)) {
            size_t drop = old + keep - sizeof(ending) + 1;
            memmove(ending, ending + drop, old - drop); old -= drop;
        }
        memcpy(ending + old, data + n - keep, keep); ending[old + keep] = 0;
        if (!headers) {
            buf_append(&head, data, (size_t)n);
            ssize_t end = header_end(head.ptr, head.len);
            if (end < 0) { if (head.len > 65536) ok = false; continue; }
            int status = 0;
            if (sscanf(head.ptr, "HTTP/1.1 %d", &status) != 1) { ok = false; break; }
            __atomic_store_n(&r->http_status, status, __ATOMIC_RELEASE);
            headers = true;
            ok = atomic_stream_append(r, file, head.ptr + end, head.len - (size_t)end);
            buf_free(&head);
        } else ok = atomic_stream_append(r, file, data, (size_t)n);
    }
    buf_free(&head);
    if (pair[0] >= 0) close(pair[0]);
    if (file >= 0) close(file);
    r->failed = !ok || !headers || __atomic_load_n(&r->cancelled, __ATOMIC_ACQUIRE);
    r->finished = time(NULL);
    __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
    client_done(r->srv);
    return NULL;
}

static void atomic_stream_tail(server *s, int fd, atomic_stream *r, size_t offset) {
    if (__atomic_load_n(&r->expired, __ATOMIC_ACQUIRE)) {
        http_error(fd, s->enable_cors, 410, "stream expired; do not replay a partially delivered request"); return;
    }
    size_t available = __atomic_load_n(&r->length, __ATOMIC_ACQUIRE);
    if (offset > available) { http_error(fd, s->enable_cors, 409, "invalid stream cursor"); return; }
    int file = open(r->path, O_RDONLY);
    if (file < 0) { http_error(fd, s->enable_cors, 410, "stream unavailable"); return; }
    int status;
    while (!(status = __atomic_load_n(&r->http_status, __ATOMIC_ACQUIRE)) &&
           !__atomic_load_n(&r->done, __ATOMIC_ACQUIRE) && !g_stop_requested) {
        if (client_socket_disconnected(fd)) { close(file); return; }
        struct timespec pause = {.tv_nsec = 50000000}; nanosleep(&pause, NULL);
    }
    if (status != 200) {
        while (!__atomic_load_n(&r->done, __ATOMIC_ACQUIRE) && !g_stop_requested) {
            if (client_socket_disconnected(fd)) { close(file); return; }
            struct timespec pause = {.tv_nsec = 50000000}; nanosleep(&pause, NULL);
        }
        size_t n = __atomic_load_n(&r->length, __ATOMIC_ACQUIRE);
        char *body = calloc(n + 1, 1);
        if (n && pread(file, body, n, 0) != (ssize_t)n) n = 0;
        if (status && n) http_response(fd, s->enable_cors, status, "application/json", body);
        else http_error(fd, s->enable_cors, 500, "resumable generation failed before response headers");
        free(body); close(file); return;
    }
    if (!sse_headers(fd, s->enable_cors)) { close(file); return; }
    while (!g_stop_requested) {
        bool done = __atomic_load_n(&r->done, __ATOMIC_ACQUIRE);
        available = __atomic_load_n(&r->length, __ATOMIC_ACQUIRE);
        if (offset < available) {
            char data[8192];
            size_t n = available - offset;
            if (n > sizeof(data)) n = sizeof(data);
            ssize_t got = pread(file, data, n, (off_t)offset);
            if (got <= 0 || !send_all(fd, data, (size_t)got)) break;
            offset += (size_t)got;
        } else if (done) {
            if (r->failed) {
                sse_error_event(fd, NULL, "Resumable generation failed; retry the request explicitly");
                send_all(fd, "data: [DONE]\n\n", 14);
            }
            break;
        } else {
            if (client_socket_disconnected(fd)) break;
            struct timespec pause = {.tv_nsec = 50000000}; nanosleep(&pause, NULL);
        }
    }
    close(file);
}

static bool atomic_stream_route(server *s, int fd, http_request *hr) {
    const char *prefix = "/v1/atomic/streams";
    size_t len = strlen(prefix);
    if (strncmp(hr->path, prefix, len)) return false;
    if (!strcmp(hr->path, prefix) && !strcmp(hr->method, "GET")) {
        http_response(fd, s->enable_cors, 200, "application/json", "{\"version\":1}"); return true;
    }
    char id[37];
    if (hr->path[len] != '/' || strlen(hr->path + len + 1) < 36) {
        http_error(fd, s->enable_cors, 400, "invalid stream ID"); return true;
    }
    memcpy(id, hr->path + len + 1, 36); id[36] = 0;
    if (!atomic_stream_id_valid(id)) { http_error(fd, s->enable_cors, 400, "invalid stream ID"); return true; }
    const char *suffix = hr->path + len + 37;
    atomic_stream_expire();
    atomic_stream *r = atomic_stream_find(id);
    if (!strcmp(hr->method, "DELETE") && !*suffix) {
        if (!r) {
            /* Cancellation may overtake the initial POST (for example while
             * its large request body is still arriving). Publish a tombstone
             * so a late POST cannot create an abandoned generation. */
            if (__atomic_fetch_add(&atomic_stream_count, 1, __ATOMIC_RELAXED) >= 4096) {
                __atomic_fetch_sub(&atomic_stream_count, 1, __ATOMIC_RELAXED);
                http_error(fd, s->enable_cors, 503, "stream registry capacity reached"); return true;
            }
            atomic_stream *tombstone = calloc(1, sizeof(*tombstone));
            tombstone->srv = s; memcpy(tombstone->id, id, sizeof(id));
            tombstone->cancelled = 1; tombstone->done = 1; tombstone->finished = time(NULL);
            for (;;) {
                tombstone->next = __atomic_load_n(&atomic_streams, __ATOMIC_ACQUIRE);
                r = atomic_stream_find(id);
                if (r) { free(tombstone); __atomic_fetch_sub(&atomic_stream_count, 1, __ATOMIC_RELAXED); break; }
                if (__atomic_compare_exchange_n(&atomic_streams, &tombstone->next, tombstone, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                    r = tombstone; break;
                }
            }
        }
        __atomic_store_n(&r->cancelled, 1, __ATOMIC_RELEASE);
        http_response(fd, s->enable_cors, 204, NULL, ""); return true;
    }
    if (!strcmp(hr->method, "POST") && !*suffix) {
        if (!r) {
            if (__atomic_fetch_add(&atomic_stream_count, 1, __ATOMIC_RELAXED) >= 4096) {
                __atomic_fetch_sub(&atomic_stream_count, 1, __ATOMIC_RELAXED);
                http_error(fd, s->enable_cors, 503, "stream registry capacity reached"); return true;
            }
            size_t reserved = __atomic_fetch_add(&atomic_stream_bytes, hr->body_len, __ATOMIC_RELAXED);
            if (reserved + hr->body_len > ATOMIC_STREAM_TOTAL) {
                __atomic_fetch_sub(&atomic_stream_bytes, hr->body_len, __ATOMIC_RELAXED);
                __atomic_fetch_sub(&atomic_stream_count, 1, __ATOMIC_RELAXED);
                http_error(fd, s->enable_cors, 503, "stream retention capacity reached"); return true;
            }
            atomic_stream *candidate = calloc(1, sizeof(*candidate));
            candidate->srv = s; memcpy(candidate->id, id, sizeof(id));
            candidate->body = xstrndup(hr->body, hr->body_len); candidate->body_len = hr->body_len;
            strcpy(candidate->path, "/tmp/atomic-sse-XXXXXX");
            int file = mkstemp(candidate->path);
            if (file < 0) { free(candidate->body); free(candidate); __atomic_fetch_sub(&atomic_stream_bytes, hr->body_len, __ATOMIC_RELAXED); __atomic_fetch_sub(&atomic_stream_count, 1, __ATOMIC_RELAXED); http_error(fd, s->enable_cors, 503, "stream storage unavailable"); return true; }
            close(file);
            snprintf(candidate->request_path, sizeof(candidate->request_path), "%s.request", candidate->path);
            file = open(candidate->request_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
            bool saved = file >= 0;
            size_t at = 0;
            while (saved && at < candidate->body_len) {
                ssize_t n = write(file, candidate->body + at, candidate->body_len - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) saved = false; else at += (size_t)n;
            }
            if (file >= 0) close(file);
            if (!saved) {
                unlink(candidate->path); unlink(candidate->request_path);
                free(candidate->body); free(candidate); __atomic_fetch_sub(&atomic_stream_bytes, hr->body_len, __ATOMIC_RELAXED);
                __atomic_fetch_sub(&atomic_stream_count, 1, __ATOMIC_RELAXED);
                http_error(fd, s->enable_cors, 503, "request storage unavailable"); return true;
            }
            /* A failed CAS must rescan before publishing: simultaneous POSTs
             * with the same ID must never start two inference jobs. */
            for (;;) {
                candidate->next = __atomic_load_n(&atomic_streams, __ATOMIC_ACQUIRE);
                r = atomic_stream_find(id);
                if (r) { unlink(candidate->path); unlink(candidate->request_path); free(candidate->body); free(candidate); __atomic_fetch_sub(&atomic_stream_bytes, hr->body_len, __ATOMIC_RELAXED); __atomic_fetch_sub(&atomic_stream_count, 1, __ATOMIC_RELAXED); break; }
                if (__atomic_compare_exchange_n(&atomic_streams, &candidate->next, candidate, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                    r = candidate;
                    pthread_t th;
                    pthread_mutex_lock(&s->mu); s->clients++; pthread_mutex_unlock(&s->mu);
                    if (pthread_create(&th, NULL, atomic_stream_capture, r) != 0) {
                        r->failed = 1; r->finished = time(NULL); __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE); client_done(s);
                    } else pthread_detach(th);
                    break;
                }
            }
        }
        if (__atomic_load_n(&r->expired, __ATOMIC_ACQUIRE)) {
            http_error(fd, s->enable_cors, 410, "stream expired"); return true;
        }
        int request_file = open(r->request_path, O_RDONLY);
        bool same = request_file >= 0 && r->body_len == hr->body_len;
        size_t at = 0;
        while (same && at < hr->body_len) {
            char data[8192]; ssize_t n = read(request_file, data, sizeof(data));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || (size_t)n > hr->body_len - at || memcmp(data, hr->body + at, (size_t)n)) same = false;
            else at += (size_t)n;
        }
        if (request_file >= 0) close(request_file);
        if (!same) {
            http_error(fd, s->enable_cors, 409, "stream ID already belongs to a different request"); return true;
        }
        atomic_stream_tail(s, fd, r, 0); return true;
    }
    if (!strcmp(hr->method, "GET") && suffix[0] == '/' && suffix[1]) {
        char *end; errno = 0; unsigned long long cursor = strtoull(suffix + 1, &end, 10);
        if (errno || *end || cursor > SIZE_MAX) http_error(fd, s->enable_cors, 400, "invalid stream cursor");
        else if (!r) http_error(fd, s->enable_cors, 410, "stream no longer exists after server restart");
        else atomic_stream_tail(s, fd, r, (size_t)cursor);
        return true;
    }
    http_error(fd, s->enable_cors, 404, "unknown stream endpoint"); return true;
}

/* Called only after every external and capture client has drained. Temporary
 * stream data is separate from model/KV caches and is never persisted as chat. */
static void atomic_stream_cleanup(void) {
    atomic_stream *r = atomic_streams;
    while (r) {
        atomic_stream *next = r->next;
        unlink(r->path); unlink(r->request_path);
        free(r->body); free(r); r = next;
    }
    atomic_streams = NULL; atomic_stream_bytes = 0; atomic_stream_count = 0;
}
