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
    CustomPath *scan[2]; /* Private/shared source, retained across PG pruning. */
    double internal_cost;
    List *pending;
};
struct prepared_relation { Oid relid,filenode; bool prepared; char device[128]; };
struct root_state { PlannerInfo *root; PlannerInfo saved; };
static void save_roots(PlannerInfo *,List **);
static void restore_roots(List *);
struct device_program { PlannerInfo root; PlannerGlobal globals; Plan *native; List *choices; bool fast,boundary; };
struct pg_sina_path_search {
    List *relations,*observations,*prepared,*indexes,*schemas,*programs;
    MemoryContext memory;
    struct qp_plan *workspace;
    unsigned choices[QP_NODES];
    List *encoding_choices;
};
struct pg_sina_path_search *pg_sina_path_search;
bool pg_sina_native_search,pg_sina_previewing_path;
static const CustomPathMethods device_methods,final_methods;

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
        apply_tlist_labeling(view->targetlist,tlist);
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
/* PG's scalar cost prunes a device source before seeing the independent
 * HOST CPU work that can overlap it. Retain that source and compare it at
 * the immediate HOST join, preserving PG's algorithm and cardinalities. */
static void add_scan_join_paths(PlannerInfo *root,RelOptInfo *rel,JoinPath *native)
{
    bool hash=IsA(native,HashPath);
    if(!hash && !IsA(native,NestPath)) return;
    if(hash && ((HashPath *)native)->num_batches!=1) return;
    Path *inputs[]={native->outerjoinpath,native->innerjoinpath},*sources[2]={NULL,NULL};
    for(unsigned side=0;side<2;++side) {
        Path *input=inputs[side];
        if(input->pathtype!=T_SeqScan || input->param_info || (!hash && side)) continue;
        CustomPath *source=relation_paths(root,input->parent)->scan[input->parallel_aware];
        if(source && source->path.rows==input->rows &&
           (!native->path.parallel_safe || source->path.parallel_safe) &&
           equal(source->path.pathkeys,input->pathkeys)) sources[side]=&source->path;
    }
    double repeats=hash?1:Max(1,inputs[0]->rows);
    double cpu=Max(0,native->path.total_cost-inputs[0]->total_cost-inputs[1]->total_cost*repeats);
    for(unsigned mask=1;mask<4;++mask) {
        if(((mask&1) && !sources[0]) || ((mask&2) && !sources[1])) continue;
        JoinPath *join=hash?(JoinPath *)palloc(sizeof(HashPath)):(JoinPath *)palloc(sizeof(NestPath));
        memcpy(join,native,hash?sizeof(HashPath):sizeof(NestPath));
        double independent=0;
        for(unsigned side=0;side<2;++side) if(mask&(1U<<side)) {
            double count=side?repeats:1;
            join->path.total_cost+=(sources[side]->total_cost-inputs[side]->total_cost)*count;
            join->path.startup_cost+=hash && side?sources[side]->total_cost-inputs[side]->total_cost:
                sources[side]->startup_cost-inputs[side]->startup_cost;
            independent+=(sources[side]->total_cost-sources[side]->startup_cost)*count;
            if(side) join->innerjoinpath=sources[side]; else join->outerjoinpath=sources[side];
        }
        join->path.total_cost=Max(join->path.startup_cost,join->path.total_cost-Min(cpu,independent));
        relation_paths(root,rel)->pending=lappend(relation_paths(root,rel)->pending,join);
    }
}
void pg_sina_rel_paths(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte)
{
    if(!pg_sina_path_search || pg_sina_exe_path!=PG_SINA_ADAPTIVE || pg_sina_compiling_plan) return;
    bool device=eligible(root);
    List *paths=list_concat(list_copy(rel->pathlist),list_copy(rel->partial_pathlist)); ListCell *c;
    foreach(c,paths) {
        Path *path=lfirst(c);
        if(rte->rtekind==RTE_RELATION)
            pg_sina_cost_host_path(root,path,rte->relid,&pg_sina_path_search->observations,!pg_sina_native_search);
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
            if(IsA(path,NestPath) && ((NestPath *)path)->outerjoinpath->parent==outerrel &&
               ((NestPath *)path)->innerjoinpath->parent==innerrel)
                pg_sina_cost_host_lookup(root,path,extra,&pg_sina_path_search->observations);
        }
        list_free(paths);
    }
    if(!eligible(root)) return;
    /* Inspect PG's current alternatives too, preserving Index/Merge choices. */
    List *paths=list_concat(list_copy(joinrel->pathlist),list_copy(joinrel->partial_pathlist)); ListCell *c;
    foreach(c,paths) {
        Path *p=lfirst(c);
        if((IsA(p,HashPath) || IsA(p,MergePath) || IsA(p,NestPath)) &&
           ((JoinPath *)p)->outerjoinpath->parent==outerrel &&
           ((JoinPath *)p)->innerjoinpath->parent==innerrel) {
            if(IsA(p,NestPath)) {
                Path *priced=pg_sina_reprice_index_join(root,(NestPath *)p,extra,&pg_sina_path_search->observations);
                if(priced!=p) {
                    joinrel->pathlist=list_delete_ptr(joinrel->pathlist,p);
                    joinrel->partial_pathlist=list_delete_ptr(joinrel->partial_pathlist,p);
                    relation_paths(root,joinrel)->pending=lappend(relation_paths(root,joinrel)->pending,priced);
                    p=priced;
                }
            }
            add_scan_join_paths(root,joinrel,(JoinPath *)p);
            add_device_path(root,joinrel,p,false);
        }
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
