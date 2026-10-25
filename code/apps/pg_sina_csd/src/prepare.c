/* Guest PostgreSQL: prepare immutable heap/schema/LBA metadata once. */
#include "postgres.h"
#include "fmgr.h"
#include "access/relation.h"
#include "access/nbtree.h"
#include "catalog/pg_opfamily_d.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
PG_FUNCTION_INFO_V1(pg_sina_csd_prepare);
static uint64 number(const char *path)
{
    unsigned long long value;
    FILE *file = AllocateFile(path, "r");
    if (!file || fscanf(file, "%llu", &value) != 1)
        ereport(ERROR, (errmsg("cannot read device geometry: %s", path)));
    FreeFile(file);
    return value;
}
static void execute(const char *sql, int count, Oid *types, Datum *values)
{
    if (SPI_execute_with_args(sql, count, types, values, NULL, false, 0) < 0)
        elog(ERROR, "cannot save pg_sina_csd mapping");
}
static void save_extents(Oid relid,char *base,uint64 total,dev_t data_device,uint64 partition,uint64 lba,uint64 namespace_bytes)
{
    uint64 mapped = 0;
    for (unsigned segment = 0; mapped < total; ++segment) {
        char *path = segment ? psprintf("%s.%u",base,segment) : base;
        int fd = OpenTransientFile(path,O_RDONLY | PG_BINARY);
        struct stat before, after;
        if (fd < 0 || fstat(fd,&before) || before.st_dev != data_device || before.st_size <= 0 ||
            (uint64)before.st_size > total-mapped || before.st_size % BLCKSZ)
            ereport(ERROR,(errmsg("invalid heap segment: %s",path)));
        struct fiemap *map = palloc0(sizeof(*map) + 128 * sizeof(struct fiemap_extent));
        uint64 cursor = 0;
        while (cursor < (uint64)before.st_size) {
            CHECK_FOR_INTERRUPTS();
            map->fm_start = cursor; map->fm_length = before.st_size-cursor;
            map->fm_flags = FIEMAP_FLAG_SYNC; map->fm_extent_count = 128; map->fm_mapped_extents = 0;
            if (ioctl(fd,FS_IOC_FIEMAP,map) || !map->fm_mapped_extents)
                ereport(ERROR,(errmsg("FIEMAP failed for %s: %m",path)));
            for (unsigned i = 0; i < map->fm_mapped_extents && cursor < (uint64)before.st_size; ++i) {
                struct fiemap_extent *e = &map->fm_extents[i];
                uint64 bytes = Min(e->fe_length,(uint64)before.st_size-cursor);
                uint64 physical = partition + e->fe_physical;
                if (e->fe_logical != cursor || !bytes ||
                    (e->fe_flags & ~(FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_MERGED)) ||
                    bytes % lba || physical % lba || physical > namespace_bytes || bytes > namespace_bytes-physical)
                    ereport(ERROR,(errmsg("heap extent is sparse, uninitialized or outside the namespace")));
                Oid types[] = {OIDOID,INT8OID,INT8OID,INT8OID};
                Datum values[] = {ObjectIdGetDatum(relid),Int64GetDatum(mapped+cursor),Int64GetDatum(physical/lba),Int64GetDatum(bytes)};
                execute("INSERT INTO sina_csd.extents VALUES($1,$2,$3,$4)",4,types,values);
                cursor += bytes;
            }
        }
        if (fstat(fd,&after) || before.st_ino != after.st_ino || before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
            ereport(ERROR,(errmsg("heap changed while preparing")));
        CloseTransientFile(fd); pfree(map); mapped += before.st_size;
        if (segment) pfree(path);
    }
}
Datum pg_sina_csd_prepare(PG_FUNCTION_ARGS)
{
    Oid relid = PG_GETARG_OID(0);
    char *device = text_to_cstring(PG_GETARG_TEXT_PP(1));
    Relation relation;
    BlockNumber pages, visible, frozen;
    struct stat raw, data;
    uint64 lba, nsid, partition = 0, namespace_bytes, total;
    char sysfs[MAXPGPATH], boot_id[80], *base;
    FILE *boot;
    if (!superuser()) ereport(ERROR, (errmsg("sina_csd.prepare requires a superuser")));
    if (PG_VERSION_NUM < 120000 || PG_VERSION_NUM >= 130000 || BLCKSZ != 8192 || sizeof(void *) != 8)
        ereport(ERROR, (errmsg("pg_sina_csd currently requires PostgreSQL 12, 8KiB pages and a 64-bit process")));
    relation = relation_open(relid, AccessShareLock);
    if (relation->rd_rel->relkind != RELKIND_RELATION ||
        relation->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
        relation->rd_rel->relam != HEAP_TABLE_AM_OID)
        ereport(ERROR, (errmsg("only permanent heap tables can be prepared")));
    for (int i = 0; i < RelationGetDescr(relation)->natts; ++i)
        if (TupleDescAttr(RelationGetDescr(relation),i)->attisdropped)
            ereport(ERROR, (errmsg("dropped columns are not supported yet")));
    pages = RelationGetNumberOfBlocks(relation);
    visibilitymap_count(relation, &visible, &frozen);
    if (visible != pages || frozen != pages)
        ereport(ERROR, (errmsg("table must be fully visible and frozen"),
                       errhint("Run VACUUM (FREEZE, ANALYZE) and prepare again.")));
    /* Flush PG buffers before FIEMAP; filesystem sync alone cannot do this. */
    RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
    total = (uint64)pages * BLCKSZ;
    base = relpathperm(relation->rd_node, MAIN_FORKNUM);
    char fullpath[MAXPGPATH];
    if (base[0]=='/') snprintf(fullpath,sizeof(fullpath),"%s",base);
    else snprintf(fullpath,sizeof(fullpath),"%s/%s",DataDir,base);
    if (stat(device, &raw) || !S_ISBLK(raw.st_mode) || stat(fullpath, &data))
        ereport(ERROR, (errmsg("cannot identify the heap or namespace: %m")));
    snprintf(sysfs,sizeof(sysfs),"/sys/dev/block/%u:%u/queue/logical_block_size",major(raw.st_rdev),minor(raw.st_rdev));
    lba = number(sysfs);
    snprintf(sysfs,sizeof(sysfs),"/sys/dev/block/%u:%u/nsid",major(raw.st_rdev),minor(raw.st_rdev));
    nsid = number(sysfs);
    snprintf(sysfs,sizeof(sysfs),"/sys/dev/block/%u:%u/size",major(raw.st_rdev),minor(raw.st_rdev));
    namespace_bytes = number(sysfs) * 512;
    if ((lba != 512 && lba != 4096) || nsid > INT_MAX)
        ereport(ERROR, (errmsg("unsupported namespace format")));
    if (data.st_dev != raw.st_rdev) {
        unsigned parent_major, parent_minor;
        FILE *parent;
        snprintf(sysfs,sizeof(sysfs),"/sys/dev/block/%u:%u/../dev",major(data.st_dev),minor(data.st_dev));
        parent = AllocateFile(sysfs,"r");
        if (!parent || fscanf(parent,"%u:%u",&parent_major,&parent_minor) != 2 ||
            makedev(parent_major,parent_minor) != raw.st_rdev)
            ereport(ERROR, (errmsg("heap is not on the selected namespace or its partition")));
        FreeFile(parent);
        snprintf(sysfs,sizeof(sysfs),"/sys/dev/block/%u:%u/start",major(data.st_dev),minor(data.st_dev));
        partition = number(sysfs) * 512;
    }
    boot = AllocateFile("/proc/sys/kernel/random/boot_id","r");
    if (!boot || !fgets(boot_id,sizeof(boot_id),boot)) elog(ERROR,"cannot read Guest boot ID");
    FreeFile(boot); boot_id[strcspn(boot_id,"\n")] = 0;
    if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid id_type[] = {OIDOID}; Datum id_value[] = {ObjectIdGetDatum(relid)};
    execute("DELETE FROM sina_csd.relations WHERE relid IN (SELECT indexid FROM sina_csd.indexes WHERE heaprelid=$1)",1,id_type,id_value);
    execute("DELETE FROM sina_csd.relations WHERE relid=$1",1,id_type,id_value);
    Oid relation_types[] = {OIDOID,OIDOID,TEXTOID,INT4OID,INT4OID,INT8OID,INT4OID,TEXTOID};
    Datum relation_values[] = {ObjectIdGetDatum(relid),ObjectIdGetDatum(relation->rd_node.relNode),
        CStringGetTextDatum(device),Int32GetDatum(nsid),Int32GetDatum(lba),Int64GetDatum(total),
        Int32GetDatum(RelationGetDescr(relation)->natts),CStringGetTextDatum(boot_id)};
    execute("INSERT INTO sina_csd.relations(relid,relfilenode,device,namespace_id,lba_bytes,bytes,nattrs,boot_id) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8)",8,relation_types,relation_values);
    for (int i = 0; i < RelationGetDescr(relation)->natts; ++i) {
        Form_pg_attribute a = TupleDescAttr(RelationGetDescr(relation),i);
        Oid types[] = {OIDOID,INT2OID,TEXTOID,OIDOID,INT4OID,INT2OID,CHAROID,BOOLOID,OIDOID};
        Datum values[] = {ObjectIdGetDatum(relid),Int16GetDatum(a->attnum),CStringGetTextDatum(NameStr(a->attname)),
            ObjectIdGetDatum(a->atttypid),Int32GetDatum(a->atttypmod),Int16GetDatum(a->attlen),
            CharGetDatum(a->attalign),BoolGetDatum(a->attnotnull),ObjectIdGetDatum(a->attcollation)};
        execute("INSERT INTO sina_csd.columns VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9)",9,types,values);
    }
    save_extents(relid,fullpath,total,data.st_dev,partition,lba,namespace_bytes);
    /* Map eligible native indexes once, alongside the frozen heap. This stores
     * file extents and metadata only, never a copy of index keys or tuples. */
    List *indexes=RelationGetIndexList(relation); ListCell *cell;
    foreach(cell,indexes) {
        Relation idx=relation_open(lfirst_oid(cell),AccessShareLock);
        Form_pg_index info=idx->rd_index;
        if(idx->rd_rel->relam!=BTREE_AM_OID || !info->indisvalid || !info->indisready || info->indcheckxmin ||
           info->indnkeyatts<1 || info->indkey.values[0]<=0 || idx->rd_indoption[0] ||
           idx->rd_opfamily[0]!=INTEGER_BTREE_FAM_OID || RelationGetIndexPredicate(idx)!=NIL || RelationGetIndexExpressions(idx)!=NIL) {
            relation_close(idx,AccessShareLock); continue;
        }
        Form_pg_attribute key=TupleDescAttr(RelationGetDescr(idx),0);
        if(key->atttypid!=INT2OID && key->atttypid!=INT4OID && key->atttypid!=INT8OID) { relation_close(idx,AccessShareLock); continue; }
        Buffer buffer=ReadBuffer(idx,0); LockBuffer(buffer,BUFFER_LOCK_SHARE);
        BTMetaPageData meta=*BTPageGetMeta(BufferGetPage(buffer)); UnlockReleaseBuffer(buffer);
        if(meta.btm_magic!=BTREE_MAGIC || meta.btm_version!=4) { relation_close(idx,AccessShareLock); continue; }
        Oid iid=RelationGetRelid(idx); uint64 size=(uint64)RelationGetNumberOfBlocks(idx)*BLCKSZ;
        char *path=relpathperm(idx->rd_node,MAIN_FORKNUM); char ipath[MAXPGPATH];
        if(path[0]=='/') snprintf(ipath,sizeof(ipath),"%s",path); else snprintf(ipath,sizeof(ipath),"%s/%s",DataDir,path);
        struct stat index_stat;
        if(stat(ipath,&index_stat) || index_stat.st_dev!=data.st_dev) elog(ERROR,"index is not on the heap filesystem");
        Datum value[]={ObjectIdGetDatum(iid)};
        execute("DELETE FROM sina_csd.relations WHERE relid=$1",1,id_type,value);
        relation_values[0]=ObjectIdGetDatum(iid); relation_values[1]=ObjectIdGetDatum(idx->rd_node.relNode);
        relation_values[5]=Int64GetDatum(size); relation_values[6]=Int32GetDatum(RelationGetDescr(idx)->natts);
        execute("INSERT INTO sina_csd.relations(relid,relfilenode,device,namespace_id,lba_bytes,bytes,nattrs,boot_id) VALUES($1,$2,$3,$4,$5,$6,$7,$8)",8,relation_types,relation_values);
        save_extents(iid,ipath,size,data.st_dev,partition,lba,namespace_bytes);
        Oid types[]={OIDOID,OIDOID,INT2OID,INT8OID,INT2OID};
        Datum values[]={ObjectIdGetDatum(iid),ObjectIdGetDatum(relid),Int16GetDatum(info->indkey.values[0]),Int64GetDatum(meta.btm_root),Int16GetDatum(key->attlen)};
        execute("INSERT INTO sina_csd.indexes VALUES($1,$2,$3,$4,$5)",5,types,values);
        pfree(path); relation_close(idx,AccessShareLock);
    }
    SPI_finish(); relation_close(relation,AccessShareLock);
    PG_RETURN_INT64(pages);
}
