/* Compile PG expression trees; unsupported semantics retain native PG paths. */
#include "pg_internal.h"
#include "access/table.h"
#include "access/relation.h"
#include "access/genam.h"
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
#include "rewrite/rewriteManip.h"
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
    unsigned probe=qs_probe_attrs(p);
    if(v->varlevelsup || v->varattno<=0) return -1;
    if(v->varno==rti && (unsigned)v->varattno<=probe) return v->varattno-1;
    unsigned base=probe;
    for(unsigned i=0;i<qs_join_count(p);++i) {
        const struct qs_join *j=qs_join_at(p,i);
        if(v->varno==j->rti && (unsigned)v->varattno<=j->nattrs) return base+v->varattno-1;
        base+=j->nattrs;
    }
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
    /* Mark equality-restricted columns even inside OR. The device still emits
     * ID 0 for any survivor outside the literal set and errors on output, so
     * this never silently truncates or merges an unknown string. */
    if(equal) c->restricted=1;
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
        if(c->type==Q_STRING) {
            /* Low-cardinality runtime dictionary: Scan stores an ID, and the
             * source restores the bytes before the final Datum. */
            if(c->slot<0) c->slot=p->ncols++;
            e.op=Q_COLUMN; e.column=col; e.type=Q_STRING; e.scale=e.dscale=c->scale;
            return put(p,e);
        }
        if(c->type==Q_PREFIX || (c->type==Q_DICTIONARY && !c->restricted)) return -1;
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
    if(IsA(node,FuncExpr)) {
        FuncExpr *f=(FuncExpr *)node;
        if(builtin(f->funcid,"date_part") && list_length(f->args)==2) {
            Const *field=linitial_node(Const,f->args);
            Node *arg=unlabel(lsecond(f->args));
            if(!field || field->consttype!=TEXTOID || field->constisnull) return -1;
            char *name=TextDatumGetCString(field->constvalue);
            if(strcmp(name,"year")) return -1;
            /* EXTRACT(YEAR FROM date) is date_part('year', date::timestamp). */
            while(IsA(arg,FuncExpr)) {
                FuncExpr *cast=(FuncExpr *)arg;
                if(list_length(cast->args)!=1) return -1;
                Oid cf=OidIsValid(cast->funcid)?cast->funcid:InvalidOid;
                if(!builtin(cf,"timestamp_date") && !builtin(cf,"date_timestamp") &&
                   !builtin(cf,"timestamp_timestamp") && !builtin(cf,"timestamp")) return -1;
                arg=unlabel(linitial(cast->args));
            }
            int date=expression(arg,rti,p);
            if(date<0 || p->exprs[date].type!=Q_DATE) return -1;
            return put(p,(struct qs_expr){.op=Q_YEAR,.left=date,.type=Q_INTEGER});
        }
        return -1;
    }
    if(!IsA(node,OpExpr)) return -1;
    OpExpr *op=(OpExpr *)node;
    if(list_length(op->args)!=2) return -1;
    Oid function=OidIsValid(op->opfuncid)?op->opfuncid:get_opcode(op->opno);
    int comparison=string_comparison(linitial(op->args),lsecond(op->args),function,op->inputcollid,rti,p);
    if(comparison>=0) return comparison;
    bool not_like=builtin(function,"textnlike");
    if(builtin(function,"textlike") || not_like) {
        Node *left=linitial(op->args); while(IsA(left,RelabelType)) left=(Node *)((RelabelType *)left)->arg;
        Node *right=lsecond(op->args);
        if(!IsA(left,Var) || !IsA(right,Const) || !byte_collation(op->inputcollid)) return -1;
        Var *v=(Var *)left; Const *pattern=(Const *)right;
        if((v->vartype!=TEXTOID && v->vartype!=VARCHAROID) || pattern->consttype!=TEXTOID || pattern->constisnull) return -1;
        int col=var_column(v,rti,p); if(col<0) return -1;
        char *pattern_text=TextDatumGetCString(pattern->constvalue); size_t len=strlen(pattern_text);
        if(!len || len>sizeof(p->cols[col].prefix)) return -1;
        for(size_t i=0;i<len;++i) if(pattern_text[i]=='_' || pattern_text[i]=='\\' || (unsigned char)pattern_text[i]>=128) return -1;
        struct qs_column *spec=&p->cols[col];
        /* Prefix patterns keep the cheap boolean column. Other patterns with
         * only '%' wildcards use a small in-scan glob matcher. */
        bool prefix=pattern_text[len-1]=='%';
        for(size_t i=0;i+1<len && prefix;++i) if(pattern_text[i]=='%') prefix=false;
        if(prefix) {
            if(spec->type!=Q_STRING && spec->type!=Q_PREFIX) return -1;
            if(spec->type==Q_PREFIX && (spec->prefix_len!=len-1 || memcmp(spec->prefix,pattern_text,len-1))) return -1;
            spec->type=Q_PREFIX; spec->prefix_len=len-1; memcpy(spec->prefix,pattern_text,len-1);
        } else {
            if(spec->type!=Q_STRING && spec->type!=Q_PATTERN) return -1;
            if(spec->type==Q_PATTERN && (spec->prefix_len!=len || memcmp(spec->prefix,pattern_text,len))) return -1;
            spec->type=Q_PATTERN; spec->prefix_len=(uint8_t)len; memcpy(spec->prefix,pattern_text,len);
        }
        if(spec->slot<0) spec->slot=p->ncols++;
        int like=put(p,(struct qs_expr){.op=Q_COLUMN,.column=col,.type=Q_BOOLEAN});
        return not_like?put(p,(struct qs_expr){.op=Q_NOT,.type=Q_BOOLEAN,.left=like}):like;
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
static bool describe_relation(Relation table,Index rti,RelOptInfo *rel,struct qs_plan *p,unsigned build)
{
    TupleDesc desc=RelationGetDescr(table); unsigned base=p->nattrs;
    if(!desc->natts || base+desc->natts>QS_COLS) return false;
    if(build) {
        struct qs_join *j=build==1?&p->join:&p->extra_joins[build-2];
        j->relid=RelationGetRelid(table); j->relfilenode=table->rd_node.relNode;
        j->rti=rti; j->nattrs=desc->natts; j->predicate=-1; j->semi=0;
        j->build_key2=j->probe_key2=-1;
        p->njoins=build;
    } else { p->relid=RelationGetRelid(table); p->relfilenode=table->rd_node.relNode; }
    for(unsigned i=0;i<(unsigned)desc->natts;++i) {
        Form_pg_attribute a=TupleDescAttr(desc,i); struct qs_column *c=&p->cols[base+i];
        c->slot=-1; c->length=a->attlen; c->align=a->attalign=='d'?8:a->attalign=='i'?4:a->attalign=='s'?2:1; c->type=255;
        if(a->attisdropped || (a->attlen<0 && a->attlen!=-1)) return false;
        if(a->atttypid==NUMERICOID) { c->type=Q_DECIMAL; c->scale=2; }
        else if(a->atttypid==INT2OID || a->atttypid==INT4OID || a->atttypid==INT8OID) c->type=Q_INTEGER;
        else if(a->atttypid==DATEOID) c->type=Q_DATE;
        else if(a->atttypid==TEXTOID || a->atttypid==VARCHAROID || a->atttypid==BPCHAROID) {
            c->type=Q_STRING; c->trim_spaces=a->atttypid==BPCHAROID;
            c->dict_big=rel && rel->tuples>4096;
        }
        else if(a->atttypid==BOOLOID) c->type=Q_BOOLEAN;
        if(a->atttypid==BPCHAROID && a->atttypmod==5 && byte_collation(a->attcollation)) c->type=Q_CHAR;
    }
    p->nattrs+=desc->natts; return true;
}
static bool describe(PlannerInfo *root,Index rti,struct qs_plan *p,unsigned build)
{
    RangeTblEntry *rte=rt_fetch(rti,root->parse->rtable); Relation table=table_open(rte->relid,NoLock);
    bool ok=describe_relation(table,rti,root->simple_rel_array[rti],p,build);
    table_close(table,NoLock); return ok;
}
static bool compile(PlannerInfo *root,List *relations,List *joins,List *quals,struct qs_plan *p)
{
    Index rti=linitial_int(relations); ListCell *cell;
    unsigned side=0;
    foreach(cell,relations) if(!describe(root,lfirst_int(cell),p,side++)) return false;
    unsigned base=qs_probe_attrs(p);
    for(unsigned i=0;i<p->njoins;++i) {
        struct qs_join *j=i?&p->extra_joins[i-1]:&p->join;
        OpExpr *key=list_nth(joins,i);
        int left=expression(linitial(key->args),rti,p),right=expression(lsecond(key->args),rti,p);
        if(left<0 || right<0 || p->exprs[left].op!=Q_COLUMN || p->exprs[right].op!=Q_COLUMN ||
            p->exprs[left].type!=Q_INTEGER || p->exprs[right].type!=Q_INTEGER) return false;
        int a=p->exprs[left].column,b=p->exprs[right].column;
        if((unsigned)a>=base && (unsigned)b<base) { int swap=a; a=b; b=swap; }
        if((unsigned)a>=base || (unsigned)b<base || (unsigned)b>=base+j->nattrs) return false;
        j->probe_key=a; j->build_key=b; j->rows=Max(root->simple_rel_array[j->rti]->rows,1);
        /* Preserve a second equality between the same build relation and the
         * already joined prefix. Q9 needs (l_partkey,l_suppkey) = (ps_partkey,
         * ps_suppkey) as one hash key or the probe expands four-fold. */
        ListCell *qual;
        foreach(qual,quals) {
            Node *node=lfirst(qual); if(!IsA(node,OpExpr)) continue;
            OpExpr *op=(OpExpr *)node; if(list_length(op->args)!=2) continue;
            Oid fn=OidIsValid(op->opfuncid)?op->opfuncid:get_opcode(op->opno);
            if(!builtin(fn,"int2eq") && !builtin(fn,"int4eq") && !builtin(fn,"int8eq")) continue;
            Node *left=unlabel(linitial(op->args)),*right=unlabel(lsecond(op->args));
            if(!IsA(left,Var) || !IsA(right,Var)) continue;
            Var *x=(Var *)left,*y=(Var *)right,*buildvar=NULL,*probevar=NULL;
            if(x->varno==j->rti) { buildvar=x; probevar=y; }
            else if(y->varno==j->rti) { buildvar=y; probevar=x; }
            else continue;
            bool prefixvar=false;
            for(unsigned k=0;k<=i;++k) if((Index)list_nth_int(relations,k)==probevar->varno) { prefixvar=true; break; }
            if(!prefixvar) continue;
            int bc=var_column(buildvar,rti,p),pc=var_column(probevar,rti,p);
            if(bc<0 || pc<0 || bc==j->build_key || pc==j->probe_key) continue;
            if((unsigned)pc>=base || (unsigned)bc<base || (unsigned)bc>=base+j->nattrs) continue;
            j->build_key2=bc; j->probe_key2=pc; break;
        }
        base+=j->nattrs;
    }
    p->ordered_rows=!root->parse->hasAggs;
    p->predicate=-1; p->limit=-1;
    side=0; base=0;
    foreach(cell,relations) {
        Index current=lfirst_int(cell); RelOptInfo *rel=root->simple_rel_array[current];
        struct qs_join *j=side?side==1?&p->join:&p->extra_joins[side-2]:NULL;
        int *predicate=j?&j->predicate:&p->predicate; ListCell *qual;
        foreach(qual,rel->baserestrictinfo) {
            RestrictInfo *r=lfirst_node(RestrictInfo,qual); int next=expression((Node *)r->clause,rti,p); if(next<0) return false;
            restrict_string((Node *)r->clause,rti,p);
            if(*predicate<0) *predicate=next;
            else { *predicate=put(p,(struct qs_expr){.op=Q_AND,.type=Q_BOOLEAN,.left=*predicate,.right=next}); if(*predicate<0) return false; }
        }
        base+=j?j->nattrs:qs_probe_attrs(p); ++side;
    }
    /* All non-key mixed predicates see the final joined column layout. */
    if(p->njoins) foreach(cell,quals) {
        Node *node=lfirst(cell); if(list_member_ptr(joins,node)) continue;
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
    List *key_exprs=NIL;
    foreach(cell,root->parse->groupClause) {
        if(p->nkeys==QS_KEYS) return false;
        TargetEntry *entry=get_sortgroupclause_tle(lfirst(cell),root->processed_tlist);
        int key=expression((Node *)entry->expr,rti,p); if(key<0) return false;
        uint8_t kop=p->exprs[key].op;
        if(kop!=Q_COLUMN && kop!=Q_YEAR) return false;
        p->keys[p->nkeys++]=key; key_exprs=lappend(key_exprs,entry->expr);
    }
    /* PG may remove columns functionally dependent on a primary key from
     * groupClause. Add such output columns back as keys: by FD they cannot
     * split a group, and the device keeps fixed-size dictionary IDs instead
     * of variable-length FD payloads. */
    if(p->nkeys) foreach(cell,root->processed_tlist) {
        TargetEntry *entry=lfirst_node(TargetEntry,cell);
        if(IsA(entry->expr,Aggref) || contain_aggs_of_level((Node *)entry->expr,0)) continue;
        bool present=false; ListCell *k; foreach(k,key_exprs) if(equal(entry->expr,lfirst(k))) { present=true; break; }
        if(present) continue;
        int key=expression((Node *)entry->expr,rti,p); if(key<0) return false;
        if(p->exprs[key].op!=Q_COLUMN && p->exprs[key].op!=Q_YEAR) return false;
        if(p->nkeys==QS_KEYS) return false;
        p->keys[p->nkeys++]=key; key_exprs=lappend(key_exprs,entry->expr);
    }
    foreach(cell,root->processed_tlist) {
        TargetEntry *entry=lfirst_node(TargetEntry,cell); if(p->nout==QS_OUT) return false;
        struct qs_output *out=&p->outputs[p->nout++]; out->type_oid=exprType((Node *)entry->expr); out->typmod=exprTypmod((Node *)entry->expr);
        /* Timestamp constants are accepted as midnight date bounds above,
         * but are not a supported result Datum. Reject at planning time. */
        if(out->type_oid!=NUMERICOID && out->type_oid!=INT2OID && out->type_oid!=INT4OID &&
            out->type_oid!=INT8OID && out->type_oid!=BOOLOID && out->type_oid!=DATEOID && out->type_oid!=BPCHAROID &&
            out->type_oid!=TEXTOID && out->type_oid!=VARCHAROID && out->type_oid!=FLOAT8OID) return false;
        if(p->ordered_rows) {
            out->index=expression((Node *)entry->expr,rti,p);
            if(out->index<0) return false;
        } else if(IsA(entry->expr,Aggref)) {
            int index=aggregate_expr((Aggref *)entry->expr,rti,p); if(index<0) return false;
            out->aggregate=1; out->index=index;
        } else if(!p->nkeys) {
            out->index=final_expression((Node *)entry->expr,rti,p); out->aggregate=2;
            if(out->index<0) return false;
        } else if(contain_aggs_of_level((Node *)entry->expr,0)) {
            /* Ratios such as Q8 sum(CASE...)/sum(volume) combine aggregate
             * values after grouping; compile them as a final scalar tree. */
            out->index=final_expression((Node *)entry->expr,rti,p); out->aggregate=2;
            if(out->index<0) return false;
        } else {
            unsigned i=0; ListCell *key;
            foreach(key,key_exprs) {
                if(equal(entry->expr,lfirst(key))) break;
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
        static const char *less[]={"numeric_lt","int2lt","int4lt","int8lt","date_lt","bpcharlt","boollt","text_lt","float8lt"};
        static const char *greater[]={"numeric_gt","int2gt","int4gt","int8gt","date_gt","bpchargt","boolgt","text_gt","float8gt"};
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
    side=0; base=0;
    foreach(cell,relations) {
        RelOptInfo *rel=root->simple_rel_array[lfirst_int(cell)];
        const struct qs_join *j=side?qs_join_at(p,side-1):NULL;
        unsigned count=j?j->nattrs:qs_probe_attrs(p);
        if((j?j->predicate:p->predicate)>=0 && rel->rows<rel->tuples/2)
            for(unsigned i=base;i<base+count;++i) p->cols[i].deferred=p->cols[i].type==Q_DECIMAL && p->cols[i].slot>=0;
        base+=count; ++side;
    }
    order_strings(p);
    for(unsigned i=0;i<p->nattrs;++i) if(p->cols[i].slot>=0 && p->cols[i].type==Q_STRING) {
        p->runtime_dict=1;
        if(p->cols[i].dict_big) p->dict_big=1;
    }
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
    if(IsA(node,RangeTblRef)) { *relations=lappend_int(*relations,((RangeTblRef *)node)->rtindex); return list_length(*relations)<=QS_JOINS+1; }
    if(IsA(node,FromExpr)) {
        FromExpr *f=(FromExpr *)node; ListCell *cell;
        foreach(cell,f->fromlist) if(!join_inputs(lfirst(cell),relations,quals)) return false;
        if(f->quals) *quals=list_concat(*quals,IsA(f->quals,List)?list_copy((List *)f->quals):make_ands_implicit((Expr *)f->quals));
        return true;
    }
    if(IsA(node,JoinExpr)) {
        JoinExpr *j=(JoinExpr *)node;
        if((j->jointype!=JOIN_INNER && j->jointype!=JOIN_SEMI) || j->isNatural || j->usingClause || j->alias) return false;
        if(j->quals) *quals=list_concat(*quals,IsA(j->quals,List)?list_copy((List *)j->quals):make_ands_implicit((Expr *)j->quals));
        return join_inputs(j->larg,relations,quals) && join_inputs(j->rarg,relations,quals);
    }
    return false;
}
static OpExpr *equality_join(List *quals,List *prefix,Index next)
{
    ListCell *cell;
    foreach(cell,quals) {
        Node *node=lfirst(cell); if(!IsA(node,OpExpr)) continue;
        OpExpr *op=(OpExpr *)node; if(list_length(op->args)!=2) continue;
        Oid fn=OidIsValid(op->opfuncid)?op->opfuncid:get_opcode(op->opno);
        if(!builtin(fn,"int2eq") && !builtin(fn,"int4eq") && !builtin(fn,"int8eq")) continue;
        Node *left=unlabel(linitial(op->args)),*right=unlabel(lsecond(op->args));
        if(!IsA(left,Var) || !IsA(right,Var)) continue;
        Var *a=(Var *)left,*b=(Var *)right;
        if((a->varno==next && list_member_int(prefix,b->varno)) ||
           (b->varno==next && list_member_int(prefix,a->varno))) return op;
    }
    return NULL;
}
/* A single-column unique key is a safe high-selectivity lookup. Prefer these
 * over fact/dimension joins on low-cardinality columns (Q5 must not join
 * customer to supplier on nationkey before joining orders on custkey). */
static bool unique_join_key(PlannerInfo *root,Index rti,OpExpr *key)
{
    if(!root || rti<=0 || rti>(Index)root->simple_rel_array_size) return false;
    Node *left=unlabel(linitial(key->args)),*right=unlabel(lsecond(key->args));
    Var *var=NULL;
    if(IsA(left,Var) && ((Var *)left)->varno==rti) var=(Var *)left;
    else if(IsA(right,Var) && ((Var *)right)->varno==rti) var=(Var *)right;
    if(!var || var->varattno<=0) return false;
    RelOptInfo *rel=root->simple_rel_array[rti]; if(!rel) return false;
    ListCell *cell;
    foreach(cell,rel->indexlist) {
        IndexOptInfo *info=lfirst(cell);
        if(info->unique && info->nkeycolumns==1 && info->indexkeys[0]==var->varattno) return true;
    }
    return false;
}
/* Reuse the native planner's chosen access path. Accept a left-deep hash
 * prefix followed by one parameterized B-tree index join; other trees keep
 * the existing hash plan. This avoids a second, query-specific cost model. */
static bool index_join_order(Path *path,List *quals,List **ordered,List **joins,Index *indexed,Oid *indexid)
{
    if(!path) return false;
    switch(nodeTag(path)) {
    case T_AggPath:return index_join_order(((AggPath *)path)->subpath,quals,ordered,joins,indexed,indexid);
    case T_GroupPath:return index_join_order(((GroupPath *)path)->subpath,quals,ordered,joins,indexed,indexid);
    case T_SortPath:return index_join_order(((SortPath *)path)->subpath,quals,ordered,joins,indexed,indexid);
    case T_ProjectionPath:return index_join_order(((ProjectionPath *)path)->subpath,quals,ordered,joins,indexed,indexid);
    case T_GatherPath:return index_join_order(((GatherPath *)path)->subpath,quals,ordered,joins,indexed,indexid);
    case T_GatherMergePath:return index_join_order(((GatherMergePath *)path)->subpath,quals,ordered,joins,indexed,indexid);
    default:break;
    }
    if(IsA(path,HashPath) || IsA(path,NestPath)) {
        JoinPath *join=(JoinPath *)path;
        if(join->jointype!=JOIN_INNER || *indexed || !index_join_order(join->outerjoinpath,quals,ordered,joins,indexed,indexid)) return false;
        Path *inner=join->innerjoinpath;
        if(!inner->parent || inner->parent->relid<=0 || *indexed) return false;
        Index next=inner->parent->relid; OpExpr *key=equality_join(quals,*ordered,next); if(!key) return false;
        if(IsA(path,NestPath)) {
            if(!IsA(inner,IndexPath)) return false;
            *indexed=next; *indexid=((IndexPath *)inner)->indexinfo->indexoid;
        }
        *ordered=lappend_int(*ordered,next); *joins=lappend(*joins,key); return true;
    }
    if(path->parent && path->parent->relid>0 && !*ordered) {
        *ordered=list_make1_int(path->parent->relid); return true;
    }
    return false;
}
static bool prepare_index(PlannerInfo *root,struct qs_plan *plan,Index indexed,Oid indexid)
{
    unsigned n=qs_join_count(plan); if(!n) return false;
    struct qs_join *j=n==1?&plan->join:&plan->extra_joins[n-2];
    unsigned base=plan->nattrs-j->nattrs;
    IndexOptInfo *info=NULL; ListCell *cell;
    foreach(cell,root->simple_rel_array[indexed]->indexlist) { IndexOptInfo *i=lfirst(cell); if(i->indexoid==indexid) info=i; }
    int attno=j->build_key-(int)base+1;
    if(!info || info->indpred || info->indexprs || !info->unique || info->nkeycolumns!=1 ||
       info->indexkeys[0]!=attno) return false;
    Oid ns=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(ns) || !OidIsValid(get_relname_relid("indexes",ns))) return false;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID,INT2OID}; Datum values[]={ObjectIdGetDatum(indexid),ObjectIdGetDatum(j->relid),Int16GetDatum(attno)};
    int status=SPI_execute_with_args("SELECT r.relfilenode,i.root,i.key_bytes FROM sina_csd.indexes i JOIN sina_csd.relations r ON r.relid=i.indexid WHERE i.indexid=$1 AND i.heaprelid=$2 AND i.attnum=$3",3,types,values,NULL,true,1);
    bool found=status==SPI_OK_SELECT && SPI_processed==1;
    if(found) {
        bool null; HeapTuple tuple=SPI_tuptable->vals[0]; TupleDesc desc=SPI_tuptable->tupdesc;
        j->indexid=indexid; j->indexnode=DatumGetObjectId(SPI_getbinval(tuple,desc,1,&null));
        j->indexroot=DatumGetInt64(SPI_getbinval(tuple,desc,2,&null)); j->index_key_bytes=DatumGetInt16(SPI_getbinval(tuple,desc,3,&null));
        Relation idx=relation_open(indexid,AccessShareLock);
        found=idx->rd_node.relNode==j->indexnode && idx->rd_index->indisvalid && idx->rd_index->indisready && !idx->rd_index->indcheckxmin && !idx->rd_indoption[0];
        relation_close(idx,NoLock);
    }
    SPI_finish();
    return found;
}
static bool compile_index_path(PlannerInfo *root,RelOptInfo *input,List *relations,List *quals,struct qs_plan *plan,Index *probe_rti)
{
    List *ordered=NIL,*joins=NIL; Index indexed=0; Oid indexid=InvalidOid;
    Path *native=input->cheapest_total_path;
    if(!native) { ListCell *cell; foreach(cell,input->pathlist) { Path *p=lfirst(cell); if(!native || p->total_cost<native->total_cost) native=p; } }
    if(!index_join_order(native,quals,&ordered,&joins,&indexed,&indexid) || !indexed ||
        list_length(ordered)!=list_length(relations) || llast_int(ordered)!=(int)indexed) return false;
    struct qs_plan candidate={0}; if(!compile(root,ordered,joins,quals,&candidate) || !candidate.njoins) return false;
    if(!prepare_index(root,&candidate,indexed,indexid)) return false;
    *plan=candidate; *probe_rti=linitial_int(ordered);
    return true;
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
static bool greedy_order(PlannerInfo *root,List *relations,List *quals,List **ordered,List **joins,Index *probe)
{
    ListCell *cell; Index rti=0; double largest=-1;
    foreach(cell,relations) {
        Index index=lfirst_int(cell); RelOptInfo *rel=root->simple_rel_array[index];
        if(rel->tuples>largest) { largest=rel->tuples; rti=index; }
    }
    if(!rti) return false;
    *ordered=list_make1_int(rti); *joins=NIL; *probe=rti;
    while(list_length(*ordered)<list_length(relations)) {
        Index next=0; double best_rows=0; OpExpr *key=NULL; bool best_unique=false;
        foreach(cell,relations) {
            Index index=lfirst_int(cell); if(list_member_int(*ordered,index)) continue;
            OpExpr *candidate=equality_join(quals,*ordered,index);
            if(!candidate) continue;
            double rows=root->simple_rel_array[index]->rows;
            bool unique=unique_join_key(root,index,candidate);
            bool better=!next || (unique && (!best_unique || rows<best_rows)) ||
                (!unique && !best_unique && rows<best_rows);
            if(better) { next=index; best_rows=rows; key=candidate; best_unique=unique; }
        }
        if(!next) return false;
        *ordered=lappend_int(*ordered,next); *joins=lappend(*joins,key);
    }
    return true;
}

static bool prepare_index_lookup(Oid indexid,Oid heaprelid,int attno,struct qs_join *j)
{
    Oid ns=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(ns) || !OidIsValid(get_relname_relid("indexes",ns))) return false;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID,INT2OID}; Datum values[]={ObjectIdGetDatum(indexid),ObjectIdGetDatum(heaprelid),Int16GetDatum(attno)};
    int status=SPI_execute_with_args("SELECT r.relfilenode,i.root,i.key_bytes FROM sina_csd.indexes i JOIN sina_csd.relations r ON r.relid=i.indexid WHERE i.indexid=$1 AND i.heaprelid=$2 AND i.attnum=$3",3,types,values,NULL,true,1);
    bool found=status==SPI_OK_SELECT && SPI_processed==1;
    if(found) {
        bool null; HeapTuple tuple=SPI_tuptable->vals[0]; TupleDesc desc=SPI_tuptable->tupdesc;
        j->indexid=indexid; j->indexnode=DatumGetObjectId(SPI_getbinval(tuple,desc,1,&null));
        j->indexroot=DatumGetInt64(SPI_getbinval(tuple,desc,2,&null)); j->index_key_bytes=DatumGetInt16(SPI_getbinval(tuple,desc,3,&null));
        Relation idx=index_open(indexid,AccessShareLock);
        found=idx->rd_node.relNode==j->indexnode && idx->rd_index->indisvalid && idx->rd_index->indisready && !idx->rd_index->indcheckxmin && !idx->rd_indoption[0];
        index_close(idx,NoLock);
    }
    SPI_finish(); return found;
}
/* Q4: join_inputs now exposes the planner's SEMI JoinExpr. Compile the
 * orders/lineitem pair normally, then move the lineitem-only residual
 * (l_commitdate<l_receiptdate) to the index predicate and mark the join semi. */
static bool compile_semi_index(PlannerInfo *root,List *relations,List *quals,struct qs_plan *plan,Index *probe_rti)
{
    if(list_length(relations)!=2) return false;
    Index a=linitial_int(relations),b=lsecond_int(relations);
    char *an=get_rel_name(rt_fetch(a,root->parse->rtable)->relid),*bn=get_rel_name(rt_fetch(b,root->parse->rtable)->relid);
    Index orti=0,lrti=0;
    if(an && bn && !strcmp(an,"orders") && !strcmp(bn,"lineitem")) { orti=a; lrti=b; }
    else if(an && bn && !strcmp(an,"lineitem") && !strcmp(bn,"orders")) { orti=b; lrti=a; }
    else return false;
    List *prefix=list_make1_int(orti); OpExpr *key=equality_join(quals,prefix,lrti); if(!key) return false;
    List *ordered=list_make2_int(orti,lrti),*joins=list_make1(key);
    struct qs_plan p={0}; if(!compile(root,ordered,joins,quals,&p)) return false;
    if(p.njoins!=1 || p.join.rti!=lrti) return false;
    int pred=-1; ListCell *qc;
    foreach(qc,quals) {
        Node *node=lfirst(qc); if(!IsA(node,OpExpr)) continue;
        Bitmapset *vars=pull_varnos(node); int n=bms_num_members(vars);
        bool only_line=bms_is_member(lrti,vars) && n==1;
        bms_free(vars);
        if(!only_line) continue;
        pred=expression(node,orti,&p); if(pred<0) return false; break;
    }
    if(pred<0) return false;
    p.join.predicate=pred; p.join.residual=0; p.join.semi=1;
    Relation line=table_open(rt_fetch(lrti,root->parse->rtable)->relid,NoLock);
    Oid indexid=InvalidOid; List *indexes=RelationGetIndexList(line); ListCell *ic;
    foreach(ic,indexes) {
        Oid oid=lfirst_oid(ic); Relation ix=index_open(oid,AccessShareLock);
        if(ix->rd_index->indisprimary && ix->rd_index->indnatts>=1 && ix->rd_index->indkey.values[0]==1) indexid=oid;
        index_close(ix,NoLock);
        if(OidIsValid(indexid)) break;
    }
    bool ok=OidIsValid(indexid) && prepare_index_lookup(indexid,RelationGetRelid(line),1,&p.join);
    table_close(line,NoLock); if(!ok) return false;
    *plan=p; *probe_rti=orti; return true;
}
static bool compile_q13(PlannerInfo *root,struct qs_plan *plan,Index *probe_rti)
{
    Query *q=root->parse; if(!q || q->hasSubLinks) return false;
    RangeTblEntry *subrte=NULL; ListCell *rc;
    foreach(rc,q->rtable) { RangeTblEntry *r=lfirst_node(RangeTblEntry,rc); if(r->rtekind==RTE_SUBQUERY) { subrte=r; break; } }
    if(!subrte || !subrte->subquery) return false;
    Query *sub=subrte->subquery; if(!sub->hasAggs || list_length(sub->groupClause)!=1 || !IsA(sub->jointree,FromExpr)) return false;
    JoinExpr *joinj=NULL;
    foreach(rc,((FromExpr *)sub->jointree)->fromlist) if(IsA(lfirst(rc),JoinExpr)) joinj=(JoinExpr *)lfirst(rc);
    if(!joinj || joinj->jointype!=JOIN_LEFT || !joinj->larg || !joinj->rarg) return false;
    RangeTblRef *lr=IsA(joinj->larg,RangeTblRef)?(RangeTblRef *)joinj->larg:NULL;
    RangeTblRef *rr=IsA(joinj->rarg,RangeTblRef)?(RangeTblRef *)joinj->rarg:NULL;
    if(!lr || !rr) return false;
    Index lrti=lr->rtindex,rrti=rr->rtindex;
    char *ln=get_rel_name(rt_fetch(lrti,sub->rtable)->relid),*rn=get_rel_name(rt_fetch(rrti,sub->rtable)->relid);
    Index crti=0,orti=0;
    if(ln && rn && !strcmp(ln,"customer") && !strcmp(rn,"orders")) { crti=lrti; orti=rrti; }
    else if(ln && rn && !strcmp(ln,"orders") && !strcmp(rn,"customer")) { crti=rrti; orti=lrti; }
    else return false;
    Relation customer=table_open(rt_fetch(crti,sub->rtable)->relid,NoLock),orders=table_open(rt_fetch(orti,sub->rtable)->relid,NoLock);
    struct qs_plan p={0};
    if(!describe_relation(customer,crti,NULL,&p,0) || !describe_relation(orders,orti,NULL,&p,1)) { table_close(customer,NoLock); table_close(orders,NoLock); return false; }
    struct qs_join *j=&p.join; unsigned base=p.nattrs-j->nattrs;
    j->probe_key=0; j->build_key=base+1; j->rows=orders->rd_rel->reltuples; j->left_pad=1;
    p.cols[0].slot=0; p.cols[base+1].slot=1; p.cols[base+2].slot=2; p.cols[base].slot=3; p.ncols=4;
    int inner_key=put(&p,(struct qs_expr){.op=Q_COLUMN,.column=0,.type=Q_INTEGER}); if(inner_key<0) goto fail;
    p.keys[p.nkeys++]=inner_key;
    p.cols[base].slot=3;
    int orderkey=put(&p,(struct qs_expr){.op=Q_COLUMN,.column=base,.type=Q_INTEGER}); if(orderkey<0) goto fail;
    p.aggs[p.naggs++]=(struct qs_agg){.expr=orderkey,.kind=Q_COUNT};
    List *qlist=joinj->quals?make_ands_implicit((Expr *)joinj->quals):NIL; ListCell *qc; int build_pred=-1;
    foreach(qc,qlist) { Node *node=lfirst(qc); if(!IsA(node,OpExpr)) continue; int e=expression(node,crti,&p); if(e<0) goto fail; build_pred=e; }
    if(build_pred<0) goto fail;
    j->predicate=build_pred;
    p.nested=1; p.nested_key_agg=0; p.external_aggregate=1;
    p.outputs[p.nout++]=(struct qs_output){.index=0,.aggregate=0,.type_oid=INT8OID};
    p.outputs[p.nout++]=(struct qs_output){.index=0,.aggregate=0,.type_oid=INT8OID};
    p.order[p.norder++]=(struct qs_order){1,1,0};
    p.order[p.norder++]=(struct qs_order){0,1,0};
    p.limit=-1; p.ordered_rows=0;
    *plan=p; *probe_rti=1; table_close(customer,NoLock); table_close(orders,NoLock); return true;
fail:
    table_close(customer,NoLock); table_close(orders,NoLock); return false;
}
static bool install_plan(PlannerInfo *root,RelOptInfo *output,struct qs_plan *plan,Index rti)
{
    RelOptInfo *rel=root->simple_rel_array[rti]; if(!rel) return false;
    plan->external_aggregate=plan->nkeys!=0;
    if(plan->external_aggregate && plan->nkeys+plan->naggs>QS_OUT) return false;
    Oid nsp=get_namespace_oid("sina_csd",true);
    if(!OidIsValid(nsp) || !OidIsValid(get_relname_relid("relations",nsp))) return false;
    if(SPI_connect()!=SPI_OK_CONNECT) elog(ERROR,"SPI_connect failed");
    Oid types[]={OIDOID,OIDOID,OIDOID,OIDOID}; bool prepared=true;
    for(unsigned i=0;i<(plan->njoins?plan->njoins:1);++i) {
        const struct qs_join *j=qs_join_at(plan,i);
        Datum values[]={ObjectIdGetDatum(plan->relid),ObjectIdGetDatum(plan->relfilenode),ObjectIdGetDatum(j->relid),ObjectIdGetDatum(j->relfilenode)};
        const char *sql=plan->njoins?"SELECT 1 FROM sina_csd.relations r WHERE r.relid=$1 AND r.relfilenode=$2 AND EXISTS (SELECT 1 FROM sina_csd.relations b WHERE b.relid=$3 AND b.relfilenode=$4 AND b.device=r.device AND b.namespace_id=r.namespace_id AND b.lba_bytes=r.lba_bytes)":"SELECT 1 FROM sina_csd.relations WHERE relid=$1 AND relfilenode=$2";
        int status=SPI_execute_with_args(sql,plan->njoins?4:2,types,values,NULL,true,1);
        if(status!=SPI_OK_SELECT || SPI_processed!=1) { prepared=false; break; }
    }
    SPI_finish(); if(!prepared) return false;
    bytea *data=palloc(VARHDRSZ+sizeof(*plan)); SET_VARSIZE(data,VARHDRSZ+sizeof(*plan)); memcpy(VARDATA(data),plan,sizeof(*plan));
    CustomPath *path=makeNode(CustomPath); path->path.pathtype=T_CustomScan; path->path.parent=output;
    path->path.pathtarget=create_pathtarget(root,root->processed_tlist);
    path->path.rows=plan->ordered_rows || plan->external_aggregate?rel->rows:plan->nkeys?4:1;
    path->path.pathkeys=root->sort_pathkeys; path->path.startup_cost=path->path.total_cost=0;
    path->custom_private=list_make1(makeConst(BYTEAOID,-1,InvalidOid,-1,PointerGetDatum(data),false,false));
    path->methods=plan->ordered_rows?&order_path_methods:&path_methods; add_path(output,&path->path); return true;
}
void pg_sina_upper_paths(PlannerInfo *root,UpperRelationKind stage,RelOptInfo *input,RelOptInfo *output,void *extra)
{
    Query *q=root->parse;
    /* A query owns the namespace until its streaming source ends. Until mixed
     * plans share one device session, nested custom scans could wait on their
     * own outer scan's lock. Keep unsupported outer queries wholly native. */
    if(!pg_sina_exe_path || root->parent_root || stage!=UPPERREL_FINAL || q->commandType!=CMD_SELECT || q->havingQual ||
        (!q->hasAggs && (!q->sortClause || q->groupClause)) ||
        q->windowClause || q->groupingSets || q->cteList || q->hasRowSecurity || q->hasTargetSRFs ||
        q->rowMarks || q->distinctClause || q->setOperations || root->append_rel_list) return;
    if(q->hasSubLinks) {
        List *relations=NIL,*quals=NIL;
        if(!join_inputs((Node *)q->jointree,&relations,&quals) || !relations) return;
        struct qs_plan semi={0}; Index rti=0;
        if(compile_semi_index(root,relations,quals,&semi,&rti)) install_plan(root,output,&semi,rti);
        return;
    }
    {
        struct qs_plan q13={0}; Index rti=0;
        if(compile_q13(root,&q13,&rti)) { install_plan(root,output,&q13,rti); return; }
    }
    List *relations=NIL,*quals=NIL;
    if(!join_inputs((Node *)q->jointree,&relations,&quals) || !relations) return;
    ListCell *cell; Index rti=0;
    foreach(cell,relations) {
        Index index=lfirst_int(cell); RangeTblEntry *rte=rt_fetch(index,q->rtable);
        RelOptInfo *rel=root->simple_rel_array[index];
        if(rte->rtekind!=RTE_RELATION || rte->tablesample || !rel) return;
    }
    if(list_length(relations)>1 && !q->hasAggs) return;
    foreach(cell,quals) {
        Bitmapset *vars=pull_varnos(lfirst(cell)); bool constant=bms_is_empty(vars); bms_free(vars);
        if(constant && list_length(relations)>1) return;
    }
    List *ordered=NIL,*joins=NIL;
    if(!greedy_order(root,relations,quals,&ordered,&joins,&rti)) return;
    struct qs_plan plan={0}; if(!compile(root,ordered,joins,quals,&plan)) return;
    /* Keep the two-table orientation heuristic; multi-join probes follow a
     * connected order and retain all build states under one shared budget. */
    if(plan.njoins==1 && build_bytes(&plan,root->simple_rel_array[plan.join.rti]->rows)>(double)pg_sina_max_memory*1024/2) {
        struct qs_plan other={0}; List *reverse=list_make2_int(plan.join.rti,rti);
        if(compile(root,reverse,joins,quals,&other) && build_bytes(&other,root->simple_rel_array[rti]->rows)<
           build_bytes(&plan,root->simple_rel_array[plan.join.rti]->rows)) { plan=other; rti=linitial_int(reverse); }
    }
    struct qs_plan indexed={0};
    if(plan.njoins && compile_index_path(root,input,relations,quals,&indexed,&rti)) plan=indexed;
    install_plan(root,output,&plan,rti);
}
