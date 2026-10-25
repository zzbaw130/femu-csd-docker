/* Guest diagnostic, normally run with sudo: metadata comes from PG; heap
 * bytes go directly NVM -> AFDM -> count CSF. No heap read by this process. */
#include "cs.h"
#include "heap_count.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define TRY(call) do { CS_STATUS status=(call); if(status!=CS_SUCCESS) { \
    fprintf(stderr,"%s: SNIA status=%d\n",#call,status); goto cleanup; } } while(0)
static PGresult *query(PGconn *db, const char *sql, const char *table)
{
    const char *params[]={table};
    PGresult *result=PQexecParams(db,sql,1,NULL,params,NULL,NULL,0);
    if (PQresultStatus(result)!=PGRES_TUPLES_OK) {
        fprintf(stderr,"%s",PQerrorMessage(db)); PQclear(result); return NULL;
    }
    return result;
}
int main(int argc, char **argv)
{
    if(argc>4) { fprintf(stderr,"usage: %s [conninfo] [relation] [chunk_pages]\n",argv[0]); return 2; }
    const char *conninfo=argc>1?argv[1]:"dbname=tpch_s1 user=femu";
    const char *table=argc>2?argv[2]:"public.lineitem";
    unsigned long chunk_pages=512;
    if(argc==4) {
        char *end; chunk_pages=strtoul(argv[3],&end,10);
        if(!*argv[3] || *end || !chunk_pages || chunk_pages>4096) return 2;
    }
    int rc=1, loaded=0, active=0;
    PGconn *db=PQconnectdb(conninfo);
    PGresult *metadata=NULL,*extents=NULL,*expected=NULL;
    CS_DEV_HANDLE device=NULL;
    CS_MEM_HANDLE input=NULL,output=NULL;
    CS_BATCH_HANDLE batch=NULL;
    CsBatchRequest *load=NULL,*compute=NULL;
    CSFIdInfo *functions=NULL;
    if(PQstatus(db)!=CONNECTION_OK) { fprintf(stderr,"%s",PQerrorMessage(db)); goto cleanup; }
    metadata=query(db,"SELECT r.device,r.namespace_id,r.lba_bytes,r.bytes,r.nattrs,r.boot_id "
        "FROM sina_csd.relations r JOIN pg_class c ON c.oid=r.relid "
        "WHERE r.relid=$1::regclass AND r.relfilenode=pg_relation_filenode(c.oid)",table);
    if(!metadata || PQntuples(metadata)!=1) { fprintf(stderr,"prepare this relation first\n"); goto cleanup; }
    char boot_id[80]; FILE *boot=fopen("/proc/sys/kernel/random/boot_id","r");
    if(!boot) goto cleanup;
    int have_boot=fgets(boot_id,sizeof(boot_id),boot)!=NULL; fclose(boot);
    if(!have_boot) goto cleanup;
    boot_id[strcspn(boot_id,"\n")]=0;
    if(strcmp(boot_id,PQgetvalue(metadata,0,5))) { fprintf(stderr,"Guest restarted: prepare mappings again\n"); goto cleanup; }
    u64 nsid=strtoull(PQgetvalue(metadata,0,1),NULL,10),lba=strtoull(PQgetvalue(metadata,0,2),NULL,10);
    u64 bytes=strtoull(PQgetvalue(metadata,0,3),NULL,10),natts=strtoull(PQgetvalue(metadata,0,4),NULL,10);
    if((lba!=512 && lba!=4096) || !nsid || !natts || natts>1600) goto cleanup;
    u64 chunk=chunk_pages*PG_SINA_PAGE_BYTES;
    extents=query(db,"SELECT logical_offset,start_lba,bytes FROM sina_csd.extents "
        "WHERE relid=$1::regclass ORDER BY logical_offset",table);
    if(!extents) goto cleanup;
    unsigned ranges=PQntuples(extents);
    u64 covered=0;
    for(unsigned i=0;i<ranges;++i) {
        u64 logical=strtoull(PQgetvalue(extents,i,0),NULL,10),length=strtoull(PQgetvalue(extents,i,2),NULL,10);
        if(logical!=covered || !length || length%lba || logical>bytes || length>bytes-logical) {
            fprintf(stderr,"incomplete or invalid prepared extents\n"); goto cleanup;
        }
        covered+=length;
    }
    if(covered!=bytes || bytes%PG_SINA_PAGE_BYTES) goto cleanup;
    TRY(csOpenCSx(PQgetvalue(metadata,0,0),NULL,&device));
    char descriptor[]="pg-sina-heap-count.so\0pg_sina_heap_count";
    CsCSFDownloadInfo download={.Type=CS_CSF_VENDOR_SPECIFIC,.SubType=CS_SUBTYPE_VENDOR_SPECIFIC,.CSEEId=1,.Index=500};
    strcpy(download.common.UniqueName,"pg-sina-heap-count");
    download.common.DataBuffer=descriptor; download.common.Length=sizeof(descriptor);
    u32 id;
    TRY(csCSFDownload(device,&download,&id)); loaded=1;
    CsConfigInfo config={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=500,.EEIId=1,.State=CS_ACTIVE_STATE}};
    CsConfigData config_data; int length=sizeof(config_data);
    TRY(csConfig(device,&length,&config,&config_data)); active=1;
    int count=0; length=0;
    if(csGetCSFId(device,download.common.UniqueName,0,&length,&count,NULL)!=CS_LENGTH_RETURNED) goto cleanup;
    functions=malloc(length);
    if(!functions) goto cleanup;
    TRY(csGetCSFId(device,download.common.UniqueName,0,&length,&count,functions));
    CsMemFlags flags={.FDMId=1};
    TRY(csAllocMem(device,chunk,&flags,&input,NULL));
    TRY(csAllocMem(device,sizeof(struct pg_sina_count),&flags,&output,NULL));
    load=calloc(1,sizeof(*load)+(ranges?ranges:1)*sizeof(CSBlockRange));
    compute=calloc(1,sizeof(*compute)+4*sizeof(CsComputeArg));
    if(!load || !compute) goto cleanup;
    load->ReqType=CS_STORAGE_IO;
    load->u.StorageIo=(CsStorageRequest){.Mode=CS_STORAGE_BLOCK_IO,.DevHandle=device,
        .u.BlockIo={.Type=CS_STORAGE_LOAD_TYPE,.DevMem={input,0}}};
    compute->ReqType=CS_QUEUE_COMPUTE;
    compute->u.Compute.CSFId=functions[0].CSFId; compute->u.Compute.NumArgs=4;
    csHelperSetComputeArg(&compute->u.Compute.Args[0],CS_AFDM_TYPE,input,0UL);
    csHelperSetComputeArg(&compute->u.Compute.Args[1],CS_AFDM_TYPE,output,0UL);
    csHelperSetComputeArg(&compute->u.Compute.Args[2],CS_64BIT_VALUE_TYPE,natts);
    TRY(csAllocBatchRequest(CS_BATCH_SERIAL,2,&batch));
    CS_BATCH_INDEX load_node=0,compute_node=0;
    uint64_t rows=0,pages=0,chunks=0;
    struct timespec start,end;
    clock_gettime(CLOCK_MONOTONIC,&start);
    for(u64 offset=0;offset<bytes;offset+=chunk) {
        u64 end_offset=offset+chunk<bytes?offset+chunk:bytes,cursor=offset;
        CsBlockIo *request=&load->u.StorageIo.u.BlockIo; request->NumRanges=0;
        for(unsigned i=0;i<ranges && cursor<end_offset;++i) {
            u64 logical=strtoull(PQgetvalue(extents,i,0),NULL,10);
            u64 first=strtoull(PQgetvalue(extents,i,1),NULL,10);
            u64 last=logical+strtoull(PQgetvalue(extents,i,2),NULL,10);
            if(last<=cursor) continue;
            u64 size=(last<end_offset?last:end_offset)-cursor;
            request->Range[request->NumRanges++]=(CSBlockRange){nsid,first+(cursor-logical)/lba,size/lba};
            cursor+=size;
        }
        csHelperSetComputeArg(&compute->u.Compute.Args[3],CS_64BIT_VALUE_TYPE,end_offset-offset);
        if(!chunks) {
            TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,load,0,0,&load_node));
            TRY(csConfigureBatchEntry(batch,CS_BATCH_ADD,compute,0,load_node,&compute_node));
        } else {
            TRY(csConfigureBatchEntry(batch,CS_BATCH_RECONFIG,load,load_node,0,NULL));
            TRY(csConfigureBatchEntry(batch,CS_BATCH_RECONFIG,compute,compute_node,0,NULL));
        }
        TRY(csQueueBatchRequest(batch,NULL,NULL,NULL,NULL,NULL));
        struct pg_sina_count result;
        CsCopyMemRequest read={.Type=CS_COPY_FROM_DEVICE,.DevMem={output,0},.u.HostVAddress=&result,.Bytes=sizeof(result)};
        TRY(csQueueCopyMemRequest(&read,NULL,NULL,NULL,NULL,NULL));
        rows+=result.rows; pages+=result.pages; ++chunks;
    }
    clock_gettime(CLOCK_MONOTONIC,&end);
    /* Resolve and quote the table name through PG, rather than SQL interpolation. */
    expected=query(db,"SELECT format('SELECT count(*) FROM %s', $1::regclass)",table);
    if(!expected || PQntuples(expected)!=1) goto cleanup;
    PGresult *reference=PQexec(db,PQgetvalue(expected,0,0));
    if(PQresultStatus(reference)!=PGRES_TUPLES_OK || PQntuples(reference)!=1) {
        fprintf(stderr,"%s",PQerrorMessage(db)); PQclear(reference); goto cleanup;
    }
    u64 reference_rows=strtoull(PQgetvalue(reference,0,0),NULL,10); PQclear(reference);
    if(rows!=reference_rows || pages!=bytes/PG_SINA_PAGE_BYTES) { fprintf(stderr,"heap count mismatch\n"); goto cleanup; }
    printf("PASS %s rows=%llu pages=%llu extents=%u chunks=%llu internal_bytes=%llu seconds=%.6f\n",table,
        (unsigned long long)rows,(unsigned long long)pages,ranges,(unsigned long long)chunks,
        (unsigned long long)bytes,end.tv_sec-start.tv_sec+(end.tv_nsec-start.tv_nsec)/1e9);
    rc=0;
cleanup:
    if(batch) csFreeBatchRequest(batch);
    CsMemFlags release={.FDMId=1};
    if(output) csFreeMem(output,&release);
    if(input) csFreeMem(input,&release);
    if(active) {
        CsConfigInfo config={.Type=CS_CSF_ACTIVATE,.u.CSFActivateInfo={.CSFId=500,.EEIId=1,.State=CS_INACTIVE_STATE}};
        CsConfigData data; int length=sizeof(data); csConfig(device,&length,&config,&data);
    }
    if(loaded) { CsCSFDownloadInfo unload={.CSEEId=1,.Index=500,.common.Unload=1}; u32 id; csCSFDownload(device,&unload,&id); }
    if(device) csCloseCSx(device);
    free(load); free(compute); free(functions);
    if(expected) PQclear(expected);
    if(extents) PQclear(extents);
    if(metadata) PQclear(metadata);
    PQfinish(db); return rc;
}
