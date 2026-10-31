/* Put device alternatives into PG's existing join/upper Path search. */
#include "pg_internal.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/memutils.h"
#include <float.h>

struct path_relation {
    PlannerInfo *root;
    RelOptInfo *rel;
    CustomPath *internal; /* Best complete device subtree, without delivery. */
    double internal_cost;
    List *pending;
};
struct prepared_relation { Oid relid,filenode; bool prepared; char device[128]; };
struct pg_sina_path_search { List *relations,*observations,*prepared,*indexes; StringInfo estimates; MemoryContext memory; };
struct pg_sina_path_search *pg_sina_path_search;
bool pg_sina_native_search,pg_sina_previewing_path;
static const CustomPathMethods device_methods,final_methods;

void pg_sina_start_path_search(void)
{
    pg_sina_path_search=palloc0(sizeof(*pg_sina_path_search));
    pg_sina_path_search->memory=CurrentMemoryContext;
    pg_sina_path_search->estimates=makeStringInfo();
}
MemoryContext pg_sina_search_context(void)
{ return pg_sina_path_search?pg_sina_path_search->memory:CurrentMemoryContext; }
List **pg_sina_search_observations(void)
{ return pg_sina_path_search?&pg_sina_path_search->observations:NULL; }
List **pg_sina_search_index_metadata(void)
{ return pg_sina_path_search?&pg_sina_path_search->indexes:NULL; }
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
    struct path_relation *p=palloc0(sizeof(*p)); p->root=root; p->rel=rel; p->internal_cost=DBL_MAX;
    pg_sina_path_search->relations=lappend(pg_sina_path_search->relations,p);
    return p;
}
static bool eligible(PlannerInfo *root)
{
    Query *q=root->parse;
    return pg_sina_path_search && pg_sina_exe_path==PG_SINA_ADAPTIVE && !pg_sina_native_search &&
        !pg_sina_compiling_plan && q->commandType==CMD_SELECT && !q->hasRowSecurity &&
        !q->hasTargetSRFs && !q->rowMarks && !q->windowClause && !q->groupingSets &&
        !q->setOperations && !root->append_rel_list;
}
struct root_state { PlannerInfo *root; PlannerInfo saved; };
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
        Plan *copy=copyObject(native);
        apply_tlist_labeling(copy->targetlist,tlist);
        return pg_sina_native_wrapper(copy);
    }
    Plan *copy=copyObject(native);
    apply_tlist_labeling(copy->targetlist,tlist);
    bool top=intVal(lthird(path->custom_private)),fast=intVal(lfourth(path->custom_private));
    bool compiling=pg_sina_compiling_plan;
    Plan *volatile device=NULL;
    double mark=pg_sina_planner_clock();
    pg_sina_compiling_plan=true;
    PG_TRY();
    { device=fast?pg_sina_fast_plan(root,copy):pg_sina_compile_subtree(root,copy,!top); }
    PG_CATCH();
    { pg_sina_compiling_plan=compiling; PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_compiling_plan=compiling;
    if(pg_sina_planning) pg_sina_planning->compile+=pg_sina_planner_clock()-mark;
    if(!device || device==copy) elog(ERROR,"selected CSD Path no longer compiles");
    if(top) root->init_plans=NIL;
    ((CustomScan *)device)->custom_private=lappend(((CustomScan *)device)->custom_private,copyObject(lsecond(path->custom_private)));
    device->plan_rows=path->path.rows;
    /* PG may add grouping/sort labels after the base/join hook. The stored
     * expressions stay unchanged, but upper Paths need their current labels. */
    apply_tlist_labeling(device->targetlist,tlist);
    return device;
}
static const CustomPathMethods device_methods={.CustomName="SniaDevicePath",.PlanCustomPath=device_plan};

static void add_device_path(PlannerInfo *root,RelOptInfo *rel,Path *native,bool top)
{
    if(native->param_info || native->parallel_aware ||
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
                                  &internal,&boundary,pg_sina_path_search->estimates);
    }
    PG_CATCH();
    { pg_sina_compiling_plan=compiling; MemoryContextSwitchTo(owner); MemoryContextDelete(temporary); PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_compiling_plan=compiling;
    MemoryContextSwitchTo(owner);
    if(!device) { MemoryContextDelete(temporary); return; }
    double mark=pg_sina_planner_clock();
    if(pg_sina_planning) ++pg_sina_planning->accepted;
    CustomPath *path=makeNode(CustomPath); path->path=*native;
    NodeSetTag(path,T_CustomPath); path->path.pathtype=T_CustomScan;
    path->path.parallel_aware=path->path.parallel_safe=false; path->path.parallel_workers=0;
    path->path.rows=device->plan_rows;
    path->path.startup_cost=path->path.total_cost=(internal+boundary)/pg_sina_host_cost_ms;
    path->custom_private=list_make4(copyObject(plan),copyObject(lsecond(((CustomScan *)device)->custom_private)),
        makeInteger(top),makeInteger(((CustomScan *)device)->methods==&pg_sina_scan_methods));
    path->methods=&device_methods;
    MemoryContextDelete(temporary);
    /* PG may prune the delivered path. Keep a distinct object for parent
     * device joins: their inputs stay in device memory and pay no boundary. */
    if(internal<state->internal_cost) {
        state->internal=palloc(sizeof(*path)); *state->internal=*path;
        state->internal->path.startup_cost=state->internal->path.total_cost=internal/pg_sina_host_cost_ms;
        state->internal_cost=internal;
    }
    state->pending=lappend(state->pending,path);
    if(pg_sina_planning) pg_sina_planning->retain+=pg_sina_planner_clock()-mark;
}
static void publish_paths(PlannerInfo *root,RelOptInfo *rel)
{
    struct path_relation *state=relation_paths(root,rel);
    List *pending=state->pending; state->pending=NIL;
    ListCell *c; foreach(c,pending) add_path(rel,lfirst(c));
    list_free(pending);
}
void pg_sina_rel_paths(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte)
{
    if(!eligible(root)) return;
    List *paths=list_concat(list_copy(rel->pathlist),list_copy(rel->partial_pathlist)); ListCell *c;
    foreach(c,paths) {
        Path *path=lfirst(c);
        if(rte->rtekind==RTE_RELATION && !path->param_info) {
            Oid oid=IsA(path,IndexPath) && path->pathtype==T_IndexOnlyScan?((IndexPath *)path)->indexinfo->indexoid:rte->relid;
            path->total_cost=Max(path->startup_cost,pg_sina_host_scan_cost(oid,path->pathtype==T_SeqScan,
                path->rows,path->total_cost*pg_sina_host_cost_ms,&pg_sina_path_search->observations)/pg_sina_host_cost_ms);
        }
        add_device_path(root,rel,path,false);
    }
    list_free(paths);
    publish_paths(root,rel);
}
void pg_sina_join_paths(PlannerInfo *root,RelOptInfo *joinrel,RelOptInfo *outerrel,
                        RelOptInfo *innerrel,JoinType jointype,JoinPathExtraData *extra)
{
    if(!eligible(root)) return;
    /* Inspect PG's current alternatives too, preserving Index/Merge choices. */
    List *paths=list_copy(joinrel->pathlist); ListCell *c;
    foreach(c,paths) {
        Path *p=lfirst(c);
        if((IsA(p,HashPath) || IsA(p,MergePath) || IsA(p,NestPath)) &&
           ((JoinPath *)p)->outerjoinpath->parent==outerrel &&
           ((JoinPath *)p)->innerjoinpath->parent==innerrel) add_device_path(root,joinrel,p,false);
    }
    list_free(paths);
    publish_paths(root,joinrel);
    if(jointype!=JOIN_INNER && jointype!=JOIN_LEFT && jointype!=JOIN_RIGHT &&
       jointype!=JOIN_SEMI && jointype!=JOIN_ANTI) return;
    struct path_relation *outer=relation_paths(root,outerrel),*inner=relation_paths(root,innerrel);
    if(!outer->internal || !inner->internal) return;
    List *keys=NIL;
    foreach(c,extra->restrictlist) {
        RestrictInfo *r=lfirst(c);
        if(IS_OUTER_JOIN(jointype) && RINFO_IS_PUSHED_DOWN(r,joinrel->relids)) continue;
        if(!r->can_join || !OidIsValid(r->hashjoinoperator)) continue;
        if((bms_is_subset(r->left_relids,outerrel->relids) && bms_is_subset(r->right_relids,innerrel->relids)) ||
           (bms_is_subset(r->right_relids,outerrel->relids) && bms_is_subset(r->left_relids,innerrel->relids)))
            keys=lappend(keys,r);
    }
    if(!keys) return;
    JoinCostWorkspace workspace;
    initial_cost_hashjoin(root,&workspace,jointype,keys,&outer->internal->path,&inner->internal->path,extra,false);
    Path *join=(Path *)create_hashjoin_path(root,joinrel,jointype,&workspace,extra,
        &outer->internal->path,&inner->internal->path,false,extra->restrictlist,NULL,keys);
    add_device_path(root,joinrel,join,false);
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
    Plan *wrapper=pg_sina_wrap_placement(plan,text->data,pg_sina_path_search->estimates->data);
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
                                     &internal,&boundary,pg_sina_path_search->estimates); }
        PG_CATCH();
        { pg_sina_compiling_plan=compiling; PG_RE_THROW(); }
        PG_END_TRY();
        pg_sina_compiling_plan=compiling;
        lfirst(list_nth_cell(((CustomScan *)wrapper)->custom_private,1))=
            makeString(pstrdup(pg_sina_path_search->estimates->data));
        if(device) {
            CustomScan *scan=(CustomScan *)wrapper;
            scan->custom_plans=lappend(scan->custom_plans,device);
            scan->custom_private=lappend(scan->custom_private,makeFloat(psprintf("%.17g",internal+boundary)));
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
    out->scan.plan.startup_cost=out->scan.plan.total_cost=floatVal(lthird(out->custom_private))/pg_sina_host_cost_ms;
    lfirst(list_head(out->custom_private))=makeString("subtree:CSD; query=original PG physical tree");
    scan->custom_plans=list_make1(linitial(scan->custom_plans));
    return device;
}
static const CustomPathMethods final_methods={.CustomName="SQLerateSelectedPath",.PlanCustomPath=final_plan};
void pg_sina_search_upper_paths(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output)
{
    if(!pg_sina_path_search || pg_sina_compiling_plan) return;
    if(eligible(root)) {
        List *paths=list_copy(output->pathlist); ListCell *c;
        foreach(c,paths) add_device_path(root,output,lfirst(c),stage==UPPERREL_FINAL && !root->parent_root);
        list_free(paths);
        publish_paths(root,output);
    }
    if(stage!=UPPERREL_FINAL || root->parent_root || pg_sina_exe_path!=PG_SINA_ADAPTIVE || !output->pathlist) return;
    Path *best=NULL; ListCell *c;
    foreach(c,output->pathlist) {
        Path *p=lfirst(c); if(!best || p->total_cost<best->total_cost) best=p;
    }
    CustomPath *path=makeNode(CustomPath); path->path=*best;
    NodeSetTag(path,T_CustomPath); path->path.pathtype=T_CustomScan;
    path->path.parallel_safe=path->path.parallel_aware=false;
    path->custom_paths=list_make1(best); path->methods=&final_methods;
    output->pathlist=list_make1(path);
}
