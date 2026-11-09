/* Personal cached summaries: existing inference_mu serializes the one-time
 * copy; the existing weighted executor owns every subsequent compute quantum.
 * No model weights are copied and foreground protocol/disk state is untouched.
 * Included after the native retained-history matcher, before generate_job. */

static uint64_t atomic_tokens_hash(const ds4_tokens *tokens) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; tokens && i < tokens->len; i++)
        for (unsigned shift = 0; shift < 32; shift += 8)
            hash = (hash ^ (((uint32_t)tokens->v[i] >> shift) & 255u)) * UINT64_C(1099511628211);
    return hash;
}

/* Read one field without accepting duplicate keys or a trailing JSON value.
 * Raw values are owned by the caller; no conversation text enters diagnostics. */
static bool atomic_field(const char *body, const char *name, char **value) {
    const char *p = body ? body : ""; *value = NULL;
    json_ws(&p); if (*p++ != '{') return false;
    for (;;) {
        json_ws(&p); if (*p == '}') { p++; break; }
        char *key = NULL, *raw = NULL;
        if (!json_string(&p, &key)) goto bad;
        json_ws(&p);
        if (*p++ != ':' || !json_raw_value(&p, &raw)) { free(key); free(raw); goto bad; }
        if (!strcmp(key, name)) {
            if (*value) { free(key); free(raw); goto bad; }
            *value = raw; raw = NULL;
        }
        free(key); free(raw); json_ws(&p);
        if (*p == '}') { p++; break; }
        if (*p++ != ',') goto bad;
        json_ws(&p); if (*p == '}') goto bad;
    }
    json_ws(&p); if (*p) goto bad;
    return true;
bad:
    free(*value); *value = NULL; return false;
}

static char *atomic_string(const char *body, const char *name, size_t maximum) {
    char *raw = NULL, *text = NULL;
    if (!atomic_field(body, name, &raw) || !raw) return NULL;
    const char *p = raw;
    bool ok = json_string(&p, &text);
    json_ws(&p); ok = ok && !*p && text && text[0] && strlen(text) <= maximum;
    free(raw);
    if (!ok) { free(text); return NULL; }
    return text;
}

static bool atomic_integer(const char *body, const char *name, int *value) {
    char *raw = NULL;
    if (!atomic_field(body, name, &raw) || !raw) return false;
    const char *p = raw;
    if (*p == '-') p++;
    const char *digits = p;
    while (*p >= '0' && *p <= '9') p++;
    bool lexical = p != digits && !(p - digits > 1 && *digits == '0');
    json_ws(&p); lexical = lexical && !*p;
    p = raw; bool ok = lexical && json_int(&p, value);
    free(raw); return ok;
}

static bool atomic_hex(const char *text, size_t size) {
    if (!text || strlen(text) != size) return false;
    for (size_t i = 0; i < size; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

static void atomic_cache_clear(server_slot *slot) {
    free(slot->atomic_cache_owner); slot->atomic_cache_owner = NULL;
    slot->atomic_history_hash[0] = 0; slot->atomic_cache_tokens = 0;
    slot->atomic_cache_hash = 0; slot->atomic_cache_tail = -1;
    slot->atomic_cache_epoch++;
}

static bool atomic_cache_matches(const server_slot *slot, const char *owner) {
    const ds4_tokens *tokens = ds4_session_tokens(slot->session);
    return owner && slot->atomic_cache_owner && !strcmp(owner, slot->atomic_cache_owner) &&
        tokens && tokens->len > 0 && tokens->len == slot->atomic_cache_tokens &&
        ds4_session_checkpoint_valid(slot->session) && !ds4_session_has_vision_state(slot->session) &&
        atomic_tokens_hash(tokens) == slot->atomic_cache_hash;
}

static void atomic_cache_complete(server *s, server_slot *slot, job *j,
                                  const char *finish, int tail, bool client_stop, int calls) {
    if (!s->atomic_clone_enabled || slot->id != 0 || j->req.atomic_summary ||
        j->req.kind != REQ_CHAT || j->req.api != API_OPENAI || !j->req.atomic_cache_owner ||
        j->req.image_count || calls || client_stop || strcmp(finish, "stop") || tail < 0 ||
        job_cancelled(j)) return;
    pthread_mutex_lock(&s->inference_mu);
    const ds4_tokens *tokens = ds4_session_tokens(slot->session);
    if (tokens && tokens->len > 0 && ds4_session_checkpoint_valid(slot->session) &&
        !ds4_session_has_vision_state(slot->session)) {
        atomic_cache_clear(slot);
        slot->atomic_cache_owner = xstrdup(j->req.atomic_cache_owner);
        slot->atomic_cache_tokens = tokens->len;
        slot->atomic_cache_hash = atomic_tokens_hash(tokens);
        slot->atomic_cache_tail = ds4_token_eos(s->engine);
    }
    pthread_mutex_unlock(&s->inference_mu);
}

static void atomic_file_identity(const struct stat *st, char out[160]);
static void atomic_model_identity(server *s, const char *path) {
    char program[PATH_MAX] = "";
#ifdef __APPLE__
    uint32_t capacity = sizeof(program);
    if (_NSGetExecutablePath(program, &capacity)) program[0] = 0;
#elif defined(__linux__)
    ssize_t size = readlink("/proc/self/exe", program, sizeof(program) - 1);
    if (size > 0) program[size] = 0;
#endif
    buf identity = {0};
    buf_printf(&identity, "dstar-clone-v1:%s:%s:%d:%d:%d:", __DATE__, __TIME__, s->ctx_size,
        ds4_engine_model_id(s->engine), ds4_engine_routed_quant_bits(s->engine));
    const char *paths[] = { path, getenv("DS4_QWEN_NGRAM_GGUF"), getenv("DS4_QWEN_MTP_GGUF"), program };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct stat st = {0}; char stamp[160];
        if (paths[i]) stat(paths[i], &st);
        atomic_file_identity(&st, stamp);
        buf_printf(&identity, "%s:%s:", paths[i] ? paths[i] : "", stamp);
    }
    const char *yarn = getenv("DS4_QWEN4_YARN_FACTOR");
    buf_puts(&identity, yarn ? yarn : "native-rope");
    ds4_kvstore_sha1_bytes_hex(identity.ptr, identity.len, s->atomic_model_fingerprint);
    buf_free(&identity);
}

static void atomic_file_identity(const struct stat *st, char out[160]) {
#ifdef __APPLE__
    long nanoseconds = st->st_mtimespec.tv_nsec;
#else
    long nanoseconds = st->st_mtim.tv_nsec;
#endif
    snprintf(out, 160, "%llu:%llu:%llu:%llu:%lu", (unsigned long long)st->st_dev,
        (unsigned long long)st->st_ino, (unsigned long long)st->st_size,
        (unsigned long long)st->st_mtime, (unsigned long)nanoseconds);
}

typedef struct {
    char *owner, *history, *sha, *identity;
    int tokens, tail;
    uint64_t hash;
} atomic_receipt;

static void atomic_receipt_free(atomic_receipt *receipt) {
    free(receipt->owner); free(receipt->history); free(receipt->sha); free(receipt->identity);
    memset(receipt, 0, sizeof(*receipt));
}

static char *atomic_receipt_path(server *s) {
    return s->kv.enabled && s->kv.dir ? ds4_kvstore_path_join(s->kv.dir, "atomic-summary-receipt.json") : NULL;
}

/* A receipt binds the UI transcript to a specific native file identity and
 * token frontier, never to a filename/text-prefix guess. Legacy caches remain
 * valid ordinary caches without being promoted to clone provenance. */
static bool atomic_receipt_read(server *s, atomic_receipt *receipt) {
    memset(receipt, 0, sizeof(*receipt));
    char *path = atomic_receipt_path(s); if (!path) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW); free(path);
    struct stat st;
    if (fd < 0) return false;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 2 || st.st_size > 4096) { close(fd); return false; }
    char body[4097]; size_t size = (size_t)st.st_size;
    ssize_t got = read(fd, body, size); close(fd);
    if (got != (ssize_t)size) return false;
    body[size] = 0;
    char *fingerprint = atomic_string(body, "fingerprint", 40);
    char *hash = atomic_string(body, "token_hash", 16);
    receipt->owner = atomic_string(body, "owner", 512);
    receipt->history = atomic_string(body, "history_hash", 64);
    receipt->sha = atomic_string(body, "cache_sha", 40);
    receipt->identity = atomic_string(body, "file_identity", 159);
    bool ok = fingerprint && !strcmp(fingerprint, s->atomic_model_fingerprint) &&
        receipt->owner && atomic_hex(receipt->history, 64) && atomic_hex(receipt->sha, 40) &&
        receipt->identity && atomic_hex(hash, 16) && atomic_integer(body, "tokens", &receipt->tokens) &&
        receipt->tokens > 0 && receipt->tokens < s->ctx_size && atomic_integer(body, "tail", &receipt->tail) &&
        receipt->tail == ds4_token_eos(s->engine);
    if (ok) receipt->hash = strtoull(hash, NULL, 16);
    free(fingerprint); free(hash);
    if (!ok) atomic_receipt_free(receipt);
    return ok;
}

/* Keep the file positioned exactly at the native payload for load_payload.
 * Reading its bounded text authenticates the existing disk key, not the model
 * state. Native layout/token checks remain mandatory after the actual restore. */
static FILE *atomic_receipt_open(server *s, const atomic_receipt *receipt, ds4_kvstore_entry *header) {
    char *path = ds4_kvstore_path_for_sha(&s->kv, receipt->sha);
    int fd = open(path, O_RDONLY | O_NOFOLLOW); free(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "rb"); if (!fp) { close(fd); return NULL; }
    struct stat st; char identity[160]; uint32_t bytes = 0;
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1;
    if (ok) { atomic_file_identity(&st, identity); ok = !strcmp(identity, receipt->identity); }
    if (ok) ok = ds4_kvstore_read_header(fp, header, &bytes) && bytes && bytes <= 16 * 1024 * 1024 &&
        header->tokens == (uint32_t)receipt->tokens && header->ctx_size <= (uint32_t)s->ctx_size &&
        header->model_id == ds4_engine_model_id(s->engine) &&
        header->quant_bits == ds4_engine_routed_quant_bits(s->engine) && !(header->ext_flags & DS4_KVSTORE_EXT_VISION);
    char *text = ok ? xmalloc((size_t)bytes + 1) : NULL;
    if (ok) {
        ok = fread(text, 1, bytes, fp) == bytes;
        if (ok) { char sha[41]; ds4_kvstore_sha1_bytes_hex(text, bytes, sha); ok = !strcmp(sha, receipt->sha); }
    }
    free(text);
    if (!ok) { fclose(fp); return NULL; }
    return fp;
}

/* Called by the existing KV writer under inference_mu and kv_mu, only after
 * publishing a full checkpoint. Partial prompt anchors cannot acquire a receipt. */
static void atomic_cache_persist(server *s, server_slot *slot, const ds4_tokens *tokens, const char *override) {
    if (!s->atomic_clone_enabled || slot->id != 0 || !slot->atomic_cache_owner ||
        !atomic_hex(slot->atomic_history_hash, 64) || !atomic_cache_matches(slot, slot->atomic_cache_owner)) return;
    char *text = override ? xstrdup(override) : render_tokens_text(s->engine, tokens, NULL);
    if (!text) return;
    char sha[41]; ds4_kvstore_sha1_bytes_hex(text, strlen(text), sha); free(text);
    char *file = ds4_kvstore_path_for_sha(&s->kv, sha), identity[160];
    struct stat st;
    char thread_id[129];
    const bool thread_owned = cache_thread_id(slot->atomic_cache_owner, thread_id);
    if ((thread_owned && cache_thread_deleted(file, thread_id)) ||
        lstat(file, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) { free(file); return; }
    atomic_file_identity(&st, identity); free(file);
    buf body = {0}; buf_puts(&body, "{\"owner\":"); json_escape(&body, slot->atomic_cache_owner);
    buf_puts(&body, ",\"history_hash\":"); json_escape(&body, slot->atomic_history_hash);
    buf_puts(&body, ",\"cache_sha\":"); json_escape(&body, sha);
    buf_puts(&body, ",\"file_identity\":"); json_escape(&body, identity);
    buf_puts(&body, ",\"fingerprint\":"); json_escape(&body, s->atomic_model_fingerprint);
    buf_printf(&body, ",\"tokens\":%d,\"tail\":%d,\"token_hash\":\"%016llx\"}\n",
        tokens->len, slot->atomic_cache_tail, (unsigned long long)slot->atomic_cache_hash);
    char *path = atomic_receipt_path(s);
    if (path) {
        buf temp = {0}; buf_printf(&temp, "%s.XXXXXX", path);
        int fd = mkstemp(temp.ptr);
        if (fd >= 0) {
            bool ok = fchmod(fd, 0600) == 0 && write(fd, body.ptr, body.len) == (ssize_t)body.len && fsync(fd) == 0;
            if (close(fd)) ok = false;
            if (!ok || rename(temp.ptr, path)) unlink(temp.ptr);
        }
        /* A deletion can land between our payload check and receipt rename. */
        if (thread_owned && cache_thread_deleted(path, thread_id)) unlink(path);
        buf_free(&temp); free(path);
    }
    buf_free(&body);
}

static bool atomic_clone_prepare(server *s, server_slot *slot, job *j, char *err, size_t errlen) {
    request *r = &j->req;
    if (!s->atomic_clone_enabled || slot->id != s->slot_count - 1 || !r->atomic_summary ||
        r->api != API_OPENAI || r->kind != REQ_CHAT || r->image_count || r->has_tools ||
        r->think_mode != DS4_THINK_NONE || r->max_tokens <= 0 || r->max_tokens > 2048) {
        snprintf(err, errlen, "cached summary requires a supported text-only Qwen Metal summary slot"); return false;
    }
    ds4_tokens separator = {0};
    ds4_tokenize_rendered_chat(s->engine, "\n", &separator);
    if (!server_prefill_enter(s, slot)) {
        ds4_tokens_free(&separator);
        snprintf(err, errlen, "cached summary cancelled before copying"); return false;
    }
    server_slot *source = &s->slots[0]; ds4_tokens combined = {0};
    atomic_receipt receipt = {0};
    bool ok = false; int tail = -1;
    if (atomic_cache_matches(source, r->atomic_summary_clone)) {
        uint64_t bytes = ds4_session_payload_bytes(source->session);
        const ds4_tokens *tokens = ds4_session_tokens(source->session);
        if (!bytes || bytes > ATOMIC_CLONE_MAX_BYTES ||
            (int64_t)tokens->len + 1 + separator.len + r->prompt.len + r->max_tokens > ds4_session_ctx(slot->session)) {
            snprintf(err, errlen, "cached summary exceeds snapshot or context headroom");
        } else {
            ds4_tokens_copy(&combined, tokens); tail = source->atomic_cache_tail;
            ok = ds4_session_clone_state(slot->session, source->session, err, errlen) == 0;
        }
    } else if (atomic_receipt_read(s, &receipt) && !strcmp(receipt.owner, r->atomic_summary_clone)) {
        ds4_kvstore_entry header = {0}; FILE *fp = atomic_receipt_open(s, &receipt, &header);
        if (fp && header.payload_bytes <= ATOMIC_CLONE_MAX_BYTES &&
            (int64_t)receipt.tokens + 1 + separator.len + r->prompt.len + r->max_tokens <= ds4_session_ctx(slot->session)) {
            ok = ds4_session_load_payload(slot->session, fp, header.payload_bytes, err, errlen) == 0;
            const ds4_tokens *tokens = ds4_session_tokens(slot->session);
            ok = ok && tokens && tokens->len == receipt.tokens && atomic_tokens_hash(tokens) == receipt.hash;
            if (ok) { ds4_tokens_copy(&combined, tokens); tail = receipt.tail; }
        }
        if (fp) fclose(fp);
    }
    atomic_receipt_free(&receipt);
    if (ok && !job_cancelled(j)) {
        request_live_state_clear(s, slot);
        free(slot->live_text); slot->live_text = NULL; slot->live_text_len = 0; slot->live_text_pos = 0;
        free(slot->anchor_text); slot->anchor_text = NULL; slot->anchor_text_len = 0; slot->anchor_pos = 0;
        /* The decoder stopped before evaluating the assistant terminator.
         * Thinking-off control stops also need the canonical EOS, never a new
         * <think> opener. Complete the native newline-delimited turn first. */
        ds4_tokens_push(&combined, tail);
        for (int i = 0; i < separator.len; i++) ds4_tokens_push(&combined, separator.v[i]);
        for (int i = 0; i < r->prompt.len; i++) ds4_tokens_push(&combined, r->prompt.v[i]);
        ds4_tokens_free(&r->prompt); r->prompt = combined; memset(&combined, 0, sizeof(combined));
        server_log(DS4_LOG_KVCACHE, "[summary-diag] event=clone source_tokens=%d task_tokens=%d", ds4_session_pos(slot->session), r->prompt.len - ds4_session_pos(slot->session));
    } else {
        ok = false; ds4_session_invalidate(slot->session);
        if (!err[0]) snprintf(err, errlen, "completed conversation cache is unavailable; originals retained");
    }
    ds4_tokens_free(&combined); ds4_tokens_free(&separator); server_prefill_leave(s);
    return ok;
}

/* The request parser renders exact Dstar tool/effort events, not a Strata
 * approximation. Dstar rebuilds indexer blocks and accepts arbitrary cuts. */
static bool atomic_history_plan(server *s, int fd, const char *body) {
    char *owner = atomic_string(body, "owner", 512), *candidates = NULL;
    char *tools = NULL, *kwargs = NULL;
    if (!owner || !atomic_field(body, "candidates", &candidates) || !candidates ||
        !atomic_field(body, "tools", &tools) || !atomic_field(body, "chat_template_kwargs", &kwargs)) {
        free(owner); free(candidates); free(tools); free(kwargs);
        return http_error(fd, s->enable_cors, 400, "invalid retained-history planning request");
    }
    ds4_tokens old = {0}; uint64_t epoch;
    pthread_mutex_lock(&s->inference_mu);
    bool available = atomic_cache_matches(&s->slots[0], owner);
    epoch = s->slots[0].atomic_cache_epoch;
    if (available) ds4_tokens_copy(&old, ds4_session_tokens(s->slots[0].session));
    pthread_mutex_unlock(&s->inference_mu);
    const char *p = candidates; json_ws(&p); bool valid = *p++ == '[';
    int selected = 0, previous = INT_MAX, count = 0; history_cache_plan plan = {0};
    while (valid) {
        json_ws(&p); if (*p == ']') { p++; break; }
        char *candidate = NULL, *messages = NULL; int boundary = 0;
        if (++count > 16 || !json_raw_value(&p, &candidate) ||
            !atomic_integer(candidate, "boundary", &boundary) || boundary <= 0 || boundary >= previous ||
            !atomic_field(candidate, "messages", &messages) || !messages) valid = false;
        if (valid && available && !selected) {
            buf request_body = {0}; buf_puts(&request_body, "{\"messages\":"); buf_puts(&request_body, messages);
            if (tools) { buf_puts(&request_body, ",\"tools\":"); buf_puts(&request_body, tools); }
            if (kwargs) { buf_puts(&request_body, ",\"chat_template_kwargs\":"); buf_puts(&request_body, kwargs); }
            buf_putc(&request_body, '}'); request request = {0}; char err[160];
            if (!parse_chat_request(s->engine, s, request_body.ptr, s->default_tokens, s->ctx_size, &request, err, sizeof(err))) valid = false;
            else {
                if (!request.image_count && (int64_t)request.prompt.len + 4096 <= s->ctx_size * 0.8) {
                    plan = history_cache_match(&old, &request.prompt, ds4_token_turn_start(s->engine),
                        prompt_system_run_for_request(s, &request, &request.prompt));
                    if (plan.length) selected = boundary;
                }
                request_free(&request);
            }
            buf_free(&request_body);
        }
        previous = boundary; free(candidate); free(messages);
        if (!valid) break;
        json_ws(&p); if (*p == ']') { p++; break; }
        if (*p++ != ',') { valid = false; break; }
        json_ws(&p); if (*p == ']') valid = false;
    }
    json_ws(&p); valid = valid && !*p && count > 0;
    pthread_mutex_lock(&s->inference_mu);
    bool changed = epoch != s->slots[0].atomic_cache_epoch;
    pthread_mutex_unlock(&s->inference_mu);
    buf result = {0};
    if (valid && available && !changed && selected)
        buf_printf(&result, "{\"boundary\":%d,\"reason\":\"aligned\",\"prefix_tokens\":%d,\"source_tokens\":%d,\"retained_tokens\":%d}", selected, plan.prefix, plan.source, plan.length);
    else buf_printf(&result, "{\"boundary\":null,\"reason\":\"%s\"}", changed ? "cache-changed" : !available ? "cache-unavailable" : "no-budgeted-aligned-turn");
    bool ok = valid ? http_response(fd, s->enable_cors, 200, "application/json", result.ptr)
                    : http_error(fd, s->enable_cors, 400, "invalid retained-history candidates");
    buf_free(&result); free(owner); free(candidates); free(tools); free(kwargs); ds4_tokens_free(&old);
    return ok;
}

static bool atomic_summary_route(server *s, int fd, const char *method, const char *path, const char *body) {
    bool status = !strcmp(method, "GET") && !strcmp(path, "/v1/status");
    bool planning = !strcmp(method, "POST") && !strcmp(path, "/v1/atomic/history-plan");
    bool binding = !strcmp(method, "POST") && !strcmp(path, "/v1/atomic/cache-receipt");
    if (!status && !planning && !binding) return false;
    if (!s->atomic_clone_enabled) { http_error(fd, s->enable_cors, 404, "cached summary capability is unavailable"); return true; }
    if (planning) { atomic_history_plan(s, fd, body); return true; }
    if (binding) {
        char *owner = atomic_string(body, "owner", 512), *hash = atomic_string(body, "history_hash", 64);
        pthread_mutex_lock(&s->inference_mu);
        bool ok = owner && atomic_hex(hash, 64) && atomic_cache_matches(&s->slots[0], owner);
        if (ok) memcpy(s->slots[0].atomic_history_hash, hash, 65);
        pthread_mutex_unlock(&s->inference_mu);
        if (ok) http_response(fd, s->enable_cors, 200, "application/json", "{\"saved\":true}");
        else http_error(fd, s->enable_cors, 409, "completed cache owner does not match this history receipt");
        free(owner); free(hash); return true;
    }
    pthread_mutex_lock(&s->model_mu);
    bool foreground = s->slots[0].running != NULL;
    bool summary = s->slots[s->slot_count - 1].running != NULL;
    pthread_mutex_unlock(&s->model_mu);
    pthread_mutex_lock(&s->inference_mu);
    bool pristine = ds4_session_pos(s->slots[0].session) == 0;
    pthread_mutex_unlock(&s->inference_mu);
    atomic_receipt receipt = {0}; bool restored = false;
    if (!foreground && !summary && pristine && atomic_receipt_read(s, &receipt)) {
        ds4_kvstore_entry header; FILE *fp = atomic_receipt_open(s, &receipt, &header);
        if (fp) { fclose(fp); restored = true; }
    }
    buf result = {0};
    buf_printf(&result, "{\"loaded\":true,\"atomic_summary\":{\"enabled\":true,\"active\":%s,\"pending\":false,\"phase\":\"%s\",\"foreground_requests\":%d,\"rollback_pending\":false,\"recovery_required\":false,\"restored_cache_owner\":", summary ? "true" : "false", summary ? "summarizing" : "idle", foreground ? 1 : 0);
    if (restored) json_escape(&result, receipt.owner); else buf_puts(&result, "null");
    buf_puts(&result, ",\"restored_cache_hash\":");
    if (restored) json_escape(&result, receipt.history); else buf_puts(&result, "null");
    buf_puts(&result, "}}"); http_response(fd, s->enable_cors, 200, "application/json", result.ptr);
    buf_free(&result); atomic_receipt_free(&receipt); return true;
}
