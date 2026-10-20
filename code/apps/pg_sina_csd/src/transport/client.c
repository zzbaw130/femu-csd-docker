/* PG backend: read prepared metadata and launch one short-lived SNIA worker. */
#include "pg_internal.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
struct qs_extent *pg_sina_mapping(struct qs_request *request)
{
    FILE *file=fopen("/proc/sys/kernel/random/boot_id","r"); char boot[80];
    if(!file || !fgets(boot,sizeof(boot),file)) elog(ERROR,"cannot read Guest boot ID");
    fclose(file); boot[strcspn(boot,"\n")]=0;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID,TEXTOID}; Datum values[]={ObjectIdGetDatum(request->plan.relid),ObjectIdGetDatum(request->plan.relfilenode),CStringGetTextDatum(boot)};
    int status=SPI_execute_with_args("SELECT r.device,r.namespace_id,r.lba_bytes,r.bytes,e.logical_offset,e.start_lba,e.bytes "
        "FROM sina_csd.relations r LEFT JOIN sina_csd.extents e ON e.relid=r.relid "
        "WHERE r.relid=$1 AND r.relfilenode=$2 AND r.boot_id=$3 ORDER BY e.logical_offset",3,types,values,NULL,true,0);
    if(status!=SPI_OK_SELECT || !SPI_processed) ereport(ERROR,(errmsg("CSD mapping is missing or stale; prepare the relation again")));
    bool isnull; TupleDesc desc=SPI_tuptable->tupdesc; HeapTuple first=SPI_tuptable->vals[0];
    strlcpy(request->device,SPI_getvalue(first,desc,1),sizeof(request->device));
    request->nsid=DatumGetInt32(SPI_getbinval(first,desc,2,&isnull));
    request->lba_bytes=DatumGetInt32(SPI_getbinval(first,desc,3,&isnull));
    request->bytes=DatumGetInt64(SPI_getbinval(first,desc,4,&isnull));
    request->nextents=request->bytes?SPI_processed:0;
    struct qs_extent *extents=SPI_palloc(Max(request->nextents,1U)*sizeof(*extents)); uint64 covered=0;
    for(unsigned i=0;i<request->nextents;++i) {
        HeapTuple tuple=SPI_tuptable->vals[i];
        extents[i].logical=DatumGetInt64(SPI_getbinval(tuple,desc,5,&isnull));
        extents[i].lba=DatumGetInt64(SPI_getbinval(tuple,desc,6,&isnull));
        extents[i].bytes=DatumGetInt64(SPI_getbinval(tuple,desc,7,&isnull));
        if(extents[i].logical!=covered || !extents[i].bytes || extents[i].bytes>request->bytes-covered)
            elog(ERROR,"incomplete CSD extent mapping");
        covered+=extents[i].bytes;
    }
    if(covered!=request->bytes) elog(ERROR,"incomplete CSD extent mapping");
    SPI_finish(); return extents;
}
void pg_sina_close(void *opaque)
{
    struct pg_sina_worker *worker=opaque;
    if(worker->fd>=0) { close(worker->fd); worker->fd=-1; }
    if(worker->pid>0) {
        /* Closing the socket cancels at the next batch boundary. The worker
         * releases its CSF contexts/AFDM before exit; it is not a daemon. */
        while(waitpid(worker->pid,NULL,0)<0 && errno==EINTR) {}
        worker->pid=-1;
    }
}
static void transfer(int fd,void *buffer,size_t bytes,bool writing)
{
    unsigned char *p=buffer;
    while(bytes) {
        CHECK_FOR_INTERRUPTS();
        struct pollfd wait={.fd=fd,.events=writing?POLLOUT:POLLIN};
        int ready=poll(&wait,1,100);
        if(ready<0 && errno==EINTR) continue;
        if(ready<0) ereport(ERROR,(errmsg("CSD worker socket: %m")));
        if(!ready) continue;
        ssize_t n=writing?send(fd,p,bytes,MSG_NOSIGNAL):recv(fd,p,bytes,0);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) ereport(ERROR,(errmsg("CSD worker disconnected; check the PostgreSQL log")));
        p+=n; bytes-=n;
    }
}
void pg_sina_execute(struct qs_request *request,struct qs_extent *extents,struct qs_result *result,struct pg_sina_worker *worker)
{
    int sockets[2]; if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)) ereport(ERROR,(errmsg("socketpair: %m")));
    posix_spawn_file_actions_t actions; posix_spawnattr_t attributes; sigset_t mask;
    posix_spawn_file_actions_init(&actions); posix_spawn_file_actions_adddup2(&actions,sockets[1],STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions,sockets[1],STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions,sockets[0]); posix_spawn_file_actions_addclose(&actions,sockets[1]);
    posix_spawnattr_init(&attributes); sigemptyset(&mask); posix_spawnattr_setsigmask(&attributes,&mask);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK);
    char *args[]={"/usr/bin/sudo","-n","--",PG_SINA_WORKER_PATH,NULL};
    int status=posix_spawn(&worker->pid,args[0],&actions,&attributes,args,environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes); close(sockets[1]);
    if(status) { close(sockets[0]); worker->pid=-1; ereport(ERROR,(errmsg("cannot start CSD worker: %s",strerror(status)))); }
    worker->fd=sockets[0];
    transfer(worker->fd,request,sizeof(*request),true); transfer(worker->fd,extents,request->nextents*sizeof(*extents),true);
    transfer(worker->fd,result,sizeof(*result),false); pg_sina_close(worker);
    if(result->status) ereport(ERROR,(errmsg("CSD execution failed: %s",result->error)));
}
