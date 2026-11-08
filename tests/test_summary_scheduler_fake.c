/* Model-free integration: exercise the real slot/decode scheduler with a
 * deterministic backend, never open a GGUF or submit GPU work. */
#define DS4_SERVER_TEST
#define DS4_SERVER_TEST_NO_MAIN
#define ds4_sessions_eval_batch fake_eval_batch
#define ds4_sessions_eval_batch_speculative_argmax fake_eval_speculative
#define ds4_session_invalidate fake_invalidate
#include "../ds4_server.c"

typedef struct { int cursor; int invalidated; } fake_session;
static int compute_owners;
static int compute_overlap;

static void fake_enter(void) {
    if (__sync_add_and_fetch(&compute_owners, 1) != 1) compute_overlap++;
    usleep(100);
}
static void fake_leave(void) { __sync_sub_and_fetch(&compute_owners, 1); }

int fake_eval_batch(ds4_decode_item *items, int count, char *err, size_t errlen) {
    (void)err; (void)errlen;
    fake_enter();
    TEST_ASSERT(count == 1); /* Dedicated lanes must never share a batch. */
    for (int i = 0; i < count; i++) {
        fake_session *state = (fake_session *)items[i].session;
        TEST_ASSERT(items[i].token == state->cursor);
        state->cursor++;
    }
    fake_leave();
    return 0;
}
int fake_eval_speculative(ds4_decode_item *items, int count, int (*accepted)[2],
                          int *n_accepted, char *err, size_t errlen) {
    int rc = fake_eval_batch(items, count, err, errlen);
    for (int i = 0; i < count; i++) { accepted[i][0] = items[i].token; n_accepted[i] = 1; }
    return rc;
}
void fake_invalidate(ds4_session *session) { ((fake_session *)session)->invalidated++; }

typedef struct { server *s; server_slot *slot; bool decode; bool speculative; int turns; int done; } fake_lane;
static void *fake_lane_main(void *arg) {
    fake_lane *lane = arg;
    fake_session *state = (fake_session *)lane->slot->session;
    for (int i = 0; i < lane->turns; i++) {
        if (lane->decode) {
            char err[160]; int accepted[2], count;
            int token = state->cursor;
            int rc = server_eval_tokens(lane->s, lane->slot, token, lane->speculative,
                                       accepted, &count, err, sizeof(err));
            TEST_ASSERT(rc == 0 && count == 1 && accepted[0] == token);
            if (rc) break;
        } else {
            if (!server_prefill_enter(lane->s, lane->slot)) break;
            fake_enter();
            state->cursor++;
            fake_leave();
            server_prefill_leave(lane->s);
        }
        lane->done++;
    }
    return NULL;
}

static void check_pair(bool foreground_decode, bool summary_decode, bool speculative) {
    fake_session states[2] = {{0}};
    server_slot slots[2] = {{.id = 0, .session = (ds4_session *)&states[0]},
                            {.id = 1, .session = (ds4_session *)&states[1]}};
    server s = {.slots = slots, .slot_count = 2, .dedicated_summary_slot = true, .batched_mode = true};
    pthread_mutex_init(&s.model_mu, NULL);
    pthread_mutex_init(&s.inference_mu, NULL);
    pthread_cond_init(&s.model_cv, NULL);
    pthread_t decoder, workers[2];
    TEST_ASSERT(pthread_create(&decoder, NULL, decode_worker_main, &s) == 0);
    fake_lane lanes[2] = {{&s, &slots[0], foreground_decode, speculative, 160, 0},
                          {&s, &slots[1], summary_decode, !speculative, 40, 0}};
    for (int i = 0; i < 2; i++) TEST_ASSERT(pthread_create(&workers[i], NULL, fake_lane_main, &lanes[i]) == 0);
    for (int i = 0; i < 2; i++) pthread_join(workers[i], NULL);
    pthread_mutex_lock(&s.model_mu);
    s.model_stopping = true;
    pthread_cond_broadcast(&s.model_cv);
    pthread_mutex_unlock(&s.model_mu);
    pthread_join(decoder, NULL);
    TEST_ASSERT(lanes[0].done == 160 && lanes[1].done == 40);
    TEST_ASSERT(states[0].cursor == 160 && states[1].cursor == 40);
    TEST_ASSERT(!states[0].invalidated && !states[1].invalidated);
    TEST_ASSERT(!s.model_busy && s.decode_pending == 0 && compute_owners == 0 && compute_overlap == 0);
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT(slots[i].session == (ds4_session *)&states[i]);
        TEST_ASSERT(!slots[i].decode_in_flight && !slots[i].decode_pending && !slots[i].prefill_waiting);
    }
    pthread_cond_destroy(&s.model_cv);
    pthread_mutex_destroy(&s.inference_mu);
    pthread_mutex_destroy(&s.model_mu);
}
int main(void) {
    for (int foreground_decode = 0; foreground_decode < 2; foreground_decode++)
        for (int summary_decode = 0; summary_decode < 2; summary_decode++)
            for (int speculative = 0; speculative < 2; speculative++)
                check_pair(foreground_decode, summary_decode, speculative);
    if (test_failures) return 1;
    puts("summary scheduler fake backend: 8 mixed-lane scenarios passed, sessions preserved, no compute overlap");
    return 0;
}
