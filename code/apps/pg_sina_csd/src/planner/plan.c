/* Lower the selected PG Plan, without choosing another join order. */
#include "pg_internal.h"
#include "access/table.h"
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
    List *left, *right, *aggregates;
    Index scan;
    struct qp_node *node;
    struct qp_plan *tree;
};
static int compile_node(PlannerInfo *,Plan *,struct qp_plan *,bool);
static int compile_tree(PlannerInfo *,Plan *,struct qp_plan *);
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
            char *s=DatumGetCString(DirectFunctionCall1(numeric_out,c->constvalue)),digits[64];
            unsigned n=0,scale=0; bool point=false;
            for(char *at=s;*at;++at) {
                if(*at=='.' && !point) { point=true; continue; }
                if(n+1==sizeof(digits) || ((*at<'0'||*at>'9') && !(at==s && *at=='-'))) return -1;
                digits[n++]=*at; if(point) ++scale;
            }
            if(scale>18) return -1;
            digits[n]=0; char *end; errno=0; e.constant=strtoll(digits,&end,10);
            if(errno || *end || end==digits) return -1;
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
static bool index_metadata(Oid indexid,Oid heap,int attnum,struct qs_join *j)
{
    Oid ns=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(ns) || !OidIsValid(get_relname_relid("indexes",ns))) return false;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID,INT2OID};
    Datum values[]={ObjectIdGetDatum(indexid),ObjectIdGetDatum(heap),Int16GetDatum(attnum)};
    int status=SPI_execute_with_args("SELECT r.relfilenode,i.root,i.key_bytes FROM sina_csd.indexes i JOIN sina_csd.relations r ON r.relid=i.indexid WHERE i.indexid=$1 AND i.heaprelid=$2 AND i.attnum=$3",3,types,values,NULL,true,1);
    bool found=status==SPI_OK_SELECT && SPI_processed==1;
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
    SPI_finish(); return found;
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
static bool normalize_parallel_aggregate(Plan *plan)
{
    if(!plan) return true;
    if(IsA(plan,Agg) && DO_AGGSPLIT_COMBINE(((Agg *)plan)->aggsplit)) {
        Agg *agg=(Agg *)plan; Plan *partial=partial_input(plan->lefttree);
        if(!partial) return false;
        for(int i=0;i<agg->numCols;++i) {
            TargetEntry *old=get_tle_by_resno(plan->lefttree->targetlist,agg->grpColIdx[i]);
            TargetEntry *next=old?matching_entry(partial->lefttree->targetlist,old->expr):NULL;
            if(!next) return false;
            agg->grpColIdx[i]=next->resno;
        }
        plan->lefttree=partial->lefttree; agg->aggsplit=AGGSPLIT_SIMPLE;
        agg->aggstrategy=((Agg *)partial)->aggstrategy;
    }
    if(IsA(plan,SubqueryScan) && !normalize_parallel_aggregate(((SubqueryScan *)plan)->subplan)) return false;
    return normalize_parallel_aggregate(plan->lefttree) && normalize_parallel_aggregate(plan->righttree);
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
    Plan *copy=copyObject(native);
    if(!normalize_parallel_aggregate(copy) || !prune_columns(copy,NULL,true)) return -1;
    return compile_node(root,copy,tree,root->parse->sortClause!=NIL);
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
/* A full index path may supply ordering without a Sort node. Reading its
 * immutable heap is valid only if we explicitly retain that ordering. */
static bool group_output(const struct qs_plan *p,int index,int key)
{
    if(index<0 || (unsigned)index>=p->nexprs) return false;
    const struct qs_expr *x=&p->exprs[index];
    if(x->op==Q_CONST) return true;
    if(x->op==Q_COLUMN) return x->column==key;
    if(x->op==Q_AGGREF) return x->column==0;
    return (x->op==Q_ADD || x->op==Q_SUB || x->op==Q_MUL || x->op==Q_DIV) &&
        group_output(p,x->left,key) && group_output(p,x->right,key);
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
        bool covered=index_only && key>=0 && key<(int)b->node->expr.nattrs;
        for(unsigned i=0;i<b->node->expr.nattrs;++i)
            if(b->node->expr.cols[i].slot>=0 && (int)i!=key) covered=false;
        if(covered) {
            b->node->ordered_index|=4;
            b->node->expr.join.build_key=key+1;
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
    struct qp_node *sort=&tree->nodes[out]; sort->kind=QP_SORT; sort->left=id; sort->right=-1;
    sort->expr.predicate=-1; sort->expr.limit=-1;
    struct qs_plan *scan=&b->node->expr;
    for(int k=0;k<index->rd_index->indnkeyatts;++k) {
        AttrNumber att=index->rd_index->indkey.values[k];
        if(att<=0 || (unsigned)att>scan->nattrs) { relation_close(index,NoLock); return -1; }
        Oid type,collation; int32 mod;
        get_atttypetypmodcoll(scan->relid,att,&type,&mod,&collation);
        Var *v=makeVar(b->scan,att,type,mod,collation,0);
        int expr=expression(b,(Node *)v); if(expr<0) { relation_close(index,NoLock); return -1; }
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
static int compile_node(PlannerInfo *root,Plan *native,struct qp_plan *tree,bool ordered)
{
    if(!native || tree->count==QP_NODES || native->initPlan) return -1;
    unsigned id=tree->count++;
    struct qp_node *n=&tree->nodes[id]; n->left=n->right=-1; n->join_qual=-1;
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
        case JOIN_SEMI:n->join_type=QP_SEMI;break;
        case JOIN_ANTI:n->join_type=QP_ANTI;break;
        default:return -1;
        }
        if(IsA(right,Hash)) right=right->lefttree;
        if(IsA(native,NestLoop) && prepare_index_join(root,(NestLoop *)native,&right,&index_extra,&index_lookup,&access))
            { n->kind=QP_INDEX_JOIN; n->sorted_input=ordered; }
        else {
            if(IsA(native,NestLoop) && ordered) return -1;
            left_order=false;
        }
    } else if(IsA(native,Agg)) {
        Agg *a=(Agg *)native; if(a->chain || a->groupingSets) return -1;
        n->kind=QP_AGGREGATE; n->sorted_input=a->aggstrategy==AGG_SORTED;
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
            struct qp_node *m=&tree->nodes[materialized]; m->kind=QP_MATERIALIZED; m->left=child; m->right=-1; m->expr=tree->nodes[child].expr;
        }
        n->left=materialized;
    } else if(IsA(native,Result) || IsA(native,Gather) || IsA(native,GatherMerge) || IsA(native,Material) || IsA(native,Hash))
        n->kind=QP_PROJECT;
    else return -1;
    if(left && n->left<0) { n->left=compile_node(root,left,tree,left_order); if(n->left<0) return -1; }
    if(right) { n->right=compile_node(root,right,tree,right_order); if(n->right<0) return -1; }
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
    /* Keep PG HashAggregate, or remove a grouping-only Sort when no parent
     * needs its order. The same bounded states consume projected/join rows. */
    if(n->kind==QP_AGGREGATE && !ordered && n->nkeys==1 && p->naggs==1 && !n->distinct && !n->distinct_aggs &&
       p->predicate<0 && p->having<0 && n->left>=0 && (tree->nodes[n->left].kind==QP_SORT ||
        (IsA(native,Agg) && ((Agg *)native)->aggstrategy==AGG_HASHED))) {
        bool remove_sort=tree->nodes[n->left].kind==QP_SORT;
        int input=remove_sort?tree->nodes[n->left].left:n->left,key=n->left_keys[0];
        unsigned kind=p->aggs[0].kind;
        bool supported=input>=0 && (!remove_sort || tree->nodes[input].kind==QP_JOIN ||
            (tree->nodes[input].kind==QP_SCAN && !tree->nodes[input].ordered_index)) &&
            p->exprs[p->keys[0]].type==Q_INTEGER &&
            (kind==Q_COUNT_STAR || ((kind==Q_SUM || kind==Q_AVG || kind==Q_COUNT) && p->aggs[0].expr>=0 &&
             p->exprs[p->aggs[0].expr].op==Q_COLUMN));
        const struct qs_plan *sorted=&tree->nodes[n->left].expr;
        if(remove_sort && input>=0) {
            if(sorted->nout!=tree->nodes[input].expr.nout) supported=false;
            for(unsigned i=0;i<sorted->nout;++i) {
                const struct qs_expr *x=&sorted->exprs[sorted->outputs[i].index];
                if(x->op!=Q_COLUMN || x->column!=(int)i) supported=false;
            }
        }
        for(unsigned i=0;i<p->nout;++i) {
            if(!group_output(p,p->outputs[i].index,key)) supported=false;
        }
        if(supported) { n->left=input; n->sorted_input=0; n->hash=1; }
    }
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
    return id;
}

Plan *pg_sina_rewrite_plan(PlannerInfo *root,Plan *native)
{
    struct qp_plan *tree=palloc0(sizeof(*tree));
    for(unsigned i=0;i<QP_PARAMS;++i) tree->param_nodes[i]=tree->cte_nodes[i]=-1;
    ListCell *c;
    foreach(c,root->init_plans) {
        SubPlan *sub=lfirst(c);
        if(sub->subLinkType==CTE_SUBLINK) continue;
        if(sub->subLinkType!=EXPR_SUBLINK || sub->parParam || sub->args || list_length(sub->setParam)!=1 || tree->ninit==QP_PARAMS) { pfree(tree); return native; }
        int param=linitial_int(sub->setParam);
        if(param<0 || param>=QP_PARAMS) { pfree(tree); return native; }
        int id=compile_tree(list_nth(root->glob->subroots,sub->plan_id-1),
            list_nth(root->glob->subplans,sub->plan_id-1),tree);
        if(id<0) { pfree(tree); return native; }
        tree->param_nodes[param]=id; tree->nparams=Max(tree->nparams,(unsigned)param+1);
        unsigned init=tree->ninit++; tree->init_nodes[init]=id; tree->init_params[init]=param;
    }
    int result=compile_tree(root,native,tree);
    if(result<0) { pfree(tree); return native; }
    bool has_scan=false;
    for(unsigned i=0;i<tree->count;++i) {
        has_scan|=tree->nodes[i].kind==QP_SCAN;
        for(unsigned j=0;j<tree->nodes[i].expr.nexprs;++j) {
            struct qs_expr *e=&tree->nodes[i].expr.exprs[j];
            if(e->op==QP_PARAMETER && tree->param_nodes[e->column]<0) { pfree(tree); return native; }
        }
    }
    if(!has_scan) { pfree(tree); return native; }
    Oid ns=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(ns) || !OidIsValid(get_relname_relid("relations",ns))) { pfree(tree); return native; }
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    bool prepared=true;
    for(unsigned i=0;i<tree->count && prepared;++i) if(tree->nodes[i].kind==QP_SCAN) {
        Oid types[]={OIDOID,OIDOID};
        Datum values[]={ObjectIdGetDatum(tree->nodes[i].expr.relid),ObjectIdGetDatum(tree->nodes[i].expr.relfilenode)};
        int status=SPI_execute_with_args("SELECT 1 FROM sina_csd.relations WHERE relid=$1 AND relfilenode=$2",2,types,values,NULL,true,1);
        prepared=status==SPI_OK_SELECT && SPI_processed==1;
    }
    SPI_finish();
    if(!prepared) { pfree(tree); return native; }
    root->init_plans=NIL; /* Scalar and CTE initializers now run on the device. */
    tree->root=result;
    bytea *bytes=palloc(VARHDRSZ+sizeof(*tree)); SET_VARSIZE(bytes,VARHDRSZ+sizeof(*tree)); memcpy(VARDATA(bytes),tree,sizeof(*tree)); pfree(tree);
    CustomScan *scan=makeNode(CustomScan);
    scan->scan.plan=*native; NodeSetTag(scan,T_CustomScan);
    scan->scan.plan.lefttree=scan->scan.plan.righttree=NULL;
    scan->scan.plan.qual=NIL; scan->scan.plan.initPlan=NIL; scan->scan.plan.parallel_aware=false; scan->scan.plan.parallel_safe=false;
    scan->custom_scan_tlist=copyObject(native->targetlist);
    scan->custom_private=list_make1(makeConst(BYTEAOID,-1,InvalidOid,-1,PointerGetDatum(bytes),false,false));
    scan->methods=&pg_sina_plan_methods;
    return (Plan *)scan;
}

/* Planner-only wrapper, removed after setrefs if conversion is rejected. */
static const CustomScanMethods native_methods={.CustomName="SniaNativePlan"};
Plan *pg_sina_native_plan(Plan *plan)
{
    if(IsA(plan,CustomScan) && ((CustomScan *)plan)->methods==&native_methods) {
        Plan *native=linitial(((CustomScan *)plan)->custom_plans);
        native->initPlan=list_concat(plan->initPlan,native->initPlan);
        native->extParam=bms_union(native->extParam,plan->extParam);
        native->allParam=bms_union(native->allParam,plan->allParam);
        return native;
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
        compiled=pg_sina_fast_plan(root,native);
        if(!compiled) compiled=pg_sina_rewrite_plan(root,native);
    }
    PG_CATCH();
    { pg_sina_compiling_plan=false; PG_RE_THROW(); }
    PG_END_TRY();
    pg_sina_compiling_plan=false;
    if(compiled!=native) return compiled;
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
    if(pg_sina_exe_path!=1 || root->parent_root || stage!=UPPERREL_FINAL || q->commandType!=CMD_SELECT ||
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
