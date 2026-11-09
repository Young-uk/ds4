#include <assert.h>
#include "ds4_cache_threads.h"
int main(void) {
    char directory[]="/tmp/dstar-cache-threads-XXXXXX";assert(mkdtemp(directory));
    char path[512],sidecar[540],markers[540],marker[560],id[129];
    snprintf(path,sizeof(path),"%s/checkpoint.kv",directory);
    FILE *f=fopen(path,"w");assert(f);fputs("synthetic cache",f);fclose(f);
    assert(cache_thread_id("room-a/turn",id)&&!strcmp(id,"room-a"));
    assert(!cache_thread_id("../escape",id));
    assert(cache_threads_record(path,"room-a/turn"));assert(cache_threads_record(path,"room-b/turn"));
    snprintf(sidecar,sizeof(sidecar),"%s.threads",path);snprintf(markers,sizeof(markers),"%s/.deleted-threads",directory);assert(!mkdir(markers,0700));
    snprintf(marker,sizeof(marker),"%s/room-a",markers);f=fopen(marker,"w");assert(f);fclose(f);
    assert(cache_threads_live(path,sidecar));assert(cache_threads_record(path,"room-b/next"));
    snprintf(marker,sizeof(marker),"%s/room-b",markers);f=fopen(marker,"w");assert(f);fclose(f);
    assert(!cache_threads_record(path,"room-b/late-shutdown"));assert(access(path,F_OK));assert(access(sidecar,F_OK));
    unlink(marker);snprintf(marker,sizeof(marker),"%s/room-a",markers);unlink(marker);rmdir(markers);rmdir(directory);
    puts("cache threads: shared owners, traversal rejection, deletion and late-writer suppression passed");return 0;
}
