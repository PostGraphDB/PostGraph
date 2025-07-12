#include "postgraph.h"

#include "access/nbtree.h"
#include "access/sysattr.h"
#include "access/heapam.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/cypher_analyze.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/catcache.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "commands/label_commands.h"
#include "nodes/ag_nodes.h"
#include "nodes/cypher_nodes.h"
#include "parser/cypher_clause.h"
#include "parser/cypher_expr.h"
#include "parser/cypher_item.h"
#include "parser/cypher_parse_agg.h"
#include "parser/cypher_parse_node.h"
#include "parser/cypher_transform_entity.h"
#include "utils/ag_cache.h"
#include "utils/ag_func.h"
#include "utils/gtype.h"
#include "utils/graphid.h"
#include "utils/vertex.h"
#include "utils/edge.h"
#include "utils/traversal.h"
#include "utils/variable_edge.h"



typedef Query *(*transform_method)(cypher_parsestate *cpstate, cypher_clause *clause);

// projection

static TargetEntry *find_target_list_entry(cypher_parsestate *cpstate, Node *node, List **target_list, ParseExprKind expr_kind);
static Node *transform_cypher_limit(cypher_parsestate *cpstate, Node *node, ParseExprKind expr_kind, const char *construct_name);
static Query *transform_cypher_clause_with_where(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause);
// match clause
static Query *transform_cypher_match(cypher_parsestate *cpstate, cypher_clause *clause);
static void transform_match_pattern(cypher_parsestate *cpstate, Query *query, List *pattern, Node *where);
static Query *transform_cypher_match_pattern(cypher_parsestate *cpstate, cypher_clause *clause);

// create clause
static Query *transform_cypher_create(cypher_parsestate *cpstate, cypher_clause *clause);
static void get_res_cols(ParseState *pstate, ParseNamespaceItem *l_pnsi, ParseNamespaceItem *r_pnsi, List **res_colnames, List **res_colvars);

// transform
#define PREV_CYPHER_CLAUSE_ALIAS    "_"
#define CYPHER_OPT_RIGHT_ALIAS      "_R"
#define transform_prev_cypher_clause(cpstate, prev_clause, add_rte_to_query) \
    transform_cypher_clause_as_subquery(cpstate, transform_cypher_clause, \
                                        prev_clause, NULL, add_rte_to_query)
static ParseNamespaceItem *transform_cypher_clause_as_subquery(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause, Alias *alias, bool add_rte_to_query);
static Query *analyze_cypher_clause(transform_method transform, cypher_clause *clause, cypher_parsestate *parent_cpstate);
static void advance_transform_entities_to_next_clause(List *entities);
static ParseNamespaceItem *get_namespace_item(ParseState *pstate, RangeTblEntry *rte);
static List *make_target_list_from_join(ParseState *pstate, RangeTblEntry *rte);
static void setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok);

List *
transform_window_definitions(ParseState *pstate, List *windowdefs, List **targetlist);

/*
 * transform a cypher_clause
 */
Query *transform_cypher_clause(cypher_parsestate *cpstate, cypher_clause *clause) {
    Node *self = clause->self;
    Query *result;

    // examine the type of clause and call the transform logic for it
    if (is_ag_node(self, cypher_return)) {
        cypher_return *n = (cypher_return *) self;

        if (n->op == SETOP_NONE)
            result = transform_cypher_return(cpstate, clause);
        else
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_return")));
    } else if (is_ag_node(self, cypher_with)) {
        result = transform_cypher_with(cpstate, clause);
    } else if (is_ag_node(self, cypher_match)) {
        result = transform_cypher_match(cpstate, clause);
    } else if (is_ag_node(self, cypher_create)) {
        result = transform_cypher_create(cpstate, clause);
    } else {
        ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));
    }

    result->querySource = QSRC_ORIGINAL;
    result->canSetTag = true;

    return result;
}
static Query *transform_cypher_create(cypher_parsestate *cpstate, cypher_clause *clause) {
    ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));
}



Query *transform_cypher_return(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_return *self = (cypher_return *)clause->self;
    Query *query;
    List *groupClause = NIL;

    query = makeNode(Query);
    query->commandType = CMD_SELECT;

    pstate->p_windowdefs = self->window_clause;

    if (clause->prev)
        transform_prev_cypher_clause(cpstate, clause->prev, true);

    query->targetList = transform_cypher_item_list(cpstate, self->items, &groupClause, EXPR_KIND_SELECT_TARGET);

    markTargetListOrigins(pstate, query->targetList);

    Expr *expr = NULL;
    if (self->where != NULL)
        expr = transform_cypher_expr(cpstate, self->where, EXPR_KIND_WHERE);

    // ORDER BY
    query->sortClause = transform_cypher_order_by(cpstate, self->order_by, &query->targetList, EXPR_KIND_ORDER_BY);

    if (self->real_group_clause != NIL)
        query->groupClause = transform_group_clause(cpstate, self->real_group_clause, &query->groupingSets,
                                        &query->targetList,
                                                    query->sortClause, EXPR_KIND_GROUP_BY);
    else if (groupClause != NIL) // auto GROUP BY
        query->groupClause = transform_group_clause(cpstate, groupClause, &query->groupingSets, &query->targetList,
                                                    query->sortClause, EXPR_KIND_GROUP_BY);
    else 
        query->groupClause = NULL;

    if (self->having) {
        query->havingQual = transform_cypher_expr(cpstate, self->having, EXPR_KIND_HAVING);
    }
    // DISTINCT
    if (self->distinct) {
        query->distinctClause = transformDistinctClause(pstate, &query->targetList, query->sortClause, false);
        query->hasDistinctOn = false;
    } else {
        query->distinctClause = NIL;
        query->hasDistinctOn = false;
    }

    // SKIP and LIMIT
    query->limitOffset = transform_cypher_limit(cpstate, self->skip, EXPR_KIND_OFFSET, "SKIP");
    query->limitCount = transform_cypher_limit(cpstate, self->limit, EXPR_KIND_LIMIT, "LIMIT");

    if (pstate->p_windowdefs != NIL) {
        //ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("window functions are not done")));
        query->windowClause = transform_window_definitions(pstate, pstate->p_windowdefs, &query->targetList);
    }

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, expr);
    query->hasWindowFuncs = pstate->p_hasWindowFuncs;
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;
    query->hasSubLinks = pstate->p_hasSubLinks;
    query->hasAggs = pstate->p_hasAggs;

    assign_query_collations(pstate, query);

    // this must be done after collations, for reliable comparison of exprs 
    if (pstate->p_hasAggs || query->groupClause || query->groupingSets || query->havingQual)
        parse_check_aggregates(pstate, query);

    return query;
}


// see findTargetlistEntrySQL99()
static TargetEntry *find_target_list_entry(cypher_parsestate *cpstate, Node *node, List **target_list,
                                           ParseExprKind expr_kind) {
    Node *expr;
    ListCell *lt;
    TargetEntry *te;

    expr = transform_cypher_expr(cpstate, node, expr_kind);

    foreach (lt, *target_list) {
        Node *te_expr;

        te = lfirst(lt);
        te_expr = strip_implicit_coercions((Node *)te->expr);

        if (equal(expr, te_expr))
            return te;
    }

    te = transform_cypher_item(cpstate, node, expr, expr_kind, NULL, true);

    *target_list = lappend(*target_list, te);

    return te;
}

// see transformSortClause()
List *transform_cypher_order_by(cypher_parsestate *cpstate, List *sort_items, List **target_list,
                                       ParseExprKind expr_kind) {
    ParseState *pstate = (ParseState *)cpstate;
    List *sort_list = NIL;
    ListCell *li;

    foreach (li, sort_items) {
        SortBy *sort_by = lfirst(li);
        TargetEntry *te;

        te = find_target_list_entry(cpstate, sort_by->node, target_list, expr_kind);

        sort_list = addTargetToSortList(pstate, te, sort_list, *target_list, sort_by);
    }

    return sort_list;
}


// see transformLimitClause()
static Node *transform_cypher_limit(cypher_parsestate *cpstate, Node *node, ParseExprKind expr_kind,
                                    const char *construct_name) {
    ParseState *pstate = (ParseState *)cpstate;
    Node *qual;

    if (!node)
        return NULL;

    qual = transform_cypher_expr(cpstate, node, expr_kind);

    qual = coerce_to_specific_type(pstate, qual, INT8OID, construct_name);

    // LIMIT can't refer to any variables of the current query.
    if (contain_vars_of_level(qual, 0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                 errmsg("argument of %s must not contain variables", construct_name),
                 parser_errposition(pstate, locate_var_of_level(qual, 0))));

    return qual;
}

Query *transform_cypher_with(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_with *self = (cypher_with *)clause->self;
    cypher_return *return_clause;
    cypher_clause *wrapper;

    // WITH clause is basically RETURN clause with optional WHERE subclause
    return_clause = make_ag_node(cypher_return);
    return_clause->distinct = self->distinct;
    return_clause->items = self->items;
    return_clause->real_group_clause = self->real_group_clause;
    return_clause->having = self->having;
    return_clause->window_clause = self->window_clause;
    return_clause->order_by = self->order_by;
    return_clause->skip = self->skip;
    return_clause->limit = self->limit;
    return_clause->where = self->where;

    wrapper = palloc(sizeof(*wrapper));
    wrapper->self = (Node *)return_clause;
    wrapper->prev = clause->prev;

    return transform_cypher_clause_with_where(cpstate, transform_cypher_return, wrapper);
}

static Query *transform_cypher_clause_with_where(cypher_parsestate *cpstate, transform_method transform,
                                                 cypher_clause *clause)
{
    ParseState *pstate = (ParseState *)cpstate;
    Query *query;
    cypher_match *self = (cypher_match *)clause->self;
    Node *where = self->where;

    if (where) {
        int rtindex;
        ParseNamespaceItem *pnsi;

        query = makeNode(Query);
        query->commandType = CMD_SELECT;

        pnsi = transform_cypher_clause_as_subquery(cpstate, transform, clause, NULL, true);
        Assert(pnsi != NULL);
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the only RangeTblEntry in pstate

        query->targetList = expandNSItemAttrs(pstate, pnsi, 0, -1);

        markTargetListOrigins(pstate, query->targetList);

        query->rtable = pstate->p_rtable;
        query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

        assign_query_collations(pstate, query);
    } else {
        query = transform(cpstate, clause);
    }

    query->hasSubLinks = pstate->p_hasSubLinks;
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;
    query->hasAggs = pstate->p_hasAggs;

    return query;
}

static Query *transform_cypher_match(cypher_parsestate *cpstate, cypher_clause *clause) {
    return transform_cypher_clause_with_where(cpstate, transform_cypher_match_pattern, clause);
}

/*
 * Transform the clause into a subquery. This subquery will be used
 * in a join so setup the namespace item and the created the rtr
 * for the join to use.
 */
static Node *transform_clause_for_join(cypher_parsestate *cpstate, cypher_clause *clause,
                                       RangeTblEntry **rte, ParseNamespaceItem **nsitem, Alias* alias) {
    RangeTblRef *rtr;

    *nsitem = transform_cypher_clause_as_subquery(cpstate, transform_cypher_clause, clause, alias, false);
    *rte = (*nsitem)->p_rte;

    rtr = makeNode(RangeTblRef);
    rtr->rtindex = (*nsitem)->p_rtindex;

    return (Node *) rtr;
}

/*
 * For cases where we need to join two subqueries together (OPTIONAL MATCH and
 * MERGE) we need to take the columns available in each rte and merge them
 * together. The l_rte has precedence when there is a conflict, because that
 * means that the pattern create in the current clause is referencing a
 * variable declared in a previous clause (the l_rte). The output is the
 * res_colnames and res_colvars that are passed in.
 */
static void get_res_cols(ParseState *pstate, ParseNamespaceItem *l_pnsi,
                         ParseNamespaceItem *r_pnsi, List **res_colnames, List **res_colvars) {
    List *l_colnames, *l_colvars;
    List *r_colnames, *r_colvars;
    ListCell *r_lname, *r_lvar;
    List *colnames = NIL;
    List *colvars = NIL;

    expandRTE(l_pnsi->p_rte, l_pnsi->p_rtindex, 0, -1, false, &l_colnames, &l_colvars);
    expandRTE(r_pnsi->p_rte, r_pnsi->p_rtindex, 0, -1, false, &r_colnames, &r_colvars);

    // add in all colnames and colvars from the l_rte.
    *res_colnames = list_concat(*res_colnames, l_colnames);
    *res_colvars = list_concat(*res_colvars, l_colvars);

    // find new columns and if they are a var, pass them in.
    forboth(r_lname, r_colnames, r_lvar, r_colvars) {
        char *r_colname = strVal(lfirst(r_lname));
        ListCell *lname;
        ListCell *lvar;
        Var *var = NULL;

        forboth(lname, *res_colnames, lvar, *res_colvars) {
            char *colname = strVal(lfirst(lname));

            if (strcmp(r_colname, colname) == 0) {
                var = lfirst(lvar);
                break;
            }
        }

        if (var == NULL) {
            colnames = lappend(colnames, lfirst(r_lname));
            colvars = lappend(colvars, lfirst(r_lvar));
        }
    }

    *res_colnames = list_concat(*res_colnames, colnames);
    *res_colvars = list_concat(*res_colvars, colvars);
}

/*
 * transform_cypher_match_clause
 *      Transform the previous clauses and OPTIONAL MATCH clauses to be LATERAL LEFT JOIN
 *   transform_cypher_match_clause   to construct a result value.
 */
static RangeTblEntry *transform_cypher_match_clause(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_clause *prevclause;
    RangeTblEntry *l_rte, *r_rte;
    ParseNamespaceItem *l_nsitem, *r_nsitem;
    ParseState *pstate = (ParseState *) cpstate;
    JoinExpr* j = makeNode(JoinExpr);
    List *res_colnames = NIL, *res_colvars = NIL;
    Alias *l_alias, *r_alias;
    ParseNamespaceItem *jnsitem;
    int i = 0;

    j->jointype = JOIN_INNER;

    l_alias = makeAlias(PREV_CYPHER_CLAUSE_ALIAS, NIL);
    r_alias = makeAlias(CYPHER_OPT_RIGHT_ALIAS, NIL);

    j->larg = transform_clause_for_join(cpstate, clause->prev, &l_rte, &l_nsitem, l_alias);
    pstate->p_namespace = lappend(pstate->p_namespace, l_nsitem);
        
    /*
     * Remove the previous clause so when the transform_clause_for_join function
     * transforms the OPTIONAL MATCH, the previous clause will not be transformed
     * again.
     */
    prevclause = clause->prev;
    clause->prev = NULL;

    //set the lateral flag to true
    pstate->p_lateral_active = true;

    j->rarg = transform_clause_for_join(cpstate, clause, &r_rte, &r_nsitem, r_alias);

    // we are done transform the lateral left join
    pstate->p_lateral_active = false;

    /*
     * We are done with the previous clause in the transform phase, but
     * reattach the previous clause for semantics.
     */
    clause->prev = prevclause;

    pstate->p_namespace = NIL;

    // get the colnames and colvars from the rtes
    get_res_cols(pstate, l_nsitem, r_nsitem, &res_colnames, &res_colvars);

    jnsitem = addRangeTableEntryForJoin(pstate, res_colnames, NULL, j->jointype, 0, res_colvars, NIL,
                                        NIL, j->alias, NULL, false);

    j->rtindex = jnsitem->p_rtindex;

    for (i = list_length(pstate->p_joinexprs) + 1; i < j->rtindex; i++)
        pstate->p_joinexprs = lappend(pstate->p_joinexprs, NULL);
    pstate->p_joinexprs = lappend(pstate->p_joinexprs, j);
    Assert(list_length(pstate->p_joinexprs) == j->rtindex);

    pstate->p_joinlist = lappend(pstate->p_joinlist, j);

    // add jrte to column namespace only 
    addNSItemToQuery(pstate, jnsitem, false, false, true);

    return jnsitem->p_rte;
}

static Query *transform_cypher_match_pattern(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_match *self = (cypher_match *)clause->self;
    Query *query;
    Node *where = self->where;
    Node *order_by = self->order_by;
    self->order_by = NULL;
    query = makeNode(Query);
    query->commandType = CMD_SELECT;

    // If there is no previous clause, transform to a general MATCH clause.
    if (clause->prev && (self->order_by)) {
        RangeTblEntry *rte = transform_cypher_match_clause(cpstate, clause);

        query->targetList = make_target_list_from_join(pstate, rte);
        query->rtable = pstate->p_rtable;
        query->jointree = makeFromExpr(pstate->p_joinlist, NULL);
    } else if (clause->prev) {
        ParseNamespaceItem *pnsi = transform_prev_cypher_clause(cpstate, clause->prev, true);

        /*
         * add all the target entries in rte to the current target list to pass
         * all the variables that are introduced in the previous clause to the
         * next clause
         */
        query->targetList = expandNSItemAttrs(pstate, get_namespace_item(pstate, pnsi->p_rte), 0, -1);
        transform_match_pattern(cpstate, query, self->pattern, where);
    } else {
        transform_match_pattern(cpstate, query, self->pattern, where);
    }

    // ORDER BY
    query->sortClause = transform_cypher_order_by(cpstate, order_by, &query->targetList, EXPR_KIND_ORDER_BY);

    markTargetListOrigins(pstate, query->targetList);

    query->hasSubLinks = pstate->p_hasSubLinks;
    query->hasWindowFuncs = pstate->p_hasWindowFuncs;
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;
    query->hasAggs = pstate->p_hasAggs;

    assign_query_collations(pstate, query);

    return query;
}


static void transform_match_pattern(cypher_parsestate *cpstate, Query *query, List *pattern, Node *where) {
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *lc;
    List *quals = NIL;

    Expr *expr = NULL;

    foreach (lc, pattern) {
        List *qual = NULL;
        cypher_path *path = (cypher_path *) lfirst(lc);
        // TODO: implement the new match logic 
        //qual = transform_match_path(cpstate, query, path);

        quals = list_concat(quals, NULL);
    }

    // AND the quals for each path together
    Expr *q = NULL;
    /*
    if (quals != NIL) {
        q = makeBoolExpr(AND_EXPR, quals, -1);
        expr = (Expr *)sql_transform_expr(&cpstate->pstate, (Node *)q, EXPR_KIND_WHERE);
    }*/

    /*
    / property constraints on MATCH Pattern
    if (cpstate->property_constraint_quals != NIL) {
        Expr *prop_qual = makeBoolExpr(AND_EXPR, cpstate->property_constraint_quals, -1);

        if (expr == NULL)
            expr = prop_qual;
        else
            expr = makeBoolExpr(AND_EXPR, list_make2(expr, prop_qual), -1);
    }
    */

    
    // WHERE Clause
    /*
    if (where != NULL) {
        Expr *where_qual;

        where_qual = (Expr *)transform_cypher_expr(cpstate, where, EXPR_KIND_WHERE);
        if (expr == NULL) {
            expr = where_qual;
        } else {
            where_qual = (Expr *)coerce_to_boolean(pstate, (Node *)where_qual, "WHERE");

            expr = makeBoolExpr(AND_EXPR, list_make2(expr, where_qual), -1);
        }
    }
    */

    /*
     * Coerce to WHERE clause to a bool, denoting whether the constructed
     * clause is true or false.
     */
    //if (expr != NULL)
    //    expr = (Expr *)coerce_to_boolean(pstate, (Node *)expr, "WHERE");

    query->rtable = cpstate->pstate.p_rtable;
    //query->jointree = makeFromExpr(cpstate->pstate.p_joinlist, (Node *)expr);
    query->jointree = makeFromExpr(cpstate->pstate.p_joinlist, (Node *)NULL);
}


static List *make_target_list_from_join(ParseState *pstate, RangeTblEntry *rte) {
    List *targetlist = NIL;
    ListCell *lt;
    ListCell *ln;

    AssertArg(rte->rtekind == RTE_JOIN);

    forboth(lt, rte->joinaliasvars, ln, rte->eref->colnames) {
        Var *varnode = lfirst(lt);
        char *resname = strVal(lfirst(ln));
        TargetEntry *tmp;

        tmp = makeTargetEntry((Expr *) varnode, (AttrNumber) pstate->p_next_resno++, pstrdup(resname), false);
        targetlist = lappend(targetlist, tmp);
    }

    return targetlist;
}



/*
 * This function is similar to transformFromClause() that is called with a
 * single RangeSubselect.
 */
static ParseNamespaceItem *
transform_cypher_clause_as_subquery(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause,
                                    Alias *alias, bool add_rte_to_query) {
    ParseState *pstate = (ParseState *)cpstate;
    ParseExprKind old_expr_kind = pstate->p_expr_kind;
    bool lateral = pstate->p_lateral_active;

    /*
     * We allow expression kinds of none, where, and subselect. Others MAY need
     * to be added depending. However, at this time, only these are needed.
     */
    Assert(pstate->p_expr_kind == EXPR_KIND_NONE || pstate->p_expr_kind == EXPR_KIND_OTHER ||
           pstate->p_expr_kind == EXPR_KIND_WHERE || pstate->p_expr_kind == EXPR_KIND_FROM_SUBSELECT);

    /*
     * As these are all sub queries, if this is just of type NONE, note it as a
     * SUBSELECT. Other types will be dealt with as needed.
     */
    if (pstate->p_expr_kind == EXPR_KIND_NONE) {
        pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;
    } else if (pstate->p_expr_kind == EXPR_KIND_OTHER) {
        // this is a lateral subselect for the MERGE
        pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;
        lateral = true;
    }
    /*
     * If this is a WHERE, pass it through and set lateral to true because it
     * needs to see what comes before it.
     */
    Query *query = analyze_cypher_clause(transform, clause, cpstate);

    pstate->p_expr_kind = old_expr_kind;

    if (!alias)
        alias = makeAlias(PREV_CYPHER_CLAUSE_ALIAS, NIL);

    ParseNamespaceItem *pnsi = addRangeTableEntryForSubquery(pstate, query, alias, lateral, true);

    /*
     * NOTE: skip namespace conflicts check if the rte will be the only
     *       RangeTblEntry in pstate
     */
    if (list_length(pstate->p_rtable) > 1) {
        List *namespace = NULL;
        int rtindex = 0;

        rtindex = list_length(pstate->p_rtable);

        if (pnsi->p_rte != rt_fetch(rtindex, pstate->p_rtable))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("rte must be last entry in p_rtable")));

        namespace = list_make1(pnsi);

        checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);
    }

    if (add_rte_to_query)
        addNSItemToQuery(pstate, pnsi, true, false, true);

    return pnsi;
}

static Query *analyze_cypher_clause(transform_method transform, cypher_clause *clause, cypher_parsestate *parent_cpstate) {
    cypher_parsestate *cpstate;
    Query *query;
    ParseState *parent_pstate = (ParseState*)parent_cpstate;
    ParseState *pstate;

    cpstate = make_cypher_parsestate(parent_cpstate);
    pstate = (ParseState*)cpstate;

    // copy the expr_kind down to the child
    pstate->p_expr_kind = parent_pstate->p_expr_kind;

    if (pstate->p_expr_kind == EXPR_KIND_WHERE) {
        cpstate->entities = list_concat(NIL, parent_cpstate->entities);
    }

    query = transform(cpstate, clause);

    advance_transform_entities_to_next_clause(cpstate->entities);

    parent_cpstate->entities = list_concat(parent_cpstate->entities, cpstate->entities);

    free_cypher_parsestate(cpstate);

    return query;
}

/*
 * When we are done transforming a clause, before transforming the next clause
 * iterate through the transform entities and mark them as not belonging to
 * the clause that is currently being transformed.
 */
static void advance_transform_entities_to_next_clause(List *entities) {
    ListCell *lc;

    foreach (lc, entities) {
        transform_entity *entity = lfirst(lc);

        entity->declared_in_current_clause = false;
    }
}

/*
 * from postgresql parse_sub_analyze
 * Entry point for recursively analyzing a sub-statement.
 */
Query *cypher_parse_sub_analyze(Node *parseTree, cypher_parsestate *cpstate, CommonTableExpr *parentCTE,
                                bool locked_from_parent, bool resolve_unknowns) {
    ParseState *pstate = make_parsestate((ParseState*)cpstate);
    cypher_clause *clause;
    Query *query;

    pstate->p_parent_cte = parentCTE;
    pstate->p_locked_from_parent = locked_from_parent;
    pstate->p_resolve_unknowns = resolve_unknowns;

    clause = palloc0(sizeof(cypher_clause));
    clause->self = parseTree;
    query = transform_cypher_clause(cpstate, clause);

    free_parsestate(pstate);

    return query;
}

/*
 * Get a namespace item for the given rte.
 */
static ParseNamespaceItem *get_namespace_item(ParseState *pstate, RangeTblEntry *rte) {
    ParseNamespaceItem *pnsi;
    ListCell *l;

    foreach(l, pstate->p_namespace) {
        pnsi = lfirst(l);
        if (rte == pnsi->p_rte)
            return pnsi;
    }

    return NULL;
}
