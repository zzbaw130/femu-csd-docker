/* Put device alternatives into PG's existing join/upper Path search. */
#include "pg_internal.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "executor/nodeHash.h"
#include "executor/spi.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include <float.h>
#include <math.h>

struct path_relation {
    PlannerInfo *root;
    RelOptInfo *rel;
    CustomPath *internal; /* Best complete device subtree, without delivery. */
    CustomPath *complete; /* Final device candidate, including HOST delivery. */
    CustomPath *scan[2]; /* Private/shared source, retained across PG pruning. */
    Path *host_parallel_scan; /* Base scan alternative for a shared Hash table. */
    Path *stream[2]; /* HOST probe chains fed by a CSD source. */
    double stream_work[2]; /* Producer work not yet hidden, in PG cost units. */
    double internal_cost,complete_cost;
    double row_uncertainty;
    List *pending;
};
struct prepared_relation { Oid relid,filenode; bool prepared; char device[128]; };
struct host_hash_cost { double rows,startup,total,outer,inner,penalty; int batches; };
struct root_state { PlannerInfo *root; PlannerInfo saved; };
static void save_roots(PlannerInfo *,List **);
static void restore_roots(List *);
struct device_program { PlannerInfo root; PlannerGlobal globals; Plan *native; List *choices; bool fast,boundary; };
struct pg_sina_path_search {
    List *relations,*observations,*prepared,*indexes,*schemas,*programs,*host_hashes;
    MemoryContext memory;
    struct qp_plan *workspace;
    unsigned choices[QP_NODES];
    List *encoding_choices;
};
struct pg_sina_path_search *pg_sina_path_search;
bool pg_sina_native_search,pg_sina_previewing_path;
static const CustomPathMethods device_methods,final_methods;

/* Only subtract base heaps actually scanned by the device. A HOST Join in
 * a streaming chain still competes for buffers; so do small/full DCP inputs.
 * Large scans with HPCP use cache.c's bounded DCP bulk-read target. */
static double host_cache_pages(PlannerInfo *root,Path *path)
{
    double host=root->total_table_pages;
    if(IsA(path,CustomPath) && ((CustomPath *)path)->methods==&device_methods &&
       IsA(linitial(((CustomPath *)path)->custom_private),SeqScan)) {
        double pages=path->parent->pages,target=0;
        if(pg_sina_dcp) {
            RangeTblEntry *rte=rt_fetch(path->parent->relid,root->parse->rtable);
            if(!pg_sina_hpcp_enabled() || rte->relkind!=RELKIND_RELATION || pages<=NBuffers/4) return host;
            target=Min(NBuffers/8,256*1024/BLCKSZ);
        }
        host-=Max(0,pages-target);
    }
    return Max(1,host);
}

/* This marker survives PG setrefs but cannot reach the executor. Only the
 * globally selected statement gets its final CSF record in finish below. */
static Plan *defer_program(PlannerInfo *root,Plan *native,bool fast,bool boundary,Node *costs,List *choices)
{
    double mark=pg_sina_planner_clock();
    MemoryContext previous=MemoryContextSwitchTo(pg_sina_search_context());
    struct device_program *program=palloc(sizeof(*program));
    program->root=*root; program->native=native; program->fast=fast; program->boundary=boundary;
    program->choices=choices;
    /* PG setrefs rewrites SubPlan expressions after create_plan. Preserve
     * their original Vars for the final device encoder. */
    program->globals=*root->glob; program->globals.subplans=copyObject(root->glob->subplans);
    program->root.glob=&program->globals;
    program->root.init_plans=copyObject(root->init_plans);
    program->root.processed_tlist=copyObject(root->processed_tlist);
    unsigned id=list_length(pg_sina_path_search->programs);
    pg_sina_path_search->programs=lappend(pg_sina_path_search->programs,program);
    MemoryContextSwitchTo(previous);
    CustomScan *scan=(CustomScan *)pg_sina_candidate_scan(native,fast,true);
    if(boundary && pg_sina_is_scan(native) && !pg_sina_scan_parameters(root,native,&scan->custom_exprs))
        elog(ERROR,"selected scan has unsupported HOST parameters");
    scan->custom_private=list_make2(makeInteger(id),copyObject(costs));
    if(pg_sina_planning) pg_sina_planning->freeze+=pg_sina_planner_clock()-mark;
    return (Plan *)scan;
}
void pg_sina_finish_device_plans(Plan *plan)
{
    if(!plan) return;
    if(IsA(plan,CustomScan)) {
        CustomScan *volatile scan=(CustomScan *)plan;
        if((scan->methods==&pg_sina_plan_methods || scan->methods==&pg_sina_scan_methods) &&
           scan->custom_private && IsA(linitial(scan->custom_private),Integer)) {
            struct device_program *program=list_nth(pg_sina_path_search->programs,intVal(linitial(scan->custom_private)));
            bool compiling=pg_sina_compiling_plan; volatile double mark=pg_sina_planner_clock();
            List *states=NIL; save_roots(&program->root,&states);
            ListCell *cell; foreach(cell,program->globals.subroots) save_roots(lfirst(cell),&states);
            foreach(cell,states) ((struct root_state *)lfirst(cell))->root->glob=&program->globals;
            List *previous=pg_sina_path_search->encoding_choices;
            pg_sina_path_search->encoding_choices=program->choices;
            Plan *volatile encoded=NULL; pg_sina_compiling_plan=true;
            PG_TRY();
            {
                lfirst(list_nth_cell(scan->custom_private,1))=pg_sina_selected_costs(&program->root,program->native,
                    !program->boundary,program->fast,lsecond(scan->custom_private));
                mark=pg_sina_planner_clock();
                encoded=program->fast?pg_sina_fast_plan(&program->root,program->native):
                    pg_sina_compile_subtree(&program->root,program->native,program->boundary);
            }
            PG_CATCH();
            { restore_roots(states); pg_sina_compiling_plan=compiling; pg_sina_path_search->encoding_choices=previous; PG_RE_THROW(); }
            PG_END_TRY();
            pg_sina_compiling_plan=compiling;
            restore_roots(states);
            pg_sina_path_search->encoding_choices=previous;
            if(!encoded || encoded==program->native) elog(ERROR,"selected device candidate no longer encodes");
            if(pg_sina_planning) pg_sina_planning->compile+=pg_sina_planner_clock()-mark;
            lfirst(list_head(scan->custom_private))=linitial(((CustomScan *)encoded)->custom_private);
        }
        ListCell *c; foreach(c,scan->custom_plans) pg_sina_finish_device_plans(lfirst(c));
    }
    pg_sina_finish_device_plans(plan->lefttree); pg_sina_finish_device_plans(plan->righttree);
    if(IsA(plan,SubqueryScan)) pg_sina_finish_device_plans(((SubqueryScan *)plan)->subplan);
}

void pg_sina_start_path_search(void)
{
    pg_sina_path_search=palloc0(sizeof(*pg_sina_path_search));
    pg_sina_path_search->memory=CurrentMemoryContext;
}
MemoryContext pg_sina_search_context(void)
{ return pg_sina_path_search?pg_sina_path_search->memory:CurrentMemoryContext; }
List **pg_sina_search_observations(void)
{ return pg_sina_path_search?&pg_sina_path_search->observations:NULL; }
List **pg_sina_search_index_metadata(void)
{ return pg_sina_path_search?&pg_sina_path_search->indexes:NULL; }
List **pg_sina_search_scan_schemas(void)
{ return pg_sina_path_search?&pg_sina_path_search->schemas:NULL; }
struct qp_plan *pg_sina_cost_workspace(void)
{
    if(!pg_sina_path_search->workspace)
        pg_sina_path_search->workspace=MemoryContextAlloc(pg_sina_search_context(),sizeof(struct qp_plan));
    return pg_sina_path_search->workspace;
}
unsigned *pg_sina_candidate_choices(void) { return pg_sina_path_search->choices; }
unsigned pg_sina_encoding_choice(unsigned id)
{
    List *choices=pg_sina_path_search?pg_sina_path_search->encoding_choices:NIL;
    return id<(unsigned)list_length(choices)?intVal(linitial((List *)list_nth(choices,id))):0;
}
bool pg_sina_encoding_matches(unsigned id,const struct qp_node *node)
{
    List *choices=pg_sina_path_search->encoding_choices;
    if(id>=(unsigned)list_length(choices)) return false;
    List *expected=list_nth(choices,id);
    return ((unsigned)intVal(linitial(expected))&~0xffffU)==pg_sina_physical_choice(node) &&
        intVal(lsecond(expected))==node->left && intVal(lthird(expected))==node->right;
}
/* A planning invocation may compile hundreds of overlapping subtrees. The
 * immutable mapping is the same for all of them; query SPI once per table. */
bool pg_sina_prepared_device(Oid relid,Oid filenode,char *device,size_t length)
{
    ListCell *c;
    if(pg_sina_path_search) foreach(c,pg_sina_path_search->prepared) {
        struct prepared_relation *r=lfirst(c);
        if(r->relid==relid && r->filenode==filenode) {
            strlcpy(device,r->device,length); return r->prepared;
        }
    }
    struct prepared_relation result={.relid=relid,.filenode=filenode};
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID}; Datum values[]={ObjectIdGetDatum(relid),ObjectIdGetDatum(filenode)};
    int status=SPI_execute_with_args("SELECT device FROM sina_csd.relations WHERE relid=$1 AND relfilenode=$2",
        2,types,values,NULL,true,1);
    result.prepared=status==SPI_OK_SELECT && SPI_processed==1;
    if(result.prepared) strlcpy(result.device,SPI_getvalue(SPI_tuptable->vals[0],SPI_tuptable->tupdesc,1),sizeof(result.device));
    SPI_finish();
    if(pg_sina_path_search) {
        MemoryContext previous=MemoryContextSwitchTo(pg_sina_search_context());
        struct prepared_relation *r=palloc(sizeof(*r)); *r=result;
        pg_sina_path_search->prepared=lappend(pg_sina_path_search->prepared,r);
        MemoryContextSwitchTo(previous);
    }
    strlcpy(device,result.device,length); return result.prepared;
}
static struct path_relation *relation_paths(PlannerInfo *root,RelOptInfo *rel)
{
    ListCell *c;
    foreach(c,pg_sina_path_search->relations) {
        struct path_relation *p=lfirst(c);
        if(p->root==root && p->rel==rel) return p;
    }
    struct path_relation *p=palloc0(sizeof(*p)); p->root=root; p->rel=rel;
    p->internal_cost=p->complete_cost=DBL_MAX;
    pg_sina_path_search->relations=lappend(pg_sina_path_search->relations,p);
    return p;
}
static bool eligible(PlannerInfo *root)
{
    Query *q=root->parse;
    return pg_sina_path_search && pg_sina_exe_path>=PG_SINA_CSD && !pg_sina_native_search &&
        !pg_sina_compiling_plan && q->commandType==CMD_SELECT && !q->hasRowSecurity &&
        !q->hasTargetSRFs && !q->rowMarks && !q->windowClause && !q->groupingSets &&
        !q->setOperations && !root->append_rel_list;
}
static void save_roots(PlannerInfo *root,List **states)
{
    ListCell *c;
    foreach(c,*states) if(((struct root_state *)lfirst(c))->root==root) return;
    struct root_state *s=palloc(sizeof(*s)); s->root=root; s->saved=*root;
    *states=lappend(*states,s);
    for(int i=1;i<root->simple_rel_array_size;++i) {
        RelOptInfo *rel=root->simple_rel_array[i];
        if(rel && rel->subroot) save_roots(rel->subroot,states);
    }
}
static void restore_roots(List *states)
{
    ListCell *c; foreach(c,states) {
        struct root_state *s=lfirst(c); *s->root=s->saved;
    }
}
/* create_plan normally runs once after path selection. Preview on a private
 * PlannerInfo/global workspace so discarded candidates cannot consume Params,
 * attach InitPlans or change the selected query's planning state. */
static Plan *preview_plan(PlannerInfo *root,Path *path)
{
    PlannerInfo preview=*root;
    PlannerGlobal *shared=root->glob,saved=*shared,globals=saved;
    List *states=NIL; save_roots(root,&states);
    ListCell *c; foreach(c,shared->subroots) save_roots(lfirst(c),&states);
    globals.paramExecTypes=list_copy(globals.paramExecTypes);
    globals.subplans=list_copy(globals.subplans);
    globals.subroots=list_copy(globals.subroots);
    /* SubqueryScan planning also visits existing subroots. Give all of them
     * the isolated global workspace, then restore their transient fields. */
    *shared=globals; preview.glob=shared;
    preview.processed_tlist=make_tlist_from_pathtarget(path->pathtarget);
    bool compiling=pg_sina_compiling_plan,previewing=pg_sina_previewing_path;
    Plan *volatile plan=NULL;
    pg_sina_compiling_plan=true; pg_sina_previewing_path=true;
    PG_TRY();
    {
        plan=pg_sina_native_plan(create_plan(&preview,path));
        /* CteScan needs its producer in init_plans while PG builds the Plan.
         * Detach only after construction; device compilation owns init work. */
        plan->initPlan=NIL;
    }
    PG_CATCH();
    { restore_roots(states); *shared=saved;
      pg_sina_compiling_plan=compiling; pg_sina_previewing_path=previewing; PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_compiling_plan=compiling; pg_sina_previewing_path=previewing;
    restore_roots(states); *shared=saved;
    return plan;
}
static Plan *device_plan(PlannerInfo *root,RelOptInfo *rel,CustomPath *path,List *tlist,List *clauses,List *plans)
{
    Plan *native=linitial(path->custom_private);
    if(pg_sina_previewing_path) {
        /* Borrow the immutable prepared child. The whole preview receives
         * its single private copy in prepare_candidate before any rewrites. */
        Plan *view=pg_sina_native_wrapper(native);
        view->targetlist=tlist;
        if(pg_sina_planning) ++pg_sina_planning->preview_borrows;
        return view;
    }
    Plan *copy=copyObject(native);
    apply_tlist_labeling(copy->targetlist,tlist);
    bool top=intVal(lthird(path->custom_private)),fast=intVal(lfourth(path->custom_private));
    Plan *device=defer_program(root,copy,fast,!top,lsecond(path->custom_private),list_nth(path->custom_private,4));
    if(top) root->init_plans=NIL;
    device->plan_rows=path->path.rows;
    /* PG may add grouping/sort labels after the base/join hook. The stored
     * expressions stay unchanged, but upper Paths need their current labels. */
    apply_tlist_labeling(device->targetlist,tlist);
    return device;
}
static const CustomPathMethods device_methods={.CustomName="SniaDevicePath",.PlanCustomPath=device_plan};

/* These unary paths can extend an existing device pipeline. Stop at the
 * first operator that could change where the pipeline's input is produced. */
static bool extends_device_path(Path *path)
{
    bool extends=false;
    for(;;) {
        if(IsA(path,ProjectionPath)) path=((ProjectionPath *)path)->subpath;
        else if(IsA(path,SortPath)) path=((SortPath *)path)->subpath;
        else if(IsA(path,LimitPath)) path=((LimitPath *)path)->subpath;
        else break;
        extends=true;
    }
    return extends && IsA(path,CustomPath) && ((CustomPath *)path)->methods==&device_methods;
}

static void add_device_path(PlannerInfo *root,RelOptInfo *rel,Path *native,bool top)
{
    /* Only SeqScan supplies a shared, partitioned PG stream. A partial Join
     * or a worker-private subtree cannot be replaced by that single stream. */
    if(native->param_info ||
       (native->parallel_workers>0 && (!native->parallel_aware || native->pathtype!=T_SeqScan)) ||
       (native->parallel_aware && native->pathtype!=T_SeqScan) ||
       (IsA(native,CustomPath) && (((CustomPath *)native)->methods==&device_methods ||
                                 ((CustomPath *)native)->methods==&final_methods))) return;
    struct path_relation *state=relation_paths(root,rel);
    if(pg_sina_planning) ++pg_sina_planning->candidates;
    /* Retain a PG plan and small cost metadata, not a serialized device
     * program for every discarded candidate. Compile the winner once later. */
    MemoryContext owner=CurrentMemoryContext;
    MemoryContext temporary=AllocSetContextCreate(owner,"CSD Path estimate",ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(temporary);
    Plan *volatile plan=NULL;
    double internal=0,boundary=0;
    bool compiling=pg_sina_compiling_plan;
    Plan *volatile device=NULL;
    PG_TRY();
    {
        double mark=pg_sina_planner_clock();
        plan=preview_plan(root,native);
        if(pg_sina_planning) pg_sina_planning->preview+=pg_sina_planner_clock()-mark;
        pg_sina_compiling_plan=true;
        device=pg_sina_cost_device(root,plan,top,&pg_sina_path_search->observations,
                                  &internal,&boundary);
    }
    PG_CATCH();
    { pg_sina_compiling_plan=compiling; MemoryContextSwitchTo(owner); MemoryContextDelete(temporary); PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_compiling_plan=compiling;
    MemoryContextSwitchTo(owner);
    if(top && IsA(native,LimitPath) && pg_sina_profile)
        ereport(NOTICE,(errmsg("SQLerate Limit candidate supported=%d host_ms=%.6f device_ms=%.6f boundary_ms=%.6f rows=%.0f",
            device!=NULL,native->total_cost*pg_sina_host_cost_ms,internal,boundary,device?device->plan_rows:0)));
    if(!device) { MemoryContextDelete(temporary); return; }
    double mark=pg_sina_planner_clock();
    if(pg_sina_planning) ++pg_sina_planning->accepted;
    CustomPath *path=makeNode(CustomPath); path->path=*native;
    NodeSetTag(path,T_CustomPath); path->path.pathtype=T_CustomScan;
    if(!intVal(list_nth(((CustomScan *)device)->custom_private,4))) path->path.pathkeys=NIL;
    path->path.parallel_aware=path->path.parallel_safe=native->parallel_aware;
    path->path.parallel_workers=native->parallel_aware?native->parallel_workers:0;
    /* The device stream has global rows. A partial PG Path describes each
     * consumer's share; all consumers read the same DSM delivery queue. */
    path->path.rows=native->parallel_aware?native->rows:device->plan_rows;
    path->path.total_cost=(internal+boundary)/pg_sina_host_cost_ms;
    path->path.startup_cost=pg_sina_is_scan(plan)?Min(path->path.total_cost,device->startup_cost):path->path.total_cost;
    path->custom_private=list_make5(lthird(((CustomScan *)device)->custom_private),copyObject(lsecond(((CustomScan *)device)->custom_private)),
        makeInteger(top),makeInteger(((CustomScan *)device)->methods==&pg_sina_scan_methods),copyObject(lfourth(((CustomScan *)device)->custom_private)));
    path->methods=&device_methods;
    /* Within PG's 1% path cost tolerance, keep supported unary operators
     * with their existing device input if the device preserves the order.
     * An ordered Limit can then supply its bound to the device Sort. */
    if(extends_device_path(native) &&
       pathkeys_contained_in(native->pathkeys,path->path.pathkeys) &&
       path->path.total_cost<=native->total_cost*1.01)
        rel->pathlist=list_delete_ptr(rel->pathlist,native);
    /* Forced offload uses the same physical alternatives and prices, but
     * retains a complete device candidate even if a HOST path is cheaper. */
    if(top && path->path.total_cost<state->complete_cost) {
        state->complete=palloc(sizeof(*path)); *state->complete=*path;
        state->complete_cost=path->path.total_cost;
    }
    if(pg_sina_planning) pg_sina_planning->retain_copy+=pg_sina_planner_clock()-mark;
    double freed=pg_sina_planner_clock();
    MemoryContextDelete(temporary);
    if(pg_sina_planning) pg_sina_planning->retain_free+=pg_sina_planner_clock()-freed;
    freed=pg_sina_planner_clock();
    /* PG may prune the delivered path. Keep a distinct object for parent
     * device joins: their inputs stay in device memory and pay no boundary. */
    if(!native->parallel_aware && internal<state->internal_cost) {
        state->internal=palloc(sizeof(*path)); *state->internal=*path;
        state->internal->path.startup_cost=state->internal->path.total_cost=internal/pg_sina_host_cost_ms;
        state->internal_cost=internal;
    }
    if(native->pathtype==T_SeqScan) {
        unsigned shared=native->parallel_aware;
        if(!state->scan[shared] || path->path.total_cost<state->scan[shared]->path.total_cost) {
            state->scan[shared]=palloc(sizeof(*path)); *state->scan[shared]=*path;
        }
    }
    state->pending=lappend(state->pending,path);
    if(pg_sina_planning) pg_sina_planning->retain_publish+=pg_sina_planner_clock()-freed;
    if(pg_sina_planning) pg_sina_planning->retain+=pg_sina_planner_clock()-mark;
}
static void publish_paths(PlannerInfo *root,RelOptInfo *rel)
{
    struct path_relation *state=relation_paths(root,rel);
    List *pending=state->pending; state->pending=NIL;
    ListCell *c; foreach(c,pending) {
        Path *path=lfirst(c);
        if(path->parallel_workers>0) add_partial_path(rel,path);
        else add_path(rel,path);
    }
    list_free(pending);
}
/* Use PG's hash budget to price one possible growth event. For an
 * already-batched Parallel Hash, PG has priced the normal read/write pass;
 * only the additional build-side repartition belongs here. */
static double hash_batch_risk(PlannerInfo *root,HashPath *hash,double *startup,double *total)
{
    JoinPath *join=&hash->jpath;
    Path *inner=join->innerjoinpath,*outer=join->outerjoinpath;
    int planned=hash->num_batches;
    bool shared=join->path.parallel_aware;
    if(planned>1 && !shared) return 0;
    double uncertainty=1;
    int member=-1;
    while((member=bms_next_member(inner->parent->relids,member))>=0)
        uncertainty=Max(uncertainty,relation_paths(root,root->simple_rel_array[member])->row_uncertainty);
    /* Joined builds have no joint cardinality statistics. Test a one-growth
     * sensitivity interval (up to twice the estimated rows), without
     * inflating the row count used for normal join or scan work. */
    if(planned>1 && bms_membership(inner->parent->relids)==BMS_MULTIPLE)
        uncertainty=Max(uncertainty,2);
    if(uncertainty<=1) return 0;
    int workers=shared?outer->parallel_workers:0;
    double divisor=shared?Max(1,inner->parallel_workers+(parallel_leader_participation?
        Max(0,1-0.3*inner->parallel_workers):0)):1;
    double rows=inner->rows*divisor,upper=rows*uncertainty;
    int width=inner->pathtarget->width,buckets,batches,skew;
    size_t space;
    ExecChooseHashTableSize(upper,width,true,shared,workers,&space,&buckets,&batches,&skew);
    if(batches<=planned) return 0;
    double low=rows,high=upper;
    while(high-low>Max(1,rows/128)) {
        double mid=(high+low)/2;
        ExecChooseHashTableSize(mid,width,true,shared,workers,&space,&buckets,&batches,&skew);
        if(batches>planned) high=mid; else low=mid;
    }
    double risk=(upper-high)/(upper-rows);
    if(planned>1) {
        /* Growth happens during the build, before partitioning the probe
         * side. At the capacity crossing, read the already-spooled batches
         * and write their replacements plus the displaced in-memory rows.
         * Use PG's parallel per-participant I/O convention for this pass. */
        double tuple_bytes=MAXALIGN(width)+MAXALIGN(SizeofMinimalTupleHeader)+sizeof(uint32);
        double read_pages=ceil(high*(1-1.0/planned)*tuple_bytes/BLCKSZ);
        double write_pages=ceil(high*(1-0.5/planned)*tuple_bytes/BLCKSZ);
        *startup=*total=risk*((read_pages+write_pages)*seq_page_cost+
            high*2*cpu_operator_cost)/divisor;
        if(pg_sina_planning)
            ereport(DEBUG1,(errmsg("SQLerate HOST Hash growth batches=%d build_rows=%.0f crossing_rows=%.0f risk=%.3f read_pages=%.0f write_pages=%.0f extra_ms=%.3f",
                planned,rows,high,risk,read_pages,write_pages,*total*pg_sina_host_cost_ms)));
        return risk;
    }
    double build=(high+upper)/2;
    double probe_divisor=shared?Max(1,outer->parallel_workers+(parallel_leader_participation?
        Max(0,1-0.3*outer->parallel_workers):0)):1;
    double build_pages=ceil(build*(MAXALIGN(width)+MAXALIGN(SizeofMinimalTupleHeader))/BLCKSZ);
    double probe_pages=ceil(outer->rows*probe_divisor*
        (MAXALIGN(outer->pathtarget->width)+MAXALIGN(SizeofMinimalTupleHeader))/BLCKSZ);
    /* Parallel workers share the temporary-file I/O path. Count all pages
     * once and scale only the CPU of dispatching tuples to batch files. */
    *startup=risk*(build_pages*seq_page_cost+build/divisor*cpu_operator_cost);
    *total=risk*(2*(build_pages+probe_pages)*seq_page_cost+
        (build/divisor+outer->rows)*cpu_operator_cost);
    return risk;
}
static double price_hash_batch_risk(PlannerInfo *root,HashPath *hash)
{
    double startup=0,total=0;
    double risk=hash_batch_risk(root,hash,&startup,&total);
    hash->jpath.path.startup_cost+=startup;
    hash->jpath.path.total_cost+=total;
    return risk;
}
static void observe_host_hash(PlannerInfo *root,HashPath *hash)
{
    double startup=0,total=0;
    if(hash_batch_risk(root,hash,&startup,&total)<=0) return;
    const JoinPath *join=&hash->jpath;
    MemoryContext previous=MemoryContextSwitchTo(pg_sina_search_context());
    struct host_hash_cost *cost=palloc(sizeof(*cost));
    *cost=(struct host_hash_cost){join->path.rows,join->path.startup_cost,join->path.total_cost,
        join->outerjoinpath->total_cost,join->innerjoinpath->total_cost,total,hash->num_batches};
    pg_sina_path_search->host_hashes=lappend(pg_sina_path_search->host_hashes,cost);
    MemoryContextSwitchTo(previous);
}
double pg_sina_host_hash_penalty(Plan *plan,bool growth)
{
    if(!pg_sina_path_search || !IsA(plan,HashJoin) || !plan->lefttree ||
       !plan->righttree || !plan->righttree->lefttree) return 0;
    ListCell *c;
    foreach(c,pg_sina_path_search->host_hashes) {
        const struct host_hash_cost *cost=lfirst(c);
        if((cost->batches>1)==growth &&
           cost->rows==plan->plan_rows && cost->startup==plan->startup_cost && cost->total==plan->total_cost &&
           cost->outer==plan->lefttree->total_cost && cost->inner==plan->righttree->lefttree->total_cost) {
            if(pg_sina_planning) ereport(NOTICE,(errmsg("SQLerate HOST Hash batches=%d penalty_ms=%.3f",
                cost->batches,cost->penalty*pg_sina_host_cost_ms)));
            return cost->penalty*pg_sina_host_cost_ms;
        }
    }
    return 0;
}
static double hash_probe_credit(HashPath *hash,double outer_work,double inner_work)
{
    JoinPath *join=&hash->jpath;
    double cpu=Max(0,join->path.total_cost-join->outerjoinpath->total_cost-join->innerjoinpath->total_cost);
    double build=Min(cpu,join->innerjoinpath->rows*(cpu_tuple_cost+
        list_length(hash->path_hashclauses)*cpu_operator_cost));
    double probe=Min(Max(0,cpu-build),outer_work);
    join->path.total_cost=Max(join->path.startup_cost,join->path.total_cost-Min(build,inner_work)-probe);
    return probe;
}
/* Retain a streaming probe alternative until downstream HOST work can hide
 * its producer. Build-side work ends at this Hash boundary; Sort/Aggregate,
 * Gather and Materialize do not enter this chain. */
static void add_scan_join_paths(PlannerInfo *root,RelOptInfo *rel,JoinPath *native,JoinPathExtraData *extra)
{
    bool hash=IsA(native,HashPath);
    if(!hash && !IsA(native,NestPath)) return;
    if(hash && ((HashPath *)native)->num_batches!=1) return;
    Path *inputs[]={native->outerjoinpath,native->innerjoinpath},*sources[2]={NULL,NULL};
    double work[2]={0,0};
    for(unsigned side=0;side<2;++side) {
        Path *input=inputs[side];
        if(input->param_info || (!hash && side)) continue;
        struct path_relation *state=relation_paths(root,input->parent);
        bool scan=input->pathtype==T_SeqScan ||
            (IsA(input,CustomPath) && ((CustomPath *)input)->methods==&device_methods &&
             pg_sina_is_scan(linitial(((CustomPath *)input)->custom_private)));
        /* Conditional SEMI/ANTI early stop has no stable per-input consumer
         * work. Keep its direct-Scan estimate, not a multi-Join credit carried
         * across an uncertain membership/output cardinality. */
        if(!scan && native->jointype!=JOIN_INNER) continue;
        unsigned shared=input->parallel_workers>0;
        Path *source=scan?(state->scan[shared]?&state->scan[shared]->path:NULL):state->stream[shared];
        if(source && source->rows==input->rows && source->parallel_workers==input->parallel_workers &&
           (!native->path.parallel_safe || source->parallel_safe) && equal(source->pathkeys,input->pathkeys) &&
           equal(source->pathtarget->exprs,input->pathtarget->exprs)) {
            sources[side]=source;
            /* PG conversion runs in the consumer, so cannot also be hidden
             * by that consumer's Join CPU. Only the producer is independent. */
            work[side]=scan?pg_sina_scan_stream_work(source):state->stream_work[shared];
        }
    }
    for(unsigned mask=1;mask<4;++mask) {
        if(((mask&1) && !sources[0]) || ((mask&2) && !sources[1])) continue;
        JoinPath *join=hash?(JoinPath *)palloc(sizeof(HashPath)):(JoinPath *)palloc(sizeof(NestPath));
        memcpy(join,native,hash?sizeof(HashPath):sizeof(NestPath));
        for(unsigned side=0;side<2;++side) if(mask&(1U<<side)) {
            if(side) join->innerjoinpath=sources[side]; else join->outerjoinpath=sources[side];
        }
        /* Revisit hooks may see a previously discounted Join. Reprice from
         * its inputs, rather than subtracting another credit from that Join. */
        JoinCostWorkspace workspace;
        double outer_credit=0;
        if(hash) {
            HashPath *h=(HashPath *)join;
            initial_cost_hashjoin(root,&workspace,join->jointype,h->path_hashclauses,
                join->outerjoinpath,join->innerjoinpath,extra,join->path.parallel_aware);
            final_cost_hashjoin(root,h,&workspace,extra);
            if(h->num_batches!=1) continue;
            double overlap=1-price_hash_batch_risk(root,h);
            outer_credit=hash_probe_credit(h,mask&1?work[0]*overlap:0,mask&2?work[1]*overlap:0);
        } else {
            initial_cost_nestloop(root,&workspace,join->jointype,join->outerjoinpath,join->innerjoinpath,extra);
            final_cost_nestloop(root,(NestPath *)join,&workspace,extra);
        }
        if(!hash && (mask&1)) {
            double before=join->path.total_cost;
            join->path.total_cost=pg_sina_host_index_pipeline_cost(root,(NestPath *)join,extra,work[0]);
            outer_credit=Max(0,before-join->path.total_cost);
        }
        if(pg_sina_planning)
            ereport(DEBUG1,(errmsg("SQLerate mixed %s workers=%d device_inputs=%u total_ms=%.3f",
                hash?"hash":"nested loop",join->path.parallel_workers,mask,
                join->path.total_cost*pg_sina_host_cost_ms)));
        double remaining=(mask&1) && native->jointype==JOIN_INNER?Max(0,work[0]-outer_credit):0;
        unsigned shared=join->path.parallel_workers>0;
        struct path_relation *state=relation_paths(root,rel);
        if((mask&1) && native->jointype==JOIN_INNER && !join->path.param_info &&
           (!state->stream[shared] || join->path.total_cost<state->stream[shared]->total_cost)) {
            /* PG may free a pruned Path; retain a separate, statement-owned
             * copy, as for the existing scan/internal device alternatives. */
            state->stream[shared]=palloc(hash?sizeof(HashPath):sizeof(NestPath));
            memcpy(state->stream[shared],join,hash?sizeof(HashPath):sizeof(NestPath));
            state->stream_work[shared]=remaining;
        }
        relation_paths(root,rel)->pending=lappend(relation_paths(root,rel)->pending,join);
    }
}
/* PG's precheck sees the original parameterized Index price before our
 * cache/FK correction and can discard the Join before the hook sees it.
 * Reconsider PG's legal indexes for the retained stream, then submit the
 * corrected price through ordinary add_path/add_partial_path pruning. */
static void add_stream_index_paths(PlannerInfo *root,RelOptInfo *rel,RelOptInfo *outer,
    RelOptInfo *inner,JoinType type,JoinPathExtraData *extra)
{
    if((type!=JOIN_INNER && type!=JOIN_SEMI && type!=JOIN_ANTI) ||
       rel->lateral_relids || outer->top_parent_relids || inner->top_parent_relids) return;
    struct path_relation *state=relation_paths(root,outer);
    for(unsigned shared=0;shared<2;++shared) {
        /* Existence joins can stop the inner lookup early. Consider a direct
         * scan producer and let PG price the semi/anti lookup itself. */
        Path *source=type==JOIN_INNER?state->stream[shared]:NULL;
        if(!source && state->scan[shared]) source=&state->scan[shared]->path;
        if(!source || source->param_info) continue;
        ListCell *c;
        foreach(c,inner->pathlist) {
            Path *index=lfirst(c);
            if(!IsA(index,IndexPath) || !index->param_info ||
               !bms_is_subset(PATH_REQ_OUTER(index),outer->relids) ||
               (shared && (!rel->consider_parallel || !index->parallel_safe))) continue;
            JoinCostWorkspace workspace;
            initial_cost_nestloop(root,&workspace,type,source,index,extra);
            NestPath *join=create_nestloop_path(root,rel,type,&workspace,extra,source,index,
                extra->restrictlist,build_join_pathkeys(root,rel,type,source->pathkeys),NULL);
            Path *priced=pg_sina_reprice_index_join(root,join,extra,&pg_sina_path_search->observations,
                host_cache_pages(root,source));
            relation_paths(root,rel)->pending=lappend(relation_paths(root,rel)->pending,priced);
            add_scan_join_paths(root,rel,(JoinPath *)priced,extra);
        }
    }
}
void pg_sina_rel_paths(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte)
{
    if(!pg_sina_path_search || pg_sina_exe_path<PG_SINA_CSD || pg_sina_compiling_plan) return;
    bool device=eligible(root);
    if(pg_sina_exe_path==PG_SINA_ADAPTIVE)
        relation_paths(root,rel)->row_uncertainty=pg_sina_refine_base_rows(root,rel,rti,rte);
    List *paths=list_concat(list_copy(rel->pathlist),list_copy(rel->partial_pathlist)); ListCell *c;
    foreach(c,paths) {
        Path *path=lfirst(c);
        if(pg_sina_exe_path==PG_SINA_ADAPTIVE && rte->rtekind==RTE_RELATION)
            pg_sina_cost_host_path(root,path,rte->relid,&pg_sina_path_search->observations,!pg_sina_native_search);
        if(device && pg_sina_exe_path==PG_SINA_ADAPTIVE && path->pathtype==T_SeqScan &&
           path->parallel_aware && !path->param_info) {
            struct path_relation *state=relation_paths(root,rel);
            if(!state->host_parallel_scan || path->total_cost<state->host_parallel_scan->total_cost) {
                state->host_parallel_scan=palloc(sizeof(*path));
                *state->host_parallel_scan=*path;
            }
        }
        if(device) add_device_path(root,rel,path,false);
    }
    list_free(paths);
    if(device) publish_paths(root,rel);
}
void pg_sina_join_paths(PlannerInfo *root,RelOptInfo *joinrel,RelOptInfo *outerrel,
                        RelOptInfo *innerrel,JoinType jointype,JoinPathExtraData *extra)
{
    if(pg_sina_path_search && pg_sina_exe_path==PG_SINA_ADAPTIVE && pg_sina_native_search && !pg_sina_compiling_plan) {
        List *paths=list_concat(list_copy(joinrel->pathlist),list_copy(joinrel->partial_pathlist)); ListCell *c;
        foreach(c,paths) {
            Path *path=lfirst(c);
            if(!IsA(path,NestPath) && !IsA(path,HashPath)) continue;
            JoinPath *join=(JoinPath *)path;
            if(join->outerjoinpath->parent!=outerrel || join->innerjoinpath->parent!=innerrel) continue;
            if(IsA(path,NestPath))
                pg_sina_cost_host_lookup(root,path,extra,&pg_sina_path_search->observations);
            else observe_host_hash(root,(HashPath *)path);
        }
        list_free(paths);
    }
    if(!eligible(root)) return;
    pg_sina_refine_host_anti(root,joinrel,outerrel,jointype,extra);
    /* Inspect PG's current alternatives too, preserving Index/Merge choices. */
    List *paths=list_concat(list_copy(joinrel->pathlist),list_copy(joinrel->partial_pathlist)); ListCell *c;
    foreach(c,paths) {
        Path *p=lfirst(c);
        if((IsA(p,HashPath) || IsA(p,MergePath) || IsA(p,NestPath)) &&
           ((JoinPath *)p)->outerjoinpath->parent==outerrel &&
           ((JoinPath *)p)->innerjoinpath->parent==innerrel) {
            JoinPath *join=(JoinPath *)p;
            if(IsA(p,HashPath) && join->jointype==jointype && pg_sina_exe_path==PG_SINA_ADAPTIVE) {
                HashPath *hash=(HashPath *)p;
                /* Charge uncertain extra passes at final placement, keeping
                 * PG's join ordering for already batched paths. */
                if(hash->num_batches>1) observe_host_hash(root,hash);
                else price_hash_batch_risk(root,hash);
            }
            /* A delivered source already received the overlap credit when
             * this Path was built. Recosting it on a later hook invocation
             * would silently erase that credit and prune the mixed Path. */
            bool delivered=IsA(join->outerjoinpath,CustomPath) &&
                ((CustomPath *)join->outerjoinpath)->methods==&device_methods;
            if(IsA(p,NestPath) && pg_sina_exe_path==PG_SINA_ADAPTIVE && !delivered) {
                Path *priced=pg_sina_reprice_index_join(root,(NestPath *)p,extra,&pg_sina_path_search->observations,
                    host_cache_pages(root,((NestPath *)p)->outerjoinpath));
                if(priced!=p) {
                    joinrel->pathlist=list_delete_ptr(joinrel->pathlist,p);
                    joinrel->partial_pathlist=list_delete_ptr(joinrel->partial_pathlist,p);
                    relation_paths(root,joinrel)->pending=lappend(relation_paths(root,joinrel)->pending,priced);
                    p=priced;
                }
            }
            if(pg_sina_exe_path==PG_SINA_ADAPTIVE) add_scan_join_paths(root,joinrel,(JoinPath *)p,extra);
            add_device_path(root,joinrel,p,false);
        }
    }
    list_free(paths);
    if(pg_sina_exe_path==PG_SINA_ADAPTIVE)
        add_stream_index_paths(root,joinrel,outerrel,innerrel,jointype,extra);
    publish_paths(root,joinrel);
    if(jointype!=JOIN_INNER && jointype!=JOIN_LEFT && jointype!=JOIN_RIGHT &&
       jointype!=JOIN_SEMI && jointype!=JOIN_ANTI) return;
    struct path_relation *outer=relation_paths(root,outerrel),*inner=relation_paths(root,innerrel);
    /* PG can discard a parameterized loop before this hook on HOST costs.
     * An internal probe has its own I/O cost and consumes the device child
     * directly, so let the device estimator price the same legal index. */
    if(jointype==JOIN_INNER && outer->internal && !joinrel->lateral_relids &&
       !outerrel->top_parent_relids && !innerrel->top_parent_relids) {
        Path *probe=&outer->internal->path;
        foreach(c,innerrel->pathlist) {
            Path *index=lfirst(c);
            if(!IsA(index,IndexPath) || !index->param_info ||
               !bms_is_subset(PATH_REQ_OUTER(index),outerrel->relids)) continue;
            JoinCostWorkspace workspace;
            initial_cost_nestloop(root,&workspace,jointype,probe,index,extra);
            Path *join=(Path *)create_nestloop_path(root,joinrel,jointype,&workspace,extra,
                probe,index,extra->restrictlist,build_join_pathkeys(root,joinrel,jointype,probe->pathkeys),NULL);
            add_device_path(root,joinrel,join,false);
        }
        publish_paths(root,joinrel);
    }
    bool device=outer->internal && inner->internal;
    bool mixed=pg_sina_exe_path==PG_SINA_ADAPTIVE && jointype==JOIN_INNER &&
        enable_hashjoin && enable_parallel_hash && joinrel->consider_parallel &&
        !joinrel->lateral_relids && !outerrel->top_parent_relids && !innerrel->top_parent_relids &&
        (outer->scan[1] || inner->scan[1]);
    if(!device && !mixed) return;
    List *keys=NIL;
    foreach(c,extra->restrictlist) {
        RestrictInfo *r=lfirst(c);
        if(IS_OUTER_JOIN(jointype) && RINFO_IS_PUSHED_DOWN(r,joinrel->relids)) continue;
        if(!r->can_join || !OidIsValid(r->hashjoinoperator)) continue;
        bool left=bms_is_subset(r->left_relids,outerrel->relids) && bms_is_subset(r->right_relids,innerrel->relids);
        bool right=bms_is_subset(r->right_relids,outerrel->relids) && bms_is_subset(r->left_relids,innerrel->relids);
        if(left || right) {
            r->outer_is_left=left;
            keys=lappend(keys,r);
        }
    }
    if(!keys) return;
    JoinCostWorkspace workspace;
    if(mixed) {
        /* PG can prune the native Hash Join before device delivery overlap
         * has been priced. Retain a cached HOST input as an alternative to
         * downloading both scans, then use normal partial-path pruning. */
        for(unsigned mask=1;mask<4;++mask) {
            if(((mask&1) && !outer->scan[1]) || ((mask&2) && !inner->scan[1])) continue;
            Path *probe=mask&1?&outer->scan[1]->path:outer->host_parallel_scan;
            Path *build=mask&2?&inner->scan[1]->path:inner->host_parallel_scan;
            if(!probe || !build) continue;
            initial_cost_hashjoin(root,&workspace,jointype,keys,probe,build,extra,true);
            HashPath *join=create_hashjoin_path(root,joinrel,jointype,&workspace,extra,
                probe,build,true,extra->restrictlist,NULL,keys);
            if(join->num_batches!=1) continue;
            double overlap=1-price_hash_batch_risk(root,join);
            hash_probe_credit(join,mask&1?pg_sina_scan_stream_work(probe)*overlap:0,
                mask&2?pg_sina_scan_stream_work(build)*overlap:0);
            if(pg_sina_planning)
                ereport(DEBUG1,(errmsg("SQLerate retained parallel hash device_inputs=%u total_ms=%.3f",
                    mask,join->jpath.path.total_cost*pg_sina_host_cost_ms)));
            add_partial_path(joinrel,&join->jpath.path);
        }
    }
    if(device) {
        initial_cost_hashjoin(root,&workspace,jointype,keys,&outer->internal->path,&inner->internal->path,extra,false);
        Path *join=(Path *)create_hashjoin_path(root,joinrel,jointype,&workspace,extra,
            &outer->internal->path,&inner->internal->path,false,extra->restrictlist,NULL,keys);
        add_device_path(root,joinrel,join,false);
    }
    publish_paths(root,joinrel);
}
static void placement(Plan *p,StringInfo text)
{
    if(!p) return;
    if(text->len) appendStringInfoString(text,"; ");
    if(IsA(p,CustomScan) && (((CustomScan *)p)->methods==&pg_sina_plan_methods ||
                            ((CustomScan *)p)->methods==&pg_sina_scan_methods)) {
        appendStringInfoString(text,"subtree:CSD"); return;
    }
    appendStringInfo(text,"%s:HOST",IsA(p,HashJoin)?"HashJoin":IsA(p,MergeJoin)?"MergeJoin":
        IsA(p,NestLoop)?"NestLoop":IsA(p,Agg)?"Aggregate":IsA(p,Sort)?"Sort":
        IsA(p,Limit)?"Limit":IsA(p,Gather)?"Gather":IsA(p,GatherMerge)?"GatherMerge":"Native");
    placement(p->lefttree,text); placement(p->righttree,text);
    if(IsA(p,SubqueryScan)) placement(((SubqueryScan *)p)->subplan,text);
}
static Plan *final_plan(PlannerInfo *root,RelOptInfo *rel,CustomPath *path,List *tlist,List *clauses,List *plans)
{
    Plan *plan=linitial(plans);
    StringInfo text=makeStringInfo(); placement(plan,text);
    appendStringInfo(text,"; query=%s",pg_sina_native_search?"original HOST":"shared PG Path search");
    Plan *wrapper=pg_sina_wrap_placement(plan,text->data,"");
    if(pg_sina_native_search) {
        /* Keep PG's original optimum as another physical device candidate.
         * Both children go through this statement's own setrefs/permissions;
         * the caller selects one before execution, not at runtime. */
        Plan *native=copyObject(plan); native->initPlan=NIL;
        double internal=0,boundary=0;
        bool compiling=pg_sina_compiling_plan;
        Plan *volatile device=NULL;
        pg_sina_compiling_plan=true;
        PG_TRY();
        { device=pg_sina_cost_device(root,native,true,&pg_sina_path_search->observations,
                                     &internal,&boundary); }
        PG_CATCH();
        { pg_sina_compiling_plan=compiling; PG_RE_THROW(); }
        PG_END_TRY();
        pg_sina_compiling_plan=compiling;
        if(device) {
            CustomScan *scan=(CustomScan *)wrapper;
            CustomScan *candidate=(CustomScan *)device;
            double rows=device->plan_rows;
            device=defer_program(root,lthird(candidate->custom_private),candidate->methods==&pg_sina_scan_methods,false,lsecond(candidate->custom_private),lfourth(candidate->custom_private));
            device->plan_rows=rows;
            scan->custom_plans=lappend(scan->custom_plans,device);
            scan->custom_private=lappend(scan->custom_private,makeConst(FLOAT8OID,-1,InvalidOid,sizeof(double),
                Float8GetDatum((internal+boundary)/pg_sina_host_cost_ms),false,FLOAT8PASSBYVAL));
        }
    }
    return wrapper;
}
PlannedStmt *pg_sina_original_device_statement(PlannedStmt *host)
{
    if(!IsA(host->planTree,CustomScan)) return NULL;
    CustomScan *scan=(CustomScan *)host->planTree;
    if(scan->methods!=&pg_sina_adaptive_methods || list_length(scan->custom_plans)!=2) return NULL;
    PlannedStmt *device=copyObject(host);
    CustomScan *out=(CustomScan *)device->planTree;
    out->custom_plans=list_make1(lsecond(out->custom_plans));
    out->scan.plan.initPlan=NIL;
    out->scan.plan.startup_cost=out->scan.plan.total_cost=DatumGetFloat8(lthird_node(Const,out->custom_private)->constvalue);
    lfirst(list_head(out->custom_private))=makeString("subtree:CSD; query=original PG physical tree");
    scan->custom_plans=list_make1(linitial(scan->custom_plans));
    return device;
}
static const CustomPathMethods final_methods={.CustomName="SQLerateSelectedPath",.PlanCustomPath=final_plan};
/* PG builds its unordered grouping and explicit sort on the cheapest HOST
 * delivery path. Keep one continuation of the retained internal candidate,
 * whose intermediate tuples never cross the device boundary. */
static void add_internal_upper_path(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,
                                    RelOptInfo *output,List *paths,void *extra)
{
    if(stage!=UPPERREL_GROUP_AGG && stage!=UPPERREL_ORDERED && stage!=UPPERREL_FINAL) return;
    CustomPath *internal=relation_paths(root,input)->internal;
    if(!internal) return;
    Path *best=input->cheapest_total_path;
    if(best && IsA(best,CustomPath) && ((CustomPath *)best)->methods==&device_methods &&
       ((CustomPath *)best)->custom_private==internal->custom_private) return;
    Path *child=&internal->path,*path=NULL;
    if(stage==UPPERREL_GROUP_AGG) {
        GroupPathExtraData *group=extra;
        AggPath *example=NULL;
        ListCell *c;
        foreach(c,paths) if(IsA(lfirst(c),AggPath)) { example=lfirst(c); break; }
        if(!group || !example) return;
        if(child->pathtarget!=input->reltarget)
            child=(Path *)create_projection_path(root,input,child,input->reltarget);
        AggStrategy strategy=AGG_PLAIN;
        if(example->groupClause) {
            if(group->flags&GROUPING_CAN_USE_HASH) strategy=AGG_HASHED;
            else if(group->flags&GROUPING_CAN_USE_SORT) {
                strategy=AGG_SORTED;
                if(!pathkeys_contained_in(root->group_pathkeys,child->pathkeys))
                    child=(Path *)create_sort_path(root,output,child,root->group_pathkeys,-1);
            } else return;
        }
        AggClauseCosts costs={0};
        get_agg_clause_costs(root,(Node *)example->path.pathtarget->exprs,AGGSPLIT_SIMPLE,&costs);
        get_agg_clause_costs(root,(Node *)example->qual,AGGSPLIT_SIMPLE,&costs);
        path=(Path *)create_agg_path(root,output,child,example->path.pathtarget,strategy,
            AGGSPLIT_SIMPLE,example->groupClause,example->qual,&costs,example->numGroups);
    } else if(stage==UPPERREL_ORDERED) {
        if(!pathkeys_contained_in(root->sort_pathkeys,child->pathkeys))
            child=(Path *)create_sort_path(root,output,child,root->sort_pathkeys,-1);
        path=(Path *)create_projection_path(root,output,child,root->upper_targets[stage]);
    } else {
        FinalPathExtraData *final=extra;
        if(!final || !final->limit_needed ||
           !pathkeys_contained_in(root->sort_pathkeys,child->pathkeys)) return;
        path=(Path *)create_limit_path(root,output,child,root->parse->limitOffset,
            root->parse->limitCount,final->offset_est,final->count_est);
    }
    add_device_path(root,output,path,stage==UPPERREL_FINAL && !root->parent_root);
}
void pg_sina_search_upper_paths(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    if(!pg_sina_path_search || pg_sina_compiling_plan) return;
    if(eligible(root)) {
        List *paths=list_copy(output->pathlist); ListCell *c;
        foreach(c,paths) add_device_path(root,output,lfirst(c),stage==UPPERREL_FINAL && !root->parent_root);
        add_internal_upper_path(root,stage,input,output,paths,extra);
        list_free(paths);
        publish_paths(root,output);
    }
    if(stage!=UPPERREL_FINAL || root->parent_root || !output->pathlist) return;
    Path *best=NULL; ListCell *c;
    foreach(c,output->pathlist) {
        Path *p=lfirst(c); if(!best || p->total_cost<best->total_cost) best=p;
    }
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE && !pg_sina_native_search) {
        CustomPath *complete=relation_paths(root,output)->complete;
        /* No complete shared-search candidate does not rule out the
         * original PG tree's device candidate, compared after planning.
         * Keep this Path's real price until then: the legacy rewrite has
         * neither frozen choices nor a comparable final wrapper cost. */
        if(complete) best=&complete->path;
    }
    CustomPath *path=makeNode(CustomPath); path->path=*best;
    NodeSetTag(path,T_CustomPath); path->path.pathtype=T_CustomScan;
    path->path.parallel_safe=path->path.parallel_aware=false;
    path->custom_paths=list_make1(best); path->methods=&final_methods;
    output->pathlist=list_make1(path);
}
