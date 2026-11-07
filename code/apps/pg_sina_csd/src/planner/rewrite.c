/* Decorrelation before PG planning; physical lowering happens afterwards.
 * Only strict WHERE uses of NULL-on-empty scalar aggregates become INNER
 * JOINs. COUNT, OR and scalar cardinality changes retain PG semantics. */
#include "pg_internal.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_oper.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_namespace_d.h"
#include "utils/lsyscache.h"

static bool named(Oid id,const char *name)
{ char *s=get_func_name(id); return s && get_func_namespace(id)==PG_CATALOG_NAMESPACE && !strcmp(s,name); }
static bool has_outer(Node *node,void *context)
{
    if(!node) return false;
    if(IsA(node,Var)) return ((Var *)node)->varlevelsup!=0;
    return expression_tree_walker(node,has_outer,context);
}
static SubLink *first_link;
static bool find_link(Node *node,void *context)
{
    if(!node) return false;
    if(IsA(node,SubLink)) { first_link=(SubLink *)node; return true; }
    return expression_tree_walker(node,find_link,context);
}
static bool find_scalar(Node *node,void *context)
{
    if(!node) return false;
    if(IsA(node,SubLink)) {
        if(((SubLink *)node)->subLinkType!=EXPR_SUBLINK) return false;
        first_link=(SubLink *)node; return true;
    }
    return expression_tree_walker(node,find_scalar,context);
}
static bool strict_path(Node *node,SubLink *target)
{
    if(!node) return false;
    if(node==(Node *)target) return true;
    if(IsA(node,RelabelType)) return strict_path((Node *)((RelabelType *)node)->arg,target);
    if(IsA(node,OpExpr)) {
        OpExpr *op=(OpExpr *)node;
        if(!func_strict(get_opcode(op->opno))) return false;
        ListCell *c;
        foreach(c,op->args) if(strict_path(lfirst(c),target)) return true;
    }
    return false;
}
static bool null_on_empty(Node *node)
{
    if(!node) return false;
    if(IsA(node,Aggref)) {
        Aggref *a=(Aggref *)node;
        return !a->agglevelsup && (named(a->aggfnoid,"sum") || named(a->aggfnoid,"avg") ||
            named(a->aggfnoid,"min") || named(a->aggfnoid,"max"));
    }
    if(IsA(node,RelabelType)) return null_on_empty((Node *)((RelabelType *)node)->arg);
    if(IsA(node,OpExpr)) {
        OpExpr *op=(OpExpr *)node;
        if(!func_strict(get_opcode(op->opno))) return false;
        bool found=false; ListCell *c;
        foreach(c,op->args) {
            Node *arg=lfirst(c);
            if(null_on_empty(arg)) found=true;
            else if(!IsA(arg,Const)) return false;
        }
        return found;
    }
    return false;
}
struct replacement { SubLink *link; Node *value; };
static Node *replace_link(Node *node,struct replacement *r)
{
    if(!node) return NULL;
    if(node==(Node *)r->link) return copyObject(r->value);
    return expression_tree_mutator(node,replace_link,r);
}
static bool rewrite_one(Query *q)
{
    if(!q->hasSubLinks || !q->jointree || q->hasRowSecurity || q->hasTargetSRFs) return false;
    first_link=NULL;
    if(!find_scalar(q->jointree->quals,NULL)) return false;
    SubLink *link=first_link;
    if(link->subLinkType!=EXPR_SUBLINK) return false;
    List *outer=make_ands_implicit((Expr *)q->jointree->quals); bool strict=false; ListCell *c;
    foreach(c,outer) if(strict_path(lfirst(c),link)) strict=true;
    if(!strict) return false;
    Query *sub=(Query *)link->subselect;
    if(!sub || sub->commandType!=CMD_SELECT || !sub->hasAggs || sub->hasSubLinks ||
       !sub->jointree || sub->groupClause || sub->havingQual || sub->windowClause || sub->groupingSets ||
       sub->sortClause || sub->limitCount || sub->limitOffset || sub->distinctClause || sub->setOperations ||
       sub->hasTargetSRFs || sub->hasRowSecurity || sub->cteList || list_length(sub->targetList)!=1 ||
       !null_on_empty((Node *)linitial_node(TargetEntry,sub->targetList)->expr)) return false;
    foreach(c,sub->rtable) {
        RangeTblEntry *rte=lfirst(c);
        if(rte->rtekind!=RTE_RELATION || rte->lateral || rte->securityQuals) return false;
    }
    List *locals=NIL,*inner_keys=NIL,*outer_keys=NIL,*operators=NIL;
    List *conditions=make_ands_implicit((Expr *)sub->jointree->quals);
    foreach(c,conditions) {
        Node *node=lfirst(c);
        if(!has_outer(node,NULL)) { locals=lappend(locals,node); continue; }
        if(!IsA(node,OpExpr)) return false;
        OpExpr *op=(OpExpr *)node;
        if(list_length(op->args)!=2) return false;
        Oid fn=get_opcode(op->opno);
        if(!named(fn,"int2eq") && !named(fn,"int4eq") && !named(fn,"int8eq")) return false;
        Node *left=linitial(op->args),*right=lsecond(op->args);
        while(IsA(left,RelabelType)) left=(Node *)((RelabelType *)left)->arg;
        while(IsA(right,RelabelType)) right=(Node *)((RelabelType *)right)->arg;
        if(!IsA(left,Var) || !IsA(right,Var)) return false;
        Var *a=(Var *)left,*b=(Var *)right;
        if(a->varlevelsup==1 && b->varlevelsup==0) { Var *swap=a; a=b; b=swap; }
        if(a->varlevelsup || b->varlevelsup!=1 || a->vartype!=b->vartype || a->varattno<=0 || b->varattno<=0) return false;
        if(list_length(inner_keys)==QS_KEYS) return false;
        inner_keys=lappend(inner_keys,a); outer_keys=lappend(outer_keys,b); operators=lappend_oid(operators,op->opno);
    }
    if(!inner_keys) return false;
    /* Correlation outside WHERE is not covered by this transformation. */
    if(has_outer((Node *)sub->targetList,NULL) || has_outer((Node *)sub->jointree->fromlist,NULL)) return false;
    Query *agg=copyObject(sub); agg->jointree->quals=(Node *)make_ands_explicit(locals); agg->canSetTag=false;
    List *names=list_make1(makeString("value")); unsigned position=0;
    foreach(c,inner_keys) {
        Var *key=lfirst(c); Oid sortop,eqop; bool hashable;
        get_sort_group_operators(key->vartype,true,true,false,&sortop,&eqop,NULL,&hashable);
        TargetEntry *t=makeTargetEntry((Expr *)copyObject(key),position+2,psprintf("key%u",position),false);
        t->ressortgroupref=position+1; agg->targetList=lappend(agg->targetList,t);
        SortGroupClause *g=makeNode(SortGroupClause); g->tleSortGroupRef=position+1; g->eqop=eqop; g->sortop=sortop; g->hashable=hashable;
        agg->groupClause=lappend(agg->groupClause,g); names=lappend(names,makeString(pstrdup(t->resname))); ++position;
    }
    RangeTblEntry *rte=makeNode(RangeTblEntry); rte->rtekind=RTE_SUBQUERY; rte->subquery=agg;
    rte->alias=makeAlias("snia_scalar",NIL); rte->eref=makeAlias("snia_scalar",names); rte->inFromCl=true;
    q->rtable=lappend(q->rtable,rte); Index rti=list_length(q->rtable);
    RangeTblRef *ref=makeNode(RangeTblRef); ref->rtindex=rti; q->jointree->fromlist=lappend(q->jointree->fromlist,ref);
    Node *value=(Node *)makeVar(rti,1,exprType((Node *)link),exprTypmod((Node *)link),exprCollation((Node *)link),0);
    struct replacement replacement={link,value};
    q->jointree->quals=replace_link(q->jointree->quals,&replacement);
    List *joins=make_ands_implicit((Expr *)q->jointree->quals); position=0;
    foreach(c,outer_keys) {
        Var *key=copyObject(lfirst(c)); key->varlevelsup=0;
        Var *derived=makeVar(rti,position+2,key->vartype,key->vartypmod,key->varcollid,0);
        Expr *qual=make_opclause(list_nth_oid(operators,position),BOOLOID,false,(Expr *)key,(Expr *)derived,InvalidOid,key->varcollid);
        joins=lappend(joins,qual); ++position;
    }
    q->jointree->quals=(Node *)make_ands_explicit(joins);
    first_link=NULL; q->hasSubLinks=find_link(q->jointree->quals,NULL);
    return true;
}
static bool rewrite_child(Node *node,bool *changed)
{
    if(!node) return false;
    if(IsA(node,Query)) { if(pg_sina_rewrite_query((Query *)node)) *changed=true; return false; }
    return expression_tree_walker(node,rewrite_child,changed);
}

static bool reusable_grouped_input(RangeTblEntry *rte)
{
    if(rte->rtekind!=RTE_SUBQUERY || rte->lateral || rte->security_barrier ||
       rte->securityQuals || rte->requiredPerms) return false;
    Query *input=rte->subquery;
    if(input->commandType!=CMD_SELECT || !input->hasAggs || !input->groupClause ||
       input->hasSubLinks || input->hasRowSecurity || input->hasTargetSRFs ||
       input->cteList || input->rowMarks || input->limitCount || input->limitOffset ||
       input->setOperations || input->windowClause || input->groupingSets) return false;
    ListCell *cell;
    foreach(cell,input->rtable) {
        RangeTblEntry *base=lfirst_node(RangeTblEntry,cell);
        if(base->rtekind!=RTE_RELATION || base->securityQuals || base->tablesample) return false;
    }
    return !query_tree_walker(input,has_outer,NULL,0) &&
        !contain_mutable_functions((Node *)input);
}

static void use_shared_input(RangeTblEntry *rte,CommonTableExpr *cte,Index levels)
{
    rte->rtekind=RTE_CTE;
    rte->subquery=NULL;
    rte->ctename=cte->ctename;
    rte->ctelevelsup=levels;
    rte->coltypes=cte->ctecoltypes;
    rte->coltypmods=cte->ctecoltypmods;
    rte->colcollations=cte->ctecolcollations;
}

static Query *merged_grouped_input(Query *left,Query *right)
{
    if(equal(left,right)) return left;
    if(list_length(left->rtable)!=list_length(right->rtable)) return NULL;
    Query *a=copyObject(left),*b=copyObject(right);
    ListCell *first,*second;
    /* The view references can require different columns. Keep the union
     * of their privilege checks while comparing all other query fields. */
    forboth(first,a->rtable,second,b->rtable) {
        RangeTblEntry *x=lfirst_node(RangeTblEntry,first),*y=lfirst_node(RangeTblEntry,second);
        x->selectedCols=bms_union(x->selectedCols,y->selectedCols);
        y->selectedCols=x->selectedCols;
    }
    return equal(a,b)?a:NULL;
}

/* Share a deterministic grouped input repeated by the main query and a
 * scalar subquery. PG plans one producer and two independent CTE readers. */
static bool share_grouped_input(Query *q)
{
    if(!q->hasSubLinks || !q->jointree || q->cteList || q->hasRowSecurity || q->rowMarks)
        return false;
    if(!find_scalar(q->jointree->quals,NULL)) return false;
    Query *scalar=castNode(Query,first_link->subselect);
    if(scalar->cteList || scalar->hasRowSecurity || scalar->rowMarks) return false;
    ListCell *main_cell,*scalar_cell;
    foreach(main_cell,q->rtable) {
        RangeTblEntry *main=lfirst_node(RangeTblEntry,main_cell);
        if(!reusable_grouped_input(main)) continue;
        foreach(scalar_cell,scalar->rtable) {
            RangeTblEntry *other=lfirst_node(RangeTblEntry,scalar_cell);
            if(!reusable_grouped_input(other)) continue;
            Query *producer=merged_grouped_input(main->subquery,other->subquery);
            if(!producer) continue;
            CommonTableExpr *cte=makeNode(CommonTableExpr);
            cte->ctename=pstrdup("snia_shared_group");
            cte->ctequery=(Node *)producer;
            cte->ctematerialized=CTEMaterializeAlways;
            cte->cterefcount=2;
            cte->location=-1;
            cte->ctecolnames=copyObject(main->eref->colnames);
            ListCell *target;
            foreach(target,producer->targetList) {
                TargetEntry *entry=lfirst_node(TargetEntry,target);
                if(entry->resjunk) continue;
                Node *expr=(Node *)entry->expr;
                cte->ctecoltypes=lappend_oid(cte->ctecoltypes,exprType(expr));
                cte->ctecoltypmods=lappend_int(cte->ctecoltypmods,exprTypmod(expr));
                cte->ctecolcollations=lappend_oid(cte->ctecolcollations,exprCollation(expr));
            }
            use_shared_input(main,cte,0);
            use_shared_input(other,cte,1);
            q->cteList=list_make1(cte);
            return true;
        }
    }
    return false;
}

bool pg_sina_rewrite_query(Query *q)
{
    if(!q || q->commandType!=CMD_SELECT) return false;
    bool changed=false;
    query_tree_walker(q,rewrite_child,&changed,0);
    while(q->hasSubLinks) {
        Query *candidate=copyObject(q);
        if(!rewrite_one(candidate)) break;
        *q=*candidate; changed=true;
    }
    return share_grouped_input(q) || changed;
}
