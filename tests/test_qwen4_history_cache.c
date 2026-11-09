/* Diagnostic retained-history test. This checks structural invariants and a
 * latest-correction answer, not equivalence to exact attention recomputation.
 * Build against the normal core objects; use DS4_TEST_MODEL and the normal
 * DS4_QWEN_NGRAM_GGUF environment. Never run beside another huge model. */
#include "ds4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed: %s at %d\n", #x, __LINE__); return 1; } } while (0)

int main(void) {
    const char *path = getenv("DS4_TEST_MODEL");
    REQUIRE(path);
    ds4_engine_options opt = {.model_path=path, .backend=DS4_BACKEND_METAL,
        .context_size=8192, .prefill_chunk=512, .n_threads=1};
    ds4_engine *e = NULL;
    REQUIRE(ds4_engine_open(&e, &opt) == 0);
    ds4_session *s = NULL;
    REQUIRE(ds4_session_create(&s, e, 8192) == 0);
    char question[10000] = "Old marker: COBALT.\n";
    for (int i=0; i<60; i++) strcat(question, "Old filler: apples are fruit and rivers flow downhill.\n");
    strcat(question, "Latest correction: marker AMBER. Answer only the current marker.");
    ds4_tokens p = {0};
    ds4_encode_chat_prompt(e, "Follow the latest correction.", question, DS4_THINK_NONE, &p);
    REQUIRE(p.len > 200);
    char err[256] = "";
    REQUIRE(ds4_session_sync(s, &p, err, sizeof(err)) == 0);
    const int original = ds4_session_pos(s);
    REQUIRE(!ds4_session_history_compact(s, 4, 4, original));
    REQUIRE(!ds4_session_history_compact(s, 4, original, original));
    REQUIRE(ds4_session_pos(s) == original);
    for (int round=0; round<2; round++) {
        const int n = ds4_session_pos(s), first=29, end=round ? n-90 : n/2+1;
        REQUIRE(ds4_session_history_compact(s, first, end, n));
        REQUIRE(ds4_session_history_approximate(s));
        REQUIRE(ds4_session_pos(s) == n-end+first);
        const ds4_tokens *kept = ds4_session_tokens(s);
        REQUIRE(kept && kept->len == ds4_session_pos(s));
    }
    FILE *fp = tmpfile();
    REQUIRE(fp);
    REQUIRE(ds4_session_save_payload(s, fp, err, sizeof(err)) == 0);
    uint32_t header[2]; rewind(fp);
    REQUIRE(fread(header, sizeof(uint32_t), 2, fp) == 2);
    /* Never label an approximate cache as exact. Version three preserves its
     * recurrent provenance and absolute positions when cloned or restarted. */
    REQUIRE(header[0] == DS4_SESSION_PAYLOAD_MAGIC && header[1] == DS4_SESSION_HISTORY_PAYLOAD_VERSION);
    rewind(fp);
    ds4_session *restored = NULL;
    REQUIRE(ds4_session_create(&restored, e, 16384) == 0);
    REQUIRE(ds4_session_load_payload(restored, fp, ds4_session_payload_bytes(s), err, sizeof(err)) == 0);
    REQUIRE(ds4_session_history_approximate(restored));
    REQUIRE(ds4_session_clone_state(restored, s, err, sizeof(err)) == 0);
    REQUIRE(ds4_session_history_approximate(restored));
    const ds4_tokens *original_tokens = ds4_session_tokens(s), *restored_tokens = ds4_session_tokens(restored);
    REQUIRE(original_tokens->len == restored_tokens->len);
    REQUIRE(!memcmp(original_tokens->v, restored_tokens->v, (size_t)original_tokens->len * sizeof(int)));
    ds4_session_free(restored); fclose(fp);
    ds4_tokens extra = {0}, follow = {0};
    ds4_tokenize_text(e, " The current marker is", &extra);
    REQUIRE(extra.len > 0);
    const ds4_tokens *kept = ds4_session_tokens(s);
    follow.len = kept->len + extra.len;
    follow.cap = follow.len;
    follow.v = malloc((size_t)follow.len * sizeof(int));
    REQUIRE(follow.v);
    memcpy(follow.v, kept->v, (size_t)kept->len * sizeof(int));
    memcpy(follow.v+kept->len, extra.v, (size_t)extra.len * sizeof(int));
    REQUIRE(ds4_session_sync(s, &follow, err, sizeof(err)) == 0);
    REQUIRE(ds4_session_history_approximate(s));
    char answer[512] = "";
    for (int i=0; i<24; i++) {
        const int token = ds4_session_argmax(s);
        if (token == ds4_token_eos(e)) break;
        size_t len=0;
        char *piece = ds4_token_text(e, token, &len);
        REQUIRE(piece && strlen(answer)+len < sizeof(answer));
        strcat(answer, piece);
        free(piece);
        REQUIRE(ds4_session_eval(s, token, err, sizeof(err)) == 0);
    }
    REQUIRE(strstr(answer, "AMBER") && !strstr(answer, "COBALT"));
    printf("history-cache: original=%d retained=%d answer=%s\n", original, follow.len-extra.len, answer);
    ds4_session_invalidate(s);
    REQUIRE(!ds4_session_history_approximate(s));
    REQUIRE(ds4_session_sync(s, &p, err, sizeof(err)) == 0);
    REQUIRE(!ds4_session_history_approximate(s));
    ds4_tokens_free(&p); ds4_tokens_free(&extra); ds4_tokens_free(&follow);
    ds4_session_free(s); ds4_engine_close(e);
    puts("history-cache: ok");
    return 0;
}
