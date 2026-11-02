/* Compiled device costs and the legacy filter/forced subtree placement.
 * SQLerate adds these costs to PG Paths in paths.c. Costs are milliseconds.
 * A device island starts at immutable base tables and returns rows to PG.
 * Host-produced intermediate rows cannot yet enter the device interpreter;
 * that edge has infinite cost, rather than pretending it is executable. */
#include "pg_internal.h"
#include "miscadmin.h"
#include "cost_model.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_constraint.h"
#include "catalog/namespace.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "access/relation.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_am_d.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/rel.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/spccache.h"
#include <float.h>
#include <math.h>

struct host_scan_cost { NodeTag type; Oid index; double rows,before,after,startup_after,cpu; int workers; };
struct host_lookup_cost { Oid index; JoinType type; double rows,before,inner,correction; };
struct residency {
    Oid oid;
    double bytes,tuples,fraction,shared;
    List *scans,*lookups;
};
struct placement {
    Plan *native,*device;
    PlannerInfo *root;
    List *children;
    double host,native_host,csd,boundary,cache,spill,io,cpu,peak,population,rows;
    double serial,parallel,schedule,combine,dictionary,fdm;
    unsigned workers;
    StringInfo stages;
    bool offload,details;
};
static double transfer_ms(double bytes,double mbps) { return bytes/(mbps*1000); }
/* Read the connection snapshot only. Catalog row counts/physical sizes remain
 * PG planner metadata; descriptor scans and mincore run outside planning. */
static struct residency *residency(Oid oid,List **observations)
{
    ListCell *c;
    foreach(c,*observations) { struct residency *r=lfirst(c); if(r->oid==oid) return r; }
    double mark=pg_sina_planner_clock(),cpu=pg_sina_planner_cpu();
    struct residency *r=MemoryContextAllocZero(pg_sina_search_context(),sizeof(*r)); r->oid=oid;
    Relation rel=relation_open(oid,AccessShareLock);
    r->bytes=(double)RelationGetNumberOfBlocks(rel)*BLCKSZ;
    r->tuples=Max(1,rel->rd_rel->reltuples);
    const struct pg_sina_cache_entry *entry=pg_sina_cache_lookup(rel->rd_node);
    if(entry) {
        r->shared=entry->blocks?Min(1.0,(double)entry->shared_pages/entry->blocks):0;
        r->fraction=entry->file_known && entry->file_total?Min(1.0,(double)entry->file_pages/entry->file_total):0;
    }
    if(pg_sina_planning) ereport(NOTICE,(errmsg("SQLerate relation cache relid=%u shared_percent=%.2f file_percent=%.2f pages=%.0f generation=%u file_generation=%u",
        oid,r->shared*100,r->fraction*100,r->bytes/BLCKSZ,entry?entry->generation:0,entry?entry->file_generation:0)));
    relation_close(rel,AccessShareLock);
    MemoryContext previous=MemoryContextSwitchTo(pg_sina_search_context());
    *observations=lappend(*observations,r);
    MemoryContextSwitchTo(previous);
    if(pg_sina_planning) {
        double elapsed=pg_sina_planner_clock()-mark;
        pg_sina_planning->cache_metadata+=elapsed;
        pg_sina_planning->cache_probe+=elapsed;
        pg_sina_planning->cache_cpu+=pg_sina_planner_cpu()-cpu;
        ++pg_sina_planning->cache_probes;
    }
    return r;
}
static Oid scan_relation(PlannerInfo *root,Plan *p)
{
    if(IsA(p,SeqScan) || IsA(p,IndexScan) || IsA(p,IndexOnlyScan) || IsA(p,BitmapHeapScan)) {
        RangeTblEntry *r=rt_fetch(((Scan *)p)->scanrelid,root->parse->rtable);
        if(r->rtekind==RTE_RELATION) return r->relid;
    }
    return InvalidOid;
}
/* Retain PG's CPU/qual/parallel costs. Only replace its actual scan I/O
 * component. Shared hits need no kernel->buffer copy; file-only hits do.
 * Aggregate residency does not identify overlap, so max(shared,file) is a
 * conservative union, never shared+file counted twice. */
static double cached_io(double io,double page_cost,struct residency *r)
{
    double copy=page_cost>0?transfer_ms(BLCKSZ,pg_sina_cache_mbps)/(page_cost*pg_sina_host_cost_ms):0;
    return cost_host_cached_io(io,r->shared,r->fraction,copy);
}
static pg_noinline void index_cpu_cost(IndexPath *path,PlannerInfo *root)
{
    double saved_seq=seq_page_cost,saved_random=random_page_cost;
    /* Backend-local prices; never leak a temporary price through ERROR. */
    PG_TRY();
    { seq_page_cost=random_page_cost=0; cost_index(path,root,1,false); }
    PG_CATCH();
    { seq_page_cost=saved_seq; random_page_cost=saved_random; PG_RE_THROW(); }
    PG_END_TRY();
    seq_page_cost=saved_seq; random_page_cost=saved_random;
}
/* ExecQual stops at the first failed conjunct. Keep PG's security/cost
 * ordering and estimate prefix survival with its existing selectivity code;
 * do not charge every later conjunct to every Heap tuple. */
static double host_qual_discount(PlannerInfo *root,Path *path)
{
    List *clauses=path->parent->baserestrictinfo;
    unsigned count=list_length(clauses);
    if(path->param_info || count<2) return 0;
    struct qual_price { RestrictInfo *clause; double cost; Index security; };
    struct qual_price *items=palloc(count*sizeof(*items));
    unsigned at=0; ListCell *c;
    foreach(c,clauses) {
        RestrictInfo *r=lfirst(c); QualCost cost;
        cost_qual_eval_node(&cost,(Node *)r,root);
        struct qual_price item={r,cost.per_tuple,
            r->leakproof && cost.per_tuple<10*cpu_operator_cost?0:r->security_level};
        unsigned i=at++;
        while(i && (item.security<items[i-1].security ||
              (item.security==items[i-1].security && item.cost<items[i-1].cost))) {
            items[i]=items[i-1]; --i;
        }
        items[i]=item;
    }
    List *prefix=NIL; double survival=1,saved=0;
    for(unsigned i=0;i<count;++i) {
        saved+=items[i].cost*(1-survival);
        prefix=lappend(prefix,items[i].clause);
        survival=clauselist_selectivity(root,prefix,path->parent->relid,JOIN_INNER,NULL);
    }
    list_free(prefix); pfree(items);
    double divisor=path->parallel_workers+(parallel_leader_participation?Max(0,1-0.3*path->parallel_workers):0);
    return saved*path->parent->tuples/Max(1,divisor);
}
void pg_sina_cost_host_path(PlannerInfo *root,Path *path,Oid oid,List **observations,bool apply)
{
    if(path->pathtype!=T_SeqScan && !IsA(path,IndexPath)) return;
    /* Internal SPI/temp relations cannot become immutable device inputs. */
    if(oid<FirstNormalObjectId || get_rel_persistence(oid)==RELPERSISTENCE_TEMP ||
       get_rel_namespace(oid)==get_namespace_oid("sina_csd",true)) return;
    struct residency *heap=residency(oid,observations),*index=NULL;
    double before=path->total_cost,heap_io=0,index_io=0,single_heap_io=0,single_index_io=0,seq,random,index_random=0;
    Oid indexid=InvalidOid;
    get_tablespace_page_costs(path->parent->reltablespace,&random,&seq);
    double qual_discount=0;
    if(path->pathtype==T_SeqScan) {
        heap_io=Min(before-path->startup_cost,path->parent->pages*seq);
        qual_discount=host_qual_discount(root,path);
    }
    else {
        IndexPath *original=(IndexPath *)path;
        if(original->indexinfo->relam!=BTREE_AM_OID) return;
        indexid=original->indexinfo->indexoid;
        index=residency(indexid,observations);
        get_tablespace_page_costs(original->indexinfo->reltablespace,&index_random,NULL);
        IndexPath cpu=*original;
        /* PG12 has no public cost_index CPU/I/O breakdown. Recost a private
         * Path with zero default page prices, leaving all CPU prices intact.
         * These globals are backend-local; restore even if PG raises ERROR.
         * Explicit tablespace prices remain in the floor (conservative).
         * B-tree CPU costs do not depend on loop_count; its original I/O
         * estimates, including repeated-probe caching, come from original. */
        index_cpu_cost(&cpu,root);
        index_io=Max(0,original->indextotalcost-cpu.indextotalcost);
        heap_io=Max(0,before-cpu.path.total_cost-index_io);
        IndexPath single=*original; cost_index(&single,root,1,false);
        single_index_io=Max(0,single.indextotalcost-cpu.indextotalcost);
        single_heap_io=Max(0,single.path.total_cost-cpu.path.total_cost-single_index_io);
        /* Cache hits remove I/O, not B-tree descent, buffer pinning or
         * comparisons. Keep PG's entire index CPU price for every probe. */
    }
    double startup=path->startup_cost;
    /* PG already amortizes repeated probes. Compare observed residency
     * against single-probe I/O; do not discount the reused residual again. */
    double cached_heap=index?Min(heap_io,cached_io(single_heap_io,random,heap)):
        cost_host_scan_io(heap_io*pg_sina_host_cost_ms,
            transfer_ms(heap->bytes,pg_sina_internal_mbps),transfer_ms(heap->bytes,pg_sina_pcie_mbps),
            transfer_ms(heap->bytes,pg_sina_cache_mbps),heap->shared,heap->fraction)/pg_sina_host_cost_ms;
    double cached_index=index?Min(index_io,cached_io(single_index_io,index_random,index)):0;
    double after=Max(startup,before-heap_io-index_io-qual_discount+cached_heap+cached_index);
    if(apply) {
        if(index) ((IndexPath *)path)->indextotalcost-=index_io-cached_index;
        path->startup_cost=startup;
        path->total_cost=after;
    }
    {
        struct host_scan_cost *scan=MemoryContextAlloc(pg_sina_search_context(),sizeof(*scan));
        *scan=(struct host_scan_cost){.type=path->pathtype,.index=indexid,.rows=path->rows,.before=before,.after=after,.startup_after=startup,
            .cpu=Max(0,before-heap_io-index_io-qual_discount-startup),.workers=path->parallel_workers};
        MemoryContext previous=MemoryContextSwitchTo(pg_sina_search_context());
        heap->scans=lappend(heap->scans,scan); MemoryContextSwitchTo(previous);
    }
}
/* A parameterized index is originally priced using its required base rels,
 * before intervening joins filter the probes. Reprice for this outer Path.
 * For a pure FK lookup, use PG's join fanout for CPU visits: the index's
 * sampled NDV may disagree with PG's matched-FK join estimate. */
Path *pg_sina_reprice_index_join(PlannerInfo *root,NestPath *native,JoinPathExtraData *extra,List **observations)
{
    if(!IsA(native->innerjoinpath,IndexPath)) return &native->path;
    IndexPath *original=(IndexPath *)native->innerjoinpath;
    if(!original->path.param_info || original->indexinfo->relam!=BTREE_AM_OID) return &native->path;
    IndexPath *inner=palloc(sizeof(*inner)); *inner=*original;
    Path *outer=native->outerjoinpath;
    double divisor=native->path.parallel_workers+
        (parallel_leader_participation?Max(0,1-0.3*native->path.parallel_workers):0);
    cost_index(inner,root,Max(1,outer->rows*Max(1,divisor)),false);
    IndexPath cpu=*inner; index_cpu_cost(&cpu,root);
    Oid oid=rt_fetch(inner->path.parent->relid,root->parse->rtable)->relid;
    pg_sina_cost_host_path(root,&inner->path,oid,observations,true);
    if(native->jointype==JOIN_INNER && !native->joinrestrictinfo &&
       list_length(inner->indexclauses)==1 && !inner->path.parent->baserestrictinfo &&
       ((IndexClause *)linitial(inner->indexclauses))->indexcol==0 &&
       !((IndexClause *)linitial(inner->indexclauses))->lossy) {
        ListCell *c; foreach(c,root->fkey_list) {
            ForeignKeyOptInfo *fk=lfirst(c);
            if(fk->nkeys!=1 || fk->con_relid!=inner->path.parent->relid ||
               !bms_is_member(fk->ref_relid,outer->parent->relids) ||
               inner->indexinfo->indexkeys[0]!=fk->conkey[0] ||
               fk->nmatched_ec+fk->nmatched_rcols!=1) continue;
            double visits=Min(inner->path.rows,Max(1,native->path.rows/Max(1,outer->rows)));
            double run=Max(0,cpu.path.total_cost-cpu.path.startup_cost);
            inner->path.total_cost=Max(inner->path.startup_cost,
                inner->path.total_cost-run*(1-visits/Max(1,inner->path.rows)));
            inner->path.rows=visits;
            break;
        }
    }
    NestPath *out=palloc(sizeof(*out)); *out=*native; out->innerjoinpath=&inner->path;
    JoinCostWorkspace workspace;
    initial_cost_nestloop(root,&workspace,out->jointype,outer,&inner->path,extra);
    final_cost_nestloop(root,out,&workspace,extra);
    return &out->path;
}
/* Preserve PG's own SEMI/ANTI early-stop and rescan pricing. Discounting one
 * full inner scan per outer row would subtract work PG never charged. */
void pg_sina_cost_host_lookup(PlannerInfo *root,Path *path,JoinPathExtraData *extra,List **observations)
{
    if(!IsA(path,NestPath)) return;
    NestPath *native=(NestPath *)path; Path *inner=native->innerjoinpath;
    if(!IsA(inner,IndexPath) || inner->rows<=1 ||
       (native->jointype!=JOIN_SEMI && native->jointype!=JOIN_ANTI && !native->inner_unique)) return;
    Oid index=((IndexPath *)inner)->indexinfo->indexoid;
    struct residency *relation=NULL; const struct host_scan_cost *price=NULL; ListCell *c,*s;
    foreach(c,*observations) {
        struct residency *r=lfirst(c);
        foreach(s,r->scans) {
            const struct host_scan_cost *scan=lfirst(s);
            if(scan->index==index && scan->type==inner->pathtype &&
               scan->rows==inner->rows && scan->before==inner->total_cost) { relation=r; price=scan; break; }
        }
        if(price) break;
    }
    if(!price || price->before==price->after) return;
    NestPath repriced=*native; JoinCostWorkspace workspace;
    double startup=inner->startup_cost,total=inner->total_cost;
    PG_TRY();
    {
        inner->startup_cost=price->startup_after; inner->total_cost=price->after;
        initial_cost_nestloop(root,&workspace,native->jointype,native->outerjoinpath,inner,extra);
        final_cost_nestloop(root,&repriced,&workspace,extra);
    }
    PG_CATCH();
    { inner->startup_cost=startup; inner->total_cost=total; PG_RE_THROW(); }
    PG_END_TRY();
    inner->startup_cost=startup; inner->total_cost=total;
    struct host_lookup_cost *record=MemoryContextAlloc(pg_sina_search_context(),sizeof(*record));
    *record=(struct host_lookup_cost){.index=index,.type=native->jointype,.rows=path->rows,
        .before=path->total_cost,.inner=total,.correction=repriced.path.total_cost-path->total_cost};
    MemoryContext previous=MemoryContextSwitchTo(pg_sina_search_context());
    relation->lookups=lappend(relation->lookups,record); MemoryContextSwitchTo(previous);
}
static bool host_lookup_correction(Plan *plan,List **observations,double *correction)
{
    if(!IsA(plan,NestLoop) || !plan->righttree ||
       (!IsA(plan->righttree,IndexScan) && !IsA(plan->righttree,IndexOnlyScan))) return false;
    Oid index=IsA(plan->righttree,IndexScan)?((IndexScan *)plan->righttree)->indexid:
        ((IndexOnlyScan *)plan->righttree)->indexid;
    ListCell *c,*l;
    foreach(c,*observations) foreach(l,((struct residency *)lfirst(c))->lookups) {
        const struct host_lookup_cost *record=lfirst(l);
        if(record->index==index && record->type==((NestLoop *)plan)->join.jointype &&
           record->rows==plan->plan_rows && record->before==plan->total_cost &&
           record->inner==plan->righttree->total_cost) {
            *correction=record->correction*pg_sina_host_cost_ms; return true;
        }
    }
    return false;
}
/* Preserve the unmodified native PG plan as a candidate. Look up the same
 * selected scan's Path estimate, instead of inferring I/O from output rows. */
double pg_sina_host_scan_cost(Plan *plan,Oid oid,double cost,List **observations)
{
    struct residency *r=NULL; ListCell *cell;
    foreach(cell,*observations) { struct residency *candidate=lfirst(cell); if(candidate->oid==oid) { r=candidate; break; } }
    if(!r) return cost;
    Oid index=IsA(plan,IndexScan)?((IndexScan *)plan)->indexid:
        IsA(plan,IndexOnlyScan)?((IndexOnlyScan *)plan)->indexid:InvalidOid;
    foreach(cell,r->scans) {
        const struct host_scan_cost *scan=lfirst(cell);
        if(scan->type==nodeTag(plan) && scan->index==index && scan->rows==plan->plan_rows && scan->before==plan->total_cost) {
            if(pg_sina_planning) ereport(NOTICE,(errmsg("SQLerate HOST scan relid=%u indexid=%u rows=%.0f original=%.3fms cached=%.3fms",
                oid,index,plan->plan_rows,scan->before*pg_sina_host_cost_ms,scan->after*pg_sina_host_cost_ms)));
            return Max(0,cost+(scan->after-scan->before)*pg_sina_host_cost_ms);
        }
    }
    return cost;
}
static double host_parallel_divisor(int workers)
{
    return workers+(parallel_leader_participation?Max(0,1-0.3*workers):0);
}
/* A PG partial subtree may have been priced for more workers than its final
 * Gather launches (e.g. a 4-worker inner Parallel Hash below a 2-worker join).
 * Reconcile that CPU work before comparing final statements. Do not divide
 * I/O, serial/private inputs, or charge the whole subtree again at parents. */
static double host_parallel_cost(Plan *plan,List *rtable,List **observations,int workers,double *divisor)
{
    *divisor=0;
    if(!plan) return 0;
    bool gather=IsA(plan,Gather) || IsA(plan,GatherMerge);
    if(gather) workers=IsA(plan,Gather)?((Gather *)plan)->num_workers:((GatherMerge *)plan)->num_workers;
    double left=0,right=0;
    double extra=host_parallel_cost(plan->lefttree,rtable,observations,workers,&left);
    double repeats=IsA(plan,NestLoop) && plan->righttree && !IsA(plan->righttree,Material)?Max(1,plan->lefttree->plan_rows):1;
    extra+=host_parallel_cost(plan->righttree,rtable,observations,workers,&right)*repeats;
    double local=Max(0,plan->total_cost-(plan->lefttree?plan->lefttree->total_cost:0)-
        (plan->righttree?plan->righttree->total_cost*repeats:0));
    if(IsA(plan,SeqScan) || IsA(plan,IndexScan) || IsA(plan,IndexOnlyScan)) {
        RangeTblEntry *entry=rt_fetch(((Scan *)plan)->scanrelid,rtable);
        ListCell *c; foreach(c,*observations) {
            struct residency *r=lfirst(c); if(r->oid!=entry->relid) continue;
            ListCell *s; foreach(s,r->scans) {
                struct host_scan_cost *scan=lfirst(s);
                if(scan->type==nodeTag(plan) && scan->rows==plan->plan_rows &&
                   (scan->before==plan->total_cost || scan->after==plan->total_cost)) {
                    if(scan->workers) { left=host_parallel_divisor(scan->workers); local=scan->cpu; }
                    break;
                }
            }
        }
    }
    if(workers && left>0 && !gather) {
        double ratio=left/host_parallel_divisor(workers);
        extra+=local*(ratio-1);
    }
    if(IsA(plan,CustomScan)) {
        ListCell *c; foreach(c,((CustomScan *)plan)->custom_plans) {
            double child=0; extra+=host_parallel_cost(lfirst(c),rtable,observations,workers,&child);
        }
    }
    if(IsA(plan,SubqueryScan)) {
        double child=0; extra+=host_parallel_cost(((SubqueryScan *)plan)->subplan,rtable,observations,workers,&child);
    }
    *divisor=gather?0:left;
    return extra;
}
double pg_sina_host_parallel_cost(PlannedStmt *stmt)
{
    List **observations=pg_sina_search_observations(); double divisor=0;
    return observations?host_parallel_cost(stmt->planTree,stmt->rtable,observations,0,&divisor)*pg_sina_host_cost_ms:0;
}
static double statement_cache_cost(Plan *p,List *rtable,List **observations,double repeats)
{
    if(!p) return 0;
    double correction=0;
    if(IsA(p,SeqScan) || IsA(p,IndexScan) || IsA(p,IndexOnlyScan) || IsA(p,BitmapHeapScan)) {
        RangeTblEntry *r=rt_fetch(((Scan *)p)->scanrelid,rtable);
        if(r->rtekind==RTE_RELATION) {
            double cost=p->total_cost*pg_sina_host_cost_ms;
            correction=(pg_sina_host_scan_cost(p,r->relid,cost,observations)-cost)*repeats;
        }
    }
    correction+=statement_cache_cost(p->lefttree,rtable,observations,repeats);
    double inner=IsA(p,NestLoop) && p->righttree && !IsA(p->righttree,Material)?Max(1,p->lefttree->plan_rows):1;
    double lookup;
    if(host_lookup_correction(p,observations,&lookup)) correction+=lookup*repeats;
    else correction+=statement_cache_cost(p->righttree,rtable,observations,repeats*inner);
    if(IsA(p,SubqueryScan)) correction+=statement_cache_cost(((SubqueryScan *)p)->subplan,rtable,observations,repeats);
    if(IsA(p,CustomScan)) { ListCell *c; foreach(c,((CustomScan *)p)->custom_plans)
        correction+=statement_cache_cost(lfirst(c),rtable,observations,repeats); }
    return correction;
}
double pg_sina_host_statement_cost(PlannedStmt *stmt)
{
    List *observations=NIL;
    List **shared=pg_sina_search_observations();
    return Max(0,stmt->planTree->total_cost*pg_sina_host_cost_ms+
        statement_cache_cost(stmt->planTree,stmt->rtable,shared?shared:&observations,1));
}
/* HOST columns use physical scalar widths and variable-value offsets.
 * PG plan_width estimates average values; add the offset/null/header work
 * of this boundary, instead of charging 32 bytes for every projected value. */
static double output_variable_bytes(Plan *p,const struct qs_plan *output)
{
    unsigned fixed=0,variable=0;
    for(unsigned col=0;col<output->nout;++col) {
        unsigned oid=output->outputs[col].type_oid;
        if(qp_host_variable(oid)) ++variable; else fixed+=qp_host_width(oid);
    }
    return variable?Max((int)(4*variable),p->plan_width-(int)fixed)+4*variable:0;
}
static double output_capacity(Plan *p,const struct qs_plan *output)
{
    double variable=output_variable_bytes(p,output),rows=qp_host_capacity(output);
    if(variable>0) rows=Min(rows,Max(1,floor(QS_RESULT_STRING_BYTES/variable)));
    return rows;
}
static double output_bytes(Plan *p,const struct qs_plan *output,double rows)
{
    double width=(double)output->nout/8+output_variable_bytes(p,output);
    for(unsigned col=0;col<output->nout;++col) width+=qp_host_width(output->outputs[col].type_oid);
    rows=Max(0,rows);
    return rows*width+Max(1,ceil(rows/output_capacity(p,output)))*offsetof(struct qs_result,data);
}
/* Match executor layout selection: try smaller input blocks before fewer tasks.
 * File-extent descriptors are not yet available here; allow one page/node. */
static unsigned device_workers(struct qp_plan *tree,double *fdm)
{
    unsigned workers=1;
    bool indexed[QP_NODES]={0};
    for(unsigned i=0;i<tree->count;++i) {
        if(tree->nodes[i].parallel) workers=qp_task_slots(tree,pg_sina_resources.cus);
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
                input=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE+qs_index_input_bytes()+TYPEALIGN(4096,
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
                if(n->kind==QP_INDEX_JOIN) bytes+=QS_SORT_IO_BYTES+QS_INDEX_CACHE*QS_PAGE+qs_index_input_bytes()+
                    TYPEALIGN(4096,qs_chunk_bytes(256,tree->nodes[n->right].expr.ncols+2));
            }
            stride=Max(stride,bytes);
        }
        unsigned slots=workers>1?workers+1:1;
        *fdm=arena+(workers-1)*stride+sizeof(*tree)+qp_host_parameter_bytes(tree)+tree->count*4096+
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
/* Text heaps retain a window in addition to each run's row window. */
static unsigned sort_windows(const struct qs_plan *p)
{
    for(unsigned c=0;c<p->nout;++c) {
        unsigned oid=p->outputs[c].type_oid;
        if(oid==TEXTOID || oid==BPCHAROID || oid==VARCHAROID) return 2;
    }
    return 1;
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
/* Follow compiled column references, including joins and projections. Only
 * recognized expressions refine PG estimates; no query-specific row counts. */
static bool column_origin(const struct qp_plan *tree,int node,int expression,Oid *rel,int *att,bool *year)
{
    const struct qp_node *n=&tree->nodes[node];
    if(expression<0 || (unsigned)expression>=n->expr.nexprs) return false;
    const struct qs_expr *e=&n->expr.exprs[expression];
    if(e->op==Q_YEAR) { *year=true; return column_origin(tree,node,e->left,rel,att,year); }
    if(e->op!=Q_COLUMN || e->column<0) return false;
    if(n->kind==QP_SCAN) { *rel=n->expr.relid; *att=e->column+1; return true; }
    if(n->left<0) return false;
    int child=n->left,column=e->column;
    if((unsigned)column>=tree->nodes[child].expr.nout) {
        column-=tree->nodes[child].expr.nout; child=n->right;
    }
    if(child<0 || (unsigned)column>=tree->nodes[child].expr.nout) return false;
    const struct qs_output *out=&tree->nodes[child].expr.outputs[column];
    return !out->aggregate && column_origin(tree,child,out->index,rel,att,year);
}
static double relation_tuples(Oid rel)
{
    Relation relation=relation_open(rel,AccessShareLock);
    double rows=Max(1,relation->rd_rel->reltuples);
    relation_close(relation,AccessShareLock); return rows;
}
static double column_distinct(Oid rel,int att,bool year)
{
    HeapTuple tuple=SearchSysCache3(STATRELATTINH,ObjectIdGetDatum(rel),Int16GetDatum(att),BoolGetDatum(false));
    if(!HeapTupleIsValid(tuple)) return 0;
    Form_pg_statistic st=(Form_pg_statistic)GETSTRUCT(tuple);
    double rows=relation_tuples(rel);
    double distinct=st->stadistinct<0?-st->stadistinct*rows:st->stadistinct;
    if(year) {
        int low=INT_MAX,high=INT_MIN;
        for(int kind=STATISTIC_KIND_MCV;kind<=STATISTIC_KIND_HISTOGRAM;++kind) {
            AttStatsSlot slot;
            if(get_attstatsslot(&slot,tuple,kind,InvalidOid,ATTSTATSSLOT_VALUES)) {
                if(slot.valuetype==DATEOID) for(int i=0;i<slot.nvalues;++i) {
                    DateADT date=DatumGetDateADT(slot.values[i]); int y,m,d;
                    if(DATE_NOT_FINITE(date)) continue;
                    j2date(date+POSTGRES_EPOCH_JDATE,&y,&m,&d);
                    low=Min(low,y); high=Max(high,y);
                }
                free_attstatsslot(&slot);
            }
        }
        distinct=low<=high?Min(distinct,(double)high-low+1):0;
    }
    if(distinct>0 && st->stanullfrac>0) distinct+=1;
    ReleaseSysCache(tuple); return distinct;
}
/* A validated FK gives a key-domain capacity bound, not observed NDV or
 * join coverage. Use it for state sizing only, never filter selectivity. */
static double column_key_domain(Oid rel,int att)
{
    Relation relation=relation_open(rel,AccessShareLock); ListCell *c; double bound=0;
    foreach(c,RelationGetFKeyList(relation)) {
        ForeignKeyCacheInfo *fk=lfirst(c);
        if(fk->nkeys!=1 || fk->conkey[0]!=att) continue;
        HeapTuple constraint=SearchSysCache1(CONSTROID,ObjectIdGetDatum(fk->conoid));
        if(!HeapTupleIsValid(constraint)) continue;
        bool valid=((Form_pg_constraint)GETSTRUCT(constraint))->convalidated;
        ReleaseSysCache(constraint);
        if(valid) bound=Max(bound,relation_tuples(fk->confrelid)+1);
    }
    relation_close(relation,AccessShareLock); return bound;
}
/* PG can replace a self-join key by its referenced PK through an equivalence
 * class. A validated single-column FK proves the same key domain; it does
 * not prove that every referenced key is present or set its distinct count. */
static bool same_key_domain(Oid a,int x,Oid b,int y)
{
    if(a==b && x==y) return true;
    Oid relations[]={a,b}; int columns[]={x,y};
    for(unsigned side=0;side<2;++side) {
        Relation relation=relation_open(relations[side],AccessShareLock); ListCell *c; bool found=false;
        foreach(c,RelationGetFKeyList(relation)) {
            ForeignKeyCacheInfo *fk=lfirst(c);
            if(fk->nkeys!=1 || fk->conkey[0]!=columns[side] ||
               fk->confrelid!=relations[1-side] || fk->confkey[0]!=columns[1-side]) continue;
            HeapTuple tuple=SearchSysCache1(CONSTROID,ObjectIdGetDatum(fk->conoid));
            if(HeapTupleIsValid(tuple)) {
                found=((Form_pg_constraint)GETSTRUCT(tuple))->convalidated;
                ReleaseSysCache(tuple);
            }
            if(found) break;
        }
        relation_close(relation,AccessShareLock);
        if(found) return true;
    }
    return false;
}
/* Match the device's existing residual self-ANTI uncertainty rule in the
 * adaptive HOST search too. PG's original baseline is never modified. */
void pg_sina_refine_host_anti(PlannerInfo *root,RelOptInfo *join,RelOptInfo *outer,
    JoinType type,JoinPathExtraData *extra)
{
    if(type!=JOIN_ANTI || join->rows>1 || outer->rows<=1) return;
    bool domain=false,residual=false; ListCell *c;
    foreach(c,extra->restrictlist) {
        RestrictInfo *r=lfirst_node(RestrictInfo,c);
        if(!OidIsValid(r->hashjoinoperator)) { residual=true; continue; }
        if(!IsA(r->clause,OpExpr)) continue;
        OpExpr *op=(OpExpr *)r->clause; if(list_length(op->args)!=2) continue;
        Node *a=strip_implicit_coercions(linitial(op->args)),*b=strip_implicit_coercions(lsecond(op->args));
        if(!IsA(a,Var) || !IsA(b,Var)) continue;
        Var *x=(Var *)a,*y=(Var *)b;
        if(x->varlevelsup || y->varlevelsup || x->varattno<=0 || y->varattno<=0) continue;
        RangeTblEntry *left=rt_fetch(x->varno,root->parse->rtable),*right=rt_fetch(y->varno,root->parse->rtable);
        if(left->rtekind==RTE_RELATION && right->rtekind==RTE_RELATION &&
           same_key_domain(left->relid,x->varattno,right->relid,y->varattno)) domain=true;
    }
    if(!domain || !residual) return;
    double before=join->rows; join->rows=cost_anti_fallback(outer->rows);
    /* ANTI output means unmatched probes. Keep PG's early-stop accounting
     * consistent with the row estimate instead of still assuming all match. */
    extra->semifactors.outer_match_frac=1-join->rows/outer->rows;
    List *lists[]={join->pathlist,join->partial_pathlist};
    for(unsigned i=0;i<2;++i) foreach(c,lists[i]) {
        Path *path=lfirst(c); double divisor=path->parallel_workers?host_parallel_divisor(path->parallel_workers):1;
        path->rows=Max(path->rows,join->rows/divisor);
    }
    if(pg_sina_planning) ereport(NOTICE,(errmsg("SQLerate residual ANTI estimate rows=%.0f corrected=%.0f outer=%.0f source=shared_statistics_fallback",before,join->rows,outer->rows)));
}
static double group_correlation(const struct qp_plan *tree,int node)
{
    const struct qp_node *n=&tree->nodes[node]; Oid rel; int att; bool year=false;
    if(n->nkeys!=1 || !column_origin(tree,node,n->expr.keys[0],&rel,&att,&year)) return 0;
    HeapTuple tuple=SearchSysCache3(STATRELATTINH,ObjectIdGetDatum(rel),Int16GetDatum(att),BoolGetDatum(false));
    if(!HeapTupleIsValid(tuple)) return 0;
    AttStatsSlot slot; double correlation=0;
    if(get_attstatsslot(&slot,tuple,STATISTIC_KIND_CORRELATION,InvalidOid,ATTSTATSSLOT_NUMBERS)) {
        if(slot.nnumbers) correlation=slot.numbers[0];
        free_attstatsslot(&slot);
    }
    ReleaseSysCache(tuple); return correlation;
}
void pg_sina_refine_groups(struct qp_plan *tree)
{
    double original[QP_NODES];
    for(unsigned i=0;i<tree->count;++i) original[i]=tree->nodes[i].estimated_rows;
    for(int i=(int)tree->count-1;i>=0;--i) {
        struct qp_node *n=&tree->nodes[i];
        /* PG's independence estimate can clamp a residual self anti-join
         * to one row. Use DuckDB's semi/anti fallback in that uncertain case,
         * rather than making all downstream index probes nearly free. */
        bool anti=false;
        if((n->kind==QP_JOIN || n->kind==QP_INDEX_JOIN) && n->join_type==QP_ANTI &&
           original[i]<=1 && n->join_qual>=0 && (n->nkeys==1 || n->kind==QP_INDEX_JOIN) &&
           n->left>=0 && n->right>=0) {
            const struct qp_node *left=&tree->nodes[n->left],*right=&tree->nodes[n->right];
            Oid lrel,rrel=right->expr.relid; int latt,ratt=right->expr.join.build_key; bool ly=false,ry=false;
            bool key=n->kind==QP_INDEX_JOIN || ((unsigned)n->right_keys[0]<right->expr.nout &&
                column_origin(tree,n->right,right->expr.outputs[n->right_keys[0]].index,&rrel,&ratt,&ry));
            if(key && (unsigned)n->left_keys[0]<left->expr.nout &&
               column_origin(tree,n->left,left->expr.outputs[n->left_keys[0]].index,&lrel,&latt,&ly) &&
               !ly && !ry && same_key_domain(lrel,latt,rrel,ratt)) {
                n->estimated_rows=Max(original[i],cost_anti_fallback(left->estimated_rows)); anti=true;
            }
        }
        if(anti) continue;
        if(n->kind==QP_AGGREGATE && n->nkeys) {
            double groups=1; bool derived=false,known=true;
            for(unsigned k=0;k<n->nkeys;++k) {
                Oid rel; int att; bool year=false;
                if(!column_origin(tree,i,n->expr.keys[k],&rel,&att,&year)) { known=false; break; }
                double ndv=column_distinct(rel,att,year);
                if(ndv<=0) { known=false; break; }
                groups*=ndv; derived|=year;
            }
            if(known && derived && n->estimated_groups>groups) {
                double selectivity=Min(1.0,n->estimated_rows/Max(1,n->estimated_groups));
                n->estimated_groups=groups;
                n->estimated_rows=groups*selectivity;
            }
            /* A sampled FK NDV can severely under-reserve local/global hash
             * tables. Its referenced unique-key domain bounds state size;
             * it does not prove all parent keys occur or change output rows. */
            if(n->nkeys==1 && n->left>=0) {
                Oid rel; int att; bool year=false;
                if(column_origin(tree,i,n->expr.keys[0],&rel,&att,&year) && !year) {
                    double domain=column_key_domain(rel,att);
                    if(domain>0) n->estimated_groups=Max(n->estimated_groups,
                        Min(tree->nodes[n->left].estimated_rows,domain));
                }
            }
            for(int id=n->left;id>=0;id=tree->nodes[id].left) {
                const struct qp_node *join=&tree->nodes[id];
                if(join->kind==QP_AGGREGATE) break;
                if(join->kind!=QP_JOIN && join->kind!=QP_INDEX_JOIN) continue;
                for(unsigned side=0;side<2;++side) {
                    /* Index inner rows are per lookup, not a global bound.
                     * SEMI/ANTI expose only their left input's columns. */
                    if(side && (join->kind==QP_INDEX_JOIN ||
                       (join->join_type!=QP_INNER && join->join_type!=QP_LEFT))) continue;
                    int input=side?join->right:join->left; if(input<0) continue;
                    bool keys=true;
                    for(unsigned k=0;k<n->nkeys;++k)
                        if(!cost_join_expression(tree,i,n->expr.keys[k],id,side)) keys=false;
                    if(keys) {
                        double bound=tree->nodes[input].estimated_rows+(side && join->join_type==QP_LEFT?1:0);
                        n->estimated_groups=Min(n->estimated_groups,bound);
                        n->estimated_rows=Min(n->estimated_rows,bound);
                    }
                }
            }
        } else if(n->kind==QP_SORT && n->left>=0 && n->expr.predicate<0) {
            /* Sort preserves logical rows. A selected child may already
             * carry refined rows; scaling the parent again double-discounts. */
            n->estimated_rows=tree->nodes[n->left].estimated_rows;
        } else if(n->kind==QP_LIMIT && n->left>=0) {
            n->estimated_rows=Min(original[i],Max(0,tree->nodes[n->left].estimated_rows-n->expr.offset));
        } else if(n->left>=0 && original[n->left]>0) {
            n->estimated_rows*=tree->nodes[n->left].estimated_rows/original[n->left];
            /* Build/probe orientation can put a refined subtree on the right.
             * Inner-join cardinality depends on both inputs, not just left. */
            if(n->kind==QP_JOIN && n->join_type==QP_INNER && n->right>=0 && original[n->right]>0)
                n->estimated_rows*=tree->nodes[n->right].estimated_rows/original[n->right];
        }
    }
}
/* The fused single-table CSF keeps at most 256 groups per CU, flushes partial
 * states when full, and merges those states rather than sorting base rows. */
static void fast_cost(struct placement *p,const struct qs_plan *plan,List **observations)
{
    struct residency *r=residency(plan->relid,observations);
    unsigned cus=Min(QS_WORKERS,Max(1,pg_sina_resources.cus));
    double tasks=Max(1,ceil(r->bytes/QS_CHUNK)); cus=Min(cus,(unsigned)tasks);
    Plan *aggregate=p->native;
    while(aggregate && !IsA(aggregate,Agg)) aggregate=aggregate->lefttree;
    double groups=plan->nkeys && aggregate?Max(1,((Agg *)aggregate)->numGroups):1;
    double rows=Max(0,p->native->plan_rows),unit=pg_sina_host_cost_ms*pg_sina_csd_cpu_ratio;
    double partial=groups<=QS_GROUPS?Min(r->tuples,cus*groups):r->tuples;
    double fixed=cus*(QS_GROUPS*(QS_KEYS+QS_AGGS)*sizeof(struct qs_cell)+
        2*sizeof(struct qs_plan)+sizeof(struct qs_result)+plan->nexprs*(256*20+128.0)+8192);
    struct cost_sort sort=cost_sort_state(partial,(plan->nkeys+plan->naggs)*sizeof(struct qs_cell),
        Max((double)QS_SORT_IO_BYTES,pg_sina_query_memory()-fixed),sort_windows(plan));
    p->workers=cus; p->rows=rows; p->cache=r->fraction;
    p->parallel=cost_parallel(r->tuples*(cpu_tuple_cost+cpu_operator_cost*(cost_expression_ops(plan)+plan->naggs))*unit,tasks,cus,0);
    p->schedule=cost_parallel(0,tasks,cus,pg_sina_parallel_task_ms);
    p->combine=(sort.comparisons+sort.merge+partial*plan->naggs)*cpu_operator_cost*unit;
    p->spill=plan->nkeys?sort.spill:0;
    p->peak=Min((double)pg_sina_query_memory(),fixed+(plan->nkeys?sort.state:0));
    p->io=transfer_ms(r->bytes,pg_sina_internal_mbps)+
        cost_spill_io(p->spill,pg_sina_internal_mbps,pg_sina_internal_write_mbps);
    p->schedule+=ceil(p->spill/QS_SORT_IO_BYTES)*pg_sina_parallel_task_ms;
    p->cpu=p->parallel+p->schedule+p->combine;
    p->population=transfer_ms(r->bytes*cost_population_fraction(pg_sina_hpcp_enabled(),pg_sina_dcp,r->fraction,r->shared),pg_sina_pcie_mbps);
    p->csd=Max(p->io+p->cpu,p->population)+2;
    p->boundary=transfer_ms(Max(1,ceil(rows/QS_RESULT_ROWS))*offsetof(struct qs_result,str_data),pg_sina_pcie_mbps)+
        rows*cpu_tuple_cost*pg_sina_host_cost_ms+Max(1,ceil(rows/QS_RESULT_ROWS))*pg_sina_result_batch_ms;
    if(p->details) p->stages=makeStringInfo();
    if(p->details) appendStringInfo(p->stages,"fused Scan/Filter/Aggregate tasks=%.0f groups=%.0f partial_rows=%.0f state=%.0fB spill=%.0fB",
        tasks,groups,partial,p->peak,p->spill);
}
static void device_cost(struct placement *p,List **observations,struct qp_plan *lowered,const struct qs_plan *fast)
{
    CustomScan *scan=(CustomScan *)p->device;
    bytea *bytes=NULL;
    if(scan->methods==&pg_sina_scan_methods) {
        if(fast) { fast_cost(p,fast,observations); return; }
        Const *constant=linitial_node(Const,scan->custom_private);
        bytes=DatumGetByteaP(constant->constvalue);
        fast_cost(p,(const struct qs_plan *)VARDATA(bytes),observations); return;
    }
    /* Search candidates borrow an aligned tree directly. Retained executable
     * plans own a Const, which must be copied before changing cost metadata. */
    struct qp_plan *tree=lowered;
    if(!tree) {
        Const *constant=linitial_node(Const,scan->custom_private);
        bytes=DatumGetByteaP(constant->constvalue);
        tree=palloc(VARSIZE_ANY_EXHDR(bytes));
        memcpy(tree,VARDATA(bytes),VARSIZE_ANY_EXHDR(bytes));
    }
    double mark=pg_sina_planner_clock();
    bool used[QP_NODES]={0},indexed[QP_NODES]={0}; used[tree->root]=true;
    for(unsigned i=0;i<tree->ninit;++i) used[tree->init_nodes[i]]=true;
    for(unsigned pass=0;pass<tree->count;++pass) for(unsigned i=0;i<tree->count;++i) if(used[i]) {
        struct qp_node *n=&tree->nodes[i];
        if(n->left>=0) used[n->left]=true;
        if(n->right>=0) used[n->right]=true;
        if(n->kind==QP_INDEX_JOIN) indexed[n->right]=true;
        if(n->kind==QP_SCAN) n->bytes=residency(n->expr.relid,observations)->bytes;
    }
    if(pg_sina_planning) pg_sina_planning->cost_reach+=pg_sina_planner_clock()-mark;
    mark=pg_sina_planner_clock();
    qp_parallel_boundaries(tree,tree->root);
    for(unsigned i=0;i<tree->ninit;++i) qp_parallel_boundaries(tree,tree->init_nodes[i]);
    unsigned slots=device_workers(tree,&p->fdm); p->workers=slots;
    /* Suspended task contexts consume buffers/state, never extra CPU cores. */
    unsigned cus=Min(slots,Max(1,pg_sina_resources.cus));
    if(p->details) p->stages=makeStringInfo();
    double tasks[QP_NODES]={0},peak[QP_NODES]={0},live[QP_NODES]={0},retained[QP_NODES]={0};
    pipeline_tasks(tree,tasks);
    if(pg_sina_planning) pg_sina_planning->cost_buffers+=pg_sina_planner_clock()-mark;
    mark=pg_sina_planner_clock();
    double pipeline_io[QP_NODES]={0},pipeline_cpu[QP_NODES]={0};
    double io=0,overlap=0,population_bytes=0,cache_bytes=0,scan_bytes=0,blocks=0;
    double memory=Max(1,pg_sina_query_memory()),unit=pg_sina_host_cost_ms*pg_sina_csd_cpu_ratio;
    p->dictionary=dictionary_state(tree,observations);
    if(pg_sina_planning) pg_sina_planning->cost_dictionary+=pg_sina_planner_clock()-mark;
    mark=pg_sina_planner_clock();
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
        double index_probes=0,index_pages=0,heap_pages=0,index_tuple_visits=0;
        double previous_cpu=p->serial+p->parallel+p->schedule+p->combine;
        double previous_parallel=p->parallel,previous_io=io;
        double l=n->left>=0?live[n->left]:0,r=n->right>=0?live[n->right]:0;
        double lp=n->left>=0?peak[n->left]:0,rp=n->right>=0?peak[n->right]:0;
        double lr=n->left>=0?retained[n->left]:0,rr=n->right>=0?retained[n->right]:0;
        /* One vector has 16-byte lanes + NULL/scale/error arrays. Actual
         * expression allocation can be lazy; this is reserved scratch. */
        unsigned copies=tasks[i]>1?slots+1:1;
        double scratch=copies*(n->expr.nexprs*(256*20+128.0)+sizeof(struct qs_plan));
        double available=Max((double)QS_SORT_IO_BYTES,memory-dictionary_meta-scratch-l-r);
        double expression=cpu_operator_cost*(cost_expression_ops(&n->expr)+(n->kind==QP_AGGREGATE?n->expr.naggs:0));
        double work=input*(cpu_tuple_cost+expression)*unit;
        if(n->kind==QP_SCAN) {
            struct residency *relation=residency(n->expr.relid,observations);
            double read=relation->bytes,hit=read*relation->fraction;
            input=relation->tuples;
            if(n->ordered_index) {
                /* This cursor walks every leaf entry; its predicates do not
                 * position an index range. Output selectivity cannot reduce
                 * visited tuples, predicate CPU or non-covering Heap reads. */
                if(n->ordered_index&4) read=0;
                else if(!(n->ordered_index&2))
                    read=BLCKSZ*cost_index_pages(input,relation->bytes/BLCKSZ,QS_INDEX_CACHE,1);
                hit=read*relation->fraction;
                if(n->expr.join.indexid) {
                    struct residency *index=residency(n->expr.join.indexid,observations);
                    read+=index->bytes; hit+=index->bytes*index->fraction;
                }
            }
            scan_bytes+=read; cache_bytes+=hit;
            population_bytes+=read*cost_population_fraction(pg_sina_hpcp_enabled(),pg_sina_dcp,relation->fraction,relation->shared);
            double read_ms=transfer_ms(read,pg_sina_internal_mbps);
            io+=read_ms;
            blocks+=read/(n->ordered_index?QS_PAGE:tree->scan_bytes);
            /* References, literals and bound parameters do not execute a
             * scalar operator. Charge actual operations, not all IR nodes. */
            bool direct=tree->count==1 && tree->nodes[tree->root].kind==QP_SCAN && !tree->ninit;
            unsigned operations=cost_expression_ops(&n->expr);
            /* All scans share the same Heap walker: stop after the last
             * required column. Returning PG values versus internal cells
             * changes materialization below, not how far Heap is walked. */
            unsigned prefix=1;
            for(unsigned col=0;col<n->expr.nattrs;++col)
                if(n->expr.cols[col].slot>=0) prefix=col+1;
            double decode=(double)prefix/Max(1,n->expr.nattrs);
            work=input*(cpu_tuple_cost*decode*pg_sina_host_cost_ms+cpu_operator_cost*operations*unit)+
                rows*(direct?(cpu_tuple_cost+cpu_operator_cost*n->expr.nout)*pg_sina_host_cost_ms:cpu_tuple_cost*unit);
            parallel_cpu(p,work,tasks[i],cus);
        } else if(n->kind==QP_JOIN && n->hash && right) {
            double build=Max(0,right->estimated_rows),width=cost_hash_row(&right->expr);
            available=Max((double)QS_SORT_IO_BYTES,Min(available,memory-2*quota));
            state=cost_hash_state(build,width);
            if((n->join_type==QP_SEMI || n->join_type==QP_ANTI) && n->join_qual<0 &&
               n->nkeys==1 && right->kind==QP_SCAN) {
                const struct qs_expr *key=&right->expr.exprs[right->expr.outputs[n->right_keys[0]].index];
                double distinct=key->op==Q_COLUMN?column_distinct(right->expr.relid,key->column+1,false):0;
                if(key->op==Q_COLUMN) distinct=Max(distinct,
                    Min(build,column_key_domain(right->expr.relid,key->column+1)));
                double compact=cost_hash_existence_state(build,distinct,width,available);
                if(compact<state) {
                    state=compact;
                    double capacity=state/(width+3*sizeof(uint32_t));
                    double rounds=ceil((build-capacity)/Max(1,capacity-Min(build,distinct)));
                    /* The compact packed-key loop runs on one device CU.
                     * Keep it serial, but use the same device instruction
                     * price as hash_reindex, not PG tuple interpretation. */
                    p->serial+=rounds*capacity*3*cpu_operator_cost*unit;
                }
            }
            parallel_cpu(p,build*(cpu_tuple_cost+cpu_operator_cost*Max(1,n->nkeys))*unit,tasks[n->right],cus);
            int build_source=qp_pipeline_source(tree,n->right);
            if(build_source>=0 && tasks[n->right]>1) pipeline_cpu[build_source]+=p->parallel-previous_parallel;
            previous_parallel=p->parallel;
            /* Coordinator reindexes at growth barriers; do not divide. */
            p->serial+=build*cpu_operator_cost*unit;
            parallel_cpu(p,(input*(cpu_operator_cost*Max(1,n->nkeys))+rows*(cpu_tuple_cost+expression))*unit,tasks[i],cus);
            if(state>available) {
                if(n->join_type==QP_INNER) {
                    double partitions=4,ratio=state/available;
                    spill=2*(build*width+input*cost_hash_row(&left->expr));
                    /* Oversized partitions replay probe batches. */
                    if(ratio>partitions) spill+=input*cost_hash_row(&left->expr)*(ceil(ratio/partitions)-1);
                } else {
                    /* hash_build_impl uses external partitions only for
                     * INNER. SEMI/ANTI/LEFT retain the prefix then sort/merge
                     * both inputs; charge that serial fallback, not Hash I/O. */
                    const struct qp_node *inputs[]={left,right};
                    int ids[]={n->left,n->right};
                    for(unsigned side=0;side<2;++side) {
                        if(!side && cost_ordered(tree,ids[side],n->nkeys,n->left_keys)) continue;
                        struct cost_sort sort=cost_sort_state(Max(0,inputs[side]->estimated_rows),
                            sort_width(tree,ids[side]),Min(quota,available/2),sort_windows(&inputs[side]->expr));
                        spill+=sort.spill;
                        p->combine+=(sort.comparisons+sort.merge)*cpu_operator_cost*pg_sina_host_cost_ms;
                        p->combine+=sort.record_copies*(cpu_tuple_cost+inputs[side]->expr.nout*cpu_operator_cost)*pg_sina_host_cost_ms;
                    }
                    p->combine+=(input+build)*cpu_operator_cost*Max(1,n->nkeys)*pg_sina_host_cost_ms;
                }
            }
        } else if(n->kind==QP_INDEX_JOIN && right) {
            const struct qs_join *access=&right->expr.join;
            struct residency *heap=residency(right->expr.relid,observations);
            struct residency *index=residency(access->indexid,observations);
            double pages=Max(1,heap->bytes/BLCKSZ),leaves=Max(1,index->bytes/BLCKSZ);
            double distinct=column_distinct(heap->oid,access->build_key,false);
            double matches=Max(1,right->estimated_rows);
            if(distinct>0) matches=Max(matches,heap->tuples/distinct);
            double correlation=0;
            HeapTuple stats=SearchSysCache3(STATRELATTINH,ObjectIdGetDatum(heap->oid),
                Int16GetDatum(access->build_key),BoolGetDatum(false));
            if(HeapTupleIsValid(stats)) {
                AttStatsSlot slot;
                if(get_attstatsslot(&slot,stats,STATISTIC_KIND_CORRELATION,InvalidOid,ATTSTATSSLOT_NUMBERS)) {
                    if(slot.nnumbers==1) correlation=slot.numbers[0];
                    free_attstatsslot(&slot);
                }
                ReleaseSysCache(stats);
            }
            /* An equality probe still decodes candidates rejected by a join
             * residual. Semi/anti probes can stop after an accepted match. */
            if((n->join_type==QP_SEMI || n->join_type==QP_ANTI) && n->join_qual<0 && n->expr.predicate<0) {
                if(right->expr.predicate<0) matches=Min(matches,1);
                else if(distinct>0) {
                    double selectivity=Min(1.0,right->estimated_rows/Max(1.0,heap->tuples/distinct));
                    matches=cost_first_match_visits(matches,selectivity);
                }
            }
            index_tuple_visits=matches;
            index_probes=input;
            double fanout=Max(2,(BLCKSZ-40)/(MAXALIGN(8+access->index_key_bytes)+4.0));
            double levels=1+ceil(log(leaves)/log(fanout));
            double leaf_visits=input*Max(1,matches/(heap->tuples/leaves));
            double heap_visits=cost_index_heap_visits(input,matches,heap->tuples/pages,correlation);
            int scan=qp_probe_scan(tree,n->left);
            Oid probe_rel=InvalidOid; int probe_att=0; bool probe_year=false;
            if(scan>=0 && tree->nodes[scan].expr.relid==right->expr.relid &&
               tree->nodes[scan].expr.relfilenode==right->expr.relfilenode &&
               column_origin(tree,n->left,left->expr.outputs[n->left_keys[0]].index,
                   &probe_rel,&probe_att,&probe_year) && !probe_year &&
               probe_rel==right->expr.relid && probe_att==access->build_key)
                heap_visits=cost_index_window_misses(heap_visits,matches,heap->tuples/pages,
                    (double)tree->scan_bytes/BLCKSZ,correlation);
            index_pages=cost_index_pages(leaf_visits,leaves,QS_INDEX_CACHE/2,QS_INDEX_KEYS)+(input>0?copies*levels:0);
            heap_pages=cost_index_pages(heap_visits,pages,QS_INDEX_CACHE/2,
                QS_INDEX_KEYS*Max(1,heap_visits/Max(1,input)));
            double index_bytes=index_pages*BLCKSZ,heap_bytes=heap_pages*BLCKSZ;
            double read=index_bytes+heap_bytes;
            io+=transfer_ms(read,pg_sina_internal_mbps); blocks+=index_pages+heap_pages;
            population_bytes+=index_bytes*cost_population_fraction(pg_sina_hpcp_enabled(),pg_sina_dcp,index->fraction,index->shared)+
                heap_bytes*cost_population_fraction(pg_sina_hpcp_enabled(),pg_sina_dcp,heap->fraction,heap->shared);
            scan_bytes+=read;
            cache_bytes+=index_bytes*index->fraction+heap_bytes*heap->fraction;
            work=(input*levels*(log2(fanout)+QS_INDEX_CACHE/2)*cpu_operator_cost+
                input*matches*(cpu_tuple_cost+cost_expression_ops(&right->expr)*cpu_operator_cost)+
                rows*(cpu_tuple_cost+expression))*unit;
            parallel_cpu(p,work,tasks[i],cus);
            p->schedule+=cost_parallel(0,ceil((index_pages+heap_pages)/QS_INDEX_READS),cus,pg_sina_parallel_task_ms);
        } else if(n->kind==QP_AGGREGATE) {
            double groups=Max(1,n->estimated_groups),group_tasks=n->left>=0?tasks[n->left]:0;
            if(!n->nkeys) {
                state=(slots+1)*n->expr.naggs*(3*sizeof(struct qs_cell)+16);
                parallel_cpu(p,work,group_tasks,cus);
                p->combine+=slots*n->expr.naggs*cpu_operator_cost*unit;
            } else if(n->hash && qp_hash_aggregate_supported(n)) {
                available=Max((double)QS_SORT_IO_BYTES,available-2*Min(quota,cost_sort_workspace()));
                double table=cost_group_state(n,groups);
                bool locals=slots>1 && group_tasks>1 && (slots+1)*table<=available;
                state=locals?(slots+1)*table:table;
                parallel_cpu(p,input*(cpu_tuple_cost+expression)*unit,group_tasks,cus);
                int sink_source=qp_pipeline_source(tree,n->left);
                if(sink_source>=0 && group_tasks>1) pipeline_cpu[sink_source]+=p->parallel-previous_parallel;
                previous_parallel=p->parallel;
                if(locals) parallel_cpu(p,slots*groups*n->expr.naggs*cpu_operator_cost*unit,ceil(groups/QP_VECTOR_ROWS),cus);
                else if(group_tasks>1) p->serial+=input*cpu_operator_cost*Max(1,n->nkeys)*unit;
                p->combine+=groups*n->expr.naggs*cpu_operator_cost*unit;
                if(table>available) {
                    double row=(n->nkeys+2*n->expr.naggs)*sizeof(struct qs_cell);
                    double hash_memory=cost_group_resident(n,groups,available);
                    double resident_groups=(hash_memory-128)/(8+(7.0/8)*cost_group_stride(n))*7/8;
                    double partial=cost_partial_rows(input,groups,resident_groups,group_correlation(tree,i));
                    double free_memory=available+2*Min(quota,cost_sort_workspace())-hash_memory;
                    double sort_memory=cost_sort_memory(free_memory,quota);
                    /* Hash buffers are freed before the final merge. Its
                     * window budget is still bounded by the child limit. */
                    struct cost_sort sort=cost_sort_state(partial,row,sort_memory,sort_windows(&n->expr));
                    spill=sort.spill;
                    /* This fallback runs on the coordinator: hashing input,
                     * serializing partial states, sorting and recombining
                     * them are distinct work, never divided by all CUs. */
                    double scalar=pg_sina_host_cost_ms;
                    double run_cpu=sort.comparisons*cpu_operator_cost*scalar;
                    if(n->nkeys==1 && n->expr.exprs[n->expr.keys[0]].type==Q_INTEGER) {
                        unsigned oid=left->expr.outputs[n->left_keys[0]].type_oid;
                        if(oid==INT2OID || oid==INT4OID || oid==INT8OID || oid==DATEOID) {
                            unsigned passes=oid==INT8OID?8:oid==INT2OID?2:4;
                            double fraction=0;
                            Oid rel; int att; bool year=false;
                            if(column_origin(tree,i,n->expr.keys[0],&rel,&att,&year)) {
                                HeapTuple stats=SearchSysCache3(STATRELATTINH,ObjectIdGetDatum(rel),Int16GetDatum(att),BoolGetDatum(false));
                                if(HeapTupleIsValid(stats)) {
                                    double nulls=((Form_pg_statistic)GETSTRUCT(stats))->stanullfrac;
                                    fraction=pow(1-nulls,partial/Max(1,sort.runs));
                                    ReleaseSysCache(stats);
                                }
                            }
                            /* Integer radix runs count and scatter buckets;
                             * they do not compare N*log(N) interpreted rows.
                             * Include the key validation and scratch copies. */
                            double radix=partial*(1+2*passes)*cpu_operator_cost*unit+
                                transfer_ms(partial*row*(2*passes+1),pg_sina_cache_mbps);
                            run_cpu=run_cpu*(1-fraction)+radix*fraction;
                        }
                    }
                    p->combine+=run_cpu+sort.merge*cpu_operator_cost*scalar;
                    /* The partial run already contains key/value/count
                     * cells. Copying it performs no PG Heap/expr evaluation. */
                    p->combine+=transfer_ms(sort.record_copies*row,pg_sina_cache_mbps);
                    p->combine+=partial*n->expr.naggs*(cpu_tuple_cost+cpu_operator_cost)*scalar;
                    if(p->details) appendStringInfo(p->stages,"%s%d/PartialSort rows=%.0f hash_resident=%.0fB sort_memory=%.0fB runs=%.0f fan=%u passes=%u",
                        p->stages->len?"; ":"",i,partial,hash_memory,sort_memory,sort.runs,sort.fan,sort.passes);
                }
            } else if(n->sorted_input && !n->distinct && !n->distinct_aggs) {
                state=n->expr.naggs*(3*sizeof(struct qs_cell)+16);
                p->serial+=work;
            } else {
                struct cost_sort sort=cost_sort_state(input,sort_width(tree,n->left),Min(quota,available),sort_windows(&left->expr));
                state=sort.state; spill=sort.spill;
                parallel_cpu(p,sort.comparisons*cpu_operator_cost*unit,group_tasks,cus);
                p->combine+=(sort.merge*cpu_operator_cost+
                    sort.record_copies*(cpu_tuple_cost+n->expr.nout*cpu_operator_cost))*pg_sina_host_cost_ms+work;
            }
        } else if(n->kind==QP_SORT) {
            double sort_memory=cost_sort_memory(available,quota);
            struct cost_sort sort=cost_topn_state(input,sort_width(tree,i),sort_memory,sort_windows(&n->expr),qp_sort_bound(tree,i));
            state=sort.state; spill=sort.spill;
            parallel_cpu(p,sort.comparisons*cpu_operator_cost*unit,n->left>=0?tasks[n->left]:0,cus);
            p->combine+=(sort.merge*cpu_operator_cost+
                sort.record_copies*(cpu_tuple_cost+n->expr.nout*cpu_operator_cost))*pg_sina_host_cost_ms;
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
                    sort_width(tree,ids[side]),Min(quota,available/2),sort_windows(&inputs[side]->expr));
                state+=sort.state; spill+=sort.spill;
                p->serial+=(sort.comparisons+sort.merge)*cpu_operator_cost*unit;
            }
            p->serial+=((input+Max(0,right->estimated_rows))*cpu_operator_cost*Max(1,n->nkeys)+
                rows*(cpu_tuple_cost+expression))*unit;
        } else parallel_cpu(p,work,tasks[i],cus);
        int source=qp_pipeline_source(tree,i);
        if(source>=0 && tasks[i]>1) {
            pipeline_io[source]+=io-previous_io;
            pipeline_cpu[source]+=p->parallel-previous_parallel;
        }
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
        if(p->details) appendStringInfo(p->stages,"%s%d/%s rows=%.0f groups=%.0f tasks=%.0f cpu=%.3fms state=%.0fB resident=%.0fB available=%.0fB spill=%.0fB",
            p->stages->len?"; ":"",i,device_kind(n->kind),rows,n->estimated_groups,tasks[i],
            p->serial+p->parallel+p->schedule+p->combine-previous_cpu,state,resident,available,spill);
        if(p->details && n->kind==QP_SCAN)
            appendStringInfo(p->stages," visited=%.0f expr_ops=%u",input,cost_expression_ops(&n->expr));
        if(p->details && (n->kind==QP_JOIN || n->kind==QP_INDEX_JOIN)) {
            static const char *names[]={"inner","left","semi","anti"};
            appendStringInfo(p->stages," join=%s",names[n->join_type]);
        }
        if(p->details && n->kind==QP_INDEX_JOIN)
            appendStringInfo(p->stages," probes=%.0f tuples_per_probe=%.3f index_pages=%.0f heap_pages=%.0f",index_probes,index_tuple_visits,index_pages,heap_pages);
    }
    if(pg_sina_planning) pg_sina_planning->cost_operators+=pg_sina_planner_clock()-mark;
    mark=pg_sina_planner_clock();
    /* One CSF task executes a whole pipeline, not one request per SQL node.
     * Charge its wave dispatch once even when several nodes share tasks. */
    if(cus>1) for(unsigned i=0;i<tree->count;++i)
        if(used[i] && tree->nodes[i].parallel && tasks[i]>1)
            p->schedule+=cost_parallel(0,tasks[i],cus,pg_sina_parallel_task_ms);
    /* Independent tasks overlap LOAD/lookup with another task's CU work.
     * Sources mark real blocking boundaries; build CPU belongs to the build
     * input's pipeline. Serial/Combine and dependency-driven spill stay out. */
    for(unsigned i=0;i<tree->count;++i)
        overlap+=cost_scan_overlap(pipeline_io[i],pipeline_cpu[i],tasks[i],cus);
    if(p->details) appendStringInfo(p->stages,"; pipeline_io_overlap=%.3fms",overlap);
    p->cache=scan_bytes?cache_bytes/scan_bytes:0;
    p->spill+=2*dictionary_evicted;
    io+=cost_spill_io(p->spill,pg_sina_internal_mbps,pg_sina_internal_write_mbps);
    p->schedule+=ceil(p->spill/QS_SORT_IO_BYTES)*pg_sina_parallel_task_ms;
    p->io=io; p->cpu=p->serial+p->parallel+p->schedule+p->combine;
    p->population=pg_sina_hpcp_enabled() || pg_sina_dcp?transfer_ms(population_bytes,pg_sina_pcie_mbps):0;
    double device=io+p->cpu-overlap;
    if(p->population>0) device=Max(device,p->population)+blocks*0.002;
    p->csd=device+2;
    p->rows=tree->nodes[tree->root].estimated_rows;
    scan->scan.plan.plan_rows=p->rows;
    double consumers=IsA(p->native,SeqScan) && p->native->parallel_aware?
        Max(1.0,p->rows/Max(1.0,p->native->plan_rows)):1;
    /* Shared CSD I/O/transfer occurs once. Only HOST tuple conversion is
     * distributed among PG consumers, using PG's partial-row estimate. */
    double batches=Max(1,ceil(p->rows/output_capacity(p->native,&tree->nodes[tree->root].expr)));
    if(tree->count==1 && tree->nodes[tree->root].kind==QP_SCAN && !tree->nodes[tree->root].ordered_index)
        batches=Max(batches,tasks[tree->root]); /* CU flushes at each input block. */
    p->boundary=transfer_ms(output_bytes(p->native,&tree->nodes[tree->root].expr,p->rows),pg_sina_pcie_mbps)+
        p->rows/consumers*cpu_operator_cost*pg_sina_host_cost_ms+batches*pg_sina_result_batch_ms;
    if(tree->count==1 && tree->nodes[tree->root].kind==QP_SCAN && !tree->ninit)
        scan->scan.plan.startup_cost=Min(p->csd+p->boundary,2+
            Max(0,p->csd-2)*Min(1.0,cus/Max(1.0,tasks[tree->root]))+p->boundary/batches)/pg_sina_host_cost_ms;
    if(bytes) { memcpy(VARDATA(bytes),tree,VARSIZE_ANY_EXHDR(bytes)); pfree(tree); }
    if(pg_sina_planning) pg_sina_planning->cost_finish+=pg_sina_planner_clock()-mark;
}
static struct placement *estimate(PlannerInfo *root,Plan *native,Plan *scope,List **observations,bool top,bool mark_restore,bool parallel_region)
{
    struct placement *p=palloc0(sizeof(*p)); p->native=native; p->root=root; p->csd=DBL_MAX; p->details=true;
    bool parallel_children=parallel_region || IsA(native,Gather) || IsA(native,GatherMerge);
    {
        if(native->lefttree) p->children=lappend(p->children,estimate(root,native->lefttree,scope,observations,false,false,parallel_children));
        if(native->righttree) p->children=lappend(p->children,estimate(root,native->righttree,scope,observations,false,
            IsA(native,MergeJoin) && !((MergeJoin *)native)->skip_mark_restore,parallel_children));
        if(IsA(native,SubqueryScan)) {
            RelOptInfo *rel=root->simple_rel_array[((SubqueryScan *)native)->scan.scanrelid];
            if(rel && rel->subroot) p->children=lappend(p->children,
                estimate(rel->subroot,((SubqueryScan *)native)->subplan,((SubqueryScan *)native)->subplan,observations,false,false,parallel_children));
        }
    }
    double original_children=0,best_children=0,native_children=0;
    ListCell *c;
    foreach(c,p->children) {
        struct placement *child=lfirst(c);
        double repeats=IsA(native,NestLoop) && child->native==native->righttree && !IsA(child->native,Material)?Max(1,native->lefttree->plan_rows):1;
        original_children+=child->native->total_cost*pg_sina_host_cost_ms*repeats;
        native_children+=child->native_host*repeats;
        best_children+=(child->offload?child->csd+child->boundary:child->host)*repeats;
    }
    double local=Max(0,native->total_cost*pg_sina_host_cost_ms-original_children);
    Oid oid=scan_relation(root,native);
    if(OidIsValid(oid)) {
        local=pg_sina_host_scan_cost(native,oid,local,observations);
    }
    p->host=local+best_children;
    p->native_host=local+native_children;
    bool eligible=pg_sina_exe_path!=PG_SINA_FILTER ||
        IsA(native,SeqScan) || IsA(native,IndexScan) || IsA(native,IndexOnlyScan);
    /* Private inputs below Gather must be replayed by every PG worker (for
     * example a private hash build). A shared device stream partitions rows;
     * it is not a broadcast/replay source. Keep those scans native. */
    if(parallel_region && (!native->parallel_aware || !IsA(native,SeqScan))) eligible=false;
    /* The shared scan cursor supports parallel SeqScan only. Reject parallel
     * index candidates before compiling their filters/SubPlans and metadata;
     * compile_subtree already rejects the same shape at its final check. */
    if(native->parallel_aware && pg_sina_is_scan(native) && !IsA(native,SeqScan)) eligible=false;
    /* HOST HashJoin invokes MultiExec on its Hash child, and MergeJoin may
     * mark/restore its inner child. A tuple-stream CustomScan is neither.
     * Keep those native nodes; their inputs can still be device islands. */
    eligible=eligible && !IsA(native,Hash) && !mark_restore;
    /* Scan-only conversion binds only its actual HOST InitPlan dependencies;
     * whole-subtree conversion still owns its complete initialization. */
    if(eligible && (pg_sina_is_scan(native) || top || root->init_plans==NIL)) {
        Plan *candidate=pg_sina_is_scan(native)?pg_sina_project_scan(scope,native):native;
        Plan *device=top && pg_sina_exe_path!=PG_SINA_FILTER?pg_sina_fast_plan(root,native):NULL;
        if(!device) device=pg_sina_compile_subtree(root,candidate,!top || pg_sina_exe_path==PG_SINA_FILTER);
        if(device!=candidate) { p->device=device; device_cost(p,observations,NULL,NULL); }
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
/* Candidate metadata is binary and copyObject-safe. Only selected plans turn
 * these diagnostic numbers into PG Float strings for EXPLAIN. PG Paths keep
 * their ordinary numeric Cost fields, in PG cost units, throughout search. */
struct candidate_costs { double values[15]; int workers; };
static List *format_costs(const struct candidate_costs *costs,const char *stages)
{
    List *out=NIL;
    for(unsigned i=0;i<lengthof(costs->values);++i)
        out=lappend(out,makeFloat(psprintf("%.9g",costs->values[i])));
    out=lappend(out,makeInteger(costs->workers));
    return lappend(out,makeString(pstrdup(stages)));
}
static void annotate_device(struct placement *p,bool candidate)
{
    CustomScan *scan=(CustomScan *)p->device;
    struct candidate_costs costs={{p->host,p->csd,p->boundary,p->cache,p->spill,p->cpu,p->io,p->peak,
        p->population,p->serial,p->parallel,p->schedule,p->combine,p->dictionary,p->fdm},p->workers};
    Node *saved;
    if(candidate) {
        bytea *record=palloc0(VARHDRSZ+sizeof(costs)); SET_VARSIZE(record,VARHDRSZ+sizeof(costs));
        memcpy(VARDATA(record),&costs,sizeof(costs));
        saved=(Node *)makeConst(BYTEAOID,-1,InvalidOid,-1,PointerGetDatum(record),false,false);
    } else saved=(Node *)format_costs(&costs,p->stages?p->stages->data:"");
    scan->custom_private=lappend(scan->custom_private,saved);
}
static bool output_ordered(const struct qp_plan *tree,int id)
{
    if(id<0) return false;
    const struct qp_node *n=&tree->nodes[id];
    if(n->kind==QP_SORT) return true;
    if(n->kind==QP_SCAN) return (n->ordered_index&1) && !(n->ordered_index&8);
    if(n->kind==QP_JOIN) return !n->hash;
    if(n->kind==QP_AGGREGATE && (n->hash || n->distinct || n->distinct_aggs || !n->sorted_input)) return false;
    if(n->kind==QP_INDEX_JOIN && !n->sorted_input) return false;
    return output_ordered(tree,n->left);
}
/* Decide physical properties in Path search, without encoding instructions.
 * Only numeric costs survive for discarded/shared-search candidates. */
Plan *pg_sina_cost_device(PlannerInfo *root,Plan *native,bool top,List **observations,
                         double *internal,double *boundary)
{
    struct placement p={.native=native,.root=root,.host=native->total_cost*pg_sina_host_cost_ms,
        .details=top && pg_sina_profile && pg_sina_native_search};
    if(!top && root->init_plans && !pg_sina_is_scan(native)) return NULL;
    double mark=pg_sina_planner_clock();
    /* Keep the prepared tree itself, rather than copying it again when the
     * estimate workspace is deleted. Rejected candidates release this context. */
    MemoryContext workspace=CurrentMemoryContext;
    MemoryContext retained=AllocSetContextCreate(pg_sina_search_context(),"CSD candidate plan",ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(retained);
    Plan *chosen=pg_sina_prepare_candidate(root,native,!top || root->parse->sortClause!=NIL);
    MemoryContextSwitchTo(workspace);
    if(!chosen) {
        if(pg_sina_planning) pg_sina_planning->properties+=pg_sina_planner_clock()-mark;
        MemoryContextDelete(retained); return NULL;
    }
    p.native=chosen;
    struct qp_plan *lowered=NULL;
    struct qs_plan *fast=NULL;
    p.device=top?pg_sina_fast_properties(root,chosen,&fast):NULL;
    if(!p.device) p.device=pg_sina_describe_candidate(root,chosen,!top,&lowered);
    if(p.device && p.device!=chosen && lowered && top && root->parse->sortClause && !output_ordered(lowered,lowered->root)) {
        MemoryContextSwitchTo(retained);
        chosen=pg_sina_require_output_order(root,chosen);
        MemoryContextSwitchTo(workspace);
        p.native=chosen;
        p.device=chosen?pg_sina_describe_candidate(root,chosen,false,&lowered):NULL;
    }
    if(pg_sina_planning) pg_sina_planning->properties+=pg_sina_planner_clock()-mark;
    if(p.device==chosen || !p.device) { MemoryContextDelete(retained); return NULL; }
    mark=pg_sina_planner_clock();
    device_cost(&p,observations,lowered,fast);
    if(p.details) ereport(NOTICE,(errmsg(
        "SQLerate original device estimate total_ms=%.3f cpu_ms=%.3f io_ms=%.3f spill_bytes=%.0f peak_bytes=%.0f stages=%s",
        p.csd+p.boundary,p.cpu,p.io,p.spill,p.peak,p.stages?p.stages->data:"")));
    if(pg_sina_planning && pg_sina_is_scan(chosen)) ereport(NOTICE,(errmsg(
        "SQLerate scan candidate relid=%u parallel=%d rows=%.0f host=%.3f device=%.3f boundary=%.3f cpu=%.3f io=%.3f",
        scan_relation(root,chosen),chosen->parallel_aware,p.rows,p.host,p.csd,p.boundary,p.cpu,p.io)));
    if(pg_sina_planning) pg_sina_planning->estimate+=pg_sina_planner_clock()-mark;
    mark=pg_sina_planner_clock();
    annotate_device(&p,true);
    *internal=p.csd; *boundary=p.boundary;
    ((CustomScan *)p.device)->custom_private=lappend(((CustomScan *)p.device)->custom_private,chosen);
    List *choices=NIL;
    /* Per node: frozen algorithms/rewrite flags and its two input IDs. */
    if(lowered) for(unsigned i=0;i<lowered->count;++i) choices=lappend(choices,list_make3(
        makeInteger(pg_sina_candidate_choices()[i]),makeInteger(lowered->nodes[i].left),makeInteger(lowered->nodes[i].right)));
    ((CustomScan *)p.device)->custom_private=lappend(((CustomScan *)p.device)->custom_private,choices);
    ((CustomScan *)p.device)->custom_private=lappend(((CustomScan *)p.device)->custom_private,
        makeInteger(fast?fast->norder!=0:output_ordered(lowered,lowered->root)));
    if(pg_sina_planning) pg_sina_planning->format+=pg_sina_planner_clock()-mark;
    return p.device;
}
/* Explain text is not a cost input. Recreate it once for a selected Path;
 * keep the numeric costs that PG actually compared, without repricing it. */
List *pg_sina_selected_costs(PlannerInfo *root,Plan *native,bool top,bool fast,Node *costs)
{
    volatile double mark=pg_sina_planner_clock(); bool compiling=pg_sina_compiling_plan;
    struct placement p={.native=native,.root=root,.details=true};
    struct qp_plan *properties=NULL; struct qs_plan *fused=NULL;
    pg_sina_compiling_plan=true;
    PG_TRY();
    {
        p.device=fast?pg_sina_fast_properties(root,native,&fused):pg_sina_describe_candidate(root,native,!top,&properties);
        if(!p.device || p.device==native) elog(ERROR,"selected device properties no longer describe");
        device_cost(&p,pg_sina_search_observations(),properties,fused);
    }
    PG_CATCH();
    { pg_sina_compiling_plan=compiling; PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_compiling_plan=compiling;
    struct candidate_costs saved;
    Const *record=castNode(Const,costs);
    memcpy(&saved,VARDATA(DatumGetByteaP(record->constvalue)),sizeof(saved));
    List *result=format_costs(&saved,p.stages->data);
    if(pg_sina_planning) pg_sina_planning->format+=pg_sina_planner_clock()-mark;
    return result;
}
static Plan *select_plan(struct placement *p,bool top,StringInfo decisions)
{
    appendStringInfo(decisions,"%s%s:%s host=%.3f device=",decisions->len?"; ":"",
        plan_name(p->native),p->offload?"CSD":"HOST",p->host);
    if(p->device) appendStringInfo(decisions,"%.3f boundary=%.3f",p->csd,p->boundary);
    else appendStringInfoString(decisions,"unsupported");
    if(p->offload) {
        Plan *out=p->device;
        if(top && pg_sina_exe_path!=PG_SINA_FILTER) {
            p->root->init_plans=NIL;
        }
        annotate_device(p,false);
        return out;
    }
    Plan *out=p->native; ListCell *c;
    foreach(c,p->children) {
        struct placement *child=lfirst(c); Plan *chosen=select_plan(child,false,decisions);
        if(child->native==out->lefttree) {
            if(chosen!=child->native && pg_sina_is_scan(child->native))
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
    ExplainPropertyText("DCP",pg_sina_dcp?"shared_buffers":"disabled",es);
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
    ExplainPropertyText("DCP",pg_sina_dcp?"shared_buffers":"disabled",es);
    ExplainPropertyText("Placement Costs","milliseconds; estimates, not execution timings",es);
    ExplainPropertyText("Placement",strVal(linitial(scan->custom_private)),es);
    const char *costs=strVal(lsecond(scan->custom_private));
    if(*costs) ExplainPropertyText("Node Cost Estimates",costs,es);
}
static const CustomExecMethods adaptive_exec={.CustomName="SQLeratePlan",.BeginCustomScan=adaptive_begin,
    .ExecCustomScan=adaptive_exec_scan,.EndCustomScan=adaptive_end,.ReScanCustomScan=adaptive_rescan,.ExplainCustomScan=adaptive_explain};
const CustomScanMethods pg_sina_adaptive_methods={.CustomName="SQLeratePlan",.CreateCustomScanState=adaptive_create};
Plan *pg_sina_adaptive_plan(PlannerInfo *root,Plan *native)
{
    List *observations=NIL;
    /* Keep InitPlan aggregate/join execution on PG. Its independent scans
     * use the same filter-only rules; never compile the whole initializer. */
    if(pg_sina_exe_path==PG_SINA_FILTER) {
        ListCell *c;
        foreach(c,root->init_plans) {
            SubPlan *sub=lfirst(c);
            if(sub->subLinkType==CTE_SUBLINK) continue;
            Plan *input=list_nth(root->glob->subplans,sub->plan_id-1);
            PlannerInfo *scope=list_nth(root->glob->subroots,sub->plan_id-1);
            struct placement *init=estimate(scope,input,input,&observations,true,false,false);
            StringInfoData decisions; initStringInfo(&decisions);
            lfirst(list_nth_cell(root->glob->subplans,sub->plan_id-1))=select_plan(init,true,&decisions);
            pfree(decisions.data);
        }
    }
    struct placement *p=estimate(root,native,native,&observations,true,false,false);
    StringInfoData decisions; initStringInfo(&decisions);
    StringInfoData costs; initStringInfo(&costs); node_costs(p,&costs);
    Plan *out=select_plan(p,true,&decisions);
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE) return out;
    out->total_cost=(p->offload?p->csd+p->boundary:p->host)/pg_sina_host_cost_ms;
    return pg_sina_wrap_placement(out,decisions.data,costs.data);
}
Plan *pg_sina_wrap_placement(Plan *out,const char *decisions,const char *costs)
{
    CustomScan *scan=makeNode(CustomScan); scan->scan.plan=*out;
    NodeSetTag(scan,T_CustomScan);
    scan->scan.plan.lefttree=scan->scan.plan.righttree=NULL; scan->scan.plan.qual=NIL;
    scan->scan.plan.initPlan=NIL;
    scan->scan.plan.parallel_safe=scan->scan.plan.parallel_aware=false;
    scan->custom_scan_tlist=copyObject(out->targetlist);
    scan->custom_plans=list_make1(out); scan->custom_private=list_make2(makeString(pstrdup(decisions)),makeString(pstrdup(costs)));
    scan->methods=&pg_sina_adaptive_methods;
    return (Plan *)scan;
}
