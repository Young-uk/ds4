/* Model-free integration of the actual clone/receipt routes and scheduler
 * entry. Only tensor/session primitives are substituted; no GGUF is opened. */
#define DS4_SERVER_TEST
#define DS4_SERVER_TEST_NO_MAIN
#define ds4_token_eos fake_eos
#define ds4_tokenize_rendered_chat fake_tokenize
#define ds4_session_clone_state fake_clone
#define ds4_session_tokens fake_tokens
#define ds4_session_pos fake_pos
#define ds4_session_ctx fake_ctx
#define ds4_session_checkpoint_valid fake_valid
#define ds4_session_has_vision_state fake_vision
#define ds4_session_payload_bytes fake_bytes
#define ds4_session_save_snapshot fake_save
#define ds4_session_load_snapshot fake_load
#define ds4_session_load_payload fake_load_payload
#define ds4_session_invalidate fake_invalidate
#define ds4_engine_model_id fake_model
#define ds4_engine_routed_quant_bits fake_quant
#include "../ds4_server.c"

typedef struct { int values[2048]; int count, capacity, hidden, valid, vision; } fake_state;
static ds4_tokens view;
static int save_calls, load_calls, fail_load;
static uint64_t reported_bytes;
static job *cancel_on_load;
int fake_eos(ds4_engine *engine) { (void)engine; return 248046; }
void fake_tokenize(ds4_engine *engine, const char *text, ds4_tokens *tokens) {
    (void)engine;
    if (!strcmp(text, "<|image_pad|>")) ds4_tokens_push(tokens, 99);
    else { TEST_ASSERT(!strcmp(text, "\n")); ds4_tokens_push(tokens, 10); }
}

const ds4_tokens *fake_tokens(ds4_session *session) {
    const fake_state *state = (const fake_state *)session;
    view = (ds4_tokens){ .v = (int *)state->values, .len = state->count, .cap = 2048 };
    return &view;
}
int fake_pos(ds4_session *s) { return ((const fake_state *)s)->count; }
int fake_ctx(ds4_session *s) { return ((const fake_state *)s)->capacity; }
bool fake_valid(const ds4_session *s) { return ((const fake_state *)s)->valid; }
bool fake_vision(const ds4_session *s) { return ((const fake_state *)s)->vision; }
uint64_t fake_bytes(ds4_session *s) { (void)s; return reported_bytes ? reported_bytes : sizeof(fake_state); }
int fake_model(ds4_engine *e) { (void)e; return 0; }
int fake_quant(ds4_engine *e) { (void)e; return 4; }
void fake_invalidate(ds4_session *s) { fake_state *state = (fake_state *)s; state->valid = 0; state->count = 0; }
int fake_save(ds4_session *s, ds4_session_snapshot *snapshot, char *err, size_t size) {
    (void)err; (void)size; save_calls++;
    snapshot->ptr = malloc(sizeof(fake_state)); snapshot->len = snapshot->cap = sizeof(fake_state);
    memcpy(snapshot->ptr, s, sizeof(fake_state)); return 0;
}
int fake_load(ds4_session *s, const ds4_session_snapshot *snapshot, char *err, size_t size) {
    (void)err; (void)size; load_calls++;
    int capacity = ((fake_state *)s)->capacity;
    if (fail_load) return 1;
    memcpy(s, snapshot->ptr, sizeof(fake_state)); ((fake_state *)s)->capacity = capacity;
    if (cancel_on_load) job_mark_cancelled(cancel_on_load);
    return 0;
}
int fake_clone(ds4_session *destination, ds4_session *source, char *err, size_t size) {
    ds4_session_snapshot snapshot = {0};
    int rc = fake_save(source, &snapshot, err, size);
    if (!rc) rc = fake_load(destination, &snapshot, err, size);
    ds4_session_snapshot_free(&snapshot); return rc;
}

int fake_load_payload(ds4_session *s, FILE *fp, uint64_t bytes, char *err, size_t size) {
    fake_state state;
    if (bytes != sizeof(state) || fread(&state, 1, sizeof(state), fp) != sizeof(state)) return 1;
    ds4_session_snapshot snapshot = { .ptr = (uint8_t *)&state, .len = sizeof(state) };
    return fake_load(s, &snapshot, err, size);
}

static void setup(server *s, server_slot slots[2], fake_state states[2], job *j) {
    memset(s, 0, sizeof(*s)); memset(slots, 0, 2 * sizeof(*slots));
    memset(states, 0, 2 * sizeof(*states)); memset(j, 0, sizeof(*j));
    s->slots = slots; s->slot_count = 2; s->dedicated_summary_slot = true;
    s->atomic_clone_enabled = true; s->ctx_size = 8192;
    strcpy(s->atomic_model_fingerprint, "0123456789012345678901234567890123456789");
    pthread_mutex_init(&s->inference_mu, NULL); pthread_mutex_init(&s->model_mu, NULL);
    pthread_mutex_init(&s->tool_mu, NULL); pthread_mutex_init(&s->kv_mu, NULL);
    pthread_mutex_init(&j->mu, NULL);
    for (int i = 0; i < 2; i++) { slots[i].id = i; slots[i].session = (ds4_session *)&states[i]; states[i].capacity = 16384; }
    states[0].count = 1024; states[0].valid = 1; states[0].hidden = 987;
    for (int i = 0; i < 1024; i++) states[0].values[i] = 1000 + i;
    slots[0].atomic_cache_owner = xstrdup("thread/u0"); slots[0].atomic_cache_tokens = states[0].count;
    slots[0].atomic_cache_tail = 248046; slots[0].atomic_cache_hash = atomic_tokens_hash(fake_tokens(slots[0].session));
    memset(slots[0].atomic_history_hash, 'a', 64); slots[0].atomic_history_hash[64] = 0;
    j->req.kind = REQ_CHAT; j->req.api = API_OPENAI; j->req.atomic_summary = true;
    j->req.atomic_summary_clone = xstrdup("thread/u0"); j->req.think_mode = DS4_THINK_NONE; j->req.max_tokens = 2048;
    ds4_tokens_push(&j->req.prompt, 300); ds4_tokens_push(&j->req.prompt, 301);
    slots[1].running = j;
    save_calls = load_calls = fail_load = 0; reported_bytes = 0; cancel_on_load = NULL;
}
static void cleanup(server *s, job *j) {
    for (int i = 0; i < 2; i++) { free(s->slots[i].atomic_cache_owner); free(s->slots[i].live_text); free(s->slots[i].anchor_text); }
    request_free(&j->req); pthread_mutex_destroy(&j->mu);
    pthread_mutex_destroy(&s->inference_mu); pthread_mutex_destroy(&s->model_mu);
    pthread_mutex_destroy(&s->tool_mu); pthread_mutex_destroy(&s->kv_mu);
}
static void test_independent_frontiers(void) {
    server s; server_slot slots[2]; fake_state states[2]; job j; setup(&s, slots, states, &j);
    fake_state before = states[0]; char err[160] = "";
    TEST_ASSERT(atomic_clone_prepare(&s, &slots[1], &j, err, sizeof(err)));
    TEST_ASSERT(save_calls == 1 && load_calls == 1 && !memcmp(&before, &states[0], sizeof(before)));
    TEST_ASSERT(j.req.prompt.len == 1028 && j.req.prompt.v[1024] == 248046 && j.req.prompt.v[1025] == 10 && j.req.prompt.v[1026] == 300);
    TEST_ASSERT(states[1].hidden == 987 && states[1].count == 1024);
    states[0].values[1024] = 700; states[0].count++; states[0].hidden++;
    states[1].values[1024] = 800; states[1].count++; states[1].hidden += 2;
    TEST_ASSERT(states[0].values[1024] == 700 && states[1].values[1024] == 800);
    TEST_ASSERT(states[0].hidden == 988 && states[1].hidden == 989);
    cleanup(&s, &j);
}
static void test_processed_image_clone(void) {
    server s; server_slot slots[2]; fake_state states[2]; job j; setup(&s, slots, states, &j);
    states[0].vision = 1;
    fake_state original = states[0]; char err[160] = "";
    TEST_ASSERT(atomic_clone_prepare(&s, &slots[1], &j, err, sizeof(err)));
    TEST_ASSERT(states[1].hidden == original.hidden && j.req.prompt.len > original.count);
    TEST_ASSERT(!memcmp(&original, &states[0], sizeof(original)));
    cleanup(&s, &j);
}
static void test_rejected_or_cancelled_copy(void) {
    for (int mode = 0; mode < 6; mode++) {
        server s; server_slot slots[2]; fake_state states[2]; job j; setup(&s, slots, states, &j);
        fake_state before = states[0]; char err[160] = "";
        if (mode == 0) { free(j.req.atomic_summary_clone); j.req.atomic_summary_clone = xstrdup("other/turn"); }
        if (mode == 1) slots[0].atomic_cache_hash++;
        if (mode == 2) states[1].capacity = 1025;
        if (mode == 3) fail_load = 1;
        if (mode == 4) job_mark_cancelled(&j);
        if (mode == 5) cancel_on_load = &j;
        before = states[0];
        TEST_ASSERT(!atomic_clone_prepare(&s, &slots[1], &j, err, sizeof(err)));
        TEST_ASSERT(!memcmp(&before, &states[0], sizeof(before)) && j.req.prompt.len == 2);
        if (mode < 3 || mode == 4) TEST_ASSERT(save_calls == 0);
        cleanup(&s, &j);
    }
}
static void test_disk_receipt_restart(bool vision) {
    server s; server_slot slots[2]; fake_state states[2]; job j; setup(&s, slots, states, &j);
    states[0].vision = vision;
    char directory[] = "/tmp/dstar-clone-test.XXXXXX"; TEST_ASSERT(mkdtemp(directory) != NULL);
    s.kv.enabled = true; s.kv.dir = directory;
    const char *text = "canonical checkpoint"; char sha[41]; ds4_kvstore_sha1_bytes_hex(text, strlen(text), sha);
    char *path = ds4_kvstore_path_for_sha(&s.kv, sha); FILE *fp = fopen(path, "wb"); TEST_ASSERT(fp);
    uint8_t cache_header[DS4_KVSTORE_FIXED_HEADER];
    ds4_kvstore_fill_header(cache_header, 0, 4, DS4_KVSTORE_REASON_SHUTDOWN, vision ? DS4_KVSTORE_EXT_VISION : 0, 1024, 0, 8192, 1, 1, sizeof(fake_state));
    fwrite(cache_header, 1, sizeof(cache_header), fp);
    uint8_t n[4]; ds4_kvstore_le_put32(n, (uint32_t)strlen(text));
    fwrite(n, 1, 4, fp); fwrite(text, 1, strlen(text), fp); fwrite(&states[0], 1, sizeof(states[0]), fp); fclose(fp);
    atomic_cache_persist(&s, &slots[0], fake_tokens(slots[0].session), text);
    atomic_receipt receipt; TEST_ASSERT(atomic_receipt_read(&s, &receipt));
    ds4_kvstore_entry header; fp = atomic_receipt_open(&s, &receipt, &header); TEST_ASSERT(fp); if (fp) fclose(fp);
    atomic_receipt_free(&receipt);
    atomic_cache_clear(&slots[0]); states[0].count = 0; states[0].valid = 0;
    char err[160] = "";
    TEST_ASSERT(atomic_clone_prepare(&s, &slots[1], &j, err, sizeof(err)));
    TEST_ASSERT(states[0].count == 0 && !states[0].valid && states[1].count == 1024);
    TEST_ASSERT(save_calls == 0 && j.req.prompt.len == 1028);
    /* Replacing even an equal-named file invalidates its old attestation. */
    fp = fopen(path, "ab"); fputc(0, fp); fclose(fp);
    TEST_ASSERT(atomic_receipt_read(&s, &receipt));
    TEST_ASSERT(!atomic_receipt_open(&s, &receipt, &header)); atomic_receipt_free(&receipt);
    char *binding = atomic_receipt_path(&s); unlink(binding); free(binding); unlink(path); free(path); rmdir(directory);
    cleanup(&s, &j);
}
static void test_large_clone_uses_destination_capacity_not_a_four_gib_ceiling(void) {
    server s; server_slot slots[2]; fake_state states[2]; job j; setup(&s, slots, states, &j);
    reported_bytes = UINT64_C(5) * 1024 * 1024 * 1024;
    char err[160] = "";
    TEST_ASSERT(atomic_clone_prepare(&s, &slots[1], &j, err, sizeof(err)));
    TEST_ASSERT(save_calls == 1 && load_calls == 1);
    cleanup(&s, &j);
}
static char *write_checkpoint(server *s, const fake_state *state, const char *text) {
    char sha[41]; ds4_kvstore_sha1_bytes_hex(text, strlen(text), sha);
    char *path = ds4_kvstore_path_for_sha(&s->kv, sha);
    FILE *fp = fopen(path, "wb"); TEST_ASSERT(fp);
    uint8_t h[DS4_KVSTORE_FIXED_HEADER], n[4];
    ds4_kvstore_fill_header(h, 0, 4, DS4_KVSTORE_REASON_CONTINUED, 0,
        state->count, 0, 8192, 1, 1, sizeof(*state));
    ds4_kvstore_le_put32(n, (uint32_t)strlen(text));
    fwrite(h, 1, sizeof(h), fp); fwrite(n, 1, 4, fp);
    fwrite(text, 1, strlen(text), fp); fwrite(state, 1, sizeof(*state), fp); fclose(fp);
    return path;
}
static void test_earlier_checkpoint(bool restart, int mode) {
    server s; server_slot slots[2]; fake_state states[2]; job j; setup(&s, slots, states, &j);
    char directory[] = "/tmp/dstar-checkpoint-test.XXXXXX"; TEST_ASSERT(mkdtemp(directory));
    s.kv.enabled = true; s.kv.dir = directory; s.kv.opt = ds4_kvstore_default_options();
    states[1].capacity = 8192; j.req.max_tokens = 7500;
    fake_state original = states[0], checkpoint = original;
    checkpoint.count = 512; checkpoint.hidden = 2222;
    if (mode == 2) checkpoint.values[20]++;
    if (mode == 4) {
        for (int i = 100; i < 150; ++i) states[0].values[i] = checkpoint.values[i] = 99;
        slots[0].atomic_cache_hash = atomic_tokens_hash(fake_tokens(slots[0].session));
        original = states[0];
    }
    char *old = write_checkpoint(&s, &checkpoint, "earlier exact source prefix");
    const int end = mode == 1 ? 700 : 400;
    for (int i = 0; i < end; ++i) {
        if (mode == 4 && i > 100 && i < 150) continue;
        ds4_tokens_push(&j.req.atomic_source_prefix, states[0].values[i]);
    }
    if (mode == 4) j.req.atomic_source_history_hash = xstrdup(slots[0].atomic_history_hash);
    char *full = NULL, *receipt = NULL;
    if (restart) {
        full = write_checkpoint(&s, &states[0], "canonical full checkpoint");
        atomic_cache_persist(&s, &slots[0], fake_tokens(slots[0].session), "canonical full checkpoint");
        receipt = atomic_receipt_path(&s); atomic_cache_clear(&slots[0]);
    }
    if (mode == 3) cancel_on_load = &j;
    char err[160] = "";
    bool ok = atomic_clone_prepare(&s, &slots[1], &j, err, sizeof(err));
    TEST_ASSERT(ok == (mode == 0 || mode == 4));
    TEST_ASSERT(!memcmp(&original, &states[0], sizeof(original)));
    if (ok) {
        TEST_ASSERT(states[1].count == 512 && states[1].hidden == 2222);
        TEST_ASSERT(j.req.prompt.len == 516 && j.req.prompt.v[512] == original.values[512]);
    }
    for (int i = 0; i < s.kv.len; ++i) ds4_kvstore_entry_free(&s.kv.entry[i]);
    free(s.kv.entry); unlink(old); free(old);
    if (full) { unlink(full); free(full); }
    if (receipt) { unlink(receipt); free(receipt); }
    rmdir(directory); cleanup(&s, &j);
}
int main(void) {
    for (int mode = 0; mode < 5; ++mode) test_earlier_checkpoint(false, mode);
    test_earlier_checkpoint(true, 0);
    test_processed_image_clone();
    test_large_clone_uses_destination_capacity_not_a_four_gib_ceiling();
    test_independent_frontiers(); test_rejected_or_cancelled_copy(); test_disk_receipt_restart(false); test_disk_receipt_restart(true);
    if (test_failures) return 1;
    puts("cached summary fake backend: independent frontiers, six rejection paths and verified disk restart passed"); return 0;
}
