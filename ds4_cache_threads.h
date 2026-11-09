/* Conversation ownership is a sidecar, never a change to native KV bytes.
 * Callers already serialize KV publication. The app publishes tombstones first;
 * writers check them again after publication to close deletion/shutdown races. */
#ifndef DS4_CACHE_THREADS_H
#define DS4_CACHE_THREADS_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool cache_thread_id(const char *owner, char id[129]) {
    if (!owner) return false;
    size_t n = strcspn(owner, "/");
    if (!n || n > 128) return false;
    for (size_t i=0;i<n;i++) {
        const unsigned char c=(unsigned char)owner[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_')) return false;
    }
    memcpy(id,owner,n);id[n]=0;return true;
}
static bool cache_thread_deleted(const char *path, const char *id) {
    const char *end=strrchr(path,'/'); if (!end) return false;
    char marker[4096];
    int n=snprintf(marker,sizeof(marker),"%.*s/.deleted-threads/%s",(int)(end-path),path,id);
    return n<0 || (size_t)n>=sizeof(marker) || access(marker,F_OK)==0;
}
static bool cache_threads_live(const char *path, const char *sidecar) {
    FILE *f=fopen(sidecar,"r");if(!f)return false;
    char line[132],id[129];bool live=false;
    while(fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;
        if(cache_thread_id(line,id)&&!strcmp(line,id)&&!cache_thread_deleted(path,id))live=true;
    }
    fclose(f);
    if(!live){unlink(path);unlink(sidecar);}
    return live;
}
static bool cache_threads_record(const char *path,const char *owner) {
    char id[129];if(!cache_thread_id(owner,id))return true; /* Non-Chat clients keep their existing format. */
    char sidecar[4096],temporary[4112];
    if(snprintf(sidecar,sizeof(sidecar),"%s.threads",path)>=(int)sizeof(sidecar))return false;
    char owners[131072]={0};size_t used=0;bool present=false;
    FILE *old=fopen(sidecar,"r");char line[132],other[129];
    if(old){while(fgets(line,sizeof(line),old)){
        line[strcspn(line,"\r\n")]=0;
        if(!cache_thread_id(line,other)||strcmp(line,other)||cache_thread_deleted(path,other))continue;
        size_t len=strlen(other);if(used+len+1>=sizeof(owners)){fclose(old);return false;}
        memcpy(owners+used,other,len);used+=len;owners[used++]='\n';
        if(!strcmp(other,id))present=true;
    }fclose(old);}
    if(!present&&!cache_thread_deleted(path,id)){
        size_t len=strlen(id);if(used+len+1>=sizeof(owners))return false;
        memcpy(owners+used,id,len);used+=len;owners[used++]='\n';
    }
    if(!used){unlink(path);unlink(sidecar);return false;}
    snprintf(temporary,sizeof(temporary),"%s.XXXXXX",sidecar);int fd=mkstemp(temporary);
    if(fd<0)return false;
    bool ok=fchmod(fd,0600)==0 && write(fd,owners,used)==(ssize_t)used && fsync(fd)==0;
    if(close(fd))ok=false;
    if(!ok||rename(temporary,sidecar)){unlink(temporary);return false;}
    return cache_threads_live(path,sidecar);
}
#endif
