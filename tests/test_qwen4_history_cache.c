/* Diagnostic retained-history test. This checks structural invariants and a
 * latest-correction answer, not equivalence to exact attention recomputation.
 * Build against the normal core objects; use DS4_TEST_MODEL and the normal
 * DS4_QWEN_NGRAM_GGUF environment. Never run beside another huge model. */
#include "ds4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "failed: %s at %d\n", #x, __LINE__); return 1; } } while (0)

static int visual_restart(ds4_engine *e) {
    char err[256] = "";
    ds4_vision_embedding embedding = {0};
    const int encoded=ds4_engine_vision_encode_file(e, getenv("DS4_TEST_IMAGE"), &embedding, err, sizeof(err));
    if(!encoded) fprintf(stderr,"vision: %s\n",err);
    REQUIRE(encoded == 1);
    ds4_session *session = NULL;
    REQUIRE(ds4_session_create(&session, e, 4096) == 0);
    ds4_tokens source = {0}, suffix = {0};
    char text[12000] = "Synthetic image history. ";
    for (int i=0;i<100;i++) strcat(text,"Stable text before the recent picture. ");
    ds4_encode_chat_prompt(e,"Inspect the picture.",text,DS4_THINK_NONE,&source);
    ds4_vision_span span = {0};
    REQUIRE(ds4_prompt_append_vision(e,&source,&span,&embedding,err,sizeof(err)) == 1);
    ds4_tokenize_text(e," Describe the picture briefly.",&suffix);
    for(int j=0;j<suffix.len;j++) ds4_tokens_push(&source,suffix.v[j]);
    REQUIRE(ds4_session_sync_multimodal(session,&source,&span,1,err,sizeof(err)) == 0);
    const int keep_end=(int)span.token_start-1, first=29, end=200;
    REQUIRE(keep_end>end+512);
    REQUIRE(ds4_session_history_compact(session,first,end,keep_end));
    const int cached=ds4_session_pos(session);
    ds4_tokens follow={0};ds4_tokens_copy(&follow,ds4_session_tokens(session));
    for(int j=keep_end;j<source.len;j++) ds4_tokens_push(&follow,source.v[j]);
    span.token_start-=keep_end-cached;
    REQUIRE(ds4_session_sync_multimodal(session,&follow,&span,1,err,sizeof(err)) == 0);
    REQUIRE(ds4_session_history_approximate(session));
    REQUIRE(ds4_session_vision_state_matches(session,&span,1));
    const int vocab=ds4_engine_vocab_size(e);
    float *before=malloc((size_t)vocab*sizeof(float)), *after=malloc((size_t)vocab*sizeof(float));
    REQUIRE(before&&after);
    for(int round=0;round<2;round++) {
        REQUIRE(ds4_session_copy_logits(session,before,vocab)==vocab);
        FILE *file=tmpfile();REQUIRE(file);
        const uint64_t bytes=ds4_session_payload_bytes(session);
        REQUIRE(ds4_session_save_payload(session,file,err,sizeof(err))==0);
        REQUIRE((uint64_t)ftell(file)==bytes);
        uint32_t header[2];rewind(file);REQUIRE(fread(header,sizeof(header),1,file)==1);
        REQUIRE(header[1]==DS4_SESSION_VISUAL_HISTORY_PAYLOAD_VERSION);
        ds4_session_free(session);session=NULL;
        REQUIRE(ds4_session_create(&session,e,4096)==0);rewind(file);
        REQUIRE(ds4_session_load_payload(session,file,bytes,err,sizeof(err))==0);fclose(file);
        REQUIRE(ds4_session_copy_logits(session,after,vocab)==vocab);
        REQUIRE(!memcmp(before,after,(size_t)vocab*sizeof(float)));
        REQUIRE(ds4_session_history_approximate(session));
        REQUIRE(ds4_session_vision_state_matches(session,&span,1));
        REQUIRE(ds4_session_common_prefix(session,&follow)==follow.len);
        ds4_tokens_push(&follow,ds4_session_argmax(session));
        REQUIRE(ds4_session_sync_multimodal(session,&follow,&span,1,err,sizeof(err))==0);
        REQUIRE(ds4_session_history_approximate(session));
    }
    printf("visual history: reused %d text rows; two v4 restores preserve logits, ownership and continuation\n",cached);
    free(before);free(after);ds4_tokens_free(&source);ds4_tokens_free(&suffix);ds4_tokens_free(&follow);
    ds4_vision_embedding_free(&span.embedding);ds4_session_free(session);return 0;
}

int main(void) {
    const char *path = getenv("DS4_TEST_MODEL");
    REQUIRE(path);
    ds4_engine_options opt = {.model_path=path, .vision_path=getenv("DS4_TEST_VISION"), .backend=DS4_BACKEND_METAL,
        .context_size=8192, .prefill_chunk=512, .n_threads=1};
    ds4_engine *e = NULL;
    REQUIRE(ds4_engine_open(&e, &opt) == 0);
    if (getenv("DS4_TEST_IMAGE")) { int rc=visual_restart(e); ds4_engine_close(e); return rc; }
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
