/* Bottom-up placement of the PG-selected tree. Costs are milliseconds.
 * A device island starts at immutable base tables and returns rows to PG.
 * Host-produced intermediate rows cannot yet enter the device interpreter;
 * that edge has infinite cost, rather than pretending it is executable. */
#include "pg_internal.h"
#include "cost_model.h"
#include "catalog/pg_statistic.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "access/relation.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/rel.h"
#include <float.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct residency { Oid oid; double bytes,tuples,fraction; };
struct placement {
    Plan *native,*device;
    PlannerInfo *root;
    List *children;
    double host,native_host,csd,boundary,cache,spill,io,cpu,peak,population;
    double serial,parallel,schedule,combine,dictionary,fdm;
    unsigned workers;
    StringInfo stages;
    bool offload;
};
static double transfer_ms(double bytes,double mbps) { return bytes/(mbps*1000); }
/* mincore on PROT_NONE mappings observes OS residency without reading Heap.
 * Cache it only within this planning invocation, never across executions. */
static struct residency *residency(Oid oid,List **observations)
{
    ListCell *c;
    foreach(c,*observations) { struct residency *r=lfirst(c); if(r->oid==oid) return r; }
    struct residency *r=palloc0(sizeof(*r)); r->oid=oid;
    Relation rel=relation_open(oid,AccessShareLock);
    r->bytes=(double)RelationGetNumberOfBlocks(rel)*BLCKSZ;
    r->tuples=Max(1,rel->rd_rel->reltuples);
    char *base=relpathperm(rel->rd_node,MAIN_FORKNUM);
    double hit=0,seen=0;
    for(uint64 at=0;at<(uint64)r->bytes;) {
        unsigned segment=at/((uint64)RELSEG_SIZE*BLCKSZ);
        char *path=segment?psprintf("%s.%u",base,segment):base;
        int fd=OpenTransientFile(path,O_RDONLY|PG_BINARY);
        if(segment) pfree(path);
        if(fd<0) break;
        struct stat st;
        if(fstat(fd,&st)) { CloseTransientFile(fd); break; }
        size_t bytes=Min((uint64)st.st_size,(uint64)r->bytes-at);
        if(!bytes) { CloseTransientFile(fd); break; }
        size_t pages=(bytes+4095)/4096;
        unsigned char *vector=palloc(pages);
        void *map=mmap(NULL,bytes,PROT_NONE,MAP_SHARED,fd,0);
        if(map!=MAP_FAILED) {
            if(!mincore(map,bytes,vector)) {
                for(size_t i=0;i<pages;++i) hit+=(vector[i]&1)!=0;
                seen+=pages;
            }
            munmap(map,bytes);
        }
        pfree(vector); CloseTransientFile(fd); at+=bytes;
    }
    r->fraction=seen?hit/seen:0;
    pfree(base); relation_close(rel,AccessShareLock);
    *observations=lappend(*observations,r); return r;
}
static Oid scan_relation(PlannerInfo *root,Plan *p)
{
    if(IsA(p,SeqScan) || IsA(p,IndexScan) || IsA(p,IndexOnlyScan) || IsA(p,BitmapHeapScan)) {
        RangeTblEntry *r=rt_fetch(((Scan *)p)->scanrelid,root->parse->rtable);
        if(r->rtekind==RTE_RELATION) return r->relid;
    }
    return InvalidOid;
}
static double output_bytes(Plan *p,const struct qs_plan *output)
{
    /* Generic boundaries transfer projected cells and actual row counts;
     * worker intermediates and legacy single-table CSFs retain their ABI. */
    double rows=Max(0,p->plan_rows);
    return rows*output->nout*sizeof(struct qs_cell)+
        Max(1,ceil(rows/qp_result_capacity(output,1)))*offsetof(struct qs_result,data)+
        rows*Max(0,p->plan_width-(int)output->nout*(int)sizeof(struct qs_cell));
}
/* Match executor layout selection: try smaller input blocks before fewer CUs.
 * File-extent descriptors are not yet available here; allow one page/node. */
static unsigned device_workers(struct qp_plan *tree,double *fdm)
{
    unsigned workers=1;
    bool indexed[QP_NODES]={0};
    for(unsigned i=0;i<tree->count;++i) {
        if(tree->nodes[i].parallel) workers=Min(QS_WORKERS,Max(1,pg_sina_resources.cus));
        if(tree->nodes[i].kind==QP_INDEX_JOIN) indexed[tree->nodes[i].right]=true;
    }
    unsigned chunk=QS_CHUNK;
    for(;;) {
        double arena=TYPEALIGN(4096,sizeof(struct qs_dict)),stride=0;
        tree->scan_bytes=chunk;
        for(unsigned i=0;i<tree->count;++i) {
            const struct qp_node *n=&tree->nodes[i];
            double input=0;
            if(n->kind==QP_SCAN && !indexed[i])
                input=(n->ordered_index?QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE:
                    qp_scan_buffer_bytes(tree,n))+TYPEALIGN(4096,qs_chunk_bytes(
                    n->ordered_index?256:qp_scan_capacity(tree,n),n->expr.ncols));
            if(n->kind==QP_INDEX_JOIN)
                input=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE+4096+TYPEALIGN(4096,
                    qs_chunk_bytes(256,tree->nodes[n->right].expr.ncols+2));
            arena+=input;
        }
        for(unsigned i=0;i<tree->count;++i) if(tree->nodes[i].parallel) {
            int source=qp_pipeline_source(tree,i); double bytes=0;
            for(int id=i;id>=0;id=tree->nodes[id].left) {
                const struct qp_node *n=&tree->nodes[id];
                if(id==source && n->kind!=QP_SCAN) break;
                if(n->kind==QP_SCAN) {
                    bytes+=(n->ordered_index?QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE:
                        qp_scan_buffer_bytes(tree,n))+TYPEALIGN(4096,qs_chunk_bytes(
                        n->ordered_index?256:qp_scan_capacity(tree,n),n->expr.ncols));
                    break;
                }
                if(n->kind==QP_INDEX_JOIN) bytes+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE+4096+
                    TYPEALIGN(4096,qs_chunk_bytes(256,tree->nodes[n->right].expr.ncols+2));
            }
            stride=Max(stride,bytes);
        }
        unsigned slots=workers>1?workers+1:1;
        *fdm=arena+(workers-1)*stride+sizeof(*tree)+tree->count*4096+
            slots*(sizeof(struct qp_control)+sizeof(struct qs_result)+QS_SORT_IO_BYTES)+65536;
        if(*fdm<=pg_sina_resources.fdm_bytes || (chunk==QP_SCAN_BYTES && workers==1)) break;
        if(chunk>QP_SCAN_BYTES) chunk/=2;
        else --workers;
    }
    return workers;
}
/* Stats are used for data volume, never for correctness or allocator limits.
 * A missing NDV uses PG's conventional 200-value fallback, bounded by rows. */
static double dictionary_state(const struct qp_plan *tree,List **observations)
{
    double entries=0,body=0;
    for(unsigned i=0;i<tree->count;++i) {
        const struct qp_node *n=&tree->nodes[i];
        if(n->kind!=QP_SCAN) continue;
        double rows=residency(n->expr.relid,observations)->tuples;
        for(unsigned c=0;c<n->expr.nattrs;++c) if(n->expr.cols[c].slot>=0 && n->expr.cols[c].type==Q_STRING) {
            int average=get_attavgwidth(n->expr.relid,c+1);
            double distinct=Min(rows,200),width=average>0?average:32;
            HeapTuple stats=SearchSysCache3(STATRELATTINH,ObjectIdGetDatum(n->expr.relid),
                Int16GetDatum(c+1),BoolGetDatum(false));
            if(HeapTupleIsValid(stats)) {
                Form_pg_statistic st=(Form_pg_statistic)GETSTRUCT(stats);
                distinct=st->stadistinct<0?-st->stadistinct*rows:st->stadistinct>0?st->stadistinct:distinct;
                distinct=Min(distinct,rows*(1-st->stanullfrac)); ReleaseSysCache(stats);
            }
            entries+=distinct; body+=distinct*width;
        }
        entries+=n->expr.nstrings;
        for(unsigned j=0;j<n->expr.nstrings;++j) body+=n->expr.strings[j].length;
    }
    if(!entries) return 0;
    double buckets=cost_slots(entries*2,512);
    /* dictionary.c: 16-byte entries, 4-byte heads, paged string bodies.
     * Rehash transiently retains both old and new heads. */
    return ceil(entries*16/QS_SORT_IO_BYTES)*QS_SORT_IO_BYTES+
        ceil(body/QS_SORT_IO_BYTES)*QS_SORT_IO_BYTES+1.5*buckets*4;
}
/* Propagate only the runtime-marked probe/scan pipeline, never through the
 * build edge or a blocking boundary. Aggregate sources use vector-sized tasks. */
static void pipeline_tasks(const struct qp_plan *tree,double *tasks)
{
    for(unsigned i=0;i<tree->count;++i) if(tree->nodes[i].parallel) {
        int source=qp_pipeline_source(tree,i); if(source<0) continue;
        const struct qp_node *n=&tree->nodes[source];
        double count=n->kind==QP_SCAN?ceil((double)n->bytes/tree->scan_bytes):
            ceil(Max(n->estimated_rows,n->estimated_groups)/QP_VECTOR_ROWS);
        for(int id=i;id>=0;id=tree->nodes[id].left) {
            tasks[id]=Max(tasks[id],count);
            if(id==source) break;
        }
    }
}
static void parallel_cpu(struct placement *p,double work,double tasks,unsigned workers)
{
    if(workers<=1 || tasks<=1) { p->serial+=work; return; }
    p->parallel+=cost_parallel(work,tasks,workers,0);
}
static unsigned blocking_depth(const struct qp_plan *tree,int id)
{
    if(id<0) return 0;
    const struct qp_node *n=&tree->nodes[id];
    unsigned left=blocking_depth(tree,n->left),right=blocking_depth(tree,n->right);
    if(n->kind==QP_JOIN) return Max(left,right)+2;
    return left+(n->kind==QP_SORT || (n->kind==QP_AGGREGATE && n->nkeys && (!n->sorted_input || n->distinct)));
}
/* Same key-preserving prefix proof as device input_ordered, including
 * Sort/Merge direction: internal merge sorting requests ascending NULL-last. */
static bool cost_ordered(const struct qp_plan *tree,int id,unsigned count,const int *columns)
{
    int keys[QS_KEYS]; memcpy(keys,columns,count*sizeof(*keys));
    while(id>=0) {
        const struct qp_node *n=&tree->nodes[id];
        bool merge=n->kind==QP_JOIN && !n->hash;
        if((n->kind!=QP_PROJECT && n->kind!=QP_SORT && !merge) || n->left<0) return false;
        for(unsigned k=0;k<count;++k) {
            if(keys[k]<0 || (unsigned)keys[k]>=n->expr.nout) return false;
            int index=n->expr.outputs[keys[k]].index;
            if(index<0 || (unsigned)index>=n->expr.nexprs || n->expr.exprs[index].op!=Q_COLUMN) return false;
            keys[k]=n->expr.exprs[index].column;
        }
        if(n->kind==QP_SORT || merge) {
            if((merge?n->nkeys:n->expr.norder)<count) return false;
            for(unsigned k=0;k<count;++k) {
                const struct qs_order *order=n->expr.norder?&n->expr.order[k]:NULL;
                if(keys[k]!=(merge?n->left_keys[k]:order->output) ||
                    (order && (order->descending || order->nulls_first))) return false;
            }
            return true;
        }
        id=n->left;
    }
    return false;
}
/* Follow column references to retain PG average text widths through device
 * projections. Computed strings without a direct origin use typmod or 32B. */
static double text_width(const struct qp_plan *tree,int id,unsigned column)
{
    const struct qp_node *n=&tree->nodes[id];
    if(column>=n->expr.nout) return 32;
    const struct qs_output *out=&n->expr.outputs[column];
    const struct qs_expr *e=&n->expr.exprs[out->index];
    if(e->type!=Q_STRING) return 0;
    if(e->op==Q_COLUMN && e->column>=0) {
        if(n->kind==QP_SCAN) {
            int width=get_attavgwidth(n->expr.relid,e->column+1);
            return width>0?width:32;
        }
        if(n->left>=0) {
            unsigned cols=tree->nodes[n->left].expr.nout;
            if((unsigned)e->column<cols) return text_width(tree,n->left,e->column);
            if(n->right>=0) return text_width(tree,n->right,e->column-cols);
        }
    }
    return out->typmod>4?Min(out->typmod-4,32):32;
}
static double sort_width(const struct qp_plan *tree,int id)
{
    double width=Max(1,tree->nodes[id].expr.nout)*sizeof(struct qs_cell);
    for(unsigned c=0;c<tree->nodes[id].expr.nout;++c) {
        double text=text_width(tree,id,c);
        if(text) width+=text+sizeof(uint16_t);
    }
    return width;
}
static const char *device_kind(int kind)
{
    static const char *names[]={"Scan","Project","Join","Aggregate","Sort","Limit","IndexJoin","CTEScan","Materialize","Set"};
    return kind>=0 && kind<(int)lengthof(names)?names[kind]:"Unknown";
}
static void device_cost(struct placement *p,List **observations)
{
    CustomScan *scan=(CustomScan *)p->device;
    Const *constant=linitial_node(Const,scan->custom_private);
    bytea *bytes=DatumGetByteaP(constant->constvalue);
    struct qp_plan *tree=palloc(sizeof(*tree)); memcpy(tree,VARDATA(bytes),sizeof(*tree));
    bool used[QP_NODES]={0},indexed[QP_NODES]={0}; used[tree->root]=true;
    for(unsigned i=0;i<tree->ninit;++i) used[tree->init_nodes[i]]=true;
    for(unsigned pass=0;pass<tree->count;++pass) for(unsigned i=0;i<tree->count;++i) if(used[i]) {
        struct qp_node *n=&tree->nodes[i];
        if(n->left>=0) used[n->left]=true;
        if(n->right>=0) used[n->right]=true;
        if(n->kind==QP_INDEX_JOIN) indexed[n->right]=true;
        if(n->kind==QP_SCAN) n->bytes=residency(n->expr.relid,observations)->bytes;
    }
    qp_parallel_boundaries(tree,tree->root);
    for(unsigned i=0;i<tree->ninit;++i) qp_parallel_boundaries(tree,tree->init_nodes[i]);
    unsigned cus=device_workers(tree,&p->fdm); p->workers=cus;
    p->stages=makeStringInfo();
    double tasks[QP_NODES]={0},peak[QP_NODES]={0},live[QP_NODES]={0},retained[QP_NODES]={0};
    pipeline_tasks(tree,tasks);
    double io=0,population_bytes=0,cache_bytes=0,scan_bytes=0,blocks=0;
    double memory=Max(1,pg_sina_query_memory()),unit=pg_sina_host_cost_ms*pg_sina_csd_cpu_ratio;
    p->dictionary=dictionary_state(tree,observations);
    /* Dictionary pages are evictable; descriptors remain resident. This is a
     * one-write/one-read lower bound, not a prediction of random reloads. */
    double dictionary_meta=ceil(p->dictionary/QS_SORT_IO_BYTES)*48;
    double dictionary_evicted=0;
    double quota=memory/(blocking_depth(tree,tree->root)+2);
    for(int i=(int)tree->count-1;i>=0;--i) if(used[i] && !indexed[i]) {
        const struct qp_node *n=&tree->nodes[i],*left=n->left>=0?&tree->nodes[n->left]:NULL;
        const struct qp_node *right=n->right>=0?&tree->nodes[n->right]:NULL;
        double input=left?Max(0,left->estimated_rows):Max(0,n->estimated_rows);
        double rows=Max(0,n->estimated_rows),state=0,spill=0;
        double previous_cpu=p->serial+p->parallel+p->schedule+p->combine;
        double l=n->left>=0?live[n->left]:0,r=n->right>=0?live[n->right]:0;
        double lp=n->left>=0?peak[n->left]:0,rp=n->right>=0?peak[n->right]:0;
        double lr=n->left>=0?retained[n->left]:0,rr=n->right>=0?retained[n->right]:0;
        /* One vector has 16-byte lanes + NULL/scale/error arrays. Actual
         * expression allocation can be lazy; this is reserved scratch. */
        unsigned copies=tasks[i]>1?cus+1:1;
        double scratch=copies*(n->expr.nexprs*(256*20+128.0)+sizeof(struct qs_plan));
        double available=Max((double)QS_SORT_IO_BYTES,memory-dictionary_meta-scratch-l-r);
        double expression=cpu_operator_cost*Max(1,n->expr.nexprs);
        double work=input*(cpu_tuple_cost+expression)*unit;
        if(n->kind==QP_SCAN) {
            struct residency *relation=residency(n->expr.relid,observations);
            double read=relation->bytes,hit=read*relation->fraction;
            input=relation->tuples;
            if(n->ordered_index) {
                double fraction=Min(1.0,Max(1,rows)/relation->tuples*4);
                read=(n->ordered_index&4)?0:read*fraction; hit=read*relation->fraction; input=rows;
                if(n->expr.join.indexid) {
                    struct residency *index=residency(n->expr.join.indexid,observations);
                    read+=index->bytes; hit+=index->bytes*index->fraction;
                }
            }
            scan_bytes+=read; cache_bytes+=hit; population_bytes+=read;
            io+=transfer_ms(read,pg_sina_internal_mbps);
            blocks+=read/(n->ordered_index?QS_PAGE:tree->scan_bytes);
            work=(input*(cpu_tuple_cost+expression)+rows*cpu_tuple_cost)*unit;
            parallel_cpu(p,work,tasks[i],cus);
        } else if(n->kind==QP_JOIN && n->hash && right) {
            double build=Max(0,right->estimated_rows),width=cost_hash_row(&right->expr);
            available=Max((double)QS_SORT_IO_BYTES,Min(available,memory-2*quota));
            state=cost_hash_state(build,width);
            parallel_cpu(p,build*(cpu_tuple_cost+cpu_operator_cost*Max(1,n->nkeys))*unit,tasks[n->right],cus);
            /* Coordinator links buckets after worker append; do not divide. */
            p->serial+=build*cpu_operator_cost*unit;
            parallel_cpu(p,(input*(cpu_operator_cost*Max(1,n->nkeys))+rows*(cpu_tuple_cost+expression))*unit,tasks[i],cus);
            if(state>available) {
                double partitions=4,ratio=state/available;
                spill=2*(build*width+input*cost_hash_row(&left->expr));
                /* Oversized/skew partitions use build batches, not recursive
                 * radix repartition: account for repeated probe reads. */
                if(ratio>partitions) spill+=input*cost_hash_row(&left->expr)*(ceil(ratio/partitions)-1);
            }
        } else if(n->kind==QP_AGGREGATE) {
            double groups=Max(1,n->estimated_groups),group_tasks=n->left>=0?tasks[n->left]:0;
            if(!n->nkeys) {
                state=(cus+1)*n->expr.naggs*(3*sizeof(struct qs_cell)+16);
                parallel_cpu(p,work,group_tasks,cus);
                p->combine+=cus*n->expr.naggs*cpu_operator_cost*unit;
            } else if(n->hash && qp_hash_aggregate_supported(n)) {
                available=Max((double)QS_SORT_IO_BYTES,available-2*quota);
                double table=cost_group_state(n,groups);
                bool locals=cus>1 && group_tasks>1 && (cus+1)*table<=available;
                state=locals?(cus+1)*table:table;
                parallel_cpu(p,input*(cpu_tuple_cost+expression)*unit,group_tasks,cus);
                if(locals) parallel_cpu(p,cus*groups*n->expr.naggs*cpu_operator_cost*unit,ceil(groups/QP_VECTOR_ROWS),cus);
                else if(group_tasks>1) p->serial+=input*cpu_operator_cost*Max(1,n->nkeys)*unit;
                p->combine+=groups*n->expr.naggs*cpu_operator_cost*unit;
                if(table>available) {
                    double row=(n->nkeys+2*n->expr.naggs)*sizeof(struct qs_cell);
                    spill=2*input*row; /* flushed states, bounded by input */
                    double runs=Max(2,ceil(table/available));
                    p->combine+=input*log2(runs)*cpu_operator_cost*unit;
                }
            } else if(n->sorted_input && !n->distinct && !n->distinct_aggs) {
                state=n->expr.naggs*(3*sizeof(struct qs_cell)+16);
                p->serial+=work;
            } else {
                struct cost_sort sort=cost_sort_state(input,sort_width(tree,n->left),Min(quota,available));
                state=sort.state; spill=sort.spill;
                parallel_cpu(p,sort.comparisons*cpu_operator_cost*unit,group_tasks,cus);
                p->combine+=(sort.merge*cpu_operator_cost*unit)+work;
            }
        } else if(n->kind==QP_SORT) {
            double sort_memory=Max(quota,Max(available/2,available-2*quota));
            struct cost_sort sort=cost_sort_state(input,sort_width(tree,i),sort_memory);
            state=sort.state; spill=sort.spill;
            parallel_cpu(p,sort.comparisons*cpu_operator_cost*unit,n->left>=0?tasks[n->left]:0,cus);
            p->combine+=sort.merge*cpu_operator_cost*unit+input*cpu_tuple_cost*unit;
        } else if(n->kind==QP_MATERIALIZED || n->kind==QP_SET) {
            state=n->kind==QP_SET?cost_slots(Max(rows,input)*2,64)*16:input*Max(1,n->expr.nout)*sizeof(struct qs_cell);
            spill=2*Max(0,state-available); p->serial+=work;
        } else if(n->kind==QP_JOIN && right) {
            /* All non-index/non-hash device joins use Merge Join, regardless
             * of the original PG node name. Preserve the two sort contexts. */
            const struct qp_node *inputs[]={left,right};
            int ids[]={n->left,n->right};
            const int *keys[]={n->left_keys,n->right_keys};
            for(unsigned side=0;side<2;++side) if(!cost_ordered(tree,ids[side],n->nkeys,keys[side])) {
                struct cost_sort sort=cost_sort_state(Max(0,inputs[side]->estimated_rows),
                    sort_width(tree,ids[side]),Min(quota,available/2));
                state+=sort.state; spill+=sort.spill;
                p->serial+=(sort.comparisons+sort.merge)*cpu_operator_cost*unit;
            }
            p->serial+=((input+Max(0,right->estimated_rows))*cpu_operator_cost*Max(1,n->nkeys)+
                rows*(cpu_tuple_cost+expression))*unit;
        } else parallel_cpu(p,work,tasks[i],cus);
        p->spill+=spill;
        double resident=Min(state,available);
        if(n->kind==QP_JOIN && n->hash) {
            peak[i]=Max(rp+Min(state,128*(right?cost_hash_row(&right->expr)+12:0)),
                resident+Max(r,lp));
            live[i]=resident+l+rr;
        } else if(n->kind==QP_SORT || n->kind==QP_AGGREGATE || n->kind==QP_MATERIALIZED || n->kind==QP_SET) {
            peak[i]=Max(Max(lp,rp),resident+l+r);
            live[i]=resident+lr+rr;
        } else { peak[i]=Max(lp,rp); live[i]=l+r; }
        retained[i]=lr+rr+((n->kind==QP_MATERIALIZED || n->kind==QP_SET)?resident:0);
        peak[i]=Max(peak[i],live[i])+scratch;
        double dictionary_live=Min(p->dictionary,Max(dictionary_meta,memory-peak[i]));
        dictionary_evicted=Max(dictionary_evicted,p->dictionary-dictionary_live);
        p->peak=Max(p->peak,peak[i]+dictionary_live);
        appendStringInfo(p->stages,"%s%d/%s tasks=%.0f cpu=%.3fms state=%.0fB resident=%.0fB available=%.0fB spill=%.0fB",
            p->stages->len?"; ":"",i,device_kind(n->kind),tasks[i],
            p->serial+p->parallel+p->schedule+p->combine-previous_cpu,state,resident,available,spill);
    }
    /* One CSF task executes a whole pipeline, not one request per SQL node.
     * Charge its wave dispatch once even when several nodes share tasks. */
    if(cus>1) for(unsigned i=0;i<tree->count;++i)
        if(used[i] && tree->nodes[i].parallel && tasks[i]>1)
            p->schedule+=cost_parallel(0,tasks[i],cus,pg_sina_parallel_task_ms);
    p->cache=scan_bytes?cache_bytes/scan_bytes:0;
    p->spill+=2*dictionary_evicted;
    io+=transfer_ms(p->spill,pg_sina_internal_mbps);
    p->io=io; p->cpu=p->serial+p->parallel+p->schedule+p->combine;
    p->population=pg_sina_hpcp_enabled()?transfer_ms(population_bytes,pg_sina_pcie_mbps):0;
    double device=io+p->cpu;
    if(pg_sina_hpcp_enabled()) device=Max(device,p->population)+blocks*0.002;
    p->csd=device+2;
    p->boundary=transfer_ms(output_bytes(p->native,&tree->nodes[tree->root].expr),pg_sina_pcie_mbps)+
        Max(0,p->native->plan_rows)*cpu_tuple_cost*pg_sina_host_cost_ms+
        Max(1,ceil(p->native->plan_rows/qp_result_capacity(&tree->nodes[tree->root].expr,1)))*pg_sina_result_batch_ms;
    pfree(tree);
}
static struct placement *estimate(PlannerInfo *root,Plan *native,Plan *scope,List **observations,bool top,bool mark_restore)
{
    struct placement *p=palloc0(sizeof(*p)); p->native=native; p->root=root; p->csd=DBL_MAX;
    /* Adaptive placement keeps Gather indivisible. Filter-only may replace
     * its scans using the executor's shared chunk allocator. */
    {
        if(native->lefttree) p->children=lappend(p->children,estimate(root,native->lefttree,scope,observations,false,false));
        if(native->righttree) p->children=lappend(p->children,estimate(root,native->righttree,scope,observations,false,
            IsA(native,MergeJoin) && !((MergeJoin *)native)->skip_mark_restore));
        if(IsA(native,SubqueryScan)) {
            RelOptInfo *rel=root->simple_rel_array[((SubqueryScan *)native)->scan.scanrelid];
            if(rel && rel->subroot) p->children=lappend(p->children,
                estimate(rel->subroot,((SubqueryScan *)native)->subplan,((SubqueryScan *)native)->subplan,observations,false,false));
        }
    }
    double original_children=0,best_children=0,native_children=0;
    ListCell *c;
    foreach(c,p->children) {
        struct placement *child=lfirst(c);
        double repeats=IsA(native,NestLoop) && child->native==native->righttree && !IsA(child->native,Material)?Max(1,native->lefttree->plan_rows):1;
        original_children+=child->native->total_cost*pg_sina_host_cost_ms*repeats;
        native_children+=child->native_host*repeats;
        best_children+=((IsA(native,Gather) || IsA(native,GatherMerge))?child->native_host:
            child->offload?child->csd+child->boundary:child->host)*repeats;
    }
    double local=Max(0,native->total_cost*pg_sina_host_cost_ms-original_children);
    Oid oid=scan_relation(root,native);
    if(IsA(native,IndexOnlyScan)) oid=((IndexOnlyScan *)native)->indexid;
    if(OidIsValid(oid)) {
        struct residency *r=residency(oid,observations);
        double read=r->bytes;
        if(!IsA(native,SeqScan)) read*=Min(1.0,Max(1,native->plan_rows)/r->tuples*4);
        local-=Min(local,read/BLCKSZ*seq_page_cost*pg_sina_host_cost_ms)*r->fraction;
        local+=transfer_ms(read*r->fraction,pg_sina_cache_mbps);
    }
    p->host=local+best_children;
    p->native_host=local+native_children;
    bool eligible=pg_sina_exe_path!=PG_SINA_FILTER ||
        IsA(native,SeqScan) || IsA(native,IndexScan) || IsA(native,IndexOnlyScan);
    /* HOST HashJoin invokes MultiExec on its Hash child, and MergeJoin may
     * mark/restore its inner child. A tuple-stream CustomScan is neither.
     * Keep those native nodes; their inputs can still be device islands. */
    eligible=eligible && !IsA(native,Hash) && !mark_restore;
    /* An island cannot consume parameters produced in a different host
     * InitPlan. Whole-query conversion keeps the existing device protocol. */
    if(eligible && (top || root->init_plans==NIL)) {
        Plan *candidate=pg_sina_exe_path==PG_SINA_FILTER?pg_sina_project_scan(scope,native):native;
        Plan *device=pg_sina_compile_subtree(root,candidate,!top);
        if(device!=candidate) { p->device=device; device_cost(p,observations); }
    }
    p->offload=p->device && (pg_sina_exe_path!=PG_SINA_ADAPTIVE || p->csd+p->boundary<p->host);
    return p;
}
static const char *plan_name(Plan *p)
{
    switch(nodeTag(p)) {
    case T_SeqScan:return "SeqScan"; case T_IndexScan:return "IndexScan";
    case T_IndexOnlyScan:return "IndexOnlyScan"; case T_HashJoin:return "HashJoin";
    case T_MergeJoin:return "MergeJoin"; case T_NestLoop:return "NestLoop";
    case T_Agg:return "Aggregate"; case T_Sort:return "Sort";
    case T_Gather:return "Gather"; case T_GatherMerge:return "GatherMerge";
    case T_Limit:return "Limit"; case T_Hash:return "Hash";
    case T_Material:return "Materialize"; case T_SubqueryScan:return "SubqueryScan";
    case T_Result:return "Projection"; default:return "Native";
    }
}
static void node_costs(struct placement *p,StringInfo text)
{
    appendStringInfo(text,"%s%s HOST=%.3f CSD=",text->len?"; ":"",plan_name(p->native),p->host);
    if(p->device) appendStringInfo(text,"%.3f boundary=%.3f cache=%.1f%% spill=%.0fB",
        p->csd,p->boundary,p->cache*100,p->spill);
    if(p->device) appendStringInfo(text," serial=%.3f parallel=%.3f schedule=%.3f combine=%.3f peak=%.0fB dictionary=%.0fB workers=%u",
        p->serial,p->parallel,p->schedule,p->combine,p->peak,p->dictionary,p->workers);
    else appendStringInfoString(text,"unsupported");
    ListCell *c;
    foreach(c,p->children) node_costs(lfirst(c),text);
}
static Plan *select_plan(struct placement *p,bool top,StringInfo decisions)
{
    appendStringInfo(decisions,"%s%s:%s host=%.3f device=",decisions->len?"; ":"",
        plan_name(p->native),p->offload?"CSD":"HOST",p->host);
    if(p->device) appendStringInfo(decisions,"%.3f boundary=%.3f",p->csd,p->boundary);
    else appendStringInfoString(decisions,"unsupported");
    if(p->offload) {
        Plan *out=p->device;
        if(top) {
            Plan *fast=pg_sina_fast_plan(p->root,p->native);
            if(fast) out=fast;
            p->root->init_plans=NIL;
        }
        CustomScan *scan=(CustomScan *)out;
        List *costs=list_make5(
            makeFloat(psprintf("%.9g",p->host)),makeFloat(psprintf("%.9g",p->csd)),
            makeFloat(psprintf("%.9g",p->boundary)),makeFloat(psprintf("%.9g",p->cache)),
            makeFloat(psprintf("%.9g",p->spill)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->cpu)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->io)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->peak)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->population)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->serial)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->parallel)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->schedule)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->combine)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->dictionary)));
        costs=lappend(costs,makeFloat(psprintf("%.9g",p->fdm)));
        costs=lappend(costs,makeInteger(p->workers));
        costs=lappend(costs,makeString(p->stages->data));
        scan->custom_private=lappend(scan->custom_private,costs);
        return out;
    }
    Plan *out=p->native; ListCell *c;
    if(pg_sina_exe_path!=PG_SINA_FILTER && (IsA(out,Gather) || IsA(out,GatherMerge))) return out;
    foreach(c,p->children) {
        struct placement *child=lfirst(c); Plan *chosen=select_plan(child,false,decisions);
        if(child->native==out->lefttree) {
            if(pg_sina_exe_path==PG_SINA_FILTER && chosen!=child->native)
                pg_sina_remap_input(out,child->native->targetlist,chosen->targetlist);
            out->lefttree=chosen;
        }
        else if(child->native==out->righttree) out->righttree=chosen;
        else if(IsA(out,SubqueryScan)) ((SubqueryScan *)out)->subplan=chosen;
        if(chosen!=child->native && !chosen->parallel_safe) out->parallel_safe=out->parallel_aware=false;
    }
    return out;
}
void pg_sina_explain_placement(CustomScan *scan,ExplainState *es)
{
    ExplainPropertyText("Execution Mode",pg_sina_path_name(),es);
    ExplainPropertyText("HPCP",pg_sina_hpcp_enabled()?"enabled":"disabled",es);
    ExplainPropertyText("Input Source","internal NVM to FDM",es);
    if(list_length(scan->custom_private)>1) {
        List *v=lsecond(scan->custom_private);
        ExplainPropertyFloat("SQLerate HOST Cost","ms",floatVal(linitial(v)),3,es);
        ExplainPropertyFloat("SQLerate CSD Cost","ms",floatVal(lsecond(v)),3,es);
        ExplainPropertyFloat("SQLerate CSD to HOST Cost","ms",floatVal(lthird(v)),3,es);
        ExplainPropertyFloat("Host Page Cache Residency","percent",floatVal(list_nth(v,3))*100,2,es);
        ExplainPropertyFloat("Estimated Spill I/O","bytes",floatVal(list_nth(v,4)),0,es);
        ExplainPropertyFloat("Estimated CSD CPU","ms",floatVal(list_nth(v,5)),3,es);
        ExplainPropertyFloat("Estimated Internal I/O including Spill","ms",floatVal(list_nth(v,6)),3,es);
        ExplainPropertyFloat("Estimated Peak Workmem","bytes",floatVal(list_nth(v,7)),0,es);
        ExplainPropertyFloat("Estimated HPCP Link Work","ms",floatVal(list_nth(v,8)),3,es);
        ExplainPropertyFloat("Estimated CSD Serial CPU","ms",floatVal(list_nth(v,9)),3,es);
        ExplainPropertyFloat("Estimated CSD Parallel CPU","ms",floatVal(list_nth(v,10)),3,es);
        ExplainPropertyFloat("Estimated CSD Scheduling","ms",floatVal(list_nth(v,11)),3,es);
        ExplainPropertyFloat("Estimated CSD Combine CPU","ms",floatVal(list_nth(v,12)),3,es);
        ExplainPropertyFloat("Estimated Dictionary Working Set","bytes",floatVal(list_nth(v,13)),0,es);
        ExplainPropertyFloat("Estimated FDM Reservation","bytes",floatVal(list_nth(v,14)),0,es);
        ExplainPropertyInteger("Estimated CSD Worker Slots",NULL,intVal(list_nth(v,15)),es);
        ExplainPropertyText("CSD Stage Estimates",strVal(list_nth(v,16)),es);
    }
}
/* A transparent host wrapper exposes the complete placement decision even
 * when the chosen tree contains no CSD node. It performs no SQL operators. */
static const CustomExecMethods adaptive_exec;
static Node *adaptive_create(CustomScan *scan)
{
    CustomScanState *s=(void *)newNode(sizeof(*s),T_CustomScanState);
    s->methods=&adaptive_exec; return (Node *)s;
}
static void adaptive_begin(CustomScanState *s,EState *estate,int flags)
{ s->custom_ps=list_make1(ExecInitNode(linitial(((CustomScan *)s->ss.ps.plan)->custom_plans),estate,flags)); }
static TupleTableSlot *adaptive_exec_scan(CustomScanState *s) { return ExecProcNode(linitial(s->custom_ps)); }
static void adaptive_end(CustomScanState *s) { ExecEndNode(linitial(s->custom_ps)); }
static void adaptive_rescan(CustomScanState *s) { ExecReScan(linitial(s->custom_ps)); }
static void adaptive_explain(CustomScanState *s,List *ancestors,ExplainState *es)
{
    CustomScan *scan=(void *)s->ss.ps.plan;
    ExplainPropertyText("Execution Mode",pg_sina_path_name(),es);
    ExplainPropertyText("HPCP",pg_sina_hpcp_enabled()?"enabled for CSD islands":"disabled",es);
    ExplainPropertyText("Placement Costs","milliseconds; estimates, not execution timings",es);
    ExplainPropertyText("Placement",strVal(linitial(scan->custom_private)),es);
    ExplainPropertyText("Node Cost Estimates",strVal(lsecond(scan->custom_private)),es);
}
static const CustomExecMethods adaptive_exec={.CustomName="SQLeratePlan",.BeginCustomScan=adaptive_begin,
    .ExecCustomScan=adaptive_exec_scan,.EndCustomScan=adaptive_end,.ReScanCustomScan=adaptive_rescan,.ExplainCustomScan=adaptive_explain};
const CustomScanMethods pg_sina_adaptive_methods={.CustomName="SQLeratePlan",.CreateCustomScanState=adaptive_create};
Plan *pg_sina_adaptive_plan(PlannerInfo *root,Plan *native)
{
    List *observations=NIL;
    struct placement *p=estimate(root,native,native,&observations,true,false);
    StringInfoData decisions; initStringInfo(&decisions);
    StringInfoData costs; initStringInfo(&costs); node_costs(p,&costs);
    Plan *out=select_plan(p,true,&decisions);
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE) return out;
    CustomScan *scan=makeNode(CustomScan); scan->scan.plan=*out;
    NodeSetTag(scan,T_CustomScan);
    scan->scan.plan.lefttree=scan->scan.plan.righttree=NULL; scan->scan.plan.qual=NIL;
    scan->scan.plan.initPlan=NIL;
    scan->scan.plan.parallel_safe=scan->scan.plan.parallel_aware=false;
    scan->scan.plan.startup_cost=0;
    scan->scan.plan.total_cost=(p->offload?p->csd+p->boundary:p->host)/pg_sina_host_cost_ms;
    scan->custom_scan_tlist=copyObject(out->targetlist);
    scan->custom_plans=list_make1(out); scan->custom_private=list_make2(makeString(decisions.data),makeString(costs.data));
    scan->methods=&pg_sina_adaptive_methods;
    return (Plan *)scan;
}
