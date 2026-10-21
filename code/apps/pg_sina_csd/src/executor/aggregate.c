#include "pg_internal.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/date.h"
typedef struct {
    CustomScanState css;
    struct qs_request request;
    struct qs_result *result;
    unsigned index;
    bool explain_only,executed,profiling;
} SniaState;
static const CustomExecMethods methods;
static Node *create_state(CustomScan *scan)
{
    SniaState *s=(SniaState *)newNode(sizeof(*s),T_CustomScanState);
    s->css.methods=&methods; return (Node *)s;
}
static void begin(CustomScanState *node,EState *estate,int flags)
{
    SniaState *s=(SniaState *)node; CustomScan *scan=(CustomScan *)node->ss.ps.plan;
    Const *value=linitial_node(Const,scan->custom_private); bytea *data=DatumGetByteaP(value->constvalue);
    if(VARSIZE_ANY_EXHDR(data)!=sizeof(struct qs_plan)) elog(ERROR,"invalid CSD plan");
    memcpy(&s->request.plan,VARDATA_ANY(data),sizeof(struct qs_plan));
    s->explain_only=flags&EXEC_FLAG_EXPLAIN_ONLY;
    /* PG12 palloc guarantees MAXALIGN=8; native __int128 records need 16. */
    s->result=(void *)TYPEALIGN(16,(uintptr_t)palloc0(sizeof(*s->result)+15));
}
static Datum decimal(struct qs_cell value)
{
    char digits[48],text[96]; unsigned n=0,pos=0;
    unsigned __int128 magnitude=value.value<0?0-(unsigned __int128)value.value:(unsigned __int128)value.value;
    do { digits[n++]='0'+magnitude%10; magnitude/=10; } while(magnitude);
    while(n<=value.scale) digits[n++]='0';
    if(value.value<0) text[pos++]='-';
    for(unsigned i=n;i>0;--i) { text[pos++]=digits[i-1]; if(i-1==value.scale && value.dscale) text[pos++]='.'; }
    /* The fixed arithmetic scale may exceed the numeric display scale. */
    if(value.dscale<value.scale) pos-=value.scale-value.dscale;
    text[pos]=0;
    return DirectFunctionCall3(numeric_in,CStringGetDatum(text),ObjectIdGetDatum(InvalidOid),Int32GetDatum(-1));
}
static TupleTableSlot *execute(CustomScanState *node)
{
    SniaState *s=(SniaState *)node; TupleTableSlot *slot=node->ss.ss_ScanTupleSlot;
    if(s->explain_only) return ExecClearTuple(slot);
    if(!s->executed) {
        struct qs_extent *extents=pg_sina_mapping(&s->request);
        s->profiling=pg_sina_profile;
        pg_sina_execute(&s->request,extents,s->result,s->profiling); s->executed=true;
        if(s->result->begin+s->result->rows>QS_GROUPS) elog(ERROR,"invalid CSD result range");
    }
    if(s->index==s->result->rows) return ExecClearTuple(slot);
    ResetExprContext(node->ss.ps.ps_ExprContext); ExecClearTuple(slot);
    const struct qs_row *row=&s->result->data[s->result->begin+s->index++];
    for(unsigned i=0;i<s->request.plan.nout;++i) {
        struct qs_cell value=row->cells[i]; slot->tts_isnull[i]=value.isnull;
        if(value.isnull) continue;
        switch(s->request.plan.outputs[i].type_oid) {
        case 1700:slot->tts_values[i]=decimal(value);break;
        case 1042: { char text[2]={(char)value.value,0}; slot->tts_values[i]=DirectFunctionCall3(bpcharin,CStringGetDatum(text),ObjectIdGetDatum(InvalidOid),Int32GetDatum(5));break; }
        case 16:slot->tts_values[i]=BoolGetDatum(value.value!=0);break;
        case 21:slot->tts_values[i]=Int16GetDatum((int16)value.value);break;
        case 23:case 1082:slot->tts_values[i]=Int32GetDatum((int32)value.value);break;
        case 20:if(value.value>PG_INT64_MAX || value.value<PG_INT64_MIN) elog(ERROR,"CSD bigint overflow");slot->tts_values[i]=Int64GetDatum((int64)value.value);break;
        default:elog(ERROR,"unsupported CSD result type");
        }
    }
    ExecStoreVirtualTuple(slot);
    /* Non-identity projections (e.g. duplicate output columns) read this slot. */
    node->ss.ps.ps_ExprContext->ecxt_scantuple=slot;
    return node->ss.ps.ps_ProjInfo?ExecProject(node->ss.ps.ps_ProjInfo):slot;
}
static void end(CustomScanState *node) {}
static void rescan(CustomScanState *node)
{
    SniaState *s=(SniaState *)node; s->executed=false; s->index=0;
}
static void explain_profile(const struct qs_profile *p,ExplainState *es)
{
    static const char *names[]={"Scan","Filter","Aggregate.sink","Aggregate.source","Sort","Limit"};
    if(es->format==EXPLAIN_FORMAT_TEXT) {
        appendStringInfoSpaces(es->str,es->indent*2);
        appendStringInfoString(es->str,"CSD Profile (operator times summed across CUs):\n"); ++es->indent;
    }
    ExplainOpenGroup("CSD-Profile","CSD Profile",true,es);
    ExplainPropertyFloat("Setup Wall","ms",p->setup_ns/1e6,3,es);
    ExplainPropertyFloat("Pipeline 1 Wall","ms",p->pipeline_ns[0]/1e6,3,es);
    ExplainPropertyFloat("Pipeline 2 Wall","ms",p->pipeline_ns[1]/1e6,3,es);
    ExplainPropertyFloat("Cleanup Wall","ms",p->cleanup_ns/1e6,3,es);
    ExplainPropertyFloat("Total Wall","ms",p->total_ns/1e6,3,es);
    for(unsigned i=0;i<QS_OPERATORS;++i) if(p->operators[i].calls) {
        const struct qs_timing *t=&p->operators[i];
        if(es->format==EXPLAIN_FORMAT_TEXT) {
            char summary[192];
            snprintf(summary,sizeof(summary),"calls=%llu wall_sum=%.3f ms cpu_sum=%.3f ms wall_max=%.3f ms",
                (unsigned long long)t->calls,t->wall_ns/1e6,t->cpu_ns/1e6,t->max_wall_ns/1e6);
            ExplainPropertyText(names[i],summary,es);
        } else {
            ExplainOpenGroup("Operator",names[i],true,es);
            ExplainPropertyText("Name",names[i],es);
            ExplainPropertyInteger("Calls",NULL,t->calls,es);
            ExplainPropertyFloat("Wall Sum","ms",t->wall_ns/1e6,3,es);
            ExplainPropertyFloat("CPU Sum","ms",t->cpu_ns/1e6,3,es);
            ExplainPropertyFloat("Wall Max","ms",t->max_wall_ns/1e6,3,es);
            ExplainCloseGroup("Operator",names[i],true,es);
        }
    }
    ExplainCloseGroup("CSD-Profile","CSD Profile",true,es);
    if(es->format==EXPLAIN_FORMAT_TEXT) --es->indent;
}
static void explain(CustomScanState *node,List *ancestors,ExplainState *es)
{
    SniaState *s=(SniaState *)node; const struct qs_plan *p=&s->request.plan;
    ExplainPropertyText("Execution Path","csd",es);
    ExplainPropertyText("CSD Scheduling","independent serial batches",es);
    StringInfoData graph; initStringInfo(&graph); appendStringInfoString(&graph,"LOAD -> Scan");
    if(p->predicate>=0) appendStringInfoString(&graph," -> Filter");
    appendStringInfoString(&graph," -> Aggregate.sink; Aggregate.source");
    if(p->norder) appendStringInfoString(&graph," -> Sort");
    if(p->limit>=0 || p->offset) appendStringInfoString(&graph," -> Limit");
    ExplainPropertyText("SNIA Pipelines",graph.data,es); ExplainPropertyInteger("CSD Columns",NULL,p->ncols,es);
    unsigned deferred=0;
    for(unsigned i=0;i<p->nattrs;++i) deferred+=p->cols[i].deferred!=0;
    ExplainPropertyInteger("CSD Deferred Columns",NULL,deferred,es);
    if(s->executed) {
        if(s->profiling) explain_profile(&s->result->profile,es);
        ExplainPropertyInteger("CSD AFDM","bytes",s->result->metrics.fdm_bytes,es);
        ExplainPropertyInteger("CSD Chunks",NULL,s->result->metrics.chunks,es);
        ExplainPropertyInteger("CSD Workers",NULL,s->result->metrics.workers,es);
        ExplainPropertyInteger("CSD Heap Rows",NULL,s->result->metrics.scanned,es);
        ExplainPropertyInteger("CSD Matched Rows",NULL,s->result->metrics.matched,es);
        ExplainPropertyInteger("CSD Internal Read","bytes",s->result->metrics.input_bytes,es);
        ExplainPropertyInteger("CSD Workmem Peak","bytes",s->result->metrics.work_peak,es);
    }
}
static const CustomExecMethods methods={.CustomName="SniaCsdAggregate",.BeginCustomScan=begin,.ExecCustomScan=execute,
    .EndCustomScan=end,.ReScanCustomScan=rescan,.ExplainCustomScan=explain};
const CustomScanMethods pg_sina_scan_methods={.CustomName="SniaCsdAggregate",.CreateCustomScanState=create_state};
