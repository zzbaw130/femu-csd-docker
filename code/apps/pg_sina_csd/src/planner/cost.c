/* Bottom-up placement of the PG-selected tree. Costs are milliseconds.
 * A device island starts at immutable base tables and returns rows to PG.
 * Host-produced intermediate rows cannot yet enter the device interpreter;
 * that edge has infinite cost, rather than pretending it is executable. */
#include "pg_internal.h"
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
static double output_bytes(Plan *p)
{
    /* The existing result ABI transfers a full fixed-width result prefix,
     * not only the projected columns. Price the bytes actually returned. */
    double rows=Max(0,p->plan_rows);
    return Max(1,ceil(rows/QS_RESULT_ROWS))*offsetof(struct qs_result,str_data)+
        rows*Max(0,p->plan_width-list_length(p->targetlist)*(int)sizeof(struct qs_cell));
}
static unsigned device_workers(const struct qp_plan *tree,List **observations)
{
    unsigned best=1,cus=Max(1,pg_sina_resources.cus);
    for(unsigned chunk=4*1024*1024;chunk>=1024*1024;chunk/=2) {
        double base=sizeof(struct qs_dict),stride=QS_PAGE;
        for(unsigned i=0;i<tree->count;++i) if(tree->nodes[i].kind==QP_SCAN) {
            const struct qp_node *n=&tree->nodes[i];
            double input=n->ordered_index?QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE:
                Min(residency(n->expr.relid,observations)->bytes,chunk);
            double vector=qs_chunk_bytes(n->ordered_index?256:QP_VECTOR_ROWS,n->expr.ncols);
            base+=input+vector; stride=Max(stride,input+vector);
        }
        double per_worker=sizeof(struct qp_control)+sizeof(struct qs_result)+QS_SORT_IO_BYTES;
        double free=pg_sina_resources.fdm_bytes-base-sizeof(*tree)-2*per_worker-65536;
        unsigned workers=free>0?Min(cus,1+(unsigned)(free/(stride+per_worker))):1;
        best=Max(best,workers);
    }
    return best;
}

static void device_cost(struct placement *p,List **observations)
{
    CustomScan *scan=(CustomScan *)p->device;
    Const *constant=linitial_node(Const,scan->custom_private);
    bytea *bytes=DatumGetByteaP(constant->constvalue);
    const struct qp_plan *tree=(const void *)VARDATA(bytes);
    double peak[QP_NODES]={0},live[QP_NODES]={0};
    bool used[QP_NODES]={0}; used[tree->root]=true;
    for(unsigned i=0;i<tree->ninit;++i) used[tree->init_nodes[i]]=true;
    for(unsigned pass=0;pass<tree->count;++pass) for(unsigned i=0;i<tree->count;++i) if(used[i]) {
        if(tree->nodes[i].left>=0) used[tree->nodes[i].left]=true;
        if(tree->nodes[i].right>=0) used[tree->nodes[i].right]=true;
    }
    double io=0,cpu=0,population_bytes=0,cache_bytes=0,scan_bytes=0,blocks=0;
    double memory=Max(1,pg_sina_query_memory());
    unsigned cus=device_workers(tree,observations);
    for(int i=(int)tree->count-1;i>=0;--i) if(used[i]) {
        const struct qp_node *n=&tree->nodes[i];
        double input=n->left>=0?Max(1,tree->nodes[n->left].estimated_rows):Max(1,n->estimated_rows);
        double width=n->expr.nout*sizeof(struct qs_cell)+24,state=0,read=0;
        double parallel=(n->kind==QP_SORT || n->kind==QP_LIMIT)?1:cus;
        if(n->kind==QP_SCAN) {
            struct residency *r=residency(n->expr.relid,observations);
            input=r->tuples; read=r->bytes;
            double hit=read*r->fraction;
            if(n->ordered_index) {
                double fraction=Min(1.0,Max(1,n->estimated_rows)/r->tuples*4);
                read=(n->ordered_index&4)?0:read*fraction;
                hit=read*r->fraction;
                if(n->expr.join.indexid) {
                    struct residency *index=residency(n->expr.join.indexid,observations);
                    read+=index->bytes; hit+=index->bytes*index->fraction;
                }
                input=Max(1,n->estimated_rows);
            }
            scan_bytes+=read; cache_bytes+=hit;
            io+=transfer_ms(read,pg_sina_internal_mbps);
            population_bytes+=read;
            blocks+=read/(n->ordered_index?QS_PAGE:4*1024*1024);
        } else if(n->kind==QP_JOIN || n->kind==QP_INDEX_JOIN) {
            if(n->right>=0) {
                const struct qp_node *right=&tree->nodes[n->right];
                state=Max(1,right->estimated_rows)*(right->expr.nout*sizeof(struct qs_cell)+32)*1.5;
                input+=Max(1,right->estimated_rows);
            }
        } else if(n->kind==QP_AGGREGATE) {
            state=Max(1,n->estimated_groups)*(n->nkeys*sizeof(struct qs_cell)+n->expr.naggs*2*sizeof(struct qs_cell)+32)*1.5;
        } else if(n->kind==QP_SORT || n->kind==QP_MATERIALIZED || n->kind==QP_SET) {
            state=input*width;
        }
        double children=(n->left>=0?live[n->left]:0)+(n->right>=0?live[n->right]:0);
        double available=Max((double)QS_SORT_IO_BYTES,memory-children);
        if(state>available) {
            /* Minimum external pass: write/read the nonresident payload.
             * Further merge/partition passes grow with the capacity ratio. */
            double passes=Max(1,ceil(log(state/available)/log(8.0)));
            p->spill+=2*(state-available)*passes;
        }
        live[i]=Min(state,available)+(state?0:children);
        peak[i]=Max(state+children,Max(n->left>=0?peak[n->left]:0,n->right>=0?peak[n->right]:0));
        cpu+=input*(cpu_tuple_cost+cpu_operator_cost*Max(1,n->expr.nexprs/4))*
            pg_sina_host_cost_ms*pg_sina_csd_cpu_ratio/parallel;
    }
    p->cache=scan_bytes?cache_bytes/scan_bytes:0;
    io+=transfer_ms(p->spill,pg_sina_internal_mbps);
    p->io=io; p->cpu=cpu; p->peak=peak[tree->root];
    p->population=pg_sina_hpcp_enabled()?transfer_ms(population_bytes,pg_sina_pcie_mbps):0;
    /* HPCP traverses the external link while device I/O/CUs continue.
     * Charge the uncovered transfer plus per-block metadata work. */
    double device=io+cpu;
    if(pg_sina_hpcp_enabled()) device=Max(device,transfer_ms(population_bytes,pg_sina_pcie_mbps))+blocks*0.002;
    p->csd=device+2; /* session/AFDM/program startup, calibrated separately */
    p->boundary=transfer_ms(output_bytes(p->native),pg_sina_pcie_mbps)+
        Max(0,p->native->plan_rows)*cpu_tuple_cost*pg_sina_host_cost_ms+
        Max(1,ceil(p->native->plan_rows/QS_RESULT_ROWS))*pg_sina_result_batch_ms;
}
static struct placement *estimate(PlannerInfo *root,Plan *native,List **observations,bool top,bool mark_restore)
{
    struct placement *p=palloc0(sizeof(*p)); p->native=native; p->root=root; p->csd=DBL_MAX;
    /* Gather is an indivisible host region unless its complete subtree can
     * be offloaded. Never replace a parallel worker's scan by a full scan. */
    {
        if(native->lefttree) p->children=lappend(p->children,estimate(root,native->lefttree,observations,false,false));
        if(native->righttree) p->children=lappend(p->children,estimate(root,native->righttree,observations,false,
            IsA(native,MergeJoin) && !((MergeJoin *)native)->skip_mark_restore));
        if(IsA(native,SubqueryScan)) {
            RelOptInfo *rel=root->simple_rel_array[((SubqueryScan *)native)->scan.scanrelid];
            if(rel && rel->subroot) p->children=lappend(p->children,
                estimate(rel->subroot,((SubqueryScan *)native)->subplan,observations,false,false));
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
        Plan *device=pg_sina_compile_subtree(root,native,!top);
        if(device!=native) { p->device=device; device_cost(p,observations); }
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
        scan->custom_private=lappend(scan->custom_private,costs);
        return out;
    }
    Plan *out=p->native; ListCell *c;
    if(IsA(out,Gather) || IsA(out,GatherMerge)) return out;
    foreach(c,p->children) {
        struct placement *child=lfirst(c); Plan *chosen=select_plan(child,false,decisions);
        if(child->native==out->lefttree) out->lefttree=chosen;
        else if(child->native==out->righttree) out->righttree=chosen;
        else if(IsA(out,SubqueryScan)) ((SubqueryScan *)out)->subplan=chosen;
        if(chosen!=child->native) out->parallel_safe=out->parallel_aware=false;
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
    struct placement *p=estimate(root,native,&observations,true,false);
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
