/* Compile PG expression trees; unsupported semantics retain native PG paths. */
#include "pg_internal.h"
#include "access/table.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include <errno.h>
#include <locale.h>
static bool builtin(Oid function,const char *name)
{
    char *actual=get_func_name(function);
    return get_func_namespace(function)==PG_CATALOG_NAMESPACE && actual && !strcmp(actual,name);
}
/* For single ASCII bpchar(1), C.UTF-8 and C.utf8 use the same byte order. */
static bool byte_collation(Oid collation)
{
    if(lc_collate_is_c(collation)) return true;
    const char *locale=setlocale(LC_COLLATE,NULL);
    return collation==DEFAULT_COLLATION_OID && locale &&
        (!strcmp(locale,"C.UTF-8") || !strcmp(locale,"C.utf8"));
}
static int put(struct qs_plan *p,struct qs_expr e)
{
    for(unsigned i=0;i<p->nexprs;++i) {
        const struct qs_expr *x=&p->exprs[i];
        if(x->op!=e.op || x->type!=e.type || x->scale!=e.scale) continue;
        if((e.op==Q_COLUMN || e.op==Q_AGGREF) && x->column==e.column) return i;
        if(e.op==Q_CONST && x->isnull==e.isnull && x->dscale==e.dscale && x->constant==e.constant) return i;
        if(e.op!=Q_COLUMN && e.op!=Q_AGGREF && e.op!=Q_CONST && x->left==e.left && x->right==e.right &&
            (e.op!=Q_CASE || x->otherwise==e.otherwise)) return i;
    }
    if(p->nexprs==QS_EXPRS) return -1;
    p->exprs[p->nexprs]=e; return p->nexprs++;
}
static bool decimal_constant(Const *c,struct qs_expr *out)
{
    char *text=DatumGetCString(DirectFunctionCall1(numeric_out,c->constvalue)),digits[80];
    unsigned n=0,scale=0; bool point=false;
    for(char *s=text;*s;++s) {
        if(*s=='.' && !point) { point=true; continue; }
        if((*s<'0'||*s>'9') && !(s==text && *s=='-')) return false;
        if(n+1>=sizeof(digits)) return false;
        digits[n++]=*s; if(point) ++scale;
    }
    if(scale>6) return false;
    digits[n]=0; char *end; errno=0; long long value=strtoll(digits,&end,10);
    if(errno || *end || end==digits) return false;
    out->constant=value; out->scale=out->dscale=scale; return true;
}
static int var_column(Var *v,Index rti,const struct qs_plan *p)
{
    unsigned probe=p->nattrs-p->join.nattrs;
    if(v->varlevelsup || v->varattno<=0) return -1;
    if(v->varno==rti && (unsigned)v->varattno<=probe) return v->varattno-1;
    if(p->join.relid && v->varno==p->join.rti && (unsigned)v->varattno<=p->join.nattrs)
        return probe+v->varattno-1;
    return -1;
}
static Node *unlabel(Node *node)
{ while(node && IsA(node,RelabelType)) node=(Node *)((RelabelType *)node)->arg; return node; }
/* Encode only equality against literals. Unknown non-NULL strings share ID 0;
 * that ID must never be used for raw string output or string/string equality. */
static int string_comparison(Node *left,Node *right,Oid function,Oid collation,Index rti,struct qs_plan *p)
{
    bool equal=builtin(function,"texteq") || builtin(function,"bpchareq");
    bool unequal=builtin(function,"textne") || builtin(function,"bpcharne");
    left=unlabel(left); right=unlabel(right);
    if(IsA(left,Const) && IsA(right,Var)) { Node *swap=left; left=right; right=swap; }
    if((!equal && !unequal) || !IsA(left,Var) || !IsA(right,Const) || !byte_collation(collation)) return -1;
    int col=var_column((Var *)left,rti,p); if(col<0) return -1;
    struct qs_column *c=&p->cols[col]; Const *literal=(Const *)right;
    if((c->type!=Q_STRING && c->type!=Q_DICTIONARY) ||
        (literal->consttype!=TEXTOID && literal->consttype!=VARCHAROID && literal->consttype!=BPCHAROID)) return -1;
    /* A bpchar-to-text cast changes trailing-space semantics; keep it native. */
    if(c->trim_spaces!=builtin(function,equal?"bpchareq":"bpcharne")) return -1;
    unsigned id=0;
    if(!literal->constisnull) {
        text *value=DatumGetTextPP(literal->constvalue); size_t len=VARSIZE_ANY_EXHDR(value);
        const char *bytes=VARDATA_ANY(value);
        if(c->trim_spaces) while(len && bytes[len-1]==' ') --len;
        if(len>=sizeof(p->strings[0].bytes) || memchr(bytes,0,len)) return -1;
        for(size_t i=0;i<len;++i) if((unsigned char)bytes[i]>=128) return -1;
        for(;id<p->nstrings;++id) if(p->strings[id].column==col && p->strings[id].length==len && !memcmp(p->strings[id].bytes,bytes,len)) break;
        if(id==p->nstrings) {
            if(id==QS_STRINGS) return -1;
            p->strings[id]=(struct qs_string){.column=col,.length=len}; memcpy(p->strings[id].bytes,bytes,len); ++p->nstrings;
        }
        ++id;
    }
    c->type=Q_DICTIONARY; if(c->slot<0) c->slot=p->ncols++;
    int a=put(p,(struct qs_expr){.op=Q_COLUMN,.column=col,.type=Q_DICTIONARY});
    int b=put(p,(struct qs_expr){.op=Q_CONST,.constant=id,.type=Q_DICTIONARY,.isnull=literal->constisnull});
    return a<0 || b<0?-1:put(p,(struct qs_expr){.op=equal?Q_EQ:Q_NE,.type=Q_BOOLEAN,.left=a,.right=b});
}
/* A top-level equality or IN restriction proves that surviving string values
 * belong to the dictionary. An equality hidden inside OR/CASE does not. */
static void restrict_string(Node *node,Index rti,struct qs_plan *p)
{
    Node *left=NULL,*right=NULL; Oid fn=InvalidOid;
    if(IsA(node,ScalarArrayOpExpr)) {
        ScalarArrayOpExpr *a=(ScalarArrayOpExpr *)node; if(!a->useOr || list_length(a->args)!=2) return;
        fn=get_opcode(a->opno); left=unlabel(linitial(a->args)); right=unlabel(lsecond(a->args));
    } else if(IsA(node,OpExpr)) {
        OpExpr *o=(OpExpr *)node; if(list_length(o->args)!=2) return;
        fn=get_opcode(o->opno); left=unlabel(linitial(o->args)); right=unlabel(lsecond(o->args));
        if(IsA(left,Const)) { Node *swap=left; left=right; right=swap; }
    }
    if(left && IsA(left,Var) && IsA(right,Const) && (builtin(fn,"texteq") || builtin(fn,"bpchareq"))) {
        int col=var_column((Var *)left,rti,p);
        if(col>=0 && p->cols[col].type==Q_DICTIONARY) p->cols[col].restricted=1;
    }
}
static int string_order(const void *a,const void *b)
{
    const struct qs_string *x=a,*y=b;
    if(x->column!=y->column) return (int)x->column-y->column;
    int cmp=memcmp(x->bytes,y->bytes,Min(x->length,y->length));
    return cmp?cmp:(int)x->length-y->length;
}
static void order_strings(struct qs_plan *p)
{
    struct qs_string original[QS_STRINGS]; memcpy(original,p->strings,sizeof(original));
    qsort(p->strings,p->nstrings,sizeof(p->strings[0]),string_order);
    for(unsigned i=0;i<p->nexprs;++i) {
        struct qs_expr *e=&p->exprs[i];
        if(e->op!=Q_CONST || e->type!=Q_DICTIONARY || e->isnull) continue;
        const struct qs_string *s=&original[e->constant-1];
        for(unsigned j=0;j<p->nstrings;++j) if(!string_order(s,&p->strings[j])) { e->constant=j+1; break; }
    }
}
static int expression(Node *node,Index rti,struct qs_plan *p)
{
    while(node && IsA(node,RelabelType)) node=(Node *)((RelabelType *)node)->arg;
    if(!node) return -1;
    struct qs_expr e={.left=-1,.right=-1,.column=-1,.otherwise=-1};
    if(IsA(node,Var)) {
        Var *v=(Var *)node;
        int col=var_column(v,rti,p); if(col<0) return -1;
        struct qs_column *c=&p->cols[col];
        if(c->type==Q_STRING || c->type==Q_PREFIX || (c->type==Q_DICTIONARY && !c->restricted)) return -1;
        if(c->type==255 || (c->type==Q_CHAR && !byte_collation(v->varcollid))) return -1;
        if(c->slot<0) c->slot=p->ncols++;
        e.op=Q_COLUMN; e.column=col; e.type=c->type; e.scale=e.dscale=c->scale;
        return put(p,e);
    }
    if(IsA(node,Const)) {
        Const *c=(Const *)node; e.op=Q_CONST; e.isnull=c->constisnull;
        switch(c->consttype) {
        case NUMERICOID:e.type=Q_DECIMAL; if(!e.isnull && !decimal_constant(c,&e)) return -1;break;
        case INT2OID:e.type=Q_INTEGER;if(!e.isnull)e.constant=DatumGetInt16(c->constvalue);break;
        case INT4OID:e.type=Q_INTEGER;if(!e.isnull)e.constant=DatumGetInt32(c->constvalue);break;
        case INT8OID:e.type=Q_INTEGER;if(!e.isnull)e.constant=DatumGetInt64(c->constvalue);break;
        case BOOLOID:e.type=Q_BOOLEAN;if(!e.isnull)e.constant=DatumGetBool(c->constvalue);break;
        case DATEOID:e.type=Q_DATE;if(!e.isnull) { e.constant=DatumGetDateADT(c->constvalue); if(DATE_NOT_FINITE(e.constant)) return -1; } break;
        case TIMESTAMPOID:
            e.type=Q_DATE;
            if(!e.isnull) { int64 value=DatumGetInt64(c->constvalue); if(value%USECS_PER_DAY || value/USECS_PER_DAY<=PG_INT32_MIN || value/USECS_PER_DAY>=PG_INT32_MAX) return -1; e.constant=value/USECS_PER_DAY; } break;
        case BPCHAROID:
            e.type=Q_CHAR;
            if(!byte_collation(c->constcollid)) return -1;
            if(!e.isnull) { char *value=TextDatumGetCString(c->constvalue); if(strlen(value)>1 || (unsigned char)value[0]>=128) return -1; e.constant=value[0]?value[0]:' '; } break;
        default:return -1;
        }
        return put(p,e);
    }
    if(IsA(node,NullTest)) {
        NullTest *test=(NullTest *)node; if(test->argisrow) return -1;
        e.op=test->nulltesttype==IS_NULL?Q_IS_NULL:Q_IS_NOT_NULL; e.type=Q_BOOLEAN;
        e.left=expression((Node *)test->arg,rti,p); return e.left<0?-1:put(p,e);
    }
    if(IsA(node,BoolExpr)) {
        BoolExpr *b=(BoolExpr *)node; ListCell *cell; int left=-1;
        e.type=Q_BOOLEAN; e.op=b->boolop==AND_EXPR?Q_AND:b->boolop==OR_EXPR?Q_OR:Q_NOT;
        foreach(cell,b->args) {
            int right=expression(lfirst(cell),rti,p); if(right<0) return -1;
            if(left<0) left=right;
            else { e.left=left; e.right=right; left=put(p,e); if(left<0) return -1; }
        }
        if(b->boolop!=NOT_EXPR) return left;
        if(list_length(b->args)!=1) return -1;
        e.left=left; return put(p,e);
    }
    if(IsA(node,CaseExpr)) {
        CaseExpr *c=(CaseExpr *)node; if(c->arg) return -1;
        int result=expression((Node *)c->defresult,rti,p); if(result<0) return -1;
        for(int i=list_length(c->args)-1;i>=0;--i) {
            CaseWhen *w=list_nth_node(CaseWhen,c->args,i);
            int condition=expression((Node *)w->expr,rti,p),value=expression((Node *)w->result,rti,p);
            if(condition<0 || value<0 || p->exprs[condition].type!=Q_BOOLEAN) return -1;
            if(p->exprs[value].type==Q_DICTIONARY || p->exprs[result].type==Q_DICTIONARY) return -1;
            unsigned scale=Max(p->exprs[value].scale,p->exprs[result].scale);
            if(scale>12) return -1;
            e=(struct qs_expr){.op=Q_CASE,.left=condition,.right=value,.otherwise=result,
                .type=p->exprs[value].type,.scale=scale};
            result=put(p,e); if(result<0) return -1;
        }
        return result;
    }
    if(IsA(node,ScalarArrayOpExpr)) {
        ScalarArrayOpExpr *s=(ScalarArrayOpExpr *)node;
        if(!s->useOr || list_length(s->args)!=2 || !IsA(lsecond(s->args),Const)) return -1;
        Const *array=lsecond_node(Const,s->args); if(array->constisnull) return -1;
        ArrayType *data=DatumGetArrayTypeP(array->constvalue); Oid type=ARR_ELEMTYPE(data);
        if(type!=TEXTOID && type!=VARCHAROID && type!=BPCHAROID && type!=INT2OID && type!=INT4OID && type!=INT8OID && type!=NUMERICOID) return -1;
        int16 length; bool byval; char align; Datum *values; bool *nulls; int count;
        get_typlenbyvalalign(type,&length,&byval,&align); deconstruct_array(data,type,length,byval,align,&values,&nulls,&count);
        if(!count || count>QS_STRINGS) return -1;
        int result=-1;
        for(int i=0;i<count;++i) {
            Const *literal=makeConst(type,-1,s->inputcollid,length,values[i],nulls[i],byval);
            OpExpr *comparison=makeNode(OpExpr);
            comparison->opno=s->opno; comparison->opfuncid=get_opcode(s->opno);
            comparison->opresulttype=BOOLOID; comparison->inputcollid=s->inputcollid;
            comparison->args=list_make2(linitial(s->args),literal);
            int next=expression((Node *)comparison,rti,p);
            if(next<0) return -1;
            result=result<0?next:put(p,(struct qs_expr){.op=Q_OR,.type=Q_BOOLEAN,.left=result,.right=next});
            if(result<0) return -1;
        }
        return result;
    }
    if(!IsA(node,OpExpr)) return -1;
    OpExpr *op=(OpExpr *)node;
    if(list_length(op->args)!=2) return -1;
    Oid function=OidIsValid(op->opfuncid)?op->opfuncid:get_opcode(op->opno);
    int comparison=string_comparison(linitial(op->args),lsecond(op->args),function,op->inputcollid,rti,p);
    if(comparison>=0) return comparison;
    if(builtin(function,"textlike")) {
        Node *left=linitial(op->args); while(IsA(left,RelabelType)) left=(Node *)((RelabelType *)left)->arg;
        Node *right=lsecond(op->args);
        if(!IsA(left,Var) || !IsA(right,Const) || !byte_collation(op->inputcollid)) return -1;
        Var *v=(Var *)left; Const *pattern=(Const *)right;
        if((v->vartype!=TEXTOID && v->vartype!=VARCHAROID) || pattern->consttype!=TEXTOID || pattern->constisnull) return -1;
        int col=var_column(v,rti,p); if(col<0) return -1;
        char *pattern_text=TextDatumGetCString(pattern->constvalue); size_t len=strlen(pattern_text);
        if(!len || len>sizeof(p->cols[col].prefix) || pattern_text[len-1]!='%') return -1;
        for(size_t i=0;i+1<len;++i) if(pattern_text[i]=='%' || pattern_text[i]=='_' || pattern_text[i]=='\\' || (unsigned char)pattern_text[i]>=128) return -1;
        struct qs_column *spec=&p->cols[col];
        if(spec->type!=Q_STRING && spec->type!=Q_PREFIX) return -1;
        if(spec->type==Q_PREFIX && (spec->prefix_len!=len-1 || memcmp(spec->prefix,pattern_text,len-1))) return -1;
        spec->type=Q_PREFIX; spec->prefix_len=len-1; memcpy(spec->prefix,pattern_text,len-1);
        if(spec->slot<0) spec->slot=p->ncols++;
        return put(p,(struct qs_expr){.op=Q_COLUMN,.column=col,.type=Q_BOOLEAN});
    }
    e.left=expression(linitial(op->args),rti,p); e.right=expression(lsecond(op->args),rti,p);
    if(e.left<0 || e.right<0) return -1;
    if(p->exprs[e.left].type==Q_DICTIONARY || p->exprs[e.right].type==Q_DICTIONARY) return -1;
    if(builtin(function,"numeric_add")) e.op=Q_ADD;
    else if(builtin(function,"numeric_sub")) e.op=Q_SUB;
    else if(builtin(function,"numeric_mul")) e.op=Q_MUL;
    else {
        static const char *families[]={"numeric_","int2","int4","int8","int24","int28","int42","int48","int82","int84","date_","bpchar","bool"};
        static const char *suffixes[]={"eq","ne","lt","le","gt","ge"};
        bool found=false;
        for(unsigned i=0;i<lengthof(families);++i) for(unsigned j=0;j<lengthof(suffixes);++j) {
            char name[64]; snprintf(name,sizeof(name),"%s%s",families[i],suffixes[j]);
            if(builtin(function,name)) { e.op=Q_EQ+j; found=true; }
            snprintf(name,sizeof(name),"date_%s_timestamp",suffixes[j]);
            if(builtin(function,name)) { e.op=Q_EQ+j; found=true; }
        }
        if(!found) return -1;
        e.type=Q_BOOLEAN;
        if((p->exprs[e.left].type==Q_CHAR || p->exprs[e.right].type==Q_CHAR) && !byte_collation(op->inputcollid)) return -1;
        return put(p,e);
    }
    e.type=Q_DECIMAL;
    unsigned a=p->exprs[e.left].scale,b=p->exprs[e.right].scale;
    e.scale=e.op==Q_MUL?a+b:Max(a,b); if(e.scale>12) return -1;
    return put(p,e);
}
static int aggregate_expr(Aggref *agg,Index rti,struct qs_plan *p)
{
    if(p->naggs==QS_AGGS || agg->aggfilter || agg->aggdistinct || agg->aggorder || agg->agglevelsup || agg->aggvariadic) return -1;
    struct qs_agg *a=&p->aggs[p->naggs]; a->expr=-1;
    if(builtin(agg->aggfnoid,"sum")) a->kind=Q_SUM;
    else if(builtin(agg->aggfnoid,"avg")) a->kind=Q_AVG;
    else if(builtin(agg->aggfnoid,"count")) a->kind=agg->aggstar?Q_COUNT_STAR:Q_COUNT;
    else return -1;
    if(a->kind!=Q_COUNT_STAR) {
        if(list_length(agg->args)!=1) return -1;
        a->expr=expression((Node *)linitial_node(TargetEntry,agg->args)->expr,rti,p); if(a->expr<0) return -1;
        if((a->kind==Q_SUM || a->kind==Q_AVG) && p->exprs[a->expr].type!=Q_DECIMAL && p->exprs[a->expr].type!=Q_INTEGER) return -1;
    }
    return p->naggs++;
}
static int final_expression(Node *node,Index rti,struct qs_plan *p)
{
    while(node && IsA(node,RelabelType)) node=(Node *)((RelabelType *)node)->arg;
    if(!node) return -1;
    if(IsA(node,Const)) return expression(node,rti,p);
    if(IsA(node,Aggref)) {
        int index=aggregate_expr((Aggref *)node,rti,p); if(index<0) return -1;
        struct qs_agg *a=&p->aggs[index];
        return put(p,(struct qs_expr){.op=Q_AGGREF,.column=index,
            .type=exprType(node)==NUMERICOID?Q_DECIMAL:Q_INTEGER,
            .scale=a->kind==Q_SUM?p->exprs[a->expr].scale:0});
    }
    if(!IsA(node,OpExpr)) return -1;
    OpExpr *op=(OpExpr *)node; if(list_length(op->args)!=2) return -1;
    Oid function=OidIsValid(op->opfuncid)?op->opfuncid:get_opcode(op->opno);
    unsigned kind;
    if(builtin(function,"numeric_add")) kind=Q_ADD;
    else if(builtin(function,"numeric_sub")) kind=Q_SUB;
    else if(builtin(function,"numeric_mul")) kind=Q_MUL;
    else if(builtin(function,"numeric_div")) kind=Q_DIV;
    else return -1;
    int left=final_expression(linitial(op->args),rti,p),right=final_expression(lsecond(op->args),rti,p);
    if(left<0 || right<0) return -1;
    return put(p,(struct qs_expr){.op=kind,.left=left,.right=right,.type=Q_DECIMAL});
}
static bool limit_value(Node *node,int64 *value)
{
    if(!node) return true;
    if(!IsA(node,Const)) return false;
    Const *c=(Const *)node; if(c->constisnull) return true;
    if(c->consttype==INT8OID) *value=DatumGetInt64(c->constvalue);
    else if(c->consttype==INT4OID) *value=DatumGetInt32(c->constvalue);
    else return false;
    return *value>=0;
}
static bool describe(PlannerInfo *root,Index rti,struct qs_plan *p,int build)
{
    RangeTblEntry *rte=rt_fetch(rti,root->parse->rtable); Relation table=table_open(rte->relid,NoLock);
    TupleDesc desc=RelationGetDescr(table); unsigned base=p->nattrs;
    if(!desc->natts || base+desc->natts>QS_COLS) { table_close(table,NoLock); return false; }
    if(build) {
        p->join.relid=rte->relid; p->join.relfilenode=table->rd_node.relNode;
        p->join.rti=rti; p->join.nattrs=desc->natts;
    } else { p->relid=rte->relid; p->relfilenode=table->rd_node.relNode; }
    for(unsigned i=0;i<(unsigned)desc->natts;++i) {
        Form_pg_attribute a=TupleDescAttr(desc,i); struct qs_column *c=&p->cols[base+i];
        c->slot=-1; c->length=a->attlen; c->align=a->attalign=='d'?8:a->attalign=='i'?4:a->attalign=='s'?2:1; c->type=255;
        if(a->attisdropped || (a->attlen<0 && a->attlen!=-1)) { table_close(table,NoLock); return false; }
        if(a->atttypid==NUMERICOID) { c->type=Q_DECIMAL; c->scale=2; }
        else if(a->atttypid==INT2OID || a->atttypid==INT4OID || a->atttypid==INT8OID) c->type=Q_INTEGER;
        else if(a->atttypid==DATEOID) c->type=Q_DATE;
        else if(a->atttypid==TEXTOID || a->atttypid==VARCHAROID || a->atttypid==BPCHAROID) { c->type=Q_STRING; c->trim_spaces=a->atttypid==BPCHAROID; }
        else if(a->atttypid==BOOLOID) c->type=Q_BOOLEAN;
        if(a->atttypid==BPCHAROID && a->atttypmod==5 && byte_collation(a->attcollation)) c->type=Q_CHAR;
    }
    p->nattrs+=desc->natts; table_close(table,NoLock); return true;
}
static bool compile(PlannerInfo *root,Index rti,RelOptInfo *rel,Index build_rti,RelOptInfo *build_rel,OpExpr *join,List *quals,struct qs_plan *p)
{
    if(!describe(root,rti,p,0) || (build_rti && !describe(root,build_rti,p,1))) return false;
    p->join.predicate=-1;
    if(join) {
        int left=expression(linitial(join->args),rti,p),right=expression(lsecond(join->args),rti,p);
        if(left<0 || right<0 || p->exprs[left].op!=Q_COLUMN || p->exprs[right].op!=Q_COLUMN ||
            p->exprs[left].type!=Q_INTEGER || p->exprs[right].type!=Q_INTEGER) return false;
        int a=p->exprs[left].column,b=p->exprs[right].column; unsigned probe=p->nattrs-p->join.nattrs;
        if((unsigned)a>=probe && (unsigned)b<probe) { int swap=a; a=b; b=swap; }
        if((unsigned)a>=probe || (unsigned)b<probe) return false;
        p->join.probe_key=a; p->join.build_key=b; p->join.rows=Max(build_rel->rows,1);
    }
    p->ordered_rows=!root->parse->hasAggs;
    ListCell *cell; p->predicate=-1; p->limit=-1;
    foreach(cell,rel->baserestrictinfo) {
        RestrictInfo *r=lfirst_node(RestrictInfo,cell); int next=expression((Node *)r->clause,rti,p); if(next<0) return false;
        restrict_string((Node *)r->clause,rti,p);
        if(p->predicate<0) p->predicate=next;
        else { p->predicate=put(p,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=p->predicate,.right=next}); if(p->predicate<0) return false; }
    }
    if(build_rel) foreach(cell,build_rel->baserestrictinfo) {
        RestrictInfo *r=lfirst_node(RestrictInfo,cell); int next=expression((Node *)r->clause,rti,p); if(next<0) return false;
        restrict_string((Node *)r->clause,rti,p);
        if(p->join.predicate<0) p->join.predicate=next;
        else { p->join.predicate=put(p,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=p->join.predicate,.right=next}); if(p->join.predicate<0) return false; }
    }
    /* PG pushes single-relation predicates into baserestrictinfo. Keep every
     * remaining mixed predicate after the hash equality, including OR trees. */
    if(join) foreach(cell,quals) {
        Node *node=lfirst(cell); if(node==(Node *)join) continue;
        Bitmapset *vars=pull_varnos(node); bool mixed=bms_num_members(vars)>1; bms_free(vars);
        if(!mixed) continue;
        int next=expression(node,rti,p); if(next<0) return false;
        if(!p->join.residual) p->join.residual=next+1;
        else {
            int combined=put(p,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=p->join.residual-1,.right=next});
            if(combined<0) return false;
            p->join.residual=combined+1;
        }
    }
    /* Columns assigned slots so far are needed by WHERE, including nested
     * arithmetic/OR/NULL tests. Later slots are only used after filtering. */
    foreach(cell,root->parse->groupClause) {
        if(p->nkeys==QS_KEYS) return false;
        TargetEntry *entry=get_sortgroupclause_tle(lfirst(cell),root->processed_tlist);
        int key=expression((Node *)entry->expr,rti,p); if(key<0 || p->exprs[key].op!=Q_COLUMN) return false;
        p->keys[p->nkeys++]=key;
    }
    foreach(cell,root->processed_tlist) {
        TargetEntry *entry=lfirst_node(TargetEntry,cell); if(p->nout==QS_OUT) return false;
        struct qs_output *out=&p->outputs[p->nout++]; out->type_oid=exprType((Node *)entry->expr); out->typmod=exprTypmod((Node *)entry->expr);
        /* Timestamp constants are accepted as midnight date bounds above,
         * but are not a supported result Datum. Reject at planning time. */
        if(out->type_oid!=NUMERICOID && out->type_oid!=INT2OID && out->type_oid!=INT4OID &&
            out->type_oid!=INT8OID && out->type_oid!=BOOLOID && out->type_oid!=DATEOID && out->type_oid!=BPCHAROID &&
            out->type_oid!=TEXTOID && out->type_oid!=VARCHAROID) return false;
        if(p->ordered_rows) {
            out->index=expression((Node *)entry->expr,rti,p);
            if(out->index<0) return false;
        } else if(IsA(entry->expr,Aggref)) {
            int index=aggregate_expr((Aggref *)entry->expr,rti,p); if(index<0) return false;
            out->aggregate=1; out->index=index;
        } else if(!p->nkeys) {
            out->index=final_expression((Node *)entry->expr,rti,p); out->aggregate=2;
            if(out->index<0) return false;
        } else {
            unsigned i=0; ListCell *key;
            foreach(key,root->parse->groupClause) {
                TargetEntry *group=get_sortgroupclause_tle(lfirst(key),root->processed_tlist);
                if(equal(entry->expr,group->expr)) break;
                ++i;
            }
            if(i==p->nkeys) return false;
            out->index=i;
        }
    }
    foreach(cell,root->parse->sortClause) {
        if(p->norder==QS_OUT) return false;
        SortGroupClause *sort=lfirst(cell); TargetEntry *entry=get_sortgroupclause_tle(sort,root->processed_tlist);
        unsigned index=0; ListCell *t; foreach(t,root->processed_tlist) { if(lfirst(t)==entry) break; ++index; }
        if(index==p->nout) return false;
        Oid function=get_opcode(sort->sortop); bool descending=false,found=false;
        static const char *less[]={"numeric_lt","int2lt","int4lt","int8lt","date_lt","bpcharlt","boollt","text_lt"};
        static const char *greater[]={"numeric_gt","int2gt","int4gt","int8gt","date_gt","bpchargt","boolgt","text_gt"};
        for(unsigned i=0;i<lengthof(less);++i) {
            if(builtin(function,less[i])) found=true;
            if(builtin(function,greater[i])) found=descending=true;
        }
        if(!found) return false;
        p->order[p->norder++]=(struct qs_order){index,descending,sort->nulls_first};
    }
    /* Late decoding pays off for selective filters. Keep the eager path when
     * PG estimates at least half the rows survive (e.g. Q1). Predicate
     * numerics are also lazy: AND/OR pass only needed rows to their children. */
    if(p->predicate>=0 && rel->rows<rel->tuples/2)
        for(unsigned i=0;i<p->nattrs-p->join.nattrs;++i) {
            struct qs_column *c=&p->cols[i];
            c->deferred=c->type==Q_DECIMAL && c->slot>=0;
        }
    if(build_rel && p->join.predicate>=0 && build_rel->rows<build_rel->tuples/2)
        for(unsigned i=p->nattrs-p->join.nattrs;i<p->nattrs;++i)
            p->cols[i].deferred=p->cols[i].type==Q_DECIMAL && p->cols[i].slot>=0;
    order_strings(p);
    return limit_value(root->parse->limitCount,&p->limit) && limit_value(root->parse->limitOffset,&p->offset);
}
static Plan *plan_path(PlannerInfo *root,RelOptInfo *rel,CustomPath *path,List *tlist,List *clauses,List *plans)
{
    CustomScan *scan=makeNode(CustomScan); scan->scan.plan.targetlist=tlist; scan->scan.scanrelid=0;
    scan->custom_scan_tlist=copyObject(root->processed_tlist);
    scan->custom_private=copyObject(path->custom_private);
    scan->methods=root->parse->hasAggs?&pg_sina_scan_methods:&pg_sina_order_methods;
    return (Plan *)scan;
}
static const CustomPathMethods path_methods={.CustomName="SniaCsdAggregate",.PlanCustomPath=plan_path};
static const CustomPathMethods order_path_methods={.CustomName="SniaCsdSort",.PlanCustomPath=plan_path};
static bool join_inputs(Node *node,List **relations,List **quals)
{
    if(IsA(node,RangeTblRef)) { *relations=lappend_int(*relations,((RangeTblRef *)node)->rtindex); return list_length(*relations)<=2; }
    if(IsA(node,FromExpr)) {
        FromExpr *f=(FromExpr *)node; ListCell *cell;
        foreach(cell,f->fromlist) if(!join_inputs(lfirst(cell),relations,quals)) return false;
        if(f->quals) *quals=list_concat(*quals,IsA(f->quals,List)?list_copy((List *)f->quals):make_ands_implicit((Expr *)f->quals));
        return true;
    }
    if(IsA(node,JoinExpr)) {
        JoinExpr *j=(JoinExpr *)node;
        if(j->jointype!=JOIN_INNER || j->isNatural || j->usingClause || j->alias) return false;
        if(j->quals) *quals=list_concat(*quals,IsA(j->quals,List)?list_copy((List *)j->quals):make_ands_implicit((Expr *)j->quals));
        return join_inputs(j->larg,relations,quals) && join_inputs(j->rarg,relations,quals);
    }
    return false;
}
static OpExpr *equality_join(List *quals,Index a,Index b)
{
    OpExpr *join=NULL; ListCell *cell;
    foreach(cell,quals) {
        Node *node=lfirst(cell); Bitmapset *vars=pull_varnos(node);
        int count=bms_num_members(vars); bool mixed=bms_is_member(a,vars) && bms_is_member(b,vars); bms_free(vars);
        if(!count) return NULL; /* Keep constant-gated joins wholly native. */
        if(!mixed) { if(count!=1) return NULL; continue; }
        if(join || !IsA(node,OpExpr)) continue;
        OpExpr *op=(OpExpr *)node; if(list_length(op->args)!=2) continue;
        Oid fn=OidIsValid(op->opfuncid)?op->opfuncid:get_opcode(op->opno);
        if(!builtin(fn,"int2eq") && !builtin(fn,"int4eq") && !builtin(fn,"int8eq")) continue;
        Node *left=unlabel(linitial(op->args)),*right=unlabel(lsecond(op->args));
        if(!IsA(left,Var) || !IsA(right,Var) || ((Var *)left)->varno==((Var *)right)->varno) continue;
        join=op;
    }
    return join;
}
static double build_bytes(const struct qs_plan *p,double rows)
{
    unsigned columns=0;
    for(unsigned i=p->nattrs-p->join.nattrs;i<p->nattrs;++i) columns+=p->cols[i].slot>=0;
    /* Current hash records: next/NULL header, 8-byte values, display scales,
     * alignment and two uint32 bucket entries per capacity row. */
    unsigned row=(16+columns*9+7)&~7U;
    double capacity=256; while(capacity<rows) capacity*=2;
    return capacity*(row+8);
}
void pg_sina_upper_paths(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    Query *q=root->parse;
    /* A query owns the namespace until its streaming source ends. Until mixed
     * plans share one device session, nested custom scans could wait on their
     * own outer scan's lock. Keep unsupported outer queries wholly native. */
    if(!pg_sina_exe_path || root->parent_root || stage!=UPPERREL_FINAL || q->commandType!=CMD_SELECT || q->havingQual ||
        (!q->hasAggs && (!q->sortClause || q->groupClause)) ||
        q->windowClause || q->groupingSets || q->cteList || q->hasSubLinks || q->hasRowSecurity || q->hasTargetSRFs ||
        q->rowMarks || q->distinctClause || q->setOperations || root->append_rel_list || root->join_info_list) return;
    List *relations=NIL,*quals=NIL;
    if(!join_inputs((Node *)q->jointree,&relations,&quals) || !relations || list_length(relations)>2) return;
    Index rti=linitial_int(relations),build_rti=0; RelOptInfo *build_rel=NULL; OpExpr *join=NULL;
    RangeTblEntry *rte=rt_fetch(rti,q->rtable); if(rte->rtekind!=RTE_RELATION || rte->tablesample) return;
    RelOptInfo *rel=root->simple_rel_array[rti]; if(!rel) return;
    if(list_length(relations)==2) {
        if(!q->hasAggs) return;
        build_rti=lsecond_int(relations); RangeTblEntry *build_rte=rt_fetch(build_rti,q->rtable);
        if(build_rte->rtekind!=RTE_RELATION || build_rte->tablesample) return;
        build_rel=root->simple_rel_array[build_rti]; if(!build_rel) return;
        if(rel->tuples<build_rel->tuples) { Index swap=rti; rti=build_rti; build_rti=swap; RelOptInfo *other=rel; rel=build_rel; build_rel=other; }
        join=equality_join(quals,rti,build_rti); if(!join) return;
    }
    struct qs_plan plan={0}; if(!compile(root,rti,rel,build_rti,build_rel,join,quals,&plan)) return;
    /* Preserve the cheap raw-size choice when it fits. If it would spill,
     * compare filtered projected records on the other side as well. Estimates
     * only choose orientation; bounded batches still guarantee correctness. */
    if(build_rel && build_bytes(&plan,build_rel->rows)>(double)pg_sina_max_memory*1024/2) {
        struct qs_plan other={0};
        if(compile(root,build_rti,build_rel,rti,rel,join,quals,&other) && build_bytes(&other,rel->rows)<build_bytes(&plan,build_rel->rows)) {
            plan=other; Index swap=rti; rti=build_rti; build_rti=swap;
            RelOptInfo *swap_rel=rel; rel=build_rel; build_rel=swap_rel;
        }
    }
    plan.external_aggregate=plan.nkeys!=0;
    if(plan.external_aggregate && plan.nkeys+plan.naggs>QS_OUT) return;
    Oid nsp=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(nsp) || !OidIsValid(get_relname_relid("relations",nsp))) return;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID,OIDOID,OIDOID};
    Datum values[]={ObjectIdGetDatum(plan.relid),ObjectIdGetDatum(plan.relfilenode),ObjectIdGetDatum(plan.join.relid),ObjectIdGetDatum(plan.join.relfilenode)};
    const char *sql=plan.join.relid?
        "SELECT 1 FROM sina_csd.relations r WHERE r.relid=$1 AND r.relfilenode=$2 AND EXISTS (SELECT 1 FROM sina_csd.relations b WHERE b.relid=$3 AND b.relfilenode=$4 AND b.device=r.device AND b.namespace_id=r.namespace_id AND b.lba_bytes=r.lba_bytes)":
        "SELECT 1 FROM sina_csd.relations WHERE relid=$1 AND relfilenode=$2";
    int status=SPI_execute_with_args(sql,plan.join.relid?4:2,types,values,NULL,true,1);
    bool prepared=status==SPI_OK_SELECT && SPI_processed==1; SPI_finish(); if(!prepared) return;
    bytea *data=palloc(VARHDRSZ+sizeof(plan)); SET_VARSIZE(data,VARHDRSZ+sizeof(plan)); memcpy(VARDATA(data),&plan,sizeof(plan));
    CustomPath *path=makeNode(CustomPath); path->path.pathtype=T_CustomScan; path->path.parent=output;
    path->path.pathtarget=create_pathtarget(root,root->processed_tlist);
    path->path.rows=plan.ordered_rows || plan.external_aggregate?rel->rows:plan.nkeys?4:1;
    path->path.pathkeys=root->sort_pathkeys; path->path.startup_cost=path->path.total_cost=0;
    path->custom_private=list_make1(makeConst(BYTEAOID,-1,InvalidOid,-1,PointerGetDatum(data),false,false));
    path->methods=plan.ordered_rows?&order_path_methods:&path_methods; add_path(output,&path->path);
}
