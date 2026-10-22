#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "parser/parse_oper.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "utils/lsyscache.h"
#include "pg_internal.h"

static bool expr_has_sublink(Node *node);
static bool fn_is(Oid oid,const char *name) { char *n=get_func_name(oid); bool r=n && !strcmp(n,name); if(n) pfree(n); return r; }

static bool
expr_has_sublink(Node *node)
{
    if (!node) return false;
    if (IsA(node, SubLink)) return true;
    if (IsA(node, BoolExpr)) {
        ListCell *c;
        foreach(c, ((BoolExpr *)node)->args)
            if (expr_has_sublink(lfirst(c))) return true;
        return false;
    }
    if (IsA(node, OpExpr)) {
        ListCell *c;
        foreach(c, ((OpExpr *)node)->args)
            if (expr_has_sublink(lfirst(c))) return true;
        return false;
    }
    if (IsA(node, NullTest)) return expr_has_sublink((Node *)((NullTest *)node)->arg);
    if (IsA(node, RelabelType)) return expr_has_sublink((Node *)((RelabelType *)node)->arg);
    if (IsA(node, ScalarArrayOpExpr)) {
        ListCell *c;
        foreach(c, ((ScalarArrayOpExpr *)node)->args)
            if (expr_has_sublink(lfirst(c))) return true;
    }
    return false;
}

static SubLink *
find_sublink(Node *node)
{
    if (!node) return NULL;
    if (IsA(node, SubLink)) return (SubLink *)node;
    if (IsA(node, BoolExpr)) {
        ListCell *c;
        foreach(c, ((BoolExpr *)node)->args) {
            SubLink *s = find_sublink(lfirst(c));
            if (s) return s;
        }
        return NULL;
    }
    if (IsA(node, OpExpr)) {
        ListCell *c;
        foreach(c, ((OpExpr *)node)->args) {
            SubLink *s = find_sublink(lfirst(c));
            if (s) return s;
        }
        return NULL;
    }
    if (IsA(node, NullTest)) return find_sublink((Node *)((NullTest *)node)->arg);
    if (IsA(node, RelabelType)) return find_sublink((Node *)((RelabelType *)node)->arg);
    if (IsA(node, ScalarArrayOpExpr)) {
        ListCell *c;
        foreach(c, ((ScalarArrayOpExpr *)node)->args) {
            SubLink *s = find_sublink(lfirst(c));
            if (s) return s;
        }
    }
    return NULL;
}

static Node *
replace_sublink(Node *node, SubLink *target, Node *replacement)
{
    if (!node) return NULL;
    if (node == (Node *)target) return copyObject(replacement);
    if (IsA(node, OpExpr)) {
        OpExpr *op = (OpExpr *)node;
        ListCell *c;
        foreach(c, op->args) lfirst(c) = replace_sublink(lfirst(c), target, replacement);
        return node;
    }
    if (IsA(node, BoolExpr)) {
        BoolExpr *b = (BoolExpr *)node;
        ListCell *c;
        foreach(c, b->args) lfirst(c) = replace_sublink(lfirst(c), target, replacement);
        return node;
    }
    if (IsA(node, NullTest)) {
        NullTest *n = (NullTest *)node;
        n->arg = (Expr *)replace_sublink((Node *)n->arg, target, replacement);
        return node;
    }
    if (IsA(node, RelabelType)) {
        RelabelType *r = (RelabelType *)node;
        r->arg = (Expr *)replace_sublink((Node *)r->arg, target, replacement);
        return node;
    }
    if (IsA(node, ScalarArrayOpExpr)) {
        ScalarArrayOpExpr *s = (ScalarArrayOpExpr *)node;
        ListCell *c;
        foreach(c, s->args) lfirst(c) = replace_sublink(lfirst(c), target, replacement);
        return node;
    }
    return node;
}

/* Q17/Q22-style: one equality-correlated, one-aggregate EXPR_SUBLINK in WHERE.
 * Build:
 *   agg = SELECT <agg> FROM <inner> WHERE <uncorrelated quals> GROUP BY <key>
 *   outer.key = agg.key AND <outer quals>
 * The original SubLink is replaced with a Var pointing at <agg>.
 */
bool
pg_sina_rewrite_query(Query *q)
{
    ListCell *qc;
    SubLink *sublink = NULL;

    if (!q || !q->hasSubLinks || !q->jointree || !IsA(q->jointree, FromExpr) ||
        !q->jointree->quals || q->havingQual || q->hasTargetSRFs || q->hasRowSecurity)
        return false;

    foreach(qc, q->targetList)
        if (expr_has_sublink((Node *)((TargetEntry *)lfirst(qc))->expr)) return false;
    if (!expr_has_sublink(q->jointree->quals)) return false;

    /* Find the single SubLink in WHERE; target-list SubLinks need LEFT JOIN
     * semantics and are intentionally not handled by this first rewrite. */
    sublink = find_sublink(q->jointree->quals);
    if (!sublink || sublink->subLinkType != EXPR_SUBLINK) return false;

    Query *sub = (Query *)sublink->subselect;
    if (!sub || !sub->hasAggs || list_length(sub->targetList) != 1 ||
        !sub->jointree || !IsA(sub->jointree, FromExpr) || sub->groupClause ||
        sub->sortClause || sub->limitCount || sub->limitOffset || sub->distinctClause ||
        sub->windowClause || sub->hasSubLinks || list_length(sub->rtable) != 1)
        return false;

    /* Find the single equality correlation Var pair, and remove that qual from
     * the inner query. */
    Node *inner_quals = sub->jointree->quals;
    OpExpr *corr = NULL;
    if (IsA(inner_quals, OpExpr)) corr = (OpExpr *)inner_quals;
    if (!corr || list_length(corr->args) != 2) return false;
    Oid corrfn = OidIsValid(corr->opfuncid) ? corr->opfuncid : get_opcode(corr->opno);
    if (!fn_is(corrfn, "int4eq") && !fn_is(corrfn, "int8eq") && !fn_is(corrfn, "int2eq"))
        return false;

    Var *inner_var = NULL, *outer_var = NULL;
    ListCell *vc;
    foreach(vc, corr->args) {
        Node *n = lfirst(vc);
        while (IsA(n, RelabelType)) n = (Node *)((RelabelType *)n)->arg;
        if (IsA(n, Var)) {
            Var *v = (Var *)n;
            if (v->varlevelsup == 0) inner_var = v;
            else if (v->varlevelsup > 0 && v->varno > 0) outer_var = v;
        }
    }
    if (!inner_var || !outer_var) return false;

    Query *agg = copyObject(sub);
    agg->jointree->quals = NULL;
    agg->groupClause = NIL;
    agg->sortClause = NIL;
    agg->limitCount = NULL;
    agg->limitOffset = NULL;
    agg->hasSubLinks = false;
    agg->canSetTag = false;

    Oid keytype = inner_var->vartype;
    Oid sortop = InvalidOid, eqop = InvalidOid;
    bool hashable = false;
    get_sort_group_operators(keytype, true, true, false, &sortop, &eqop, NULL, &hashable);
    if (!OidIsValid(eqop)) return false;

    int keyresno = list_length(agg->targetList) + 1;
    TargetEntry *keytle = makeTargetEntry((Expr *)copyObject(inner_var), keyresno,
                                          "sina_key", false);
    keytle->ressortgroupref = 1;
    agg->targetList = lappend(agg->targetList, keytle);

    SortGroupClause *gc = makeNode(SortGroupClause);
    gc->tleSortGroupRef = 1;
    gc->eqop = eqop;
    gc->sortop = sortop;
    gc->nulls_first = false;
    gc->hashable = hashable;
    agg->groupClause = list_make1(gc);

    RangeTblEntry *rte = makeNode(RangeTblEntry);
    rte->rtekind = RTE_SUBQUERY;
    rte->subquery = agg;
    rte->alias = makeAlias("sina_sq", NIL);
    rte->eref = makeAlias("sina_sq", list_make2(makeString("sina_value"), makeString("sina_key")));
    rte->inFromCl = true;
    q->rtable = lappend(q->rtable, rte);
    Index rtindex = list_length(q->rtable);

    RangeTblRef *rtr = makeNode(RangeTblRef);
    rtr->rtindex = rtindex;
    q->jointree->fromlist = lappend(q->jointree->fromlist, rtr);

    Var *outkey = makeVar(rtindex, keyresno, keytype, inner_var->vartypmod,
                          inner_var->varcollid, 0);
    Var *outerkey = copyObject(outer_var);
    outerkey->varlevelsup = 0;
    OpExpr *joinqual = (OpExpr *)make_opclause(eqop, BOOLOID, false,
                                    (Expr *)outerkey, (Expr *)outkey,
                                    InvalidOid, inner_var->varcollid);

    /* Replace the SubLink in the outer WHERE with the subquery's value column. */
    Oid valtype = exprType((Node *)sublink);
    int32 valtypmod = exprTypmod((Node *)sublink);
    Oid valcoll = exprCollation((Node *)sublink);
    Node *replacement = (Node *)makeVar(rtindex, 1, valtype, valtypmod, valcoll, 0);
    q->jointree->quals = replace_sublink(q->jointree->quals, sublink, replacement);
    if (expr_has_sublink(q->jointree->quals)) return false;

    q->jointree->quals = (Node *)makeBoolExpr(AND_EXPR,
        list_make2(q->jointree->quals, (Node *)joinqual), -1);
    q->hasSubLinks = false;
    return true;
}
