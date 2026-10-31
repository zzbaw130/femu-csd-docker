/* Lower the selected PG Plan, without choosing another join order. */
#include "pg_internal.h"
#include "access/table.h"
#include "access/sysattr.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_statistic.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/pg_locale.h"
#include "utils/date.h"
#include "utils/syscache.h"
#include <errno.h>
#include <locale.h>

struct binding {
    PlannerInfo *root;
    Plan *plan;
    List *left, *right, *aggregates,*literals;
    Index scan;
    struct qp_node *node;
    struct qp_plan *tree;
    bool properties;
};
static int compile_node(PlannerInfo *,Plan *,struct qp_plan *,bool);
static int compile_tree(PlannerInfo *,Plan *,struct qp_plan *);
static int describe_expression(struct binding *,Node *);
static bool function_is(Oid id,const char *name)
{ char *s=get_func_name(id); return s && get_func_namespace(id)==PG_CATALOG_NAMESPACE && !strcmp(s,name); }
static int add_expr(struct qs_plan *p,struct qs_expr e)
{
    for(unsigned i=0;i<p->nexprs;++i) if(!memcmp(&p->exprs[i],&e,sizeof(e))) return i;
    if(p->nexprs==QS_EXPRS) return -1;
    p->exprs[p->nexprs]=e; return p->nexprs++;
}
static unsigned type_kind(Oid type,int32 mod)
{
    switch(type) {
    case NUMERICOID:return Q_DECIMAL;
    case INT2OID:case INT4OID:case INT8OID:case FLOAT8OID:return Q_INTEGER;
    case DATEOID:case TIMESTAMPOID:return Q_DATE;
    case BOOLOID:return Q_BOOLEAN;
    case BPCHAROID:return Q_STRING;
    case TEXTOID:case VARCHAROID:return Q_STRING;
    default:return 255;
    }
}
static bool byte_order(Oid collation)
{
    if(lc_collate_is_c(collation)) return true;
    const char *locale=setlocale(LC_COLLATE,NULL);
    return collation==DEFAULT_COLLATION_OID && locale &&
        (!strcmp(locale,"C.UTF-8") || !strcmp(locale,"C.utf8"));
}
static int input_column(struct binding *b,Node *expr)
{
    if(b->plan && IsA(expr,Var)) {
        Var *v=(Var *)expr; Index scan=0;
        if(IsA(b->plan,SubqueryScan)) scan=((SubqueryScan *)b->plan)->scan.scanrelid;
        if(IsA(b->plan,CteScan)) scan=((CteScan *)b->plan)->scan.scanrelid;
        if(scan && v->varno==scan && !v->varlevelsup && v->varattno>0) return v->varattno-1;
    }
    List *inputs[]={b->left,b->right}; unsigned base=0;
    for(unsigned side=0;side<2;++side) {
        ListCell *c;
        foreach(c,inputs[side]) {
            TargetEntry *t=lfirst_node(TargetEntry,c);
            if(equal(expr,t->expr)) return base+t->resno-1;
        }
        base+=list_length(inputs[side]);
    }
    return -1;
}
static int expression(struct binding *,Node *);
static int aggregate(struct binding *b,Aggref *a)
{
    struct qs_plan *p=&b->node->expr; ListCell *c; int at=0;
    foreach(c,b->aggregates) { if(equal(a,lfirst(c))) return at; ++at; }
    if(p->naggs==QS_AGGS || a->aggfilter || a->aggorder || a->agglevelsup || a->aggvariadic) return -1;
    /* No typmod survives a bpchar aggregate, while Scan interns trimmed
     * keys. Keep its padded result, and floating aggregate rounding, native. */
    if(a->aggtype==BPCHAROID || a->aggtype==FLOAT8OID) return -1;
    unsigned kind;
    if(function_is(a->aggfnoid,"sum")) kind=Q_SUM;
    else if(function_is(a->aggfnoid,"avg")) kind=Q_AVG;
    else if(function_is(a->aggfnoid,"count")) kind=a->aggstar?Q_COUNT_STAR:Q_COUNT;
    else if(function_is(a->aggfnoid,"min")) kind=Q_MIN;
    else if(function_is(a->aggfnoid,"max")) kind=Q_MAX;
    else return -1;
    int arg=-1;
    if(kind!=Q_COUNT_STAR) {
        if(list_length(a->args)!=1) return -1;
        arg=expression(b,(Node *)linitial_node(TargetEntry,a->args)->expr);
        if(arg<0) return -1;
    }
    if(a->aggdistinct) {
        /* One shared DISTINCT argument covers COUNT(DISTINCT ...) in Q16.
         * It is sorted after the group keys; NULL is ignored at accumulation. */
        if(list_length(a->aggdistinct)!=1 || kind!=Q_COUNT || p->exprs[arg].op!=Q_COLUMN) return -1;
        unsigned col=p->exprs[arg].column+1;
        if(b->node->distinct && b->node->distinct!=col) return -1;
        b->node->distinct=col;
        b->node->distinct_aggs|=1U<<p->naggs;
    }
    p->aggs[p->naggs]=(struct qs_agg){.kind=kind,.expr=arg};
    b->aggregates=lappend(b->aggregates,a);
    return p->naggs++;
}
static int literal(struct qs_plan *p,const char *bytes,unsigned length)
{
    if(length>=sizeof(p->strings[0].bytes)) return -1;
    for(unsigned i=0;i<p->nstrings;++i)
        if(p->strings[i].length==length && !memcmp(p->strings[i].bytes,bytes,length)) return i;
    if(p->nstrings==QS_STRINGS) return -1;
    struct qs_string *s=&p->strings[p->nstrings];
    s->length=length; memcpy(s->bytes,bytes,length); return p->nstrings++;
}
static bool numeric_literal(Datum value,int64 *number,unsigned *scale)
{
    char *text=DatumGetCString(DirectFunctionCall1(numeric_out,value)),digits[64];
    unsigned n=0; bool point=false; *scale=0;
    for(char *at=text;*at;++at) {
        if(*at=='.' && !point) { point=true; continue; }
        if(n+1==sizeof(digits) || ((*at<'0'||*at>'9') && !(at==text && *at=='-'))) return false;
        digits[n++]=*at; if(point) ++*scale;
    }
    if(*scale>18) return false;
    digits[n]=0; char *end; errno=0; *number=strtoll(digits,&end,10);
    return !errno && !*end && end!=digits;
}
static int expression(struct binding *b,Node *node)
{
    if(!node) return -1;
    while(IsA(node,RelabelType)) node=(Node *)((RelabelType *)node)->arg;
    struct qs_plan *p=&b->node->expr;
    struct qs_expr e={.left=-1,.right=-1,.otherwise=-1,.column=-1};
    unsigned type=type_kind(exprType(node),exprTypmod(node));
    if(type==255) return -1;
    if((type==Q_STRING || type==Q_CHAR) && !byte_order(exprCollation(node))) return -1;
    e.type=type; e.scale=e.dscale=type==Q_DECIMAL?2:0;
    int col=input_column(b,node);
    if(col>=0) {
        if(col>=QS_COLS) return -1;
        e.op=Q_COLUMN; e.column=col; return add_expr(p,e);
    }
    if(IsA(node,Var)) {
        Var *v=(Var *)node;
        if(v->varlevelsup || v->varno!=b->scan || v->varattno<=0 || (unsigned)v->varattno>p->nattrs) return -1;
        e.op=Q_COLUMN; e.column=v->varattno-1;
        if(p->cols[e.column].type==255) return -1;
        if(type==Q_STRING && p->cols[e.column].type!=Q_STRING) return -1;
        if(p->cols[e.column].slot<0) p->cols[e.column].slot=p->ncols++;
        return add_expr(p,e);
    }
    if(IsA(node,Param)) {
        Param *v=(Param *)node;
        if(v->paramkind!=PARAM_EXEC || v->paramid>=QP_PARAMS) return -1;
        e.op=QP_PARAMETER; e.column=v->paramid; return add_expr(p,e);
    }
    if(IsA(node,SubPlan)) {
        SubPlan *sub=(SubPlan *)node;
        if(sub->subLinkType!=ANY_SUBLINK || !sub->useHashTable || sub->parParam || sub->args ||
           list_length(sub->paramIds)!=1 || !sub->testexpr || !IsA(sub->testexpr,OpExpr)) return -1;
        OpExpr *op=(OpExpr *)sub->testexpr; Oid fn=get_opcode(op->opno);
        if(list_length(op->args)!=2 || (!function_is(fn,"int2eq") && !function_is(fn,"int4eq") && !function_is(fn,"int8eq"))) return -1;
        Node *left=linitial(op->args),*right=lsecond(op->args);
        if(IsA(left,Param)) { Node *swap=left; left=right; right=swap; }
        if(!IsA(right,Param) || ((Param *)right)->paramid!=linitial_int(sub->paramIds)) return -1;
        e.left=expression(b,left); if(e.left<0) return -1;
        int id=compile_tree(list_nth(b->root->glob->subroots,sub->plan_id-1),
            list_nth(b->root->glob->subplans,sub->plan_id-1),b->tree);
        if(id<0 || b->tree->count==QP_NODES || b->tree->ninit==QP_PARAMS) return -1;
        unsigned set=b->tree->count++; struct qp_node *n=&b->tree->nodes[set];
        memset(n,0,sizeof(*n));
        n->kind=QP_SET; n->left=id; n->right=-1; n->expr=b->tree->nodes[id].expr;
        unsigned init=b->tree->ninit++; b->tree->init_nodes[init]=set; b->tree->init_params[init]=-1;
        e.op=QP_INSET; e.column=set; return add_expr(p,e);
    }
    if(IsA(node,Const)) {
        Const *c=(Const *)node; e.op=Q_CONST; e.isnull=c->constisnull;
        if(!c->constisnull) switch(c->consttype) {
        case INT2OID:e.constant=DatumGetInt16(c->constvalue);break;
        case INT4OID:e.constant=DatumGetInt32(c->constvalue);break;
        case INT8OID:e.constant=DatumGetInt64(c->constvalue);break;
        case BOOLOID:e.constant=DatumGetBool(c->constvalue);break;
        case DATEOID:e.constant=DatumGetInt32(c->constvalue);if(DATE_NOT_FINITE(e.constant)) return -1;break;
        case TIMESTAMPOID: {
            int64 value=DatumGetInt64(c->constvalue);
            if(value%INT64CONST(86400000000)) return -1;
            e.constant=value/INT64CONST(86400000000); break;
        }
        case BPCHAROID:case VARCHAROID:case TEXTOID: {
            text *v=DatumGetTextPP(c->constvalue); unsigned n=VARSIZE_ANY_EXHDR(v);
            const char *bytes=VARDATA_ANY(v);
            if(c->consttype==BPCHAROID) while(n && bytes[n-1]==' ') --n;
            if(type==Q_CHAR) { if(n!=1) return -1; e.constant=(unsigned char)*bytes; }
            else { e.constant=literal(p,bytes,n); if(e.constant<0) return -1; }
            break;
        }
        case NUMERICOID: {
            unsigned scale;
            if(!numeric_literal(c->constvalue,&e.constant,&scale)) return -1;
            e.scale=e.dscale=scale; break;
        }
        default:return -1;
        }
        return add_expr(p,e);
    }
    if(IsA(node,Aggref)) {
        int a=aggregate(b,(Aggref *)node); if(a<0) return -1;
        e.op=Q_AGGREF; e.column=a; return add_expr(p,e);
    }
    if(IsA(node,NullTest)) {
        NullTest *v=(NullTest *)node; if(v->argisrow) return -1;
        e.left=expression(b,(Node *)v->arg); e.op=v->nulltesttype==IS_NULL?Q_IS_NULL:Q_IS_NOT_NULL;
        return e.left<0?-1:add_expr(p,e);
    }
    if(IsA(node,BoolExpr) || IsA(node,CoalesceExpr)) {
        List *args=IsA(node,BoolExpr)?((BoolExpr *)node)->args:((CoalesceExpr *)node)->args;
        e.op=IsA(node,CoalesceExpr)?QP_COALESCE:((BoolExpr *)node)->boolop==AND_EXPR?Q_AND:((BoolExpr *)node)->boolop==OR_EXPR?Q_OR:Q_NOT;
        int result=-1; ListCell *c;
        foreach(c,args) {
            int arg=expression(b,lfirst(c)); if(arg<0) return -1;
            if(result<0) result=arg;
            else { e.left=result; e.right=arg; result=add_expr(p,e); }
        }
        if(e.op==Q_NOT) { e.left=result; result=add_expr(p,e); }
        return result;
    }
    if(IsA(node,CaseExpr)) {
        CaseExpr *v=(CaseExpr *)node; if(v->arg) return -1;
        int result=expression(b,(Node *)v->defresult); if(result<0) return -1;
        for(int i=list_length(v->args)-1;i>=0;--i) {
            CaseWhen *w=list_nth_node(CaseWhen,v->args,i);
            e.op=Q_CASE; e.left=expression(b,(Node *)w->expr); e.right=expression(b,(Node *)w->result); e.otherwise=result;
            if(e.left<0 || e.right<0) return -1;
            result=add_expr(p,e);
        }
        return result;
    }
    if(IsA(node,ScalarArrayOpExpr)) {
        ScalarArrayOpExpr *s=(ScalarArrayOpExpr *)node;
        if(list_length(s->args)!=2 || !IsA(lsecond(s->args),Const)) return -1;
        Const *array=lsecond_node(Const,s->args); if(array->constisnull) return -1;
        ArrayType *data=DatumGetArrayTypeP(array->constvalue); Oid t=ARR_ELEMTYPE(data);
        int16 len; bool byval; char align; Datum *values; bool *nulls; int count;
        get_typlenbyvalalign(t,&len,&byval,&align);
        deconstruct_array(data,t,len,byval,align,&values,&nulls,&count);
        int result=-1;
        for(int i=0;i<count;++i) {
            Const *c=makeConst(t,-1,array->constcollid,len,values[i],nulls[i],byval);
            OpExpr *op=(OpExpr *)make_opclause(s->opno,BOOLOID,false,linitial(s->args),(Expr *)c,InvalidOid,s->inputcollid);
            int arg=expression(b,(Node *)op); if(arg<0) return -1;
            if(result<0) result=arg;
            else { e.op=s->useOr?Q_OR:Q_AND; e.left=result; e.right=arg; result=add_expr(p,e); }
        }
        if(!count) { e.op=Q_CONST; e.constant=!s->useOr; e.isnull=0; return add_expr(p,e); }
        return result;
    }
    List *args=NIL; Oid fn=InvalidOid;
    if(IsA(node,OpExpr)) { args=((OpExpr *)node)->args; fn=get_opcode(((OpExpr *)node)->opno); }
    else if(IsA(node,FuncExpr)) { args=((FuncExpr *)node)->args; fn=((FuncExpr *)node)->funcid; }
    else return -1;
    if(list_length(args)==1) {
        if(function_is(fn,"text")) {
            int arg=expression(b,linitial(args));
            return arg>=0 && p->exprs[arg].type==Q_STRING?arg:-1;
        }
        static const char *casts[]={"numeric","int2_numeric","int4_numeric","int8_numeric","date_timestamp","timestamp_date","timestamp_timestamp","timestamp","bpchar"};
        for(unsigned i=0;i<lengthof(casts);++i) if(function_is(fn,casts[i])) return expression(b,linitial(args));
    }
    if(function_is(fn,"date_part") && list_length(args)==2 && IsA(linitial(args),Const)) {
        Const *part=linitial_node(Const,args);
        if(part->consttype!=TEXTOID || part->constisnull || strcmp(TextDatumGetCString(part->constvalue),"year")) return -1;
        e.op=Q_YEAR; e.left=expression(b,lsecond(args)); return e.left<0?-1:add_expr(p,e);
    }
    if((function_is(fn,"substring") || function_is(fn,"text_substr") || function_is(fn,"text_substr_no_len")) && list_length(args)==3) {
        e.op=QP_SUBSTRING; e.left=expression(b,linitial(args)); e.right=expression(b,lsecond(args)); e.otherwise=expression(b,lthird(args));
        return e.left<0 || e.right<0 || e.otherwise<0?-1:add_expr(p,e);
    }
    if(list_length(args)!=2) return -1;
    /* Predicate-only LIKE columns can be decoded directly as booleans by
     * Scan. In particular, Q13 must not intern 1.5M Orders comments. */
    if(b->scan && (function_is(fn,"textlike") || function_is(fn,"textnlike") ||
       function_is(fn,"bpcharlike") || function_is(fn,"bpcharnlike"))) {
        Node *value=linitial(args),*pattern=lsecond(args);
        bool trim=false;
        while(IsA(value,RelabelType)) value=(Node *)((RelabelType *)value)->arg;
        if(IsA(value,FuncExpr) && list_length(((FuncExpr *)value)->args)==1 &&
           function_is(((FuncExpr *)value)->funcid,"text")) { value=linitial(((FuncExpr *)value)->args); trim=true; }
        if(IsA(value,Var) && IsA(pattern,Const)) {
            Var *v=(Var *)value; Const *c=(Const *)pattern;
            if(v->varno==b->scan && !v->varlevelsup && v->varattno>0 && (unsigned)v->varattno<=p->nattrs &&
               !c->constisnull && (c->consttype==TEXTOID || c->consttype==BPCHAROID || c->consttype==VARCHAROID)) {
                text *literal_text=DatumGetTextPP(c->constvalue); unsigned bytes=VARSIZE_ANY_EXHDR(literal_text);
                const char *data=VARDATA_ANY(literal_text); struct qs_column *col=&p->cols[v->varattno-1];
                if(bytes<=sizeof(col->prefix) && !memchr(data,'_',bytes) && !memchr(data,'\\',bytes) &&
                   ((col->type==Q_STRING && col->slot<0) || (col->type==Q_PATTERN && col->trim_spaces==trim && col->prefix_len==bytes && !memcmp(col->prefix,data,bytes)))) {
                    col->type=Q_PATTERN; col->prefix_len=bytes; memcpy(col->prefix,data,bytes);
                    col->trim_spaces=trim;
                    if(col->slot<0) col->slot=p->ncols++;
                    int id=add_expr(p,(struct qs_expr){.op=Q_COLUMN,.column=v->varattno-1,.type=Q_BOOLEAN});
                    if(function_is(fn,"textnlike") || function_is(fn,"bpcharnlike"))
                        id=add_expr(p,(struct qs_expr){.op=Q_NOT,.left=id,.type=Q_BOOLEAN});
                    return id;
                }
            }
        }
    }
    e.left=expression(b,linitial(args)); e.right=expression(b,lsecond(args));
    if(e.left<0 || e.right<0) return -1;
    static const char *arith[]={"numeric_add","numeric_sub","numeric_mul","numeric_div"};
    static const unsigned kinds[]={Q_ADD,Q_SUB,Q_MUL,Q_DIV};
    for(unsigned i=0;i<lengthof(arith);++i) if(function_is(fn,arith[i])) { e.op=kinds[i]; return add_expr(p,e); }
    if(function_is(fn,"textlike") || function_is(fn,"textnlike") || function_is(fn,"bpcharlike") || function_is(fn,"bpcharnlike")) {
        e.op=QP_LIKE; int id=add_expr(p,e);
        if(function_is(fn,"textnlike") || function_is(fn,"bpcharnlike")) { e.op=Q_NOT; e.left=id; e.right=-1; id=add_expr(p,e); }
        return id;
    }
    static const char *prefix[]={"numeric_","int2","int4","int8","int24","int28","int42","int48","int82","int84","date_","bpchar","bool","text","text_","float8"};
    static const char *suffix[]={"eq","ne","lt","le","gt","ge"};
    for(unsigned i=0;i<lengthof(prefix);++i) for(unsigned j=0;j<lengthof(suffix);++j) {
        char name[64]; snprintf(name,sizeof(name),"%s%s",prefix[i],suffix[j]);
        if(function_is(fn,name)) { e.op=Q_EQ+j; return add_expr(p,e); }
        snprintf(name,sizeof(name),"date_%s_timestamp",suffix[j]);
        if(function_is(fn,name)) { e.op=Q_EQ+j; return add_expr(p,e); }
    }
    return -1;
}
static int conditions(struct binding *b,List *list)
{
    int result=-1; ListCell *c;
    foreach(c,list) {
        int arg=expression(b,lfirst(c)); if(arg<0) return -2;
        if(result<0) result=arg;
        else result=add_expr(&b->node->expr,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=result,.right=arg});
        if(result<0) return -2;
    }
    return result;
}
static bool describe_scan(PlannerInfo *root,Scan *scan,struct qs_plan *p)
{
    RangeTblEntry *rte=rt_fetch(scan->scanrelid,root->parse->rtable);
    if(rte->rtekind!=RTE_RELATION || rte->tablesample || rte->securityQuals) return false;
    Relation table=table_open(rte->relid,NoLock); TupleDesc d=RelationGetDescr(table);
    bool ok=d->natts<=QS_COLS;
    if(ok) {
        p->relid=rte->relid; p->relfilenode=table->rd_node.relNode; p->nattrs=d->natts;
        for(int i=0;i<d->natts;++i) {
            Form_pg_attribute a=TupleDescAttr(d,i);
            p->cols[i]=(struct qs_column){.slot=-1,.length=a->attlen,.align=a->attalign=='d'?8:a->attalign=='i'?4:a->attalign=='s'?2:1,
                .type=type_kind(a->atttypid,a->atttypmod),.scale=a->atttypid==NUMERICOID?2:0,.trim_spaces=a->atttypid==BPCHAROID};
            if(a->atttypid==TIMESTAMPOID || a->atttypid==FLOAT8OID) p->cols[i].type=255;
            if(a->attisdropped || (a->attlen<0 && a->attlen!=-1)) ok=false;
        }
    }
    table_close(table,NoLock); return ok;
}
static bool has_param(Node *node,void *context)
{
    if(!node) return false;
    if(IsA(node,Param)) return ((Param *)node)->paramkind==PARAM_EXEC;
    return expression_tree_walker(node,has_param,context);
}
static Node *index_params(Node *node,List *params)
{
    if(!node) return NULL;
    if(IsA(node,Param)) {
        Param *v=(Param *)node; ListCell *c;
        if(v->paramkind==PARAM_EXEC) foreach(c,params) {
            NestLoopParam *p=lfirst(c); if(p->paramno==v->paramid) return (Node *)copyObject(p->paramval);
        }
    }
    return expression_tree_mutator(node,index_params,params);
}
struct planned_index {
    Oid indexid,heap;
    int attnum;
    bool found,described; /* A mapped row can exist while live-index checks reject it. */
    uint32 indexnode,indexroot;
    uint8 key_bytes;
};
static bool index_metadata(Oid indexid,Oid heap,int attnum,struct qs_join *j)
{
    double started=pg_sina_planner_clock();
    if(pg_sina_planning) ++pg_sina_planning->index_calls;
    List **cached=pg_sina_search_index_metadata(); ListCell *c;
    if(cached) foreach(c,*cached) {
        const struct planned_index *m=lfirst(c);
        if(m->indexid!=indexid || m->heap!=heap || m->attnum!=attnum) continue;
        if(m->described) {
            j->indexid=indexid; j->indexnode=m->indexnode; j->indexroot=m->indexroot; j->index_key_bytes=m->key_bytes;
        }
        if(pg_sina_planning) pg_sina_planning->indexes+=pg_sina_planner_clock()-started;
        return m->found;
    }
    Oid ns=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(ns) || !OidIsValid(get_relname_relid("indexes",ns))) return false;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    if(pg_sina_planning) ++pg_sina_planning->index_queries;
    Oid types[]={OIDOID,OIDOID,INT2OID};
    Datum values[]={ObjectIdGetDatum(indexid),ObjectIdGetDatum(heap),Int16GetDatum(attnum)};
    int status=SPI_execute_with_args("SELECT r.relfilenode,i.root,i.key_bytes FROM sina_csd.indexes i JOIN sina_csd.relations r ON r.relid=i.indexid WHERE i.indexid=$1 AND i.heaprelid=$2 AND i.attnum=$3",3,types,values,NULL,true,1);
    bool found=status==SPI_OK_SELECT && SPI_processed==1;
    bool described=found;
    if(found) {
        bool null; HeapTuple t=SPI_tuptable->vals[0]; TupleDesc d=SPI_tuptable->tupdesc;
        j->indexid=indexid; j->indexnode=DatumGetObjectId(SPI_getbinval(t,d,1,&null));
        j->indexroot=DatumGetInt64(SPI_getbinval(t,d,2,&null)); j->index_key_bytes=DatumGetInt16(SPI_getbinval(t,d,3,&null));
        Relation index=relation_open(indexid,AccessShareLock);
        found=index->rd_node.relNode==j->indexnode && index->rd_index->indisvalid && index->rd_index->indisready &&
            !index->rd_index->indcheckxmin && !index->rd_indoption[0] &&
            !RelationGetIndexPredicate(index) && !RelationGetIndexExpressions(index);
        relation_close(index,NoLock);
    }
    SPI_finish();
    if(cached) {
        MemoryContext previous=MemoryContextSwitchTo(pg_sina_search_context());
        struct planned_index *m=palloc0(sizeof(*m));
        m->indexid=indexid; m->heap=heap; m->attnum=attnum; m->found=found; m->described=described;
        if(described) { m->indexnode=j->indexnode; m->indexroot=j->indexroot; m->key_bytes=j->index_key_bytes; }
        *cached=lappend(*cached,m); MemoryContextSwitchTo(previous);
    }
    if(pg_sina_planning) pg_sina_planning->indexes+=pg_sina_planner_clock()-started;
    return found;
}
/* Preserve a parameterized PG B-tree lookup at any depth in the join tree.
 * Outer-dependent inner filters become this join's residual, not scan filters. */
static bool prepare_index_join(PlannerInfo *root,NestLoop *join,Plan **right,List **extra,Node **lookup,struct qs_join *access)
{
    if(!IsA(*right,IndexScan)) return false;
    IndexScan *index=(IndexScan *)copyObject(*right);
    if(!index->indexqualorig || index->indexorderbyorig) return false;
    Node *qual=linitial(index->indexqualorig); if(!IsA(qual,OpExpr)) return false;
    OpExpr *op=(OpExpr *)qual;
    Oid fn=get_opcode(op->opno);
    if(list_length(op->args)!=2 || (!function_is(fn,"int2eq") && !function_is(fn,"int4eq") && !function_is(fn,"int8eq"))) return false;
    Node *a=linitial(op->args),*b=lsecond(op->args);
    if(IsA(b,Var)) { Node *swap=a; a=b; b=swap; }
    if(!IsA(a,Var) || !IsA(b,Param) || ((Var *)a)->varno!=index->scan.scanrelid) return false;
    *lookup=index_params(b,join->nestParams);
    if(IsA(*lookup,Param)) return false;
    Oid heap=rt_fetch(index->scan.scanrelid,root->parse->rtable)->relid;
    if(!index_metadata(index->indexid,heap,((Var *)a)->varattno,access)) return false;
    access->build_key=((Var *)a)->varattno;
    List *local=NIL; ListCell *c;
    foreach(c,index->indexqualorig) if(lfirst(c)!=qual)
        *extra=lappend(*extra,index_params(lfirst(c),join->nestParams));
    foreach(c,index->scan.plan.qual) {
        Node *q=lfirst(c);
        if(has_param(q,NULL)) *extra=lappend(*extra,index_params(q,join->nestParams));
        else local=lappend(local,q);
    }
    /* Residuals may need heap attributes absent from the inner output. */
    List *vars=pull_var_clause((Node *)*extra,PVC_RECURSE_AGGREGATES|PVC_RECURSE_PLACEHOLDERS);
    foreach(c,vars) {
        Var *v=lfirst(c);
        if(!IsA(v,Var) || v->varno!=index->scan.scanrelid) continue;
        bool present=false; ListCell *t;
        foreach(t,index->scan.plan.targetlist) if(equal(v,lfirst_node(TargetEntry,t)->expr)) present=true;
        if(!present) index->scan.plan.targetlist=lappend(index->scan.plan.targetlist,
            makeTargetEntry((Expr *)copyObject(v),list_length(index->scan.plan.targetlist)+1,NULL,false));
    }
    index->scan.plan.qual=local; index->indexqualorig=NIL;
    index->indexorderdir=NoMovementScanDirection; /* Private point-lookup marker. */
    *right=(Plan *)index; return true;
}
static Plan *partial_input(Plan *p)
{
    while(p && (IsA(p,Gather) || IsA(p,GatherMerge) || IsA(p,Sort) || IsA(p,Material) || IsA(p,Result))) {
        if(p->qual) return NULL;
        p=p->lefttree;
    }
    return p && IsA(p,Agg) && DO_AGGSPLIT_SKIPFINAL(((Agg *)p)->aggsplit)?p:NULL;
}
static TargetEntry *matching_entry(List *list,Expr *expr)
{
    ListCell *c;
    foreach(c,list) { TargetEntry *t=lfirst(c); if(equal(t->expr,expr)) return t; }
    return NULL;
}
/* Collect HOST consumers, not the physical Scan/pass-through targetlists
 * that PG deliberately leaves wide to avoid adding a host projection. */
static void scan_references(Plan *plan,Plan *scan,Bitmapset **attributes)
{
    if(!plan || plan==scan) return;
    Index relation=((Scan *)scan)->scanrelid;
    if(!IsA(plan,Sort) && !IsA(plan,Hash) && !IsA(plan,Material) &&
       !IsA(plan,Gather) && !IsA(plan,GatherMerge))
        pull_varattnos((Node *)plan->targetlist,relation,attributes);
    pull_varattnos((Node *)plan->qual,relation,attributes);
    if(IsA(plan,HashJoin)) {
        pull_varattnos((Node *)((HashJoin *)plan)->join.joinqual,relation,attributes);
        pull_varattnos((Node *)((HashJoin *)plan)->hashclauses,relation,attributes);
    }
    if(IsA(plan,MergeJoin)) {
        pull_varattnos((Node *)((MergeJoin *)plan)->join.joinqual,relation,attributes);
        pull_varattnos((Node *)((MergeJoin *)plan)->mergeclauses,relation,attributes);
    }
    if(IsA(plan,NestLoop)) {
        pull_varattnos((Node *)((NestLoop *)plan)->join.joinqual,relation,attributes);
        ListCell *c; foreach(c,((NestLoop *)plan)->nestParams)
            pull_varattnos((Node *)((NestLoopParam *)lfirst(c))->paramval,relation,attributes);
    }
    if(IsA(plan,Hash)) pull_varattnos((Node *)((Hash *)plan)->hashkeys,relation,attributes);
    if(IsA(plan,Result)) pull_varattnos(((Result *)plan)->resconstantqual,relation,attributes);
    AttrNumber *keys=NULL; unsigned count=0;
    if(IsA(plan,Sort)) { keys=((Sort *)plan)->sortColIdx; count=((Sort *)plan)->numCols; }
    if(IsA(plan,GatherMerge)) { keys=((GatherMerge *)plan)->sortColIdx; count=((GatherMerge *)plan)->numCols; }
    if(IsA(plan,Agg)) { keys=((Agg *)plan)->grpColIdx; count=((Agg *)plan)->numCols; }
    for(unsigned i=0;i<count;++i) {
        TargetEntry *entry=get_tle_by_resno(plan->lefttree->targetlist,keys[i]);
        if(entry) pull_varattnos((Node *)entry->expr,relation,attributes);
    }
    /* Subquery RT indexes belong to another PlannerInfo; its own scope is
     * passed by placement's SubqueryScan recursion. */
    scan_references(plan->lefttree,scan,attributes);
    scan_references(plan->righttree,scan,attributes);
}
Plan *pg_sina_project_scan(Plan *scope,Plan *scan)
{
    if(scope==scan) return scan;
    Bitmapset *attributes=NULL;
    scan_references(scope,scan,&attributes);
    if(bms_is_member(-FirstLowInvalidHeapAttributeNumber,attributes)) return scan;
    Plan *copy=copyObject(scan); List *outputs=NIL; ListCell *c;
    foreach(c,copy->targetlist) {
        TargetEntry *entry=lfirst(c); Var *v=IsA(entry->expr,Var)?(Var *)entry->expr:NULL;
        if(!v || v->varno!=((Scan *)scan)->scanrelid ||
           bms_is_member(v->varattno-FirstLowInvalidHeapAttributeNumber,attributes)) {
            entry->resno=list_length(outputs)+1; outputs=lappend(outputs,entry);
        }
    }
    /* COUNT(*) needs row presence. Retain one field without introducing a
     * separate zero-column wire format. */
    if(outputs==NIL && copy->targetlist!=NIL) {
        TargetEntry *entry=linitial(copy->targetlist); entry->resno=1; outputs=list_make1(entry);
    }
    copy->targetlist=outputs; return copy;
}
void pg_sina_remap_input(Plan *parent,List *old,List *projected)
{
    AttrNumber *keys=NULL; unsigned count=0;
    if(IsA(parent,Sort)) { keys=((Sort *)parent)->sortColIdx; count=((Sort *)parent)->numCols; }
    if(IsA(parent,GatherMerge)) { keys=((GatherMerge *)parent)->sortColIdx; count=((GatherMerge *)parent)->numCols; }
    if(IsA(parent,Agg)) { keys=((Agg *)parent)->grpColIdx; count=((Agg *)parent)->numCols; }
    for(unsigned i=0;i<count;++i) {
        TargetEntry *entry=get_tle_by_resno(old,keys[i]);
        TargetEntry *next=entry?matching_entry(projected,entry->expr):NULL;
        if(!next) elog(ERROR,"CSD scan projection lost a HOST grouping/sort key");
        keys[i]=next->resno;
    }
}
/* Gather Merge orders partial states, not raw input. After collapsing the
 * parallel aggregate, preserve that order above the completed groups so an
 * ordered consumer does not force a sort of every input row. */
static Sort *aggregate_output_sort(Plan *aggregate)
{
    Plan *input=aggregate->lefttree;
    while(input && !IsA(input,GatherMerge) && !IsA(input,Sort)) input=input->lefttree;
    if(!input) return NULL;
    Sort *sort=makeNode(Sort);
    AttrNumber *columns;
    if(IsA(input,GatherMerge)) {
        GatherMerge *merge=(GatherMerge *)input;
        sort->numCols=merge->numCols; columns=merge->sortColIdx;
        sort->sortOperators=merge->sortOperators; sort->collations=merge->collations;
        sort->nullsFirst=merge->nullsFirst;
    } else {
        Sort *old=(Sort *)input;
        sort->numCols=old->numCols; columns=old->sortColIdx;
        sort->sortOperators=old->sortOperators; sort->collations=old->collations;
        sort->nullsFirst=old->nullsFirst;
    }
    sort->sortColIdx=palloc(sizeof(*columns)*sort->numCols);
    for(int i=0;i<sort->numCols;++i) {
        TargetEntry *key=get_tle_by_resno(input->targetlist,columns[i]);
        TargetEntry *output=key?matching_entry(aggregate->targetlist,key->expr):NULL;
        if(!output) return NULL;
        sort->sortColIdx[i]=output->resno;
    }
    sort->plan.targetlist=copyObject(aggregate->targetlist);
    sort->plan.plan_rows=aggregate->plan_rows;
    sort->plan.plan_width=aggregate->plan_width;
    sort->plan.lefttree=aggregate;
    return sort;
}
static bool normalize_parallel_aggregate(Plan **slot)
{
    Plan *plan=*slot;
    if(!plan) return true;
    Sort *output_sort=NULL;
    if(IsA(plan,Agg) && DO_AGGSPLIT_COMBINE(((Agg *)plan)->aggsplit)) {
        Agg *agg=(Agg *)plan; Plan *partial=partial_input(plan->lefttree);
        if(!partial) return false;
        if(agg->aggstrategy==AGG_SORTED && ((Agg *)partial)->aggstrategy==AGG_HASHED) {
            output_sort=aggregate_output_sort(plan);
            if(!output_sort) return false;
        }
        for(int i=0;i<agg->numCols;++i) {
            TargetEntry *old=get_tle_by_resno(plan->lefttree->targetlist,agg->grpColIdx[i]);
            TargetEntry *next=old?matching_entry(partial->lefttree->targetlist,old->expr):NULL;
            if(!next) return false;
            agg->grpColIdx[i]=next->resno;
        }
        plan->lefttree=partial->lefttree; agg->aggsplit=AGGSPLIT_SIMPLE;
        agg->aggstrategy=((Agg *)partial)->aggstrategy;
    }
    if(IsA(plan,SubqueryScan) && !normalize_parallel_aggregate(&((SubqueryScan *)plan)->subplan)) return false;
    if(!normalize_parallel_aggregate(&plan->lefttree) || !normalize_parallel_aggregate(&plan->righttree)) return false;
    if(output_sort) *slot=(Plan *)output_sort;
    return true;
}
/* PG partial paths report rows per participant. Restore full-stream rows
 * before removing Gather/partial Agg; private join build inputs stay whole. */
static void global_parallel_rows(Plan *plan,double divisor)
{
    if(!plan) return;
    if(divisor>1) plan->plan_rows*=divisor;
    if(IsA(plan,Gather) || IsA(plan,GatherMerge)) {
        unsigned workers=IsA(plan,Gather)?((Gather *)plan)->num_workers:((GatherMerge *)plan)->num_workers;
        double leader=parallel_leader_participation?Max(0.0,1.0-0.3*workers):0;
        global_parallel_rows(plan->lefttree,Max(1.0,workers+leader));
        return;
    }
    global_parallel_rows(plan->lefttree,divisor);
    double inner=IsA(plan,HashJoin) && plan->parallel_aware?divisor:1;
    global_parallel_rows(plan->righttree,inner);
    if(IsA(plan,SubqueryScan)) global_parallel_rows(((SubqueryScan *)plan)->subplan,divisor);
}
struct required_columns {
    Plan *plan,*left,*right;
    Bitmapset *left_columns,*right_columns;
};
static bool require_expression(Node *node,struct required_columns *r)
{
    if(!node) return false;
    if(IsA(r->plan,SubqueryScan) && IsA(node,Var) &&
       ((Var *)node)->varno==((SubqueryScan *)r->plan)->scan.scanrelid) {
        r->left_columns=bms_add_member(r->left_columns,((Var *)node)->varattno); return false;
    }
    TargetEntry *t=r->left?matching_entry(r->left->targetlist,(Expr *)node):NULL;
    if(t) { r->left_columns=bms_add_member(r->left_columns,t->resno); return false; }
    t=r->right?matching_entry(r->right->targetlist,(Expr *)node):NULL;
    if(t) { r->right_columns=bms_add_member(r->right_columns,t->resno); return false; }
    return expression_tree_walker(node,require_expression,r);
}
/* PG heap scans often expose the entire physical tuple to avoid a host
 * projection. A device scan must instead decode only required columns. Keep
 * PG's logical expressions, and update the few positional Sort/Agg fields. */
static bool prune_columns(Plan *plan,Bitmapset *keep,bool all)
{
    if(!plan) return true;
    List *outputs=NIL; ListCell *c;
    foreach(c,plan->targetlist) {
        TargetEntry *t=lfirst(c);
        if(all || bms_is_member(t->resno,keep)) { t->resno=list_length(outputs)+1; outputs=lappend(outputs,t); }
    }
    plan->targetlist=outputs;
    Plan *left=IsA(plan,SubqueryScan)?((SubqueryScan *)plan)->subplan:plan->lefttree,*right=plan->righttree;
    struct required_columns r={.plan=plan,.left=left,.right=right};
    require_expression((Node *)outputs,&r); require_expression((Node *)plan->qual,&r);
    if(IsA(plan,Result)) require_expression(((Result *)plan)->resconstantqual,&r);
    List *position_keys=NIL;
    if(IsA(plan,Sort)) {
        Sort *sort=(Sort *)plan;
        for(int i=0;i<sort->numCols;++i) {
            TargetEntry *t=get_tle_by_resno(left->targetlist,sort->sortColIdx[i]); if(!t) return false;
            position_keys=lappend(position_keys,t->expr); require_expression((Node *)t->expr,&r);
        }
    }
    if(IsA(plan,Agg)) {
        Agg *agg=(Agg *)plan;
        for(int i=0;i<agg->numCols;++i) {
            TargetEntry *t=get_tle_by_resno(left->targetlist,agg->grpColIdx[i]); if(!t) return false;
            position_keys=lappend(position_keys,t->expr); require_expression((Node *)t->expr,&r);
        }
    }
    if(IsA(plan,HashJoin) || IsA(plan,MergeJoin) || IsA(plan,NestLoop)) {
        require_expression((Node *)((Join *)plan)->joinqual,&r);
        if(IsA(plan,HashJoin)) require_expression((Node *)((HashJoin *)plan)->hashclauses,&r);
        if(IsA(plan,MergeJoin)) require_expression((Node *)((MergeJoin *)plan)->mergeclauses,&r);
        if(IsA(plan,NestLoop)) {
            foreach(c,((NestLoop *)plan)->nestParams) require_expression((Node *)((NestLoopParam *)lfirst(c))->paramval,&r);
            if(right && IsA(right,IndexScan)) {
                require_expression((Node *)right->qual,&r);
                require_expression((Node *)((IndexScan *)right)->indexqualorig,&r);
            }
        }
    }
    if(IsA(plan,Hash)) require_expression((Node *)((Hash *)plan)->hashkeys,&r);
    if(!prune_columns(left,r.left_columns,IsA(plan,SubqueryScan)) || !prune_columns(right,r.right_columns,false)) return false;
    int i=0;
    foreach(c,position_keys) {
        TargetEntry *t=matching_entry(left->targetlist,lfirst(c)); if(!t) return false;
        if(IsA(plan,Sort)) ((Sort *)plan)->sortColIdx[i]=t->resno;
        else ((Agg *)plan)->grpColIdx[i]=t->resno;
        ++i;
    }
    return true;
}
static int compile_tree(PlannerInfo *root,Plan *native,struct qp_plan *tree)
{
    if(pg_sina_exe_path==PG_SINA_ADAPTIVE) {
        Plan *chosen=pg_sina_prepare_candidate(root,native,root->parse->sortClause!=NIL);
        return chosen?compile_node(root,chosen,tree,root->parse->sortClause!=NIL):-1;
    }
    Plan *copy=copyObject(native);
    global_parallel_rows(copy,1);
    if(!normalize_parallel_aggregate(&copy) || !prune_columns(copy,NULL,true)) return -1;
    return compile_node(root,copy,tree,root->parse->sortClause!=NIL);
}

/* These are physical choices, made before candidate costing. Encoding below
 * must honor them; it cannot silently turn a Merge/Aggregate into Hash. */
struct candidate_group { bool supported; List *aggregates; };
static bool simple_group(Node *node,void *context)
{
    struct candidate_group *group=context;
    if(!node || !group->supported) return false;
    if(IsA(node,Aggref)) {
        Aggref *a=(Aggref *)node;
        if(a->aggdistinct || a->aggorder || a->aggfilter) group->supported=false;
        group->aggregates=list_append_unique(group->aggregates,a);
        return false;
    }
    return expression_tree_walker(node,simple_group,context);
}
static bool identity_target(Plan *a,Plan *b)
{
    if(!a || !b || list_length(a->targetlist)!=list_length(b->targetlist)) return false;
    ListCell *x,*y;
    forboth(x,a->targetlist,y,b->targetlist)
        if(!equal(lfirst_node(TargetEntry,x)->expr,lfirst_node(TargetEntry,y)->expr)) return false;
    return true;
}
struct group_payload { Agg *aggregate; bool supported; };
/* PG may omit functionally dependent payload from GROUP BY. The device
 * stores keys and aggregate states, so retain that payload as physical keys.
 * PG has already proved the dependency; this does not invent extra groups. */
static bool retain_group_payload(Node *node,struct group_payload *context)
{
    if(!node || !context->supported || IsA(node,Aggref) || IsA(node,Const)) return false;
    Agg *a=context->aggregate;
    TargetEntry *entry=matching_entry(a->plan.lefttree->targetlist,(Expr *)node);
    if(entry) {
        for(int i=0;i<a->numCols;++i) if(a->grpColIdx[i]==entry->resno) return false;
        if(a->numCols==QS_KEYS) { context->supported=false; return false; }
        Oid equality; bool hashable;
        get_sort_group_operators(exprType(node),false,true,false,NULL,&equality,NULL,&hashable);
        if(!hashable) { context->supported=false; return false; }
        int i=a->numCols++; a->grpColIdx[i]=entry->resno; a->grpOperators[i]=equality; a->grpCollations[i]=exprCollation(node);
        return false;
    }
    if(IsA(node,Var)) { context->supported=false; return false; }
    return expression_tree_walker(node,retain_group_payload,context);
}
static bool choose_algorithms(PlannerInfo *root,Plan **slot,bool ordered)
{
    Plan *p=*slot; if(!p) return true;
    /* Rows were already restored to the whole stream. Do not leave a PG
     * Gather behind to multiply those rows again when a parent reuses this
     * candidate. A child Sort now sorts the entire device stream. */
    if(IsA(p,Gather) || IsA(p,GatherMerge)) {
        if(identity_target(p,p->lefttree)) { *slot=p->lefttree; return choose_algorithms(root,slot,ordered); }
        Result *projection=makeNode(Result); projection->plan=*p; NodeSetTag(projection,T_Result);
        *slot=p=(Plan *)projection;
    }
    /* A child Path may have needed order at its HOST boundary. Once fused
     * below a Hash/unsorted pipeline that delivery Sort is unnecessary. Keep
     * its projection; Limit requires order and Merge sorts its own inputs. */
    if(IsA(p,Sort) && !ordered) {
        if(identity_target(p,p->lefttree)) { *slot=p->lefttree; return choose_algorithms(root,slot,false); }
        Result *projection=makeNode(Result); projection->plan=*p; NodeSetTag(projection,T_Result);
        *slot=p=(Plan *)projection;
    }
    if(IsA(p,SubqueryScan)) {
        RelOptInfo *r=root->simple_rel_array[((SubqueryScan *)p)->scan.scanrelid];
        if(!r || !r->subroot || !choose_algorithms(r->subroot,&((SubqueryScan *)p)->subplan,ordered)) return false;
    }
    if(IsA(p,NestLoop)) {
        Plan *right=p->righttree; List *extra=NIL; Node *lookup=NULL; struct qs_join access={0};
        if(!prepare_index_join(root,(NestLoop *)p,&right,&extra,&lookup,&access)) {
            List *keys=NIL; ListCell *c;
            foreach(c,((Join *)p)->joinqual) {
                Node *q=lfirst(c); Oid left_hash,right_hash;
                if(IsA(q,OpExpr) && get_op_hash_functions(((OpExpr *)q)->opno,&left_hash,&right_hash)) keys=lappend(keys,q);
            }
            if(ordered || !keys) return false;
            HashJoin *hash=makeNode(HashJoin); hash->join=*(Join *)p; NodeSetTag(hash,T_HashJoin);
            hash->hashclauses=keys; hash->join.joinqual=list_difference_ptr(hash->join.joinqual,keys);
            *slot=p=(Plan *)hash;
        }
    }
    if(IsA(p,HashJoin) && IsA(p->righttree,Hash)) p->righttree=p->righttree->lefttree;
    if(IsA(p,HashJoin) && ((Join *)p)->jointype==JOIN_INNER &&
       p->lefttree->plan_rows*Max(1,p->lefttree->plan_width)<p->righttree->plan_rows*Max(1,p->righttree->plan_width)) {
        Plan *swap=p->lefttree; p->lefttree=p->righttree; p->righttree=swap;
    }
    if(IsA(p,Agg)) {
        Agg *a=(Agg *)p;
        struct candidate_group group={.supported=a->numCols>0 && a->numCols<=QS_KEYS};
        simple_group((Node *)p->targetlist,&group); simple_group((Node *)p->qual,&group);
        bool supported=group.supported && group.aggregates && a->numCols+2*list_length(group.aggregates)<=QS_OUT;
        bool sort=IsA(p->lefttree,Sort);
        Sort *output=supported && sort && ordered?aggregate_output_sort(p):NULL;
        if(supported && (a->aggstrategy==AGG_HASHED || (sort && (!ordered || output)))) {
            a->aggstrategy=AGG_HASHED;
            if(sort) {
                Plan *input=p->lefttree;
                if(identity_target(input,input->lefttree)) p->lefttree=input->lefttree;
                else {
                    Result *projection=makeNode(Result); projection->plan=*input; NodeSetTag(projection,T_Result);
                    p->lefttree=(Plan *)projection;
                }
            }
            if(output) *slot=(Plan *)output;
            AttrNumber *columns=palloc0(QS_KEYS*sizeof(*columns));
            Oid *operators=palloc0(QS_KEYS*sizeof(*operators)),*collations=palloc0(QS_KEYS*sizeof(*collations));
            memcpy(columns,a->grpColIdx,a->numCols*sizeof(*columns));
            memcpy(operators,a->grpOperators,a->numCols*sizeof(*operators));
            memcpy(collations,a->grpCollations,a->numCols*sizeof(*collations));
            a->grpColIdx=columns; a->grpOperators=operators; a->grpCollations=collations;
            struct group_payload payload={.aggregate=a,.supported=true};
            retain_group_payload((Node *)p->targetlist,&payload); retain_group_payload((Node *)p->qual,&payload);
            if(!payload.supported) return false;
            if(a->numCols+2*list_length(group.aggregates)>QS_OUT) a->aggstrategy=AGG_SORTED;
        }
        else if(a->aggstrategy==AGG_HASHED && !supported) a->aggstrategy=AGG_SORTED;
    }
    bool left_order=ordered;
    if(IsA(p,Sort) || IsA(p,HashJoin) || IsA(p,MergeJoin)) left_order=false;
    if(IsA(p,Limit)) left_order=true;
    if(IsA(p,Agg)) left_order=((Agg *)p)->aggstrategy==AGG_SORTED;
    return choose_algorithms(root,&p->lefttree,left_order) && choose_algorithms(root,&p->righttree,false);
}
Plan *pg_sina_prepare_candidate(PlannerInfo *root,Plan *native,bool ordered)
{
    double mark=pg_sina_planner_clock();
    Plan *copy=copyObject(native);
    if(pg_sina_planning) pg_sina_planning->prepare_copy+=pg_sina_planner_clock()-mark;
    mark=pg_sina_planner_clock();
    global_parallel_rows(copy,1);
    bool supported=normalize_parallel_aggregate(&copy);
    if(pg_sina_planning) pg_sina_planning->prepare_normalize+=pg_sina_planner_clock()-mark;
    if(!supported) return NULL;
    mark=pg_sina_planner_clock(); supported=prune_columns(copy,NULL,true);
    if(pg_sina_planning) pg_sina_planning->prepare_prune+=pg_sina_planner_clock()-mark;
    if(!supported) return NULL;
    mark=pg_sina_planner_clock(); supported=choose_algorithms(root,&copy,ordered);
    if(pg_sina_planning) pg_sina_planning->prepare_algorithms+=pg_sina_planner_clock()-mark;
    if(!supported) return NULL;
    return copy;
}
static Node *heap_index_var(Node *node,List *tlist)
{
    if(!node) return NULL;
    if(IsA(node,Var) && ((Var *)node)->varno==INDEX_VAR) {
        TargetEntry *t=get_tle_by_resno(tlist,((Var *)node)->varattno);
        return t?(Node *)copyObject(t->expr):NULL;
    }
    return expression_tree_mutator(node,heap_index_var,tlist);
}
Plan *pg_sina_require_output_order(PlannerInfo *root,Plan *input)
{
    unsigned count=list_length(root->parse->sortClause); if(!count || count>QS_OUT) return NULL;
    Sort *sort=makeNode(Sort); sort->plan=*input; NodeSetTag(sort,T_Sort);
    sort->plan.lefttree=input; sort->plan.righttree=NULL; sort->plan.qual=NIL;
    sort->numCols=count; sort->sortColIdx=palloc(count*sizeof(AttrNumber));
    sort->sortOperators=palloc(count*sizeof(Oid)); sort->collations=palloc(count*sizeof(Oid));
    sort->nullsFirst=palloc(count*sizeof(bool)); unsigned i=0; ListCell *c;
    foreach(c,root->parse->sortClause) {
        SortGroupClause *key=lfirst(c); TargetEntry *entry=get_sortgroupclause_tle(key,input->targetlist);
        if(!entry) return NULL;
        sort->sortColIdx[i]=entry->resno; sort->sortOperators[i]=key->sortop;
        sort->collations[i]=exprCollation((Node *)entry->expr); sort->nullsFirst[i]=key->nulls_first; ++i;
    }
    return (Plan *)sort;
}

/* Validate expressions over group keys and the aggregate result. When
 * lowering a PG-validated group, dependent output columns can be retained
 * as additional physical keys without changing the SQL groups. */
static bool group_output(struct qs_plan *p,int index,struct qp_node *n,bool retain)
{
    if(index<0 || (unsigned)index>=p->nexprs) return false;
    const struct qs_expr *x=&p->exprs[index];
    if(x->op==Q_CONST) return true;
    if(x->op==Q_COLUMN) {
        for(unsigned k=0;k<n->nkeys;++k) if(x->column==n->left_keys[k]) return true;
        if(!retain || n->nkeys==QS_KEYS) return false;
        p->keys[p->nkeys++]=index; n->left_keys[n->nkeys++]=x->column;
        return true;
    }
    if(x->op==Q_AGGREF) return x->column>=0 && (unsigned)x->column<p->naggs;
    if(x->op==QP_PARAMETER) return true;
    if(x->op==Q_NOT || x->op==Q_IS_NULL || x->op==Q_IS_NOT_NULL)
        return group_output(p,x->left,n,retain);
    return (x->op==Q_ADD || x->op==Q_SUB || x->op==Q_MUL || x->op==Q_DIV ||
        (x->op>=Q_EQ && x->op<=Q_OR)) &&
        group_output(p,x->left,n,retain) && group_output(p,x->right,n,retain);
}
/* A unique integer-key join that only returns its key is a membership test.
 * Move that test below GROUP BY, where it removes whole groups, never rows
 * within a retained group. Reuse the existing device IN set and initializer. */
static void push_group_key_filter(struct qp_plan *tree,struct qp_node *join)
{
    if(join->join_type!=QP_INNER || join->nkeys!=1 || join->join_qual>=0 ||
       join->expr.predicate>=0 || tree->count+2>QP_NODES || tree->ninit==QP_PARAMS) return;
    const struct qp_node *right=&tree->nodes[join->right];
    if(right->kind!=QP_SCAN || right->expr.nout!=1 || join->right_keys[0]!=0 ||
       right->expr.exprs[right->expr.outputs[0].index].type!=Q_INTEGER) return;
    int id=join->left,column=join->left_keys[0];
    while(id>=0 && tree->nodes[id].kind==QP_PROJECT) {
        const struct qp_node *p=&tree->nodes[id];
        const struct qs_expr *x=&p->expr.exprs[p->expr.outputs[column].index];
        if(x->op!=Q_COLUMN || p->expr.predicate>=0) return;
        column=x->column; id=p->left;
    }
    if(id<0) return;
    struct qp_node *aggregate=&tree->nodes[id];
    if(aggregate->kind!=QP_AGGREGATE || !aggregate->hash || aggregate->left<0) return;
    const struct qs_expr *key=&aggregate->expr.exprs[aggregate->expr.outputs[column].index];
    if(key->op!=Q_COLUMN || key->type!=Q_INTEGER) return;
    int grouped=0;
    for(unsigned k=0;k<aggregate->nkeys;++k) if(aggregate->left_keys[k]==key->column) grouped=1;
    if(!grouped) return;
    const struct qp_node *input=&tree->nodes[aggregate->left];
    if(input->kind!=QP_SCAN || right->estimated_rows>=aggregate->estimated_rows/4) return;
    unsigned width=tree->nodes[join->left].expr.nout;
    for(unsigned i=0;i<join->expr.nexprs;++i) {
        const struct qs_expr *x=&join->expr.exprs[i];
        if(x->op==Q_COLUMN && x->column>=(int)width && x->column!=(int)width) return;
    }
    unsigned set=tree->count++,filter=tree->count++;
    tree->nodes[set]=(struct qp_node){.kind=QP_SET,.left=join->right,.right=-1,.expr=right->expr};
    unsigned init=tree->ninit++; tree->init_nodes[init]=set; tree->init_params[init]=-1;
    struct qp_node *f=&tree->nodes[filter];
    *f=(struct qp_node){.kind=QP_PROJECT,.left=aggregate->left,.right=-1,.estimated_rows=input->estimated_rows};
    f->expr.nattrs=f->expr.ncols=f->expr.nout=f->expr.nexprs=input->expr.nout;
    f->expr.having=-1; f->expr.limit=-1;
    for(unsigned i=0;i<f->expr.nout;++i) {
        f->expr.outputs[i]=input->expr.outputs[i]; f->expr.outputs[i].index=i;
        f->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,.type=input->expr.exprs[input->expr.outputs[i].index].type};
        f->expr.cols[i]=(struct qs_column){.slot=i,.type=f->expr.exprs[i].type};
    }
    f->expr.predicate=add_expr(&f->expr,(struct qs_expr){.op=QP_INSET,.type=Q_BOOLEAN,.left=key->column,.column=set});
    aggregate->left=filter;
    for(unsigned i=0;i<join->expr.nexprs;++i) {
        struct qs_expr *x=&join->expr.exprs[i];
        if(x->op==Q_COLUMN && x->column==(int)width) x->column=join->left_keys[0];
    }
    join->kind=QP_PROJECT; join->right=-1; join->hash=join->nkeys=0;
    join->expr.nattrs=join->expr.ncols=width; join->expr.norder=0;
}
/* GROUP BY a left primary key after LEFT JOIN COUNT(right.value): count the
 * right rows first and join one count per key. Missing matches still yield 0.
 * The primary-key guard is essential: duplicate/NULL left keys cannot bypass
 * the original grouping. No extra scan or intermediate row format is needed. */
static void push_left_join_count(struct qp_plan *tree,struct qp_node *aggregate)
{
    struct qs_plan *p=&aggregate->expr;
    if(!aggregate->hash || aggregate->nkeys!=1 || p->naggs!=1 || p->aggs[0].kind!=Q_COUNT ||
       p->predicate>=0 || p->having>=0 || tree->count==QP_NODES || p->nexprs+2>QS_EXPRS) return;
    struct qp_node *join=&tree->nodes[aggregate->left];
    if(join->kind!=QP_JOIN || !join->hash || join->join_type!=QP_LEFT || join->nkeys!=1 ||
       join->join_qual>=0 || join->expr.predicate>=0) return;
    const struct qp_node *left=&tree->nodes[join->left],*right=&tree->nodes[join->right];
    if(left->kind!=QP_SCAN || !(left->ordered_index&4) || right->kind!=QP_SCAN) return;
    unsigned width=left->expr.nout;
    const struct qs_expr *primary=&left->expr.exprs[left->expr.outputs[join->left_keys[0]].index];
    if(primary->op!=Q_COLUMN || primary->column!=left->expr.join.build_key-1) return;
    const struct qs_expr *group=&join->expr.exprs[join->expr.outputs[aggregate->left_keys[0]].index];
    const struct qs_expr *argument=&p->exprs[p->aggs[0].expr];
    if(group->op!=Q_COLUMN || group->column!=join->left_keys[0] || argument->op!=Q_COLUMN) return;
    const struct qs_expr *value=&join->expr.exprs[join->expr.outputs[argument->column].index];
    if(value->op!=Q_COLUMN || value->column<(int)width) return;
    int column=value->column-width,key=join->right_keys[0];
    if(column==key) return;
    for(unsigned i=0;i<join->expr.nexprs;++i) {
        const struct qs_expr *x=&join->expr.exprs[i];
        if(x->op==Q_COLUMN && x->column>=(int)width && x->column!=(int)width+key && x->column!=(int)width+column) return;
    }
    for(unsigned i=0;i<p->nout;++i) if(!group_output(p,p->outputs[i].index,aggregate,false)) return;
    Relation index=relation_open(left->expr.join.indexid,NoLock);
    bool unique=index->rd_index->indisprimary && index->rd_index->indnkeyatts==1;
    relation_close(index,NoLock); if(!unique) return;
    unsigned id=tree->count++; struct qp_node *count=&tree->nodes[id];
    *count=(struct qp_node){.kind=QP_AGGREGATE,.left=join->right,.right=-1,.hash=1,.nkeys=1,
        .estimated_rows=left->estimated_rows,.estimated_groups=left->estimated_rows};
    count->left_keys[0]=key; count->expr.nkeys=1; count->expr.keys[0]=0;
    count->expr.predicate=count->expr.having=-1; count->expr.limit=-1;
    count->expr.nattrs=count->expr.ncols=right->expr.nout; count->expr.nexprs=3; count->expr.nout=2;
    count->expr.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=key,.type=Q_INTEGER};
    count->expr.exprs[1]=(struct qs_expr){.op=Q_COLUMN,.column=column,.type=argument->type};
    count->expr.exprs[2]=(struct qs_expr){.op=Q_AGGREF,.column=0,.type=Q_INTEGER};
    count->expr.outputs[0]=right->expr.outputs[key]; count->expr.outputs[0].index=0;
    count->expr.outputs[1]=(struct qs_output){.index=2,.type_oid=INT8OID};
    count->expr.naggs=1; count->expr.aggs[0]=(struct qs_agg){.kind=Q_COUNT,.expr=1};
    for(unsigned i=0;i<join->expr.nexprs;++i) {
        struct qs_expr *x=&join->expr.exprs[i];
        if(x->op==Q_COLUMN && x->column>=(int)width) x->column=width+(x->column==(int)width+column);
    }
    join->right=id; join->right_keys[0]=0; join->expr.nattrs=join->expr.ncols=width+2;
    join->expr.cols[width].type=join->expr.cols[width+1].type=Q_INTEGER;
    for(unsigned i=0;i<join->expr.nout;++i)
        if(join->expr.exprs[join->expr.outputs[i].index].op==Q_COLUMN &&
           join->expr.exprs[join->expr.outputs[i].index].column==(int)width+1)
            join->expr.outputs[i]=(struct qs_output){.index=join->expr.outputs[i].index,.type_oid=INT8OID,.typmod=-1};
    int input=add_expr(p,(struct qs_expr){.op=Q_COLUMN,.column=argument->column,.type=Q_INTEGER});
    int zero=add_expr(p,(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER});
    for(unsigned i=0;i<p->nexprs;++i) if(p->exprs[i].op==Q_AGGREF)
        p->exprs[i]=(struct qs_expr){.op=QP_COALESCE,.left=input,.right=zero,.type=Q_INTEGER};
    aggregate->kind=QP_PROJECT; aggregate->hash=aggregate->nkeys=aggregate->sorted_input=0;
    p->naggs=p->nkeys=0;
}
static void bypass_distinct_sort(struct qp_plan *tree,struct qp_node *n)
{
    /* DISTINCT already sorts by group keys plus its argument inside the
     * aggregate. Bypass PG's grouping-only Sort when it is a pure identity. */
    if(n->kind==QP_AGGREGATE && n->distinct && n->left>=0 && tree->nodes[n->left].kind==QP_SORT) {
        struct qp_node *sort=&tree->nodes[n->left];
        bool identity=sort->left>=0 && sort->expr.nout==tree->nodes[sort->left].expr.nout;
        for(unsigned i=0;identity && i<sort->expr.nout;++i) {
            const struct qs_expr *x=&sort->expr.exprs[sort->expr.outputs[i].index];
            if(x->op!=Q_COLUMN || x->column!=(int)i) identity=false;
        }
        if(identity) { n->left=sort->left; n->sorted_input=0; }
    }
}
static void choose_distinct(struct qp_plan *tree,struct qp_node *n)
{
    struct qs_plan *p=&n->expr;
    /* Like DuckDB's DISTINCT table, first deduplicate (group, argument), then
     * COUNT the non-NULL arguments. Both stages use the existing bounded Hash
     * Aggregate and spill protocol. Restrict this to an exact key projection. */
    if(n->kind==QP_AGGREGATE && n->distinct && n->nkeys<QS_KEYS && p->naggs==1 &&
       p->aggs[0].kind==Q_COUNT && p->predicate<0 && p->having<0 && tree->count<QP_NODES) {
        unsigned width=tree->nodes[n->left].expr.nout,mask=1U<<(n->distinct-1);
        bool supported=width==n->nkeys+1;
        for(unsigned k=0;k<n->nkeys;++k) mask|=1U<<n->left_keys[k];
        if(mask!=((1U<<width)-1)) supported=false;
        for(unsigned i=0;i<p->nout;++i) if(!group_output(p,p->outputs[i].index,n,false)) supported=false;
        if(supported) {
            unsigned dedup=tree->count++; struct qp_node *d=&tree->nodes[dedup];
            *d=(struct qp_node){.kind=QP_AGGREGATE,.left=n->left,.right=-1,.hash=1,
                .estimated_rows=tree->nodes[n->left].estimated_rows,.estimated_groups=tree->nodes[n->left].estimated_rows};
            d->expr.predicate=d->expr.having=-1; d->expr.limit=-1;
            d->expr.nattrs=d->expr.ncols=d->expr.nexprs=d->expr.nout=width;
            for(unsigned i=0;i<width;++i) {
                const struct qs_output *out=&tree->nodes[n->left].expr.outputs[i];
                d->expr.outputs[i]=*out; d->expr.outputs[i].index=i;
                d->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,
                    .type=tree->nodes[n->left].expr.exprs[out->index].type};
                d->expr.cols[i]=(struct qs_column){.slot=i,.type=d->expr.exprs[i].type};
                d->left_keys[i]=d->expr.keys[i]=i;
            }
            d->nkeys=d->expr.nkeys=width; d->expr.naggs=1;
            d->expr.aggs[0]=(struct qs_agg){.kind=Q_COUNT_STAR,.expr=-1};
            n->left=dedup; n->distinct=n->distinct_aggs=n->sorted_input=0; n->hash=1;
        }
    }
}
static int ordered_heap_scan(struct binding *b,struct qp_plan *tree,unsigned id,Oid indexid,ScanDirection direction,bool ordered,bool index_only)
{
    if(direction==NoMovementScanDirection || (!ordered && !index_only)) return id;
    if(tree->count==QP_NODES) return -1;
    Relation index=relation_open(indexid,NoLock);
    if(direction==ForwardScanDirection &&
       index_metadata(indexid,b->node->expr.relid,index->rd_index->indkey.values[0],&b->node->expr.join)) {
        b->node->ordered_index=1;
        int key=index->rd_index->indkey.values[0]-1;
        int second=index->rd_index->indnatts>1?index->rd_index->indkey.values[1]-1:-1;
        bool second_integer=second>=0 && second<(int)b->node->expr.nattrs &&
            b->node->expr.cols[second].type==Q_INTEGER &&
            (b->node->expr.cols[second].length==2 || b->node->expr.cols[second].length==4 || b->node->expr.cols[second].length==8);
        bool covered=index_only && key>=0 && key<(int)b->node->expr.nattrs;
        for(unsigned i=0;i<b->node->expr.nattrs;++i)
            if(b->node->expr.cols[i].slot>=0 && (int)i!=key && (!second_integer || (int)i!=second)) covered=false;
        if(covered) {
            b->node->ordered_index|=4;
            if(!ordered) b->node->ordered_index|=8; /* Independent physical leaf ranges. */
            b->node->expr.join.build_key=key+1;
            /* The existing second-key field also describes a covered leaf
             * tuple; point lookups retain their original argument layout. */
            b->node->expr.join.build_key2=second_integer?second+1:0;
        }
        if(!ordered && !covered) {
            b->node->ordered_index=0; memset(&b->node->expr.join,0,sizeof(b->node->expr.join));
            relation_close(index,NoLock); return id;
        }
        HeapTuple stats=SearchSysCache3(STATRELATTINH,ObjectIdGetDatum(b->node->expr.relid),
            Int16GetDatum(index->rd_index->indkey.values[0]),BoolGetDatum(false));
        if(HeapTupleIsValid(stats)) {
            AttStatsSlot slot;
            if(get_attstatsslot(&slot,stats,STATISTIC_KIND_CORRELATION,InvalidOid,ATTSTATSSLOT_NUMBERS)) {
                if(!covered && slot.nnumbers==1 && slot.numbers[0]>0.9) b->node->ordered_index|=2;
                free_attstatsslot(&slot);
            }
            ReleaseSysCache(stats);
        }
        relation_close(index,NoLock); return id;
    }
    if(!ordered) { relation_close(index,NoLock); return id; }
    if(index->rd_rel->relam!=BTREE_AM_OID || RelationGetIndexPredicate(index) || RelationGetIndexExpressions(index) ||
       index->rd_index->indnkeyatts>QS_OUT) { relation_close(index,NoLock); return -1; }
    unsigned out=tree->count++,original=b->node->expr.nout;
    struct qp_node *sort=&tree->nodes[out];
    memset(sort,0,sizeof(*sort)); sort->kind=QP_SORT; sort->left=id; sort->right=-1;
    sort->estimated_rows=b->node->estimated_rows;
    sort->expr.predicate=-1; sort->expr.limit=-1;
    struct qs_plan *scan=&b->node->expr;
    for(int k=0;k<index->rd_index->indnkeyatts;++k) {
        AttrNumber att=index->rd_index->indkey.values[k];
        if(att<=0 || (unsigned)att>scan->nattrs) { relation_close(index,NoLock); return -1; }
        Oid type,collation; int32 mod;
        get_atttypetypmodcoll(scan->relid,att,&type,&mod,&collation);
        Var *v=makeVar(b->scan,att,type,mod,collation,0);
        int expr=b->properties?describe_expression(b,(Node *)v):expression(b,(Node *)v);
        if(expr<0) { relation_close(index,NoLock); return -1; }
        unsigned col=0; while(col<scan->nout && scan->outputs[col].index!=expr) ++col;
        if(col==scan->nout) {
            if(scan->nout==QS_OUT) { relation_close(index,NoLock); return -1; }
            scan->outputs[scan->nout++]=(struct qs_output){.index=expr,.type_oid=type,.typmod=mod};
        }
        bool reverse=direction==BackwardScanDirection;
        sort->expr.order[sort->expr.norder++]=(struct qs_order){.output=col,
            .descending=!!(index->rd_indoption[k]&1)^reverse,.nulls_first=!!(index->rd_indoption[k]&2)^reverse};
    }
    relation_close(index,NoLock);
    sort->expr.nattrs=scan->nout; sort->expr.nout=sort->expr.nexprs=original;
    for(unsigned i=0;i<original;++i) {
        sort->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,.type=scan->exprs[scan->outputs[i].index].type};
        sort->expr.outputs[i]=scan->outputs[i]; sort->expr.outputs[i].index=i;
    }
    return out;
}
static int compile_node_impl(PlannerInfo *root,Plan *native,struct qp_plan *tree,bool ordered)
{
    if(!native || tree->count==QP_NODES || native->initPlan) return -1;
    unsigned id=tree->count++;
    struct qp_node *n=&tree->nodes[id]; memset(n,0,sizeof(*n));
    n->left=n->right=-1; n->join_qual=-1;
    struct qs_plan *p=&n->expr; p->predicate=p->having=-1; p->limit=-1; n->estimated_rows=native->plan_rows;
    struct binding b={.root=root,.plan=native,.node=n,.tree=tree};
    Plan *left=native->lefttree,*right=native->righttree;
    bool left_order=ordered,right_order=false;
    List *index_extra=NIL; Node *index_lookup=NULL; struct qs_join access={0};
    if(IsA(native,SeqScan) || IsA(native,IndexScan) || IsA(native,IndexOnlyScan)) {
        /* Unparameterized indexes can be read as an immutable heap scan.
         * Parameterized index paths are handled by their parent below. */
        n->kind=QP_SCAN; b.scan=((Scan *)native)->scanrelid;
        if(!describe_scan(root,(Scan *)native,p)) return -1;
        /* PG parallel scan rows are per worker; this device node consumes the
         * complete relation. Use PG's total filtered estimate for reservation. */
        if(native->parallel_aware && root->simple_rel_array[b.scan])
            n->estimated_rows=root->simple_rel_array[b.scan]->rows;
    } else if(IsA(native,HashJoin) || IsA(native,MergeJoin) || IsA(native,NestLoop)) {
        Join *j=(Join *)native; n->kind=QP_JOIN;
        n->hash=IsA(native,HashJoin);
        switch(j->jointype) {
        case JOIN_INNER:n->join_type=QP_INNER;break;
        case JOIN_LEFT:n->join_type=QP_LEFT;break;
        /* RIGHT(A,B) is LEFT(B,A). Bind expressions against the swapped
         * inputs below, retaining the original output targetlist. This also
         * keeps unmatched-row handling in the existing LEFT join operator. */
        case JOIN_RIGHT:n->join_type=QP_LEFT;break;
        case JOIN_SEMI:n->join_type=QP_SEMI;break;
        case JOIN_ANTI:n->join_type=QP_ANTI;break;
        default:return -1;
        }
        if(IsA(right,Hash)) right=right->lefttree;
        if(j->jointype==JOIN_RIGHT) { Plan *swap=left; left=right; right=swap; }
        if(IsA(native,NestLoop) && prepare_index_join(root,(NestLoop *)native,&right,&index_extra,&index_lookup,&access))
            { n->kind=QP_INDEX_JOIN; n->sorted_input=ordered; }
        else {
            if(IsA(native,NestLoop) && ordered) return -1;
            left_order=false;
        }
    } else if(IsA(native,Agg)) {
        Agg *a=(Agg *)native; if(a->chain || a->groupingSets) return -1;
        n->kind=QP_AGGREGATE; n->sorted_input=a->aggstrategy==AGG_SORTED;
        n->estimated_groups=a->numGroups;
        if(DO_AGGSPLIT_COMBINE(a->aggsplit)) {
            Plan *partial=partial_input(left); if(!partial) return -1;
            left=partial->lefttree;
            n->sorted_input=((Agg *)partial)->aggstrategy==AGG_SORTED;
        } else if(DO_AGGSPLIT_SKIPFINAL(a->aggsplit)) return -1;
        left_order=n->sorted_input;
    } else if(IsA(native,Sort)) { n->kind=QP_SORT; left_order=false; }
    else if(IsA(native,Limit)) { n->kind=QP_LIMIT; left_order=ordered || root->parse->sortClause!=NIL; }
    else if(IsA(native,SubqueryScan)) {
        n->kind=QP_PROJECT; left=((SubqueryScan *)native)->subplan;
        RelOptInfo *rel=root->simple_rel_array[((SubqueryScan *)native)->scan.scanrelid];
        if(!rel || !rel->subroot) return -1;
        n->left=compile_node(rel->subroot,left,tree,ordered || (rel->subroot->parse->limitCount && rel->subroot->parse->sortClause)); if(n->left<0) return -1;
    } else if(IsA(native,CteScan)) {
        CteScan *cte=(CteScan *)native;
        if(cte->ctePlanId<=0 || cte->ctePlanId>QP_PARAMS) return -1;
        n->kind=QP_CTE_SCAN; left=list_nth(root->glob->subplans,cte->ctePlanId-1);
        int materialized=tree->cte_nodes[cte->ctePlanId-1];
        if(materialized<0) {
            if(tree->count==QP_NODES) return -1;
            materialized=tree->count++; tree->cte_nodes[cte->ctePlanId-1]=materialized;
            int child=compile_tree(list_nth(root->glob->subroots,cte->ctePlanId-1),left,tree);
            if(child<0) return -1;
            struct qp_node *m=&tree->nodes[materialized]; memset(m,0,sizeof(*m));
            m->kind=QP_MATERIALIZED; m->left=child; m->right=-1; m->expr=tree->nodes[child].expr;
        }
        n->left=materialized;
    } else if(IsA(native,Result) || IsA(native,Gather) || IsA(native,GatherMerge) || IsA(native,Material) || IsA(native,Hash))
        n->kind=QP_PROJECT;
    else return -1;
    if(left && n->left<0) { n->left=compile_node(root,left,tree,left_order); if(n->left<0) return -1; }
    if(right) { n->right=compile_node(root,right,tree,right_order); if(n->right<0) return -1; }
    /* DuckDB's build/probe selection prices cardinality AND row width.
     * Commute only an INNER hash join: its relation pair stays unchanged,
     * and expression binding below preserves the original output columns. */
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE && n->kind==QP_JOIN && n->hash && n->join_type==QP_INNER && left && right &&
       tree->nodes[n->left].estimated_rows*Max(1,left->plan_width)<
       tree->nodes[n->right].estimated_rows*Max(1,right->plan_width)) {
        int swap=n->left; n->left=n->right; n->right=swap;
        Plan *input=left; left=right; right=input;
    }
    b.left=left?left->targetlist:NIL; b.right=right?right->targetlist:NIL;
    if(n->kind!=QP_SCAN) {
        p->nattrs=list_length(b.left)+list_length(b.right);
        if(p->nattrs>QS_COLS) return -1;
        List *lists[]={b.left,b.right}; unsigned col=0;
        for(unsigned side=0;side<2;++side) { ListCell *c; foreach(c,lists[side]) {
            TargetEntry *t=lfirst_node(TargetEntry,c);
            p->cols[col]=(struct qs_column){.slot=col,.type=type_kind(exprType((Node *)t->expr),exprTypmod((Node *)t->expr))}; ++col;
        }}
        p->ncols=p->nattrs;
    }
    if(n->kind==QP_INDEX_JOIN) {
        int key=expression(&b,index_lookup);
        if(key<0 || p->exprs[key].op!=Q_COLUMN || p->exprs[key].column>=list_length(b.left)) return -1;
        n->left_keys[0]=p->exprs[key].column;
        tree->nodes[n->right].expr.join=access;
        n->join_qual=conditions(&b,list_concat(list_copy(((Join *)native)->joinqual),index_extra));
        if(n->join_qual==-2) return -1;
    } else if(n->kind==QP_JOIN) {
        List *keys=IsA(native,HashJoin)?((HashJoin *)native)->hashclauses:IsA(native,MergeJoin)?((MergeJoin *)native)->mergeclauses:NIL;
        ListCell *c;
        foreach(c,keys) {
            OpExpr *key=lfirst_node(OpExpr,c);
            if(n->nkeys==QS_KEYS || list_length(key->args)!=2) return -1;
            int a=expression(&b,linitial(key->args)),z=expression(&b,lsecond(key->args));
            if(a<0 || z<0 || p->exprs[a].op!=Q_COLUMN || p->exprs[z].op!=Q_COLUMN) return -1;
            unsigned width=list_length(b.left);
            if((unsigned)p->exprs[a].column>=width) { int swap=a; a=z; z=swap; }
            if((unsigned)p->exprs[a].column>=width || (unsigned)p->exprs[z].column<width) return -1;
            n->left_keys[n->nkeys]=p->exprs[a].column;
            n->right_keys[n->nkeys++]=p->exprs[z].column-width;
        }
        /* Non-index nested loops use their equality join conditions. */
        if(!n->nkeys && IsA(native,NestLoop)) {
            foreach(c,((Join *)native)->joinqual) {
                Node *node=lfirst(c); if(!IsA(node,OpExpr)) continue;
                OpExpr *key=(OpExpr *)node; Oid fn=get_opcode(key->opno);
                if(list_length(key->args)!=2 || (!function_is(fn,"int4eq") && !function_is(fn,"int8eq") && !function_is(fn,"int2eq"))) continue;
                int a=expression(&b,linitial(key->args)),z=expression(&b,lsecond(key->args));
                if(a<0 || z<0 || p->exprs[a].op!=Q_COLUMN || p->exprs[z].op!=Q_COLUMN) continue;
                unsigned width=list_length(b.left);
                if((unsigned)p->exprs[a].column>=width) { int swap=a; a=z; z=swap; }
                if((unsigned)p->exprs[a].column>=width || (unsigned)p->exprs[z].column<width || n->nkeys==QS_KEYS) continue;
                n->left_keys[n->nkeys]=p->exprs[a].column; n->right_keys[n->nkeys++]=p->exprs[z].column-width;
            }
        }
        if(!n->nkeys) return -1;
        /* Keep PG's join order, but equality joins whose output need not be
         * ordered can use Hash without sorting both complete inputs. */
        if(!ordered && pg_sina_exe_path!=PG_SINA_ADAPTIVE) n->hash=1;
        if(IsA(native,MergeJoin)) {
            MergeJoin *m=(MergeJoin *)native;
            for(unsigned i=0;i<n->nkeys;++i)
                p->order[p->norder++]=(struct qs_order){.output=n->left_keys[i],.descending=m->mergeStrategies[i]==5,.nulls_first=m->mergeNullsFirst[i]};
        }
        n->join_qual=conditions(&b,((Join *)native)->joinqual);
        if(n->join_qual==-2) return -1;
    }
    if(n->kind==QP_AGGREGATE) {
        Agg *a=(Agg *)native;
        if(pg_sina_exe_path==PG_SINA_ADAPTIVE) n->hash=a->aggstrategy==AGG_HASHED;
        if(a->numCols>QS_KEYS) return -1;
        for(int i=0;i<a->numCols;++i) {
            TargetEntry *t=get_tle_by_resno(native->lefttree->targetlist,a->grpColIdx[i]);
            if(!t) return -1;
            int key=expression(&b,(Node *)t->expr); if(key<0) return -1;
            p->keys[p->nkeys++]=key;
            if(p->exprs[key].op!=Q_COLUMN) return -1;
            n->left_keys[n->nkeys++]=p->exprs[key].column;
        }
        if(tree->nodes[n->left].kind!=QP_SORT && !tree->nodes[n->left].ordered_index) n->sorted_input=0;
    }
    ListCell *c;
    foreach(c,native->targetlist) {
        TargetEntry *t=lfirst_node(TargetEntry,c); if(p->nout==QS_OUT) return -1;
        int expr=expression(&b,(Node *)t->expr); if(expr<0) return -1;
        p->outputs[p->nout++]=(struct qs_output){.index=expr,.type_oid=exprType((Node *)t->expr),.typmod=exprTypmod((Node *)t->expr)};
    }
    p->predicate=conditions(&b,native->qual); if(p->predicate==-2) return -1;
    if(IsA(native,Result) && ((Result *)native)->resconstantqual) {
        Node *qual=((Result *)native)->resconstantqual;
        int extra=IsA(qual,List)?conditions(&b,(List *)qual):expression(&b,qual); if(extra<0) return -1;
        p->predicate=p->predicate<0?extra:add_expr(p,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=p->predicate,.right=extra});
        if(p->predicate<0) return -1;
    }
    if(n->kind==QP_SORT) {
        Sort *s=(Sort *)native; if(s->numCols>QS_OUT) return -1;
        for(int i=0;i<s->numCols;++i) {
            Oid opfamily,intype; int16 strategy;
            if(!get_ordering_op_properties(s->sortOperators[i],&opfamily,&intype,&strategy)) return -1;
            p->order[p->norder++]=(struct qs_order){.output=s->sortColIdx[i]-1,.descending=strategy==5,.nulls_first=s->nullsFirst[i]};
        }
    }
    if(n->kind==QP_LIMIT) {
        Limit *l=(Limit *)native;
        if(l->limitCount) { if(!IsA(l->limitCount,Const)) return -1; Const *v=(Const *)l->limitCount; if(!v->constisnull) p->limit=DatumGetInt64(v->constvalue); }
        if(l->limitOffset) { if(!IsA(l->limitOffset,Const)) return -1; Const *v=(Const *)l->limitOffset; if(!v->constisnull) p->offset=DatumGetInt64(v->constvalue); }
    }
    /* Keep PG HashAggregate or replace grouping-only sorting with bounded
     * states. Required output order is reapplied above the aggregate. */
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE && n->kind==QP_AGGREGATE && qp_hash_aggregate_supported(n) && !n->distinct && !n->distinct_aggs &&
       p->having<0 && n->left>=0 && (tree->nodes[n->left].kind==QP_SORT ||
        (!ordered && IsA(native,Agg) && ((Agg *)native)->aggstrategy==AGG_HASHED))) {
        bool remove_sort=tree->nodes[n->left].kind==QP_SORT;
        int sort_id=n->left,input=remove_sort?tree->nodes[sort_id].left:n->left;
        /* Hash and Index joins both expose resumable column input to the sink. */
        bool supported=input>=0 && (!remove_sort || tree->nodes[input].kind==QP_JOIN ||
            tree->nodes[input].kind==QP_INDEX_JOIN ||
            (tree->nodes[input].kind==QP_SCAN && !tree->nodes[input].ordered_index));
        const struct qs_plan *sorted=&tree->nodes[n->left].expr;
        if(remove_sort && input>=0) {
            if(sorted->nout!=tree->nodes[input].expr.nout) supported=false;
            for(unsigned i=0;i<sorted->nout;++i) {
                const struct qs_expr *x=&sorted->exprs[sorted->outputs[i].index];
                if(x->op!=Q_COLUMN || x->column!=(int)i) supported=false;
            }
        }
        for(unsigned i=0;i<p->nout;++i) {
            if(!group_output(p,p->outputs[i].index,n,false)) supported=false;
        }
        if(p->predicate>=0 && !group_output(p,p->predicate,n,false)) supported=false;
        /* A sorted GroupAggregate can also supply SQL ORDER BY. Preserve
         * that order by moving its Sort above the aggregate, using output
         * column positions. The two existing nodes suffice. */
        struct qs_order orders[QS_KEYS]; unsigned count=sorted->norder;
        if(ordered) {
            if(!remove_sort || count>QS_KEYS) supported=false;
            for(unsigned k=0;supported && k<count;++k) {
                int output=-1;
                for(unsigned i=0;i<p->nout;++i) {
                    const struct qs_expr *x=&p->exprs[p->outputs[i].index];
                    if(x->op==Q_COLUMN && x->column==sorted->order[k].output) { output=i; break; }
                }
                if(output<0) supported=false;
                else { orders[k]=sorted->order[k]; orders[k].output=output; }
            }
        }
        if(supported) {
            n->left=input; n->sorted_input=0; n->hash=1;
            /* The removed grouping Sort imposed order on the probe stream.
             * Hash aggregation no longer needs it: keep Index Join's vector
             * batch instead of accidentally retaining one-key ordered probes. */
            for(int at=input;at>=0;at=tree->nodes[at].left) {
                struct qp_node *child=&tree->nodes[at];
                if(child->kind==QP_INDEX_JOIN) child->sorted_input=0;
                else if(child->kind!=QP_PROJECT && !(child->kind==QP_JOIN && child->hash)) break;
            }
            if(ordered) {
                tree->nodes[sort_id]=*n;
                const struct qp_node *aggregate=&tree->nodes[sort_id];
                *n=(struct qp_node){.kind=QP_SORT,.left=sort_id,.right=-1,.join_qual=-1,.estimated_rows=aggregate->estimated_rows};
                n->expr.predicate=n->expr.having=-1; n->expr.limit=-1;
                n->expr.nout=n->expr.nexprs=n->expr.nattrs=n->expr.ncols=aggregate->expr.nout;
                for(unsigned i=0;i<n->expr.nout;++i) {
                    const struct qs_expr *x=&aggregate->expr.exprs[aggregate->expr.outputs[i].index];
                    n->expr.exprs[i]=(struct qs_expr){.op=Q_COLUMN,.column=i,.type=x->type,.scale=x->scale,.dscale=x->dscale};
                    n->expr.outputs[i]=aggregate->expr.outputs[i]; n->expr.outputs[i].index=i;
                }
                n->expr.norder=count; memcpy(n->expr.order,orders,count*sizeof(*orders));
            }
        }
    }
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE || (pg_sina_encoding_choice(id)&PG_SINA_DISTINCT_SORT)) bypass_distinct_sort(tree,n);
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE || (pg_sina_encoding_choice(id)&PG_SINA_DISTINCT)) choose_distinct(tree,n);
    if(n->kind==QP_SCAN && IsA(native,IndexScan)) {
        int extra=conditions(&b,((IndexScan *)native)->indexqualorig); if(extra==-2) return -1;
        if(extra>=0) p->predicate=p->predicate<0?extra:add_expr(p,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=p->predicate,.right=extra});
        return ordered_heap_scan(&b,tree,id,((IndexScan *)native)->indexid,((IndexScan *)native)->indexorderdir,ordered,false);
    }
    if(n->kind==QP_SCAN && IsA(native,IndexOnlyScan)) {
        IndexOnlyScan *index=(IndexOnlyScan *)native;
        List *quals=(List *)heap_index_var((Node *)index->recheckqual,index->indextlist);
        int extra=conditions(&b,quals); if(extra==-2) return -1;
        if(extra>=0) p->predicate=p->predicate<0?extra:add_expr(p,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=p->predicate,.right=extra});
        return ordered_heap_scan(&b,tree,id,index->indexid,index->indexorderdir,ordered,true);
    }
    if(n->kind==QP_JOIN && !ordered && ((Join *)native)->inner_unique &&
       (pg_sina_exe_path!=PG_SINA_ADAPTIVE || (pg_sina_encoding_choice(id)&PG_SINA_GROUP_FILTER))) push_group_key_filter(tree,n);
    if(n->kind==QP_AGGREGATE &&
       (pg_sina_exe_path!=PG_SINA_ADAPTIVE || (pg_sina_encoding_choice(id)&PG_SINA_LEFT_COUNT))) push_left_join_count(tree,n);
    /* If no consumer requires group order, avoid sorting the full input or
     * serializing an unfiltered Heap scan through a non-covering index.
     * Keep PG's join tree and the input target list; use bounded Hash Group. */
    if(pg_sina_exe_path!=PG_SINA_ADAPTIVE && n->kind==QP_AGGREGATE && !ordered && n->sorted_input && n->nkeys &&
       qp_hash_aggregate_supported(n) && !n->distinct && !n->distinct_aggs && p->having<0 && n->left>=0) {
        struct qp_node *input=&tree->nodes[n->left];
        bool sorting=input->kind==QP_SORT && input->expr.limit<0 && !input->expr.offset;
        bool index=input->kind==QP_SCAN && input->ordered_index && !(input->ordered_index&4) && input->expr.predicate<0;
        bool supported=sorting || index;
        unsigned keys=n->nkeys,plan_keys=p->nkeys;
        /* PG has already validated that non-aggregate output attributes are
         * functionally dependent on the grouping keys. Retain them as extra
         * physical keys, so Hash Group can return that payload without
         * changing the SQL groups. Do not infer dependencies on the device. */
        for(unsigned i=0;supported && i<p->nout;++i)
            supported=group_output(p,p->outputs[i].index,n,sorting);
        if(supported && p->predicate>=0) supported=group_output(p,p->predicate,n,sorting);
        if(supported) supported=qp_hash_aggregate_supported(n);
        if(supported) {
            if(sorting) { input->kind=QP_PROJECT; input->expr.norder=0; }
            else { input->ordered_index=0; memset(&input->expr.join,0,sizeof(input->expr.join)); }
            n->sorted_input=0; n->hash=1;
        } else { n->nkeys=keys; p->nkeys=plan_keys; }
    }
    return id;
}

static int compile_node(PlannerInfo *root,Plan *native,struct qp_plan *tree,bool ordered)
{
    if(pg_sina_planning) ++pg_sina_planning->encoded_nodes;
    return compile_node_impl(root,native,tree,ordered);
}

/* These expressions describe device output slots, not host computation.
 * A relabelled NULL keeps parameter type metadata and remains a non-Const
 * expression that setrefs will replace by the matching output slot. */
static Node *device_output_expression(Node *node,void *context)
{
    if(!node) return NULL;
    if(IsA(node,Param)) {
        Param *param=(Param *)node;
        return (Node *)makeRelabelType((Expr *)makeNullConst(param->paramtype,
            param->paramtypmod,param->paramcollid),param->paramtype,
            param->paramtypmod,param->paramcollid,COERCE_IMPLICIT_CAST);
    }
    return expression_tree_mutator(node,device_output_expression,context);
}

/* Candidate properties use the existing cost-model workspace, not a device
 * program: expressions retain only type/column lineage and a count. Literal
 * values, executable predicates and scalar instruction operands are omitted.
 * A selected candidate goes through expression() exactly once afterwards. */
static int describe_tree(PlannerInfo *,Plan *,struct qp_plan *,bool);
static int describe_expression(struct binding *b,Node *node)
{
    if(!node) return -1;
    while(IsA(node,RelabelType)) node=(Node *)((RelabelType *)node)->arg;
    struct qs_plan *p=&b->node->expr;
    unsigned type=type_kind(exprType(node),exprTypmod(node));
    if(type==255 || ((type==Q_STRING || type==Q_CHAR) && !byte_order(exprCollation(node)))) return -1;
    struct qs_expr e={.op=Q_AGGREF,.left=-1,.right=-1,.otherwise=-1,.column=-1,.type=type};
    int col=input_column(b,node);
    if(col>=0) { e.op=Q_COLUMN; e.column=col; return add_expr(p,e); }
    if(IsA(node,Var)) {
        Var *v=(Var *)node;
        if(v->varlevelsup || v->varno!=b->scan || v->varattno<=0 || (unsigned)v->varattno>p->nattrs) return -1;
        e.op=Q_COLUMN; e.column=v->varattno-1;
        if(p->cols[e.column].type==255 || (type==Q_STRING && p->cols[e.column].type!=Q_STRING)) return -1;
        if(p->cols[e.column].slot<0) p->cols[e.column].slot=p->ncols++;
        return add_expr(p,e);
    }
    if(IsA(node,Param)) {
        Param *v=(Param *)node;
        if(v->paramkind!=PARAM_EXEC || v->paramid>=QP_PARAMS || b->tree->param_nodes[v->paramid]<0) return -1;
        e.op=QP_PARAMETER; e.column=v->paramid; return add_expr(p,e);
    }
    if(IsA(node,Const)) {
        Const *c=(Const *)node;
        if(!c->constisnull) switch(c->consttype) {
        case INT2OID:case INT4OID:case INT8OID:case BOOLOID:case DATEOID:case TIMESTAMPOID:
        case NUMERICOID:case BPCHAROID:case VARCHAROID:case TEXTOID:break;
        default:return -1;
        }
        ListCell *at; foreach(at,b->literals) if(equal(c,lfirst(at))) {
            e.op=Q_CONST; e.constant=(intptr_t)lfirst(at); return add_expr(p,e);
        }
        b->literals=lappend(b->literals,c);
        if(!c->constisnull && c->consttype==NUMERICOID) {
            int64 value; unsigned scale; if(!numeric_literal(c->constvalue,&value,&scale)) return -1;
        }
        if(!c->constisnull && c->consttype==DATEOID && DATE_NOT_FINITE(DatumGetInt32(c->constvalue))) return -1;
        if(!c->constisnull && c->consttype==TIMESTAMPOID && DatumGetInt64(c->constvalue)%INT64CONST(86400000000)) return -1;
        if(!c->constisnull && (type==Q_STRING || type==Q_CHAR)) {
            text *value=DatumGetTextPP(c->constvalue); unsigned bytes=VARSIZE_ANY_EXHDR(value);
            if(c->consttype==BPCHAROID) while(bytes && VARDATA_ANY(value)[bytes-1]==' ') --bytes;
            if(bytes>=sizeof(p->strings[0].bytes) || p->nstrings==QS_STRINGS) return -1;
            p->strings[p->nstrings++].length=bytes;
        }
        e.op=Q_CONST; e.constant=(intptr_t)node; /* Distinguish syntax, never an execution value. */
        return add_expr(p,e);
    }
    if(IsA(node,ScalarArrayOpExpr)) {
        ScalarArrayOpExpr *s=(ScalarArrayOpExpr *)node;
        if(list_length(s->args)!=2 || !IsA(lsecond(s->args),Const)) return -1;
        Const *array=lsecond_node(Const,s->args); if(array->constisnull) return -1;
        ArrayType *data=DatumGetArrayTypeP(array->constvalue); Oid type=ARR_ELEMTYPE(data);
        int16 length; bool byval; char align; Datum *values; bool *nulls; int count;
        get_typlenbyvalalign(type,&length,&byval,&align);
        deconstruct_array(data,type,length,byval,align,&values,&nulls,&count);
        for(int i=0;i<count;++i) {
            Const *value=makeConst(type,-1,array->constcollid,length,values[i],nulls[i],byval);
            OpExpr *op=(OpExpr *)make_opclause(s->opno,BOOLOID,false,linitial(s->args),(Expr *)value,InvalidOid,s->inputcollid);
            if(describe_expression(b,(Node *)op)<0) return -1;
            if(i>0) { e.constant=(intptr_t)value; if(add_expr(p,e)<0) return -1; }
        }
        e.constant=(intptr_t)node; return add_expr(p,e);
    }
    if(IsA(node,Aggref)) {
        Aggref *a=(Aggref *)node; int at=0; ListCell *c;
        foreach(c,b->aggregates) { if(equal(a,lfirst(c))) { e.column=at; return add_expr(p,e); } ++at; }
        if(a->aggfilter || a->aggorder || a->agglevelsup || a->aggvariadic || a->aggtype==BPCHAROID ||
           a->aggtype==FLOAT8OID || p->naggs==QS_AGGS) return -1;
        const char *name=get_func_name(a->aggfnoid); unsigned kind;
        if(!name || get_func_namespace(a->aggfnoid)!=PG_CATALOG_NAMESPACE) return -1;
        if(!strcmp(name,"sum")) kind=Q_SUM; else if(!strcmp(name,"avg")) kind=Q_AVG;
        else if(!strcmp(name,"count")) kind=a->aggstar?Q_COUNT_STAR:Q_COUNT;
        else if(!strcmp(name,"min")) kind=Q_MIN; else if(!strcmp(name,"max")) kind=Q_MAX; else return -1;
        int arg=-1;
        if(kind!=Q_COUNT_STAR) {
            if(list_length(a->args)!=1) return -1;
            arg=describe_expression(b,(Node *)linitial_node(TargetEntry,a->args)->expr); if(arg<0) return -1;
        }
        if(a->aggdistinct) {
            if(list_length(a->aggdistinct)!=1 || kind!=Q_COUNT || p->exprs[arg].op!=Q_COLUMN) return -1;
            unsigned column=p->exprs[arg].column+1;
            if(b->node->distinct && b->node->distinct!=column) return -1;
            b->node->distinct=column; b->node->distinct_aggs|=1U<<p->naggs;
        }
        e.column=p->naggs; p->aggs[p->naggs++]=(struct qs_agg){.kind=kind,.expr=arg};
        b->aggregates=lappend(b->aggregates,a); return add_expr(p,e);
    }
    if(IsA(node,SubPlan)) {
        SubPlan *s=(SubPlan *)node;
        if(s->subLinkType!=ANY_SUBLINK || !s->useHashTable || s->parParam || s->args ||
           list_length(s->paramIds)!=1 || !IsA(s->testexpr,OpExpr)) return -1;
        OpExpr *op=(OpExpr *)s->testexpr;
        if(list_length(op->args)!=2) return -1;
        Node *input=linitial(op->args),*param=lsecond(op->args);
        if(IsA(input,Param)) { Node *swap=input; input=param; param=swap; }
        if(!IsA(param,Param) || ((Param *)param)->paramid!=linitial_int(s->paramIds)) return -1;
        Oid fn=get_opcode(op->opno);
        if(!function_is(fn,"int2eq") && !function_is(fn,"int4eq") && !function_is(fn,"int8eq")) return -1;
        if(describe_expression(b,input)<0 || b->tree->ninit==QP_PARAMS) return -1;
        PlannerInfo *root=list_nth(b->root->glob->subroots,s->plan_id-1);
        Plan *chosen=pg_sina_prepare_candidate(root,list_nth(b->root->glob->subplans,s->plan_id-1),false);
        int child=chosen?describe_tree(root,chosen,b->tree,false):-1;
        if(child<0 || b->tree->count==QP_NODES) return -1;
        unsigned set=b->tree->count++;
        b->tree->nodes[set]=(struct qp_node){.kind=QP_SET,.left=child,.right=-1,.expr=b->tree->nodes[child].expr};
        unsigned init=b->tree->ninit++; b->tree->init_nodes[init]=set; b->tree->init_params[init]=-1;
        e.op=QP_INSET; e.column=set; return add_expr(p,e);
    }
    List *args=NIL; const char *name=NULL;
    if(IsA(node,OpExpr) || IsA(node,FuncExpr)) {
        Oid fn;
        if(IsA(node,OpExpr)) { args=((OpExpr *)node)->args; fn=get_opcode(((OpExpr *)node)->opno); }
        else { args=((FuncExpr *)node)->args; fn=((FuncExpr *)node)->funcid; }
        if(get_func_namespace(fn)!=PG_CATALOG_NAMESPACE || !(name=get_func_name(fn))) return -1;
        bool supported=false;
        static const char *functions[]={"numeric_add","numeric_sub","numeric_mul","numeric_div","textlike","textnlike",
            "bpcharlike","bpcharnlike","text","numeric","int2_numeric","int4_numeric","int8_numeric","date_timestamp",
            "timestamp_date","timestamp_timestamp","timestamp","bpchar","date_part","substring","text_substr","text_substr_no_len"};
        for(unsigned i=0;i<lengthof(functions);++i) if(!strcmp(name,functions[i])) supported=true;
        static const char *prefix[]={"numeric_","int2","int4","int8","int24","int28","int42","int48","int82","int84","date_","bpchar","bool","text","text_","float8"};
        static const char *suffix[]={"eq","ne","lt","le","gt","ge"};
        for(unsigned i=0;i<lengthof(prefix);++i) {
            size_t length=strlen(prefix[i]); if(strncmp(name,prefix[i],length)) continue;
            for(unsigned j=0;j<lengthof(suffix);++j) if(!strcmp(name+length,suffix[j])) supported=true;
        }
        static const char *timestamp[]={"date_eq_timestamp","date_ne_timestamp","date_lt_timestamp","date_le_timestamp","date_gt_timestamp","date_ge_timestamp"};
        for(unsigned i=0;i<lengthof(timestamp);++i) if(!strcmp(name,timestamp[i])) supported=true;
        if(!supported) return -1;
        unsigned arity=list_length(args);
        bool cast=!strcmp(name,"text") || !strcmp(name,"numeric") || !strcmp(name,"int2_numeric") ||
            !strcmp(name,"int4_numeric") || !strcmp(name,"int8_numeric") || !strcmp(name,"date_timestamp") ||
            !strcmp(name,"timestamp_date") || !strcmp(name,"timestamp_timestamp") || !strcmp(name,"timestamp") || !strcmp(name,"bpchar");
        bool substring=!strcmp(name,"substring") || !strcmp(name,"text_substr") || !strcmp(name,"text_substr_no_len");
        if(arity!=(cast?1U:substring?3U:2U)) return -1;
        /* The cost of a predicate-only LIKE is boolean decoding, not a
         * dictionary containing every input string. No pattern is encoded. */
        if(b->scan && strstr(name,"like") && list_length(args)==2) {
            Node *value=linitial(args),*pattern=lsecond(args);
            bool trim=false;
            while(IsA(value,RelabelType)) value=(Node *)((RelabelType *)value)->arg;
            if(IsA(value,FuncExpr) && list_length(((FuncExpr *)value)->args)==1 && function_is(((FuncExpr *)value)->funcid,"text")) {
                value=linitial(((FuncExpr *)value)->args); trim=true;
            }
            if(IsA(value,Var) && IsA(pattern,Const) && !((Const *)pattern)->constisnull) {
                Var *v=(Var *)value; text *literal_text=DatumGetTextPP(((Const *)pattern)->constvalue);
                unsigned bytes=VARSIZE_ANY_EXHDR(literal_text); const char *data=VARDATA_ANY(literal_text);
                if(v->varno==b->scan && v->varattno>0 && (unsigned)v->varattno<=p->nattrs &&
                   bytes<=32 && !memchr(data,'_',bytes) && !memchr(data,'\\',bytes)) {
                    struct qs_column *column=&p->cols[v->varattno-1];
                    if((column->type==Q_STRING && column->slot<0) || (column->type==Q_PATTERN &&
                       column->trim_spaces==trim && column->prefix_len==bytes && !memcmp(column->prefix,data,bytes))) {
                        column->type=Q_PATTERN; column->trim_spaces=trim; column->prefix_len=bytes;
                        memcpy(column->prefix,data,bytes); if(column->slot<0) column->slot=p->ncols++;
                        e.op=Q_COLUMN; e.column=v->varattno-1; e.type=Q_BOOLEAN;
                        int decoded=add_expr(p,e); if(decoded<0 || !strstr(name,"nlike")) return decoded;
                        e.op=Q_AGGREF; e.column=-1; e.constant=(intptr_t)node; return add_expr(p,e);
                    }
                }
            }
        }
        if(!strcmp(name,"date_part")) {
            if(list_length(args)!=2 || !IsA(linitial(args),Const)) return -1;
            Const *part=linitial_node(Const,args);
            if(part->constisnull || part->consttype!=TEXTOID || strcmp(TextDatumGetCString(part->constvalue),"year")) return -1;
            e.op=Q_YEAR; e.left=describe_expression(b,lsecond(args)); return e.left<0?-1:add_expr(p,e);
        }
    } else if(IsA(node,NullTest)) {
        if(((NullTest *)node)->argisrow) return -1;
        args=list_make1(((NullTest *)node)->arg);
    } else if(IsA(node,BoolExpr)) args=((BoolExpr *)node)->args;
    else if(IsA(node,CoalesceExpr)) args=((CoalesceExpr *)node)->args;
    else if(IsA(node,CaseExpr)) {
        CaseExpr *v=(CaseExpr *)node; if(v->arg) return -1;
        args=list_make1(v->defresult); ListCell *c;
        foreach(c,v->args) { CaseWhen *w=lfirst(c); args=lappend(args,w->expr); args=lappend(args,w->result); }
    } else return -1;
    int first=-1; ListCell *c;
    foreach(c,args) { int child=describe_expression(b,lfirst(c)); if(child<0) return -1; if(first<0) first=child; }
    /* Transparent casts preserve column lineage for NDV/text-width proofs. */
    if(name && list_length(args)==1) return first;
    e.constant=(intptr_t)node; return add_expr(p,e);
}
static bool describe_list(struct binding *b,List *expressions)
{
    ListCell *c; foreach(c,expressions) if(describe_expression(b,lfirst(c))<0) return false;
    /* conditions() combines a list with AND. Count those operations without
     * constructing their executable operands. */
    for(int i=1;i<list_length(expressions);++i) {
        struct qs_expr conjunction={.op=Q_AGGREF,.type=Q_BOOLEAN,.column=-1-i,.constant=(intptr_t)expressions};
        if(add_expr(&b->node->expr,conjunction)<0) return false;
    }
    return true;
}
static int describe_tree(PlannerInfo *root,Plan *native,struct qp_plan *tree,bool ordered)
{
    if(pg_sina_planning) ++pg_sina_planning->property_nodes;
    if(!native || native->initPlan || tree->count==QP_NODES) return -1;
    unsigned id=tree->count++; struct qp_node *n=&tree->nodes[id]; memset(n,0,sizeof(*n));
    n->left=n->right=n->join_qual=-1; n->estimated_rows=native->plan_rows;
    struct qs_plan *p=&n->expr; p->predicate=native->qual?0:-1; p->having=-1; p->limit=-1;
    struct binding b={.root=root,.plan=native,.node=n,.tree=tree,.properties=true};
    Plan *left=native->lefttree,*right=native->righttree; List *extra=NIL; Node *lookup=NULL; struct qs_join access={0};
    if(IsA(native,SeqScan) || IsA(native,IndexScan) || IsA(native,IndexOnlyScan)) {
        n->kind=QP_SCAN; b.scan=((Scan *)native)->scanrelid;
        if(!describe_scan(root,(Scan *)native,p)) return -1;
        if(native->parallel_aware && root->simple_rel_array[b.scan]) n->estimated_rows=root->simple_rel_array[b.scan]->rows;
    } else if(IsA(native,HashJoin) || IsA(native,MergeJoin) || IsA(native,NestLoop)) {
        Join *j=(Join *)native; n->kind=QP_JOIN; n->hash=IsA(native,HashJoin);
        switch(j->jointype) {
        case JOIN_INNER:n->join_type=QP_INNER;break; case JOIN_LEFT:case JOIN_RIGHT:n->join_type=QP_LEFT;break;
        case JOIN_SEMI:n->join_type=QP_SEMI;break; case JOIN_ANTI:n->join_type=QP_ANTI;break; default:return -1;
        }
        if(IsA(right,Hash)) right=right->lefttree;
        if(j->jointype==JOIN_RIGHT) { Plan *swap=left; left=right; right=swap; }
        if(IsA(native,NestLoop)) {
            if(!prepare_index_join(root,(NestLoop *)native,&right,&extra,&lookup,&access)) return -1;
            n->kind=QP_INDEX_JOIN; n->sorted_input=ordered;
        }
        n->join_qual=j->joinqual?0:-1;
    } else if(IsA(native,Agg)) {
        Agg *a=(Agg *)native; if(a->chain || a->groupingSets || a->aggsplit!=AGGSPLIT_SIMPLE) return -1;
        n->kind=QP_AGGREGATE; n->hash=a->aggstrategy==AGG_HASHED; n->sorted_input=a->aggstrategy==AGG_SORTED;
        n->estimated_groups=a->numGroups;
    } else if(IsA(native,Sort)) n->kind=QP_SORT;
    else if(IsA(native,Limit)) n->kind=QP_LIMIT;
    else if(IsA(native,SubqueryScan)) {
        RelOptInfo *r=root->simple_rel_array[((SubqueryScan *)native)->scan.scanrelid];
        if(!r || !r->subroot) return -1;
        n->kind=QP_PROJECT; n->left=describe_tree(r->subroot,((SubqueryScan *)native)->subplan,tree,ordered); if(n->left<0) return -1;
        left=((SubqueryScan *)native)->subplan;
    } else if(IsA(native,CteScan)) {
        int plan=((CteScan *)native)->ctePlanId;
        if(plan<=0 || plan>QP_PARAMS) return -1;
        n->kind=QP_CTE_SCAN;
        if(tree->cte_nodes[plan-1]<0) {
            if(tree->count==QP_NODES) return -1;
            unsigned materialized=tree->count++; tree->cte_nodes[plan-1]=materialized;
            PlannerInfo *subroot=list_nth(root->glob->subroots,plan-1);
            Plan *chosen=pg_sina_prepare_candidate(subroot,list_nth(root->glob->subplans,plan-1),false);
            int child=chosen?describe_tree(subroot,chosen,tree,false):-1; if(child<0) return -1;
            tree->nodes[materialized]=(struct qp_node){.kind=QP_MATERIALIZED,.left=child,.right=-1,.expr=tree->nodes[child].expr};
        }
        n->left=tree->cte_nodes[plan-1];
    } else if(IsA(native,Result) || IsA(native,Gather) || IsA(native,GatherMerge) || IsA(native,Material) || IsA(native,Hash)) n->kind=QP_PROJECT;
    else return -1;
    bool left_order=n->kind==QP_SORT || (n->kind==QP_JOIN && n->hash)?false:n->kind==QP_AGGREGATE?n->sorted_input:ordered;
    if(IsA(native,MergeJoin)) left_order=false;
    if(IsA(native,Limit)) left_order=ordered || root->parse->sortClause!=NIL;
    if(left && n->left<0) { n->left=describe_tree(root,left,tree,left_order); if(n->left<0) return -1; }
    if(right) { n->right=describe_tree(root,right,tree,false); if(n->right<0) return -1; }
    b.left=left?left->targetlist:NIL; b.right=right?right->targetlist:NIL;
    if(n->kind!=QP_SCAN) {
        p->nattrs=p->ncols=list_length(b.left)+list_length(b.right); if(p->nattrs>QS_COLS) return -1;
        unsigned at=0; List *lists[]={b.left,b.right};
        for(unsigned side=0;side<2;++side) { ListCell *c; foreach(c,lists[side]) {
            Node *value=(Node *)lfirst_node(TargetEntry,c)->expr;
            p->cols[at]=(struct qs_column){.slot=at,.type=type_kind(exprType(value),exprTypmod(value))}; ++at;
        }}
    }
    if(n->kind==QP_JOIN || n->kind==QP_INDEX_JOIN) {
        List *keys=IsA(native,HashJoin)?((HashJoin *)native)->hashclauses:IsA(native,MergeJoin)?((MergeJoin *)native)->mergeclauses:NIL;
        if(n->kind==QP_INDEX_JOIN) {
            int key=describe_expression(&b,lookup); if(key<0 || p->exprs[key].op!=Q_COLUMN) return -1;
            n->left_keys[n->nkeys++]=p->exprs[key].column; tree->nodes[n->right].expr.join=access;
        }
        ListCell *c; foreach(c,keys) {
            if(n->nkeys==QS_KEYS || !IsA(lfirst(c),OpExpr)) return -1;
            OpExpr *key=lfirst(c); if(list_length(key->args)!=2) return -1;
            int a=describe_expression(&b,linitial(key->args)),z=describe_expression(&b,lsecond(key->args));
            if(a<0 || z<0 || p->exprs[a].op!=Q_COLUMN || p->exprs[z].op!=Q_COLUMN) return -1;
            unsigned width=list_length(b.left);
            if((unsigned)p->exprs[a].column>=width) { int swap=a; a=z; z=swap; }
            n->left_keys[n->nkeys]=p->exprs[a].column; n->right_keys[n->nkeys++]=p->exprs[z].column-width;
        }
        if(!n->nkeys || !describe_list(&b,list_concat(list_copy(((Join *)native)->joinqual),extra))) return -1;
    }
    if(n->kind==QP_AGGREGATE) {
        Agg *a=(Agg *)native; if(a->numCols>QS_KEYS) return -1;
        for(int i=0;i<a->numCols;++i) {
            TargetEntry *key=get_tle_by_resno(left->targetlist,a->grpColIdx[i]);
            int value=key?describe_expression(&b,(Node *)key->expr):-1;
            if(value<0 || p->exprs[value].op!=Q_COLUMN) return -1;
            p->keys[p->nkeys++]=value; n->left_keys[n->nkeys++]=p->exprs[value].column;
        }
        if(tree->nodes[n->left].kind!=QP_SORT && !tree->nodes[n->left].ordered_index) n->sorted_input=0;
    }
    ListCell *c; foreach(c,native->targetlist) {
        TargetEntry *entry=lfirst(c); Node *value=(Node *)entry->expr;
        if(p->nout==QS_OUT) return -1;
        int property=describe_expression(&b,value); if(property<0) return -1;
        p->outputs[p->nout++]=(struct qs_output){.index=property,.type_oid=exprType(value),.typmod=exprTypmod(value)};
    }
    if(!describe_list(&b,native->qual)) return -1;
    if(IsA(native,Result) && ((Result *)native)->resconstantqual) {
        Node *qual=((Result *)native)->resconstantqual;
        if(IsA(qual,List)?!describe_list(&b,(List *)qual):describe_expression(&b,qual)<0) return -1;
        p->predicate=0;
    }
    if(IsA(native,IndexScan)) {
        if(!describe_list(&b,((IndexScan *)native)->indexqualorig)) return -1;
        if(((IndexScan *)native)->indexqualorig) p->predicate=0;
        return ordered_heap_scan(&b,tree,id,((IndexScan *)native)->indexid,((IndexScan *)native)->indexorderdir,ordered,false);
    }
    if(IsA(native,IndexOnlyScan)) {
        IndexOnlyScan *index=(IndexOnlyScan *)native;
        if(!describe_list(&b,(List *)heap_index_var((Node *)index->recheckqual,index->indextlist))) return -1;
        if(index->recheckqual) p->predicate=0;
        return ordered_heap_scan(&b,tree,id,index->indexid,index->indexorderdir,ordered,true);
    }
    if(n->kind==QP_SORT) {
        Sort *sort=(Sort *)native; if(sort->numCols>QS_OUT) return -1;
        for(int i=0;i<sort->numCols;++i) {
            Oid family,type; int16 strategy;
            if(!get_ordering_op_properties(sort->sortOperators[i],&family,&type,&strategy)) return -1;
            p->order[p->norder++]=(struct qs_order){.output=sort->sortColIdx[i]-1,.descending=strategy==5,.nulls_first=sort->nullsFirst[i]};
        }
    }
    if(n->kind==QP_LIMIT) {
        Limit *limit=(Limit *)native;
        if(limit->limitCount) { if(!IsA(limit->limitCount,Const)) return -1; if(!((Const *)limit->limitCount)->constisnull) p->limit=DatumGetInt64(((Const *)limit->limitCount)->constvalue); }
        if(limit->limitOffset) { if(!IsA(limit->limitOffset,Const)) return -1; if(!((Const *)limit->limitOffset)->constisnull) p->offset=DatumGetInt64(((Const *)limit->limitOffset)->constvalue); }
    }
    unsigned *choices=pg_sina_candidate_choices();
    if(n->kind==QP_JOIN && !ordered && ((Join *)native)->inner_unique) {
        push_group_key_filter(tree,n);
        if(n->kind!=QP_JOIN) choices[id]|=PG_SINA_GROUP_FILTER;
    }
    if(n->kind==QP_AGGREGATE) {
        push_left_join_count(tree,n);
        if(n->kind!=QP_AGGREGATE) choices[id]|=PG_SINA_LEFT_COUNT;
        int left=n->left; bypass_distinct_sort(tree,n);
        if(n->left!=left) choices[id]|=PG_SINA_DISTINCT_SORT;
        unsigned count=tree->count; choose_distinct(tree,n);
        if(tree->count!=count) choices[id]|=PG_SINA_DISTINCT;
    }
    return id;
}

Plan *pg_sina_candidate_scan(Plan *native,bool fast)
{
    CustomScan *scan=makeNode(CustomScan); scan->scan.plan=*native; NodeSetTag(scan,T_CustomScan);
    scan->scan.plan.lefttree=scan->scan.plan.righttree=NULL; scan->scan.plan.qual=NIL;
    scan->scan.plan.parallel_aware=scan->scan.plan.parallel_safe=false;
    scan->scan.plan.targetlist=(List *)device_output_expression((Node *)native->targetlist,NULL);
    scan->custom_scan_tlist=copyObject(scan->scan.plan.targetlist);
    scan->custom_private=list_make1(NULL); scan->methods=fast?&pg_sina_scan_methods:&pg_sina_plan_methods;
    return (Plan *)scan;
}

struct scan_parameters { PlannerInfo *root; List *values; };
/* Only parameters produced by a HOST InitPlan may cross this boundary.
 * NestLoop's per-row parameters are not an InitPlan snapshot. */
static bool scan_parameter(Node *node,struct scan_parameters *parameters)
{
    if(!node) return false;
    if(IsA(node,SubPlan) || IsA(node,AlternativeSubPlan)) return true;
    if(IsA(node,Param)) {
        Param *v=(Param *)node; bool producer=false; ListCell *c;
        if(v->paramkind!=PARAM_EXEC || v->paramid<0 || v->paramid>=QP_PARAMS) return true;
        switch(v->paramtype) {
        case INT2OID:case INT4OID:case INT8OID:case BOOLOID:case DATEOID:case NUMERICOID:break;
        default:return true;
        }
        for(PlannerInfo *root=parameters->root;root && !producer;root=root->parent_root)
            foreach(c,root->init_plans) {
                SubPlan *sub=lfirst(c);
                if(!sub->parParam && !sub->args && list_member_int(sub->setParam,v->paramid)) producer=true;
            }
        if(!producer) return true;
        foreach(c,parameters->values) if(((Param *)lfirst(c))->paramid==v->paramid) return false;
        parameters->values=lappend(parameters->values,copyObject(v)); return false;
    }
    return expression_tree_walker(node,scan_parameter,parameters);
}
Plan *pg_sina_compile_subtree(PlannerInfo *root,Plan *native,bool boundary)
{
    bool host_scan=pg_sina_exe_path==PG_SINA_FILTER;
    struct scan_parameters parameters={.root=root};
    if(host_scan) {
        if(!IsA(native,SeqScan) && !IsA(native,IndexScan) && !IsA(native,IndexOnlyScan)) return native;
        if(scan_parameter((Node *)native->qual,&parameters) || scan_parameter((Node *)native->targetlist,&parameters)) return native;
        if(IsA(native,IndexScan) && scan_parameter((Node *)((IndexScan *)native)->indexqualorig,&parameters)) return native;
        if(IsA(native,IndexOnlyScan) && scan_parameter((Node *)((IndexOnlyScan *)native)->indexqual,&parameters)) return native;
    }
    /* This allocation is only for a selected executable subtree. */
    struct qp_plan *tree=palloc(sizeof(*tree));
    memset(tree,0,offsetof(struct qp_plan,nodes));
    for(unsigned i=0;i<QP_PARAMS;++i) tree->param_nodes[i]=tree->cte_nodes[i]=-1;
    ListCell *c;
    foreach(c,parameters.values) {
        Param *v=lfirst(c); tree->param_nodes[v->paramid]=QP_HOST_PARAMETER;
        tree->nparams=Max(tree->nparams,(unsigned)v->paramid+1);
    }
    foreach(c,host_scan?NIL:root->init_plans) {
        SubPlan *sub=lfirst(c);
        if(sub->subLinkType==CTE_SUBLINK) continue;
        if(sub->subLinkType!=EXPR_SUBLINK || sub->parParam || sub->args || list_length(sub->setParam)!=1 || tree->ninit==QP_PARAMS) { goto failed; }
        int param=linitial_int(sub->setParam);
        if(param<0 || param>=QP_PARAMS) { goto failed; }
        int id=compile_tree(list_nth(root->glob->subroots,sub->plan_id-1),
            list_nth(root->glob->subplans,sub->plan_id-1),tree);
        if(id<0) { goto failed; }
        tree->param_nodes[param]=id; tree->nparams=Max(tree->nparams,(unsigned)param+1);
        unsigned init=tree->ninit++; tree->init_nodes[init]=id; tree->init_params[init]=param;
    }
    Plan *copy=copyObject(native);
    global_parallel_rows(copy,1);
    int result=-1;
    if(normalize_parallel_aggregate(&copy) && prune_columns(copy,NULL,true))
        result=compile_node(root,copy,tree,boundary || root->parse->sortClause!=NIL);
    if(result<0) { goto failed; }
    bool has_scan=false;
    for(unsigned i=0;i<tree->count;++i) {
        has_scan|=tree->nodes[i].kind==QP_SCAN;
        for(unsigned j=0;j<tree->nodes[i].expr.nexprs;++j) {
            struct qs_expr *e=&tree->nodes[i].expr.exprs[j];
            if(e->op==QP_PARAMETER && tree->param_nodes[e->column]<0 && tree->param_nodes[e->column]!=QP_HOST_PARAMETER) { goto failed; }
        }
    }
    if(!has_scan) { goto failed; }
    Oid ns=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(ns) || !OidIsValid(get_relname_relid("relations",ns))) { goto failed; }
    bool prepared=true; char device[128]={0};
    for(unsigned i=0;i<tree->count && prepared;++i) if(tree->nodes[i].kind==QP_SCAN) {
        char path[128]={0};
        prepared=pg_sina_prepared_device(tree->nodes[i].expr.relid,tree->nodes[i].expr.relfilenode,path,sizeof(path));
        if(prepared && !device[0]) strlcpy(device,path,sizeof(device));
    }
    if(!prepared) { goto failed; }
    tree->root=result;
    pg_sina_refine_groups(tree);
    pg_sina_read_resources(device);
    if(pg_sina_exe_path==PG_SINA_FILTER && native->parallel_aware &&
       (!IsA(native,SeqScan) || pg_sina_resources.cus<2)) { goto failed; }
    tree->nodes[result].expr.work_mem_bytes=pg_sina_query_memory();
    if(pg_sina_exe_path==PG_SINA_ADAPTIVE) {
        if(pg_sina_encoding_choice(tree->count)) elog(ERROR,"device encoder omitted a planned operator");
        for(unsigned i=0;i<tree->count;++i) {
            if(!pg_sina_encoding_matches(i,&tree->nodes[i])) elog(ERROR,"device operator %u differs from its selected physical choice",i);
        }
    }
    if(pg_sina_planning) ++pg_sina_planning->serialized_plans;
    size_t length=qp_plan_constant_bytes(tree);
    bytea *bytes=palloc(VARHDRSZ+length); SET_VARSIZE(bytes,VARHDRSZ+length);
    memcpy(VARDATA(bytes),tree,length); pfree(tree);
    Const *program=makeConst(BYTEAOID,-1,InvalidOid,-1,PointerGetDatum(bytes),false,false);
    CustomScan *scan=makeNode(CustomScan);
    scan->scan.plan=*native; NodeSetTag(scan,T_CustomScan);
    scan->scan.plan.lefttree=scan->scan.plan.righttree=NULL;
    scan->scan.plan.qual=NIL; scan->scan.plan.initPlan=NIL; scan->scan.plan.parallel_aware=false; scan->scan.plan.parallel_safe=false;
    if(pg_sina_exe_path==PG_SINA_FILTER) {
        scan->scan.plan.parallel_safe=native->parallel_safe;
        scan->scan.plan.parallel_aware=native->parallel_aware;
    }
    /* PG projects device results; it must not retain removed InitPlan deps. */
    scan->scan.plan.targetlist=(List *)device_output_expression((Node *)native->targetlist,NULL);
    scan->custom_scan_tlist=copyObject(scan->scan.plan.targetlist);
    scan->custom_exprs=parameters.values; /* PG tracks dependencies and evaluates InitPlans. */
    scan->custom_private=list_make1(program);
    scan->methods=&pg_sina_plan_methods;
    return (Plan *)scan;
failed:
    pfree(tree);
    return native;
}
Plan *pg_sina_describe_candidate(PlannerInfo *root,Plan *native,bool boundary,struct qp_plan **lowered)
{
    double mark=pg_sina_planner_clock();
    struct qp_plan *tree=pg_sina_cost_workspace();
    memset(pg_sina_candidate_choices(),0,QP_NODES*sizeof(unsigned));
    memset(tree,0,offsetof(struct qp_plan,nodes));
    for(unsigned i=0;i<QP_PARAMS;++i) tree->param_nodes[i]=tree->cte_nodes[i]=-1;
    ListCell *c;
    foreach(c,root->init_plans) {
        SubPlan *s=lfirst(c); if(s->subLinkType==CTE_SUBLINK) continue;
        if(s->subLinkType!=EXPR_SUBLINK || s->parParam || s->args || list_length(s->setParam)!=1 || tree->ninit==QP_PARAMS) return native;
        int param=linitial_int(s->setParam); if(param<0 || param>=QP_PARAMS) return native;
        PlannerInfo *subroot=list_nth(root->glob->subroots,s->plan_id-1);
        Plan *chosen=pg_sina_prepare_candidate(subroot,list_nth(root->glob->subplans,s->plan_id-1),false);
        int child=chosen?describe_tree(subroot,chosen,tree,false):-1; if(child<0) return native;
        tree->param_nodes[param]=child; tree->nparams=Max(tree->nparams,(unsigned)param+1);
        unsigned init=tree->ninit++; tree->init_nodes[init]=child; tree->init_params[init]=param;
    }
    int result=describe_tree(root,native,tree,boundary || root->parse->sortClause!=NIL);
    if(pg_sina_planning) pg_sina_planning->describe+=pg_sina_planner_clock()-mark;
    if(result<0) return native;
    mark=pg_sina_planner_clock();
    bool has_scan=false; char device[128]={0};
    Oid ns=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(ns) || !OidIsValid(get_relname_relid("relations",ns))) return native;
    for(unsigned i=0;i<tree->count;++i) if(tree->nodes[i].kind==QP_SCAN) {
        struct qs_plan *scan=&tree->nodes[i].expr; char path[128]={0}; has_scan=true;
        if(!pg_sina_prepared_device(scan->relid,scan->relfilenode,path,sizeof(path))) return native;
        if(!device[0]) strlcpy(device,path,sizeof(device));
    }
    if(!has_scan) return native;
    if(pg_sina_planning) pg_sina_planning->mapping+=pg_sina_planner_clock()-mark;
    tree->root=result; mark=pg_sina_planner_clock();
    pg_sina_refine_groups(tree);
    if(pg_sina_planning) pg_sina_planning->refine+=pg_sina_planner_clock()-mark;
    pg_sina_read_resources(device);
    for(unsigned i=0;i<tree->count;++i) {
        const struct qp_node *n=&tree->nodes[i];
        pg_sina_candidate_choices()[i]|=pg_sina_physical_choice(n);
    }
    tree->nodes[result].expr.work_mem_bytes=pg_sina_query_memory();
    *lowered=tree; return pg_sina_candidate_scan(native,false);
}
Plan *pg_sina_rewrite_plan(PlannerInfo *root,Plan *native)
{ return pg_sina_compile_subtree(root,native,false); }

/* Planner-only wrapper, removed after setrefs if conversion is rejected. */
static const CustomScanMethods native_methods={.CustomName="SniaNativePlan"};
Plan *pg_sina_native_wrapper(Plan *native)
{
    CustomScan *scan=makeNode(CustomScan);
    scan->scan.plan=*native; NodeSetTag(scan,T_CustomScan);
    scan->scan.plan.lefttree=scan->scan.plan.righttree=NULL;
    scan->scan.plan.qual=NIL; scan->scan.plan.initPlan=NIL;
    scan->custom_scan_tlist=copyObject(native->targetlist);
    scan->custom_plans=list_make1(native); scan->methods=&native_methods;
    return (Plan *)scan;
}
Plan *pg_sina_native_plan(Plan *plan)
{
    if(!plan) return NULL;
    if(IsA(plan,CustomScan) && ((CustomScan *)plan)->methods==&native_methods) {
        Plan *native=linitial(((CustomScan *)plan)->custom_plans);
        native->initPlan=list_concat(plan->initPlan,native->initPlan);
        native->extParam=bms_union(native->extParam,plan->extParam);
        native->allParam=bms_union(native->allParam,plan->allParam);
        return pg_sina_native_plan(native);
    }
    if(pg_sina_previewing_path) {
        plan->lefttree=pg_sina_native_plan(plan->lefttree);
        plan->righttree=pg_sina_native_plan(plan->righttree);
        if(IsA(plan,SubqueryScan)) ((SubqueryScan *)plan)->subplan=pg_sina_native_plan(((SubqueryScan *)plan)->subplan);
    }
    return plan;
}
static Plan *rewrite_selected(PlannerInfo *root,RelOptInfo *rel,CustomPath *path,List *tlist,List *clauses,List *plans)
{
    Plan *native=linitial(plans);
    Plan *volatile compiled=NULL;
    pg_sina_compiling_plan=true;
    PG_TRY();
    {
        if(pg_sina_exe_path==PG_SINA_FILTER || pg_sina_exe_path==PG_SINA_ADAPTIVE)
            compiled=pg_sina_adaptive_plan(root,native);
        else {
            compiled=pg_sina_fast_plan(root,native);
            if(!compiled) compiled=pg_sina_rewrite_plan(root,native);
            if(compiled==native) compiled=pg_sina_adaptive_plan(root,native);
            if(compiled!=native) root->init_plans=NIL;
        }
    }
    PG_CATCH();
    { pg_sina_compiling_plan=false; PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_compiling_plan=false;
    if(compiled!=native) {
        if(pg_sina_exe_path==PG_SINA_ADAPTIVE) {
            path->path.startup_cost=compiled->startup_cost;
            path->path.total_cost=compiled->total_cost;
        }
        return compiled;
    }
    CustomScan *fallback=makeNode(CustomScan);
    fallback->scan.plan.targetlist=tlist;
    fallback->custom_scan_tlist=copyObject(native->targetlist);
    fallback->custom_plans=list_make1(native);
    fallback->methods=&native_methods;
    return (Plan *)fallback;
}
static const CustomPathMethods selected_methods={.CustomName="SniaSelectedPlan",.PlanCustomPath=rewrite_selected};
void pg_sina_upper_paths(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    Query *q=root->parse;
    if(pg_sina_exe_path==PG_SINA_HOST || root->parent_root || stage!=UPPERREL_FINAL || q->commandType!=CMD_SELECT ||
       q->hasRowSecurity || q->hasTargetSRFs || q->rowMarks || q->windowClause || q->groupingSets ||
       q->setOperations || root->append_rel_list) return;
    Path *selected=NULL; ListCell *c;
    foreach(c,output->pathlist) {
        Path *candidate=lfirst(c);
        if(!selected || candidate->total_cost<selected->total_cost) selected=candidate;
    }
    if(!selected) return;
    CustomPath *path=makeNode(CustomPath); path->path=*selected;
    NodeSetTag(path,T_CustomPath); path->path.pathtype=T_CustomScan;
    path->path.parallel_aware=path->path.parallel_safe=false;
    path->path.startup_cost=path->path.total_cost=0;
    path->custom_paths=list_make1(selected); path->methods=&selected_methods;
    /* add_path may pfree the dominated path, which is our own child here. */
    output->pathlist=list_make1(path);
}
