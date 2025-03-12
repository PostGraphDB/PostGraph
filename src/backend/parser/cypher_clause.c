/*
 * For PostgreSQL Database Management System:
 * (formerly known as Postgres, then as Postgres95)
 *
 * Portions Copyright (c) 1996-2010, The PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * Permission to use, copy, modify, and distribute this software and its documentation for any purpose,
 * without fee, and without a written agreement is hereby granted, provided that the above copyright notice
 * and this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY
 * OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA
 * HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

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

#define INCLUDE_NODE_IN_JOIN_TREE(path, node) \
    (path->var_name || node->name || node->props)

typedef Query *(*transform_method)(cypher_parsestate *cpstate, cypher_clause *clause);

// projection
static Query *transform_cypher_return(cypher_parsestate *cpstate, cypher_clause *clause);
static TargetEntry *find_target_list_entry(cypher_parsestate *cpstate, Node *node, List **target_list, ParseExprKind expr_kind);
static Node *transform_cypher_limit(cypher_parsestate *cpstate, Node *node, ParseExprKind expr_kind, const char *construct_name);
static Query *transform_cypher_with(cypher_parsestate *cpstate, cypher_clause *clause);
static Query *transform_cypher_clause_with_where(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause);
// match clause
static Query *transform_cypher_match(cypher_parsestate *cpstate, cypher_clause *clause);
static Query *transform_cypher_match_pattern(cypher_parsestate *cpstate, cypher_clause *clause);
static List *transform_match_entities(cypher_parsestate *cpstate, Query *query, cypher_path *path);
static void transform_match_pattern(cypher_parsestate *cpstate, Query *query, List *pattern, Node *where);
static List *transform_match_path(cypher_parsestate *cpstate, Query *query, cypher_path *path);
static Expr *transform_cypher_edge(cypher_parsestate *cpstate, cypher_relationship *rel, List **target_list);
static Expr *transform_cypher_node(cypher_parsestate *cpstate, cypher_node *node, List **target_list, bool output_node);
static Node *make_vertex_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi);
static Node *make_edge_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi);
static Node *make_qual(cypher_parsestate *cpstate, transform_entity *entity, char *name);
static TargetEntry * transform_match_create_path_variable(cypher_parsestate *cpstate, cypher_path *path, List *entities);
static List *make_path_join_quals(cypher_parsestate *cpstate, List *entities);
static List *make_directed_edge_join_conditions( cypher_parsestate *cpstate, transform_entity *prev_entity, transform_entity *next_entity, Node *prev_qual, Node *next_qual, char *prev_node_label, char *next_node_label);
static List *join_to_entity(cypher_parsestate *cpstate, transform_entity *entity, Node *qual, enum transform_entity_join_side side);
static List *make_join_condition_for_edge(cypher_parsestate *cpstate, transform_entity *prev_edge, transform_entity *prev_node, transform_entity *entity, transform_entity *next_node, transform_entity *next_edge);
static List *make_edge_quals(cypher_parsestate *cpstate, transform_entity *edge, enum transform_entity_join_side side);
static A_Expr *filter_vertices_on_label_id(cypher_parsestate *cpstate, Node *id_field, char *label);
static Node *create_property_constraints(cypher_parsestate *cpstate, transform_entity *entity, Node *property_constraints);
static TargetEntry *findTarget(List *targetList, char *resname);
static transform_entity *transform_VLE_edge_entity(cypher_parsestate *cpstate, cypher_relationship *rel, Query *query, FuncCall *func);
static ParseNamespaceItem *add_vle_to_query(cypher_parsestate *cpstate, Node *n);
// create clause
static Query *transform_cypher_create(cypher_parsestate *cpstate, cypher_clause *clause);
static List *transform_cypher_create_pattern(cypher_parsestate *cpstate, Query *query, List *pattern);
static cypher_create_path * transform_cypher_create_path(cypher_parsestate *cpstate, List **target_list, cypher_path *cp);
static cypher_target_node * transform_create_cypher_node(cypher_parsestate *cpstate, List **target_list, cypher_node *node); static cypher_target_node *
transform_create_cypher_new_node(cypher_parsestate *cpstate, List **target_list, cypher_node *node);
static cypher_target_node *transform_create_cypher_existing_node( cypher_parsestate *cpstate, List **target_list, bool declared_in_current_clause, cypher_node *node);
static cypher_target_node *
transform_create_cypher_edge(cypher_parsestate *cpstate, List **target_list, cypher_relationship *edge);
static Expr *cypher_create_properties(cypher_parsestate *cpstate, cypher_target_node *rel, Relation label_relation, Node *props, enum transform_entity_type type);
static Expr *add_volatile_wrapper(Expr *node);
static Expr *add_volatile_edge_wrapper(Expr *node);
static Expr *add_volatile_vertex_wrapper(Expr *node);
static Expr *add_volatile_vle_edge_wrapper(Expr *node);
static bool variable_exists(cypher_parsestate *cpstate, char *name);
static int get_target_entry_resno(cypher_parsestate *cpstate, List *target_list, char *name);
static void handle_prev_clause(cypher_parsestate *cpstate, Query *query, cypher_clause *clause, bool first_rte);
static Query *transform_cypher_sub_pattern(cypher_parsestate *cpstate, cypher_clause *clause);
// set and remove clause
static Query *transform_cypher_set(cypher_parsestate *cpstate, cypher_clause *clause);
static cypher_update_information *transform_cypher_set_item_list(cypher_parsestate *cpstate, List *set_item_list, Query *query);
static cypher_update_information *transform_cypher_remove_item_list(cypher_parsestate *cpstate, List *remove_item_list, Query *query);
// delete
static Query *transform_cypher_delete(cypher_parsestate *cpstate, cypher_clause *clause);
static List *transform_cypher_delete_item_list(cypher_parsestate *cpstate, List *delete_item_list, Query *query);
//set operators
static Query *transform_cypher_union(cypher_parsestate *cpstate, cypher_clause *clause);
static Node * transform_cypher_union_tree(cypher_parsestate *cpstate, cypher_clause *clause, bool isTopLevel, List **targetlist);
Query *cypher_parse_sub_analyze_union(cypher_clause *clause, cypher_parsestate *cpstate, CommonTableExpr *parentCTE, bool locked_from_parent, bool resolve_unknowns);
static void get_res_cols(ParseState *pstate, ParseNamespaceItem *l_pnsi, ParseNamespaceItem *r_pnsi, List **res_colnames, List **res_colvars);
// unwind
static Query *transform_cypher_unwind(cypher_parsestate *cpstate, cypher_clause *clause);
// merge
static Query *transform_cypher_merge(cypher_parsestate *cpstate, cypher_clause *clause);
static cypher_create_path *transform_merge_make_lateral_join(cypher_parsestate *cpstate, Query *query, cypher_clause *clause, cypher_clause *isolated_merge_clause);
static cypher_create_path *transform_cypher_merge_path(cypher_parsestate *cpstate, List **target_list, cypher_path *path);
static cypher_target_node *transform_merge_cypher_edge(cypher_parsestate *cpstate, List **target_list, cypher_relationship *edge);
static cypher_target_node *transform_merge_cypher_node(cypher_parsestate *cpstate, List **target_list, cypher_node *node);
static Node *transform_clause_for_join(cypher_parsestate *cpstate, cypher_clause *clause, RangeTblEntry **rte, ParseNamespaceItem **nsitem, Alias* alias);
static cypher_clause *convert_merge_to_match(cypher_merge *merge);
static void transform_cypher_merge_mark_tuple_position(cypher_parsestate *cpstate, List *target_list, cypher_create_path *path);
static TargetEntry *placeholder_vertex(cypher_parsestate *cpstate, char *name);
static TargetEntry *placeholder_edge(cypher_parsestate *cpstate, char *name);
static TargetEntry *placeholder_traversal(cypher_parsestate *cpstate, char *name);
// call
static Query *transform_cypher_call(cypher_parsestate *cpstate, cypher_clause *clause);
// load csv
static Query *transform_cypher_load_csv(cypher_parsestate *cpstate, cypher_clause *clause);
// transform
#define PREV_CYPHER_CLAUSE_ALIAS    "_"
#define CYPHER_OPT_RIGHT_ALIAS      "_R"
#define transform_prev_cypher_clause(cpstate, prev_clause, add_rte_to_query) \
    transform_cypher_clause_as_subquery(cpstate, transform_cypher_clause, \
                                        prev_clause, NULL, add_rte_to_query)
static ParseNamespaceItem *transform_cypher_clause_as_subquery(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause, Alias *alias, bool add_rte_to_query);
static Query *analyze_cypher_clause(transform_method transform, cypher_clause *clause, cypher_parsestate *parent_cpstate);
static List *transform_group_clause(cypher_parsestate *cpstate, List *grouplist, List **groupingSets, List **targetlist, List *sortClause, ParseExprKind exprKind);
static Node *flatten_grouping_sets(Node *expr, bool toplevel, bool *hasGroupingSets);
static Index transform_group_clause_expr(List **flatresult, Bitmapset *seen_local, cypher_parsestate *cpstate, Node *gexpr, List **targetlist, List *sortClause, ParseExprKind exprKind, bool toplevel);
static List *add_target_to_group_list(cypher_parsestate *cpstate, TargetEntry *tle, List *grouplist, List *targetlist, int location);
static void advance_transform_entities_to_next_clause(List *entities);
static ParseNamespaceItem *get_namespace_item(ParseState *pstate, RangeTblEntry *rte);
static List *make_target_list_from_join(ParseState *pstate, RangeTblEntry *rte);
static Expr *add_volatile_wrapper(Expr *node);
static FuncExpr *make_clause_func_expr(char *function_name, Node *clause_information);
static ParseNamespaceItem *transform_RangeFunction(cypher_parsestate *cpstate, RangeFunction *r);
static Node *transform_VLE_Function(cypher_parsestate *cpstate, Node *n, RangeTblEntry **top_rte, int *top_rti, List **namespace);
static void setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok);


// utils XXX: Should be moved into its own file
RangeFunction *make_range_function(FuncCall *func, Alias *alias, bool is_lateral, bool ordinality, bool is_rows_from);
Alias *make_alias(char *name, List *colnames);
static char *make_property_alias(char *var_name);
static char *make_id_alias(char *var_name);
static char *make_startid_alias(char *var_name);
static char *make_endid_alias(char *var_name);
List *
transform_window_definitions(ParseState *pstate, List *windowdefs, List **targetlist);
static ParseNamespaceItem *
transform_cypher_clause_as_subquery_2(cypher_parsestate *cpstate, cypher_clause *clause, Alias *alias, bool add_rte_to_query, Query *query);
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
        else if (n->op == SETOP_UNION || n->op == SETOP_INTERSECT || n->op == SETOP_EXCEPT)
            result = transform_cypher_union(cpstate, clause);
        else
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_return")));
    } else if (is_ag_node(self, cypher_with)) {
        result = transform_cypher_with(cpstate, clause);
    } else if (is_ag_node(self, cypher_call)) {
        result = transform_cypher_call(cpstate, clause);
    } else if (is_ag_node(self, cypher_match)) {
        result = transform_cypher_match(cpstate, clause);
    } else if (is_ag_node(self, cypher_create)) {
        result = transform_cypher_create(cpstate, clause);
    } else if (is_ag_node(self, cypher_set)) {
        result = transform_cypher_set(cpstate, clause);
    } else if (is_ag_node(self, cypher_delete)) {
        result = transform_cypher_delete(cpstate, clause);
    } else if (is_ag_node(self, cypher_merge)) {
        result = transform_cypher_merge(cpstate, clause);
    } else if (is_ag_node(self, cypher_sub_pattern)) {
        result = transform_cypher_sub_pattern(cpstate, clause);
    } else if (is_ag_node(self, cypher_unwind)) {
        result = transform_cypher_unwind(cpstate, clause);
    } else if (is_ag_node(self, cypher_load_csv)) {
        result = transform_cypher_load_csv(cpstate, clause);
    } else {
        ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));
    }

    result->querySource = QSRC_ORIGINAL;
    result->canSetTag = true;

    return result;
}

static A_Expr *
makeSimpleCypherA_Expr(A_Expr_Kind kind, char *name,
				 Node *lexpr, Node *rexpr, int location)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->kind = kind;
	a->name = list_make2(makeString("postgraph"), makeString((char *) name));
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	a->location = location;
	return a;
}

/*
 * Transform the UNION operator/clause. Creates a cypher_union
 * node and the necessary information needed in the execution
 * phase
 */
static cypher_clause *make_cypher_clause(List *stmt) {
    cypher_clause *clause;
    ListCell *lc;

    /*
     * Since the first clause in stmt is the innermost subquery, the order of
     * the clauses is inverted.
     */
    clause = NULL;
    foreach (lc, stmt) {
        cypher_clause *next;

        next = palloc0(sizeof(*next));
        next->next = NULL;
        next->self = lfirst(lc);
        next->prev = clause;

        if (clause != NULL)
            clause->next = next;
        clause = next;
    }
    return clause;
}

static Node *transform_srf_function(cypher_parsestate *cpstate, Node *n, RangeTblEntry **top_rte, int *top_rti, List **namespace);

static ParseNamespaceItem *add_srf_to_query(cypher_parsestate *cpstate, Node *n, char *var_name) {
    RangeTblEntry *rte = NULL;
    RangeTblRef *rtr;
    List *namespace = NULL;
    int rtindex;
    ParseState *pstate = (ParseState *)cpstate;

    Alias *alias = make_alias(var_name, NIL);
    RangeFunction *rf = make_range_function(n, alias, true, false, false);


    rtr = transform_srf_function(cpstate, rf, &rte, &rtindex, &namespace);
    Assert(rtr != NULL);

    checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);

    setNamespaceLateralState(namespace, true, true);

    pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
    pstate->p_namespace = list_concat(pstate->p_namespace, namespace);

    setNamespaceLateralState(pstate->p_namespace, true, true);

    return lfirst(list_head(namespace));
}

static Node *transform_srf_function(cypher_parsestate *cpstate, Node *n, RangeTblEntry **top_rte, int *top_rti, List **namespace) {
    ParseState *pstate = (ParseState *)cpstate;
    
    Assert(IsA(n, RangeFunction));
    
    if (IsA(n, RangeFunction)) {
        RangeTblRef *rtr;
        RangeTblEntry *rte;
        ParseNamespaceItem *nsitem;
        int rtindex;
    
        nsitem = transform_RangeFunction(cpstate, (RangeFunction *) n);
        rte = nsitem->p_rte;
        rtindex = list_length(pstate->p_rtable);
        Assert(rte == rt_fetch(rtindex, pstate->p_rtable));
        *top_rte = rte;
        *top_rti = rtindex;
        *namespace = list_make1(nsitem);
        rtr = makeNode(RangeTblRef);
        rtr->rtindex = rtindex;
        return (Node *) rtr;
    }
    
    return NULL;
}

static Query *transform_cypher_load_csv(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_parsestate *child_cpstate = make_cypher_parsestate(cpstate);
    ParseState *pstate = (ParseState *) child_cpstate;
    cypher_load_csv *load = clause->self;
    TargetEntry *te;
    ParseNamespaceItem *pnsi;
    FuncCall *fc = makeFuncCall(list_make2(makeString("postgraph"), makeString("cypher_load_csv")),
                                list_make1(makeString(load->file)),
                                COERCE_SQL_SYNTAX,
                                -1);

    Query *query = makeNode(Query);
    query->commandType = CMD_SELECT;

    /*if (clause->prev) {
        int rtindex;

        pnsi = transform_prev_cypher_clause(child_cpstate, clause->prev, true);
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate
        query->targetList = expandNSItemAttrs(pstate, pnsi, 0, -1);
    }*/

    Expr *expr = NULL;
    //if (!call->where)
     {
        expr = transform_cypher_expr(child_cpstate, fc, EXPR_KIND_SELECT_TARGET);
        //te = makeTargetEntry((Expr *) expr, (AttrNumber) pstate->p_next_resno++, call->alias, false);
        te = makeTargetEntry((Expr *) expr, (AttrNumber) pstate->p_next_resno++, "test", false);

        expr = NULL;
        query->targetList = lappend(query->targetList, te);
    } /*else {
       pnsi = add_srf_to_query(child_cpstate, call->func, call->alias);
       Node *var = scanNSItemForColumn(pstate, pnsi, 0, call->alias, -1);
       te = makeTargetEntry((Expr *) var, (AttrNumber) pstate->p_next_resno++, call->alias, false);
       query->targetList = lappend(query->targetList, te);

       transform_entity *entity = make_transform_entity(child_cpstate, ENT_FUNC_CALL, NULL, (Node *)var, call->alias);
       child_cpstate->entities = lappend(child_cpstate->entities, entity);


       expr = transform_cypher_expr(child_cpstate, call->where, EXPR_KIND_WHERE);
    }*/

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, expr);
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;

    assign_query_collations(pstate, query);

    free_cypher_parsestate(child_cpstate);
    return query;
}

static Query *transform_cypher_call(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_parsestate *child_cpstate = make_cypher_parsestate(cpstate);
    ParseState *pstate = (ParseState *) child_cpstate;
    cypher_call *call = clause->self;
    TargetEntry *te;
    ParseNamespaceItem *pnsi;


    Query *query = makeNode(Query);
    query->commandType = CMD_SELECT;


    if (call->cck == CCK_CYPHER_SUBQUERY) {
        cypher_clause *call_clause = make_cypher_clause(call->cypher);

        //ereport(ERROR, (errmsg_internal("Call only supports set-returning functions at this time")));
        Node *self = call_clause->self;

        // examine the type of clause and call the transform logic for it
        if (!is_ag_node(self, cypher_return)) 
            ereport(ERROR, (errmsg_internal("Call Cypher Subquery must end with a RETURN")));

        if (clause->prev) {
ereport(ERROR, (errmsg_internal("Call only supports set-returning functions at this time")));
             /*         RangeTblEntry *rte;
            int rtindex;
            ParseNamespaceItem *pnsi;

            pnsi = transform_prev_cypher_clause(child_cpstate, clause->prev, true);
            rte = pnsi->p_rte;
            rtindex = list_length(pstate->p_rtable);
            Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate

            pnsi = get_namespace_item(pstate, rte);
            query->targetList = expandNSItemAttrs(pstate, pnsi, 0, -1);
            transform_match_pattern(child_cpstate, query, self->pattern, where);
        } else if () {
        RangeTblEntry *rte = transform_cypher_match_clause(cpstate, clause);

        query->targetList = make_target_list_from_join(pstate, rte);
        query->rtable = pstate->p_rtable;
        query->jointree = makeFromExpr(pstate->p_joinlist, NULL);
 */
    } else {
            return transform_cypher_clause(child_cpstate, call_clause);
        }

    markTargetListOrigins(pstate, query->targetList);

    query->hasSubLinks = pstate->p_hasSubLinks;
    query->hasWindowFuncs = pstate->p_hasWindowFuncs;
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;
    query->hasAggs = pstate->p_hasAggs;

    assign_query_collations(pstate, query);

    return query;

    } else {
    if (call->cck != CCK_FUNCTION)
        ereport(ERROR, (errmsg_internal("Call only supports set-returning functions at this time")));

    if (clause->prev) {
        int rtindex;

        pnsi = transform_prev_cypher_clause(child_cpstate, clause->prev, true);
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate
        query->targetList = expandNSItemAttrs(pstate, pnsi, 0, -1);
    }

    Expr *expr = NULL;
    if (!call->where) {
        expr = transform_cypher_expr(child_cpstate, call->func, EXPR_KIND_SELECT_TARGET);
        te = makeTargetEntry((Expr *) expr, (AttrNumber) pstate->p_next_resno++, call->alias, false);

        expr = NULL;
        query->targetList = lappend(query->targetList, te);
    } else {
       pnsi = add_srf_to_query(child_cpstate, call->func, call->alias);
       Node *var = scanNSItemForColumn(pstate, pnsi, 0, call->alias, -1);
       te = makeTargetEntry((Expr *) var, (AttrNumber) pstate->p_next_resno++, call->alias, false);
       query->targetList = lappend(query->targetList, te);

       transform_entity *entity = make_transform_entity(child_cpstate, ENT_FUNC_CALL, NULL, (Node *)var, call->alias);
       child_cpstate->entities = lappend(child_cpstate->entities, entity);


       expr = transform_cypher_expr(child_cpstate, call->where, EXPR_KIND_WHERE);
    }

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, expr);
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;

    assign_query_collations(pstate, query);

    free_cypher_parsestate(child_cpstate);
    return query;
    }
}

/*
 * transform_cypher_union -
 *    transforms a union tree, derived from postgresql's
 *    transformSetOperationStmt. A lot of the general logic is similar,
 *    with adjustments made for AGE.
 *
 * A union tree is just a return, but with UNION structure to it.
 * We must transform each leaf SELECT and build up a top-level Query
 * that contains the leaf SELECTs as subqueries in its rangetable.
 * The tree of unions is converted into the setOperations field of
 * the top-level Query.
 */
static Query *transform_cypher_union(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    Query *qry = makeNode(Query);
    int leftmostRTI;
    Query *leftmostQuery;
    SetOperationStmt *cypher_union_statement;
    Node *skip = NULL; // equivalent to postgres limitOffset 
    Node *limit = NULL; // equivalent to postgres limitCount 
    List *order_by = NIL;
    Node *node;
    cypher_return *self = (cypher_return *)clause->self;
    ListCell *left_tlist, *lct, *lcm, *lcc;
    List *targetvars, *targetnames, *sv_namespace;
    int sv_rtable_length;
    int tllen;
    ParseNamespaceItem *nsitem;
    ParseNamespaceColumn *sortnscolumns;
    int sortcolindex;

    qry->commandType = CMD_SELECT;

    /*
     * Union is a node that should never have a previous node because
     * of where it is used in the parse logic. The query parts around it
     * are children located in larg or rarg. Something went wrong if the
     * previous clause field is not null.
     */
    if (clause->prev)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Union is a parent node, there are no previous"),
                        parser_errposition(&cpstate->pstate, 0)));

    order_by = self->order_by;
    skip = self->skip;
    limit = self->limit;

    self->order_by = NIL;
    self->skip = NULL;
    self->limit = NULL;

    // Recursively transform the components of the tree.
    cypher_union_statement = (SetOperationStmt *) transform_cypher_union_tree(cpstate, clause, true, NULL);

    Assert(cypher_union_statement);
    qry->setOperations = (Node *) cypher_union_statement;

    /*
     * Re-find leftmost return (now it's a sub-query in rangetable)
     */
    node = cypher_union_statement->larg;
    while (node && IsA(node, SetOperationStmt))
        node = ((SetOperationStmt *) node)->larg;
    
    Assert(node && IsA(node, RangeTblRef));
    leftmostRTI = ((RangeTblRef *) node)->rtindex;
    leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
    Assert(leftmostQuery != NULL);

    /*
     * Generate dummy targetlist for outer query using column names of
     * leftmost return and common datatypes/collations of topmost set
     * operation.  Also make lists of the dummy vars and their names for use
     * in parsing ORDER BY.
     *
     * Note: we use leftmostRTI as the varno of the dummy variables. It
     * shouldn't matter too much which RT index they have, as long as they
     * have one that corresponds to a real RT entry; else funny things may
     * happen when the tree is mashed by rule rewriting.
     */
    qry->targetList = NIL;
    targetvars = NIL;
    targetnames = NIL;
    sortnscolumns = (ParseNamespaceColumn *)
        palloc0(list_length(cypher_union_statement->colTypes) * sizeof(ParseNamespaceColumn));
    sortcolindex = 0;

    forfour(lct, cypher_union_statement->colTypes, lcm, cypher_union_statement->colTypmods,
            lcc, cypher_union_statement->colCollations, left_tlist, leftmostQuery->targetList) {
        Oid colType = lfirst_oid(lct);
        int32 colTypmod = lfirst_int(lcm);
        Oid colCollation = lfirst_oid(lcc);
        TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
        char *colName;
        TargetEntry *tle;
        Var *var;

        Assert(!lefttle->resjunk);
        colName = pstrdup(lefttle->resname);
        var = makeVar(leftmostRTI, lefttle->resno, colType, colTypmod, colCollation, 0);
        var->location = exprLocation((Node *) lefttle->expr);
        tle = makeTargetEntry((Expr *) var, (AttrNumber) pstate->p_next_resno++, colName, false);
        qry->targetList = lappend(qry->targetList, tle);
        targetvars = lappend(targetvars, var);
        targetnames = lappend(targetnames, makeString(colName));
        sortnscolumns[sortcolindex].p_varno = leftmostRTI;
        sortnscolumns[sortcolindex].p_varattno = lefttle->resno;
        sortnscolumns[sortcolindex].p_vartype = colType;
        sortnscolumns[sortcolindex].p_vartypmod = colTypmod;
        sortnscolumns[sortcolindex].p_varcollid = colCollation;
        sortnscolumns[sortcolindex].p_varnosyn = leftmostRTI;
        sortnscolumns[sortcolindex].p_varattnosyn = lefttle->resno;
        sortcolindex++;
    }

    /*
     * As a first step towards supporting sort clauses that are expressions
     * using the output columns, generate a namespace entry that makes the
     * output columns visible.  A Join RTE node is handy for this, since we
     * can easily control the Vars generated upon matches.
     *
     * Note: we don't yet do anything useful with such cases, but at least
     * "ORDER BY upper(foo)" will draw the right error message rather than
     * "foo not found".
     */
    sv_rtable_length = list_length(pstate->p_rtable);

    nsitem = addRangeTableEntryForJoin(pstate, targetnames, sortnscolumns, JOIN_INNER, 0, targetvars, NIL, NIL, NULL, NULL, false);

    sv_namespace = pstate->p_namespace;
    pstate->p_namespace = NIL;

    // add jrte to column namespace only 
    addNSItemToQuery(pstate, nsitem, false, false, true);

    tllen = list_length(qry->targetList);

    qry->sortClause = transformSortClause(pstate, order_by, &qry->targetList, EXPR_KIND_ORDER_BY, false);

    // restore namespace, remove jrte from rtable 
    pstate->p_namespace = sv_namespace;
    pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);

    if (tllen != list_length(qry->targetList))
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("invalid UNION ORDER BY clause"),
             errdetail("Only result column names can be used, not expressions or functions."),
             parser_errposition(pstate, exprLocation(list_nth(qry->targetList, tllen)))));

    qry->limitOffset = transform_cypher_limit(cpstate, skip, EXPR_KIND_OFFSET, "OFFSET");
    qry->limitCount = transform_cypher_limit(cpstate, limit, EXPR_KIND_LIMIT, "LIMIT");

    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);
    qry->hasAggs = pstate->p_hasAggs;

    assign_query_collations(pstate, qry);

    // this must be done after collations, for reliable comparison of exprs 
    if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
        parse_check_aggregates(pstate, qry);

    return qry;

}

/*
 * transform_cypher_union_tree
 *      Recursively transform leaves and internal nodes of a set-op tree,
 *      derived from postgresql's transformSetOperationTree. A lot of
 *      the general logic is similar, with adjustments made for AGE.
 *
 * In addition to returning the transformed node, if targetlist isn't NULL
 * then we return a list of its non-resjunk TargetEntry nodes.  For a leaf
 * set-op node these are the actual targetlist entries; otherwise they are
 * dummy entries created to carry the type, typmod, collation, and location
 * (for error messages) of each output column of the set-op node.  This info
 * is needed only during the internal recursion of this function, so outside
 * callers pass NULL for targetlist.  Note: the reason for passing the
 * actual targetlist entries of a leaf node is so that upper levels can
 * replace UNKNOWN Consts with properly-coerced constants.
 */
static Node *
transform_cypher_union_tree(cypher_parsestate *cpstate, cypher_clause *clause, bool isTopLevel, List **targetlist) {
    bool isLeaf;

    ParseState *pstate = (ParseState *)cpstate;
    cypher_return *cmp;
    ParseNamespaceItem *pnsi;

    // Guard against queue overflow due to overly complex set-expressions 
    check_stack_depth();

    if (IsA(clause, List))
        clause = make_cypher_clause((List *)clause);

    if (is_ag_node(clause->self, cypher_return))
        cmp = (cypher_return *) clause->self;
    else
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Cypher found an unsupported node"),
                parser_errposition(pstate, 0)));

    if (cmp->op == SETOP_NONE) {
        Assert(cmp->larg == NULL && cmp->rarg == NULL);
        isLeaf = true;
    } else if (cmp->op == SETOP_UNION | cmp->op == SETOP_EXCEPT | cmp->op == SETOP_INTERSECT) {
        Assert(cmp->larg != NULL && cmp->rarg != NULL);
        if (cmp->order_by || cmp->limit || cmp->skip)
            isLeaf = true;
        else
            isLeaf = false;
    } else {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Cypher found an unsupported SETOP"),
                parser_errposition(pstate, 0)));
    }

    if (isLeaf) {
        //process leaf return 
        Query *returnQuery;
        char returnName[32];
        RangeTblEntry *rte PG_USED_FOR_ASSERTS_ONLY;
        RangeTblRef *rtr;
        ListCell *tl;

        /*
         * Transform SelectStmt into a Query.
         *
         * This works the same as RETURN transformation normally would, except
         * that we prevent resolving unknown-type outputs as TEXT.  This does
         * not change the subquery's semantics since if the column type
         * matters semantically, it would have been resolved to something else
         * anyway.  Doing this lets us resolve such outputs using
         * select_common_type(), below.
         *
         * Note: previously transformed sub-queries don't affect the parsing
         * of this sub-query, because they are not in the toplevel pstate's
         * namespace list.
         */

        /*
         * Convert the List * that the grammar gave us to a cypher_clause.
         * cypher_analyze doesn't do this because the cypher_union clause
         * is hiding it.
         */

        returnQuery = cypher_parse_sub_analyze_union((cypher_clause *) clause, cpstate, NULL, false, false);
        /*
         * Check for bogus references to Vars on the current query level (but
         * upper-level references are okay). Normally this can't happen
         * because the namespace will be empty, but it could happen if we are
         * inside a rule.
         */
        if (pstate->p_namespace && contain_vars_of_level((Node *) returnQuery, 1))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                         errmsg("UNION member statement cannot refer to other relations of same query level"),
                         parser_errposition(pstate, locate_var_of_level((Node *) returnQuery, 1))));

        /*
         * Extract a list of the non-junk TLEs for upper-level processing.
         */
        if (targetlist) {
            *targetlist = NIL;
            foreach(tl, returnQuery->targetList) {
                TargetEntry *tle = (TargetEntry *) lfirst(tl);

                if (!tle->resjunk)
                    *targetlist = lappend(*targetlist, tle);
            }
        }

        /*
         * Make the leaf query be a subquery in the top-level rangetable.
         */
        snprintf(returnName, sizeof(returnName), "*SELECT* %d ", list_length(pstate->p_rtable) + 1);
        pnsi = addRangeTableEntryForSubquery(pstate, returnQuery, makeAlias(returnName, NIL), false, false);
        rte = pnsi->p_rte;
        rtr = makeNode(RangeTblRef);
        // assume new rte is at end 
        rtr->rtindex = list_length(pstate->p_rtable);
        Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
        return (Node *) rtr;
    }
    else //is not a leaf 
    {
        // Process an internal node (set operation node) 
        SetOperationStmt *op = makeNode(SetOperationStmt);
        List *ltargetlist;
        List *rtargetlist;
        ListCell *ltl;
        ListCell *rtl;
        cypher_return *self = (cypher_return *) clause->self;
        const char *context;

        context = "UNION";

        op->op = self->op;
        op->all = self->all_or_distinct;

        /*
         * Recursively transform the left child node.
         */
        op->larg = transform_cypher_union_tree(cpstate, (cypher_clause *) self->larg, false, &ltargetlist);

        /*
         * If we find ourselves processing a recursive CTE here something
         * went horribly wrong. That is an SQL contruct with no parallel in
         * cypher.
         */
        if (isTopLevel && pstate->p_parent_cte && pstate->p_parent_cte->cterecursive)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Cypher does not support recursive CTEs"),
                    parser_errposition(pstate, 0)));

        /*
         * Recursively transform the right child node.
         */
        op->rarg = transform_cypher_union_tree(cpstate, (cypher_clause *) self->rarg, false, &rtargetlist);

        /*
         * Verify that the two children have the same number of non-junk
         * columns, and determine the types of the merged output columns.
         */
        if (list_length(ltargetlist) != list_length(rtargetlist))
            ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("each %s query must have the same number of columns", context),
                     parser_errposition(pstate, exprLocation((Node *) rtargetlist))));

        if (targetlist)
            *targetlist = NIL;

        op->colTypes = NIL;
        op->colTypmods = NIL;
        op->colCollations = NIL;
        op->groupClauses = NIL;

        forboth(ltl, ltargetlist, rtl, rtargetlist) {
            TargetEntry *ltle = (TargetEntry *) lfirst(ltl);
            TargetEntry *rtle = (TargetEntry *) lfirst(rtl);
            Node *lcolnode = (Node *) ltle->expr;
            Node *rcolnode = (Node *) rtle->expr;
            Oid lcoltype = exprType(lcolnode);
            Oid rcoltype = exprType(rcolnode);
            int32 lcoltypmod = exprTypmod(lcolnode);
            int32 rcoltypmod = exprTypmod(rcolnode);
            Node *bestexpr;
            int bestlocation;
            Oid rescoltype;
            int32 rescoltypmod;
            Oid rescolcoll;

            // select common type, same as CASE et al 
            rescoltype = select_common_type(pstate, list_make2(lcolnode, rcolnode), context, &bestexpr);
            bestlocation = exprLocation(bestexpr);
            // if same type and same typmod, use typmod; else default 
            if (lcoltype == rcoltype && lcoltypmod == rcoltypmod)
                rescoltypmod = lcoltypmod;
            else
                rescoltypmod = -1;

            /*
             * Verify the coercions are actually possible.  If not, we'd fail
             * later anyway, but we want to fail now while we have sufficient
             * context to produce an error cursor position.
             *
             * For all non-UNKNOWN-type cases, we verify coercibility but we
             * don't modify the child's expression, for fear of changing the
             * child query's semantics.
             *
             * If a child expression is an UNKNOWN-type Const or Param, we
             * want to replace it with the coerced expression.  This can only
             * happen when the child is a leaf set-op node.  It's safe to
             * replace the expression because if the child query's semantics
             * depended on the type of this output column, it'd have already
             * coerced the UNKNOWN to something else.  We want to do this
             * because (a) we want to verify that a Const is valid for the
             * target type, or resolve the actual type of an UNKNOWN Param,
             * and (b) we want to avoid unnecessary discrepancies between the
             * output type of the child query and the resolved target type.
             * Such a discrepancy would disable optimization in the planner.
             *
             * If it's some other UNKNOWN-type node, eg a Var, we do nothing
             * (knowing that coerce_to_common_type would fail).  The planner
             * is sometimes able to fold an UNKNOWN Var to a constant before
             * it has to coerce the type, so failing now would just break
             * cases that might work.
             */
            if (lcoltype != UNKNOWNOID)
                lcolnode = coerce_to_common_type(pstate, lcolnode, rescoltype, context);
            else if (IsA(lcolnode, Const) || IsA(lcolnode, Param))
                ltle->expr = (Expr *) coerce_to_common_type(pstate, lcolnode, rescoltype, context);

            if (rcoltype != UNKNOWNOID)
                rcolnode = coerce_to_common_type(pstate, rcolnode, rescoltype, context);
            else if (IsA(rcolnode, Const) || IsA(rcolnode, Param))
                rtle->expr = (Expr *) coerce_to_common_type(pstate, rcolnode, rescoltype, context);

            /*
             * Select common collation.  A common collation is required for
             * all set operators except UNION ALL; see SQL:2008 7.13 <query
             * expression> Syntax Rule 15c.  (If we fail to identify a common
             * collation for a UNION ALL column, the curCollations element
             * will be set to InvalidOid, which may result in a runtime error
             * if something at a higher query level wants to use the column's
             * collation.)
             */
            rescolcoll = select_common_collation(pstate, list_make2(lcolnode, rcolnode),
                                                 (op->op == SETOP_UNION && op->all));

            // emit results 
            op->colTypes = lappend_oid(op->colTypes, rescoltype);
            op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);
            op->colCollations = lappend_oid(op->colCollations, rescolcoll);

            /*
             * For all cases except UNION ALL, identify the grouping operators
             * (and, if available, sorting operators) that will be used to
             * eliminate duplicates.
             */
            if (op->op != SETOP_UNION || !op->all) {
                SortGroupClause *grpcl = makeNode(SortGroupClause);
                Oid sortop;
                Oid eqop;
                bool hashable = false;
                ParseCallbackState pcbstate;

                /*
                 * determine the eqop and optional sortop
                 *
                 * NOTE: for UNION, we set hashable to false and pass a NULL to
                 * isHashable in get_sort_group_operators to prevent a logic error
                 * where UNION fails to exclude duplicate results.
                 *
                 */
        setup_parser_errposition_callback(&pcbstate, pstate, bestlocation);
                get_sort_group_operators(rescoltype, false, true, false, &sortop, &eqop, NULL, NULL);
                cancel_parser_errposition_callback(&pcbstate);

                // we don't have a tlist yet, so can't assign sortgrouprefs 
                grpcl->tleSortGroupRef = 0;
                grpcl->eqop = eqop;
                grpcl->sortop = sortop;
                grpcl->nulls_first = false; // OK with or without sortop 
                grpcl->hashable = hashable;

                op->groupClauses = lappend(op->groupClauses, grpcl);
            }

            /*
             * Construct a dummy tlist entry to return.  We use a SetToDefault
             * node for the expression, since it carries exactly the fields
             * needed, but any other expression node type would do as well.
             */
            if (targetlist) {
                SetToDefault *rescolnode = makeNode(SetToDefault);
                TargetEntry *restle;

                rescolnode->typeId = rescoltype;
                rescolnode->typeMod = rescoltypmod;
                rescolnode->collation = rescolcoll;
                rescolnode->location = bestlocation;
                restle = makeTargetEntry((Expr *) rescolnode, 0, NULL, false);
                *targetlist = lappend(*targetlist, restle);
            }
        }

        return (Node *)op;
    }//end else (is not leaf)
}

/*
 * Transform the Delete clause. Creates a _cypher_delete_clause
 * and passes the necessary information that is needed in the
 * execution phase.
 */
static Query *transform_cypher_delete(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_delete *self = (cypher_delete *)clause->self;
    Query *query;
    TargetEntry *tle;
    FuncExpr *func_expr;

    cypher_delete_information *delete_data;

    delete_data = make_ag_node(cypher_delete_information);

    query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    Const *null_const = makeNullConst(GTYPEOID, -1, InvalidOid);
    tle = makeTargetEntry((Expr *)null_const, pstate->p_next_resno++, "_null", false);
    query->targetList = lappend(query->targetList, tle);

    if (!clause->prev)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("DELETE cannot be the first clause in a Cypher query"),
                 parser_errposition(pstate, self->location)));
    else
        handle_prev_clause(cpstate, query, clause->prev, true);

    delete_data->delete_items = transform_cypher_delete_item_list(cpstate, self->exprs, query);
    delete_data->graph_name = cpstate->graph_name;
    delete_data->graph_oid = cpstate->graph_oid;
    delete_data->detach = self->detach;

    if (!clause->next)
        delete_data->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;

    func_expr = make_clause_func_expr(DELETE_CLAUSE_FUNCTION_NAME, (Node *)delete_data);

    // Create the target entry
    tle = makeTargetEntry((Expr *)func_expr, pstate->p_next_resno++, "_delete_clause", false);
    query->targetList = lappend(query->targetList, tle);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);


    if (clause->next)
        return query;

    {
        Query *topquery;
        cypher_parsestate *new_cpstate = make_cypher_parsestate(cpstate);
        topquery = makeNode(Query);
        topquery->commandType = CMD_SELECT;
        topquery->targetList = NIL;

        ParseState *pstate = (ParseState *) cpstate;
        int rtindex;

        ParseNamespaceItem *pnsi = transform_cypher_clause_as_subquery_2(new_cpstate, clause, NULL, false, query);
        topquery->rtable = new_cpstate->pstate.p_rtable;
        topquery->jointree = makeFromExpr(new_cpstate->pstate.p_joinlist, NULL);

        return topquery;
    }
}
static Node *make_bool_const(bool b, int location)
{
    cypher_bool_const *n;

    n = make_ag_node(cypher_bool_const);
    n->boolean = b;
    n->location = location;

    return (Node *)n;
}

/*
 * transform_cypher_unwind
 *      It contains logic to convert the form of an array into a row. Here, we
 *      are simply calling `unnest` function, and the actual transformation
 *      is handled by `unnest` function.
 */
static Query *transform_cypher_unwind(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_unwind *self = (cypher_unwind *) clause->self;
    int target_syntax_loc;
    Query *query;
    Node *expr;
    FuncCall *unwind;
    ParseExprKind old_expr_kind;
    Node *funcexpr;
    TargetEntry *te;
    ParseNamespaceItem *pnsi;
    cypher_parsestate *child_cpstate = make_cypher_parsestate(cpstate);
    ParseState *pstate = (ParseState *) child_cpstate;
    
    query = makeNode(Query);
    query->commandType = CMD_SELECT;

    if (clause->prev) {
        int rtindex;

        pnsi = transform_prev_cypher_clause(child_cpstate, clause->prev, true);
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate
        query->targetList = expandNSItemAttrs(pstate, pnsi, 0, -1);
    }

    target_syntax_loc = exprLocation((const Node *) self->target);

    if (findTarget(query->targetList, self->target->name) != NULL)
        ereport(ERROR, (errcode(ERRCODE_DUPLICATE_ALIAS),
                        errmsg("duplicate variable \"%s\"", self->target->name),
                        parser_errposition((ParseState *) child_cpstate, target_syntax_loc)));

    expr = transform_cypher_expr(child_cpstate, self->target->val, EXPR_KIND_SELECT_TARGET);

    unwind = makeFuncCall(list_make2(makeString(CATALOG_SCHEMA), makeString("unnest")), list_make2(self->target->val, make_bool_const(true, -1)), COERCE_SQL_SYNTAX, -1);
    pnsi = add_srf_to_query(child_cpstate, unwind, self->target->name); 
    Node *var = scanNSItemForColumn(pstate, pnsi, 0, self->target->name, -1);
    te = makeTargetEntry((Expr *) var, (AttrNumber) pstate->p_next_resno++, self->target->name, false);
    query->targetList = lappend(query->targetList, te);

    transform_entity *entity = make_transform_entity(child_cpstate, ENT_FUNC_CALL, NULL, (Node *)var, self->target->name);
    child_cpstate->entities = lappend(child_cpstate->entities, entity);

    if (self->where)
       expr = transform_cypher_expr(child_cpstate, self->where, EXPR_KIND_WHERE);

       /*
    old_expr_kind = pstate->p_expr_kind;
    pstate->p_expr_kind = EXPR_KIND_SELECT_TARGET;
    funcexpr = ParseFuncOrColumn(pstate, unwind->funcname, list_make2(expr, makeBoolConst(true, false)),
                                 pstate->p_last_srf, unwind, false, target_syntax_loc);

    pstate->p_expr_kind = old_expr_kind;

    te = makeTargetEntry((Expr *) funcexpr, (AttrNumber) pstate->p_next_resno++, res->name, false);

    expr = transform_cypher_expr(child_cpstate, call->where, EXPR_KIND_WHERE);

    transform_entity *entity = make_transform_entity(child_cpstate, ENT_FUNC_CALL, NULL, (Node *)var, call->alias);
    child_cpstate->entities = lappend(child_cpstate->entities, entity);

    query->targetList = lappend(query->targetList, te);
    */
    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, expr);
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;

    assign_query_collations(pstate, query);

    free_cypher_parsestate(child_cpstate);
    return query;
}

/*
 * Iterate through the list of items to delete and extract the variable name.
 * Then find the resno that the variable name belongs to.
 */
static List *transform_cypher_delete_item_list(cypher_parsestate *cpstate, List *delete_item_list, Query *query) {
    ParseState *pstate = (ParseState *)cpstate;
    List *items = NIL;
    ListCell *lc;

    foreach(lc, delete_item_list) {
        Node *expr = lfirst(lc);
        ColumnRef *col;
        Value *val, *pos;
        int resno;

        cypher_delete_item *item = make_ag_node(cypher_delete_item);

        if (!IsA(expr, ColumnRef))
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));

        col = (ColumnRef *)expr;

        if (list_length(col->fields) != 1)
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));

        val = linitial(col->fields);

        if (!IsA(val, String))
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));

        resno = get_target_entry_resno(cpstate, query->targetList, val->val.str);

        if (resno == -1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                     errmsg("undefined reference to variable %s in DELETE clause", val->val.str),
                     parser_errposition(pstate, col->location)));

        pos = makeInteger(resno);

        item->var_name = val->val.str;
        item->entity_position = pos;

        items = lappend(items, item);
    }

    return items;
}

static Query *transform_cypher_set(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_set *self = (cypher_set *)clause->self;
    Query *query;
    cypher_update_information *set_items_target_list;
    TargetEntry *tle;
    FuncExpr *func_expr;
    char *clause_name;
    int rtindex;

    query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;
    Const *null_const = makeNullConst(GTYPEOID, -1, InvalidOid);
    tle = makeTargetEntry((Expr *)null_const, pstate->p_next_resno++, "_null", false);
    query->targetList = lappend(query->targetList, tle);

    if (self->is_remove == true)
        clause_name = UPDATE_CLAUSE_REMOVE;
    else
        clause_name = UPDATE_CLAUSE_SET;

    if (!clause->prev)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("%s cannot be the first clause in a Cypher query", clause_name),
                 parser_errposition(pstate, self->location)));
    else
        handle_prev_clause(cpstate, query, clause->prev, true);

    if (self->is_remove == true)
        set_items_target_list = transform_cypher_remove_item_list(cpstate, self->items, query);
    else
        set_items_target_list = transform_cypher_set_item_list(cpstate, self->items, query);

    set_items_target_list->clause_name = clause_name;
    set_items_target_list->graph_name = cpstate->graph_name;
    set_items_target_list->graph_oid = cpstate->graph_oid;

    if (!clause->next)
        set_items_target_list->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;

    func_expr = make_clause_func_expr(SET_CLAUSE_FUNCTION_NAME, (Node *)set_items_target_list);

    // Create the target entry
    tle = makeTargetEntry((Expr *)func_expr, pstate->p_next_resno++, "_set_clause", false);
    query->targetList = lappend(query->targetList, tle);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    if (clause->next)
        return query;

    {
        Query *topquery;
        cypher_parsestate *new_cpstate = make_cypher_parsestate(cpstate);
        topquery = makeNode(Query);
        topquery->commandType = CMD_SELECT;
        topquery->targetList = NIL;

        ParseState *pstate = (ParseState *) cpstate;
        int rtindex;

        ParseNamespaceItem *pnsi = transform_cypher_clause_as_subquery_2(new_cpstate, clause, NULL, false, query);
        topquery->rtable = new_cpstate->pstate.p_rtable;
        topquery->jointree = makeFromExpr(new_cpstate->pstate.p_joinlist, NULL);

        return topquery;
    }
}

cypher_update_information *transform_cypher_remove_item_list(cypher_parsestate *cpstate, List *remove_item_list, Query *query) {
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *li;
    cypher_update_information *info = make_ag_node(cypher_update_information);

    info->set_items = NIL;
    info->flags = 0;

    foreach (li, remove_item_list) {
        cypher_set_item *set_item = lfirst(li);
        cypher_update_item *item;
        ColumnRef *ref;
        char *variable_name, *property_name;
        Value *property_node, *variable_node;

        item = make_ag_node(cypher_update_item);
        item->remove_item = true;

        ColumnRef *set_item_prop = set_item->prop;
        if (!IsA(set_item_prop, ColumnRef))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("REMOVE clause must be in the format: REMOVE variable.property_name"),
                     parser_errposition(pstate, set_item->location)));

        ref = (ColumnRef *)set_item_prop;
        variable_node = linitial(ref->fields);


        variable_name = variable_node->val.str;

        item->var_name = variable_name;
        item->entity_position = get_target_entry_resno(cpstate, query->targetList, variable_name);

        if (item->entity_position == -1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                     errmsg("undefined reference to variable %s in REMOVE clause", variable_name),
                     parser_errposition(pstate, set_item->location)));

        // extract property name
        if (list_length(ref->fields) != 2)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("REMOVE clause must be in the format: REMOVE variable.property_name"),
                     parser_errposition(pstate, set_item->location)));

        property_node = lsecond(ref->fields);

        if (!IsA(property_node, String))
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                     errmsg("REMOVE clause expects a property name"),
                     parser_errposition(pstate, set_item->location)));
        
        property_name = property_node->val.str;
        item->prop_name = property_name;

        info->set_items = lappend(info->set_items, item);
    }

    return info;
}


cypher_update_information *transform_cypher_set_item_list(cypher_parsestate *cpstate, List *set_item_list, Query *query) {
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *li;
    cypher_update_information *info = make_ag_node(cypher_update_information);

    info->set_items = NIL;
    info->flags = 0;

    foreach (li, set_item_list) {
        cypher_set_item *set_item = lfirst(li);
        TargetEntry *target_item;
        cypher_update_item *item;
        ColumnRef *ref;
        A_Indirection *ind;
        char *variable_name, *property_name;
        Value *property_node, *variable_node;

        if (set_item->is_add)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("SET clause does not yet support adding propereties from maps"),
                     parser_errposition(pstate, set_item->location)));

        if (!IsA(set_item->prop, ColumnRef))
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                            errmsg("SET clause expects a variable name"),
                            parser_errposition(pstate, set_item->location)));

        ColumnRef *set_item_prop = set_item->prop;

        //ind = (A_Indirection *)set_item->prop;
        item = make_ag_node(cypher_update_item);
        item->remove_item = false;

        // extract variable name
        ref = (ColumnRef *)set_item_prop;

        variable_node = linitial(ref->fields);
        if (!IsA(variable_node, String))
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                     errmsg("SET clause expects a variable name"),
                     parser_errposition(pstate, set_item->location)));

        variable_name = variable_node->val.str;
        item->var_name = variable_name;
        item->entity_position = get_target_entry_resno(cpstate, query->targetList, variable_name);

        if (item->entity_position == -1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                     errmsg("undefined reference to variable %s in SET clause", variable_name),
                            parser_errposition(pstate, set_item->location)));

        // extract property name
        if (list_length(ref->fields) != 2)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("SET clause doesnt not support updating maps or lists in a property"),
                     parser_errposition(pstate, set_item->location)));

        property_node = lsecond(ref->fields);
        if (!IsA(property_node, String))
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                     errmsg("SET clause expects a property name"),
                     parser_errposition(pstate, set_item->location)));

        property_name = property_node->val.str;
        item->prop_name = property_name;

        // create target entry for the new property value
        item->prop_position = (AttrNumber)pstate->p_next_resno;
        cpstate->default_alias_num++;
        target_item = transform_cypher_item(cpstate, set_item->expr, NULL, EXPR_KIND_SELECT_TARGET,
                                            get_next_default_alias(cpstate), false);
    target_item->expr = add_volatile_wrapper(target_item->expr);

        query->targetList = lappend(query->targetList, target_item);
        info->set_items = lappend(info->set_items, item);
    }

    return info;
}

/*-------------------------------------------------------------------------
 * Flatten out parenthesized sublists in grouping lists, and some cases
 * of nested grouping sets.
 *
 * Inside a grouping set (ROLLUP, CUBE, or GROUPING SETS), we expect the
 * content to be nested no more than 2 deep: i.e. ROLLUP((a,b),(c,d)) is
 * ok, but ROLLUP((a,(b,c)),d) is flattened to ((a,b,c),d), which we then
 * (later) normalize to ((a,b,c),(d)).
 *
 * CUBE or ROLLUP can be nested inside GROUPING SETS (but not the reverse),
 * and we leave that alone if we find it. But if we see GROUPING SETS inside
 * GROUPING SETS, we can flatten and normalize as follows:
 *       GROUPING SETS (a, (b,c), GROUPING SETS ((c,d),(e)), (f,g))
 * becomes
 *       GROUPING SETS ((a), (b,c), (c,d), (e), (f,g))
 *
 * This is per the spec's syntax transformations, but these are the only such
 * transformations we do in parse analysis, so that queries retain the
 * originally specified grouping set syntax for CUBE and ROLLUP as much as
 * possible when deparsed. (Full expansion of the result into a list of
 * grouping sets is left to the planner.)
 *
 * When we're done, the resulting list should contain only these possible
 * elements:
 *       - an expression
 *       - a CUBE or ROLLUP with a list of expressions nested 2 deep
 *       - a GROUPING SET containing any of:
 *              - expression lists
 *              - empty grouping sets
 *              - CUBE or ROLLUP nodes with lists nested 2 deep
 * The return is a new list, but doesn't deep-copy the old nodes except for
 * GroupingSet nodes.
 *
 * As a side effect, flag whether the list has any GroupingSet nodes.
 *-------------------------------------------------------------------------
 */
static Node *flatten_grouping_sets(Node *expr, bool toplevel, bool *hasGroupingSets) {
    // just in case of pathological input 
    check_stack_depth();

    if (expr == (Node *) NIL)
        return (Node *) NIL;

    switch (expr->type)
    {
        case T_RowExpr:
        {
            RowExpr *r = (RowExpr *) expr;

            if (r->row_format == COERCE_IMPLICIT_CAST)
                return flatten_grouping_sets((Node *) r->args, false, NULL);
            break;
        }
        case T_GroupingSet:
        {
            GroupingSet *gset = (GroupingSet *) expr;
            ListCell   *l2;
            List       *result_set = NIL;

            if (hasGroupingSets)
                *hasGroupingSets = true;

            /*
             * at the top level, we skip over all empty grouping sets; the
             * caller can supply the canonical GROUP BY () if nothing is
             * left.
             */
            if (toplevel && gset->kind == GROUPING_SET_EMPTY)
                return (Node *) NIL;

            foreach(l2, gset->content) {
                Node       *n1 = lfirst(l2);
                Node       *n2 = flatten_grouping_sets(n1, false, NULL);

                if (IsA(n1, GroupingSet) && ((GroupingSet *) n1)->kind == GROUPING_SET_SETS)
                    result_set = list_concat(result_set, (List *) n2);
                else
                    result_set = lappend(result_set, n2);
            }

            /*
             * At top level, keep the grouping set node; but if we're in a
             * nested grouping set, then we need to concat the flattened
             * result into the outer list if it's simply nested.
             */

            if (toplevel || (gset->kind != GROUPING_SET_SETS))
                return (Node *) makeGroupingSet(gset->kind, result_set, gset->location);
            else
                return (Node *) result_set;

            break;
    }
        case T_List:
        {
            List *result = NIL;
            ListCell *l;

            foreach(l, (List *) expr) {
                Node *n = NULL;

                n = flatten_grouping_sets(lfirst(l), toplevel, hasGroupingSets);

                if (n != (Node *) NIL) {
                    if (IsA(n, List))
                        result = list_concat(result, (List *) n);
                    else
                        result = lappend(result, n);
                }
            }
            return (Node *) result;
        }
        default:
            break;
    }
    return expr;
}

// from PG's addTargetToGroupList 
static List *add_target_to_group_list(cypher_parsestate *cpstate, TargetEntry *tle, List *grouplist, List *targetlist, int location) {
    ParseState *pstate = (ParseState *)cpstate;
    Oid restype = exprType((Node *) tle->expr);

    // if tlist item is an UNKNOWN literal, change it to TEXT 
    if (restype == UNKNOWNOID) {
        tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr, restype, GTYPEOID, -1, COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
        restype = GTYPEOID;
    }

    // avoid making duplicate grouplist entries 
    if (!targetIsInSortList(tle, InvalidOid, grouplist)) {
        SortGroupClause *grpcl = makeNode(SortGroupClause);
        Oid sortop;
        Oid eqop;
        bool hashable;
        ParseCallbackState pcbstate;

        // determine the eqop and optional sortop 
        setup_parser_errposition_callback(&pcbstate, pstate, location);
    get_sort_group_operators(restype, false, true, false, &sortop, &eqop, NULL, &hashable);
        cancel_parser_errposition_callback(&pcbstate);

        grpcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);
        grpcl->eqop = eqop;
        grpcl->sortop = sortop;
        grpcl->nulls_first = false; // OK with or without sortop 
        grpcl->hashable = hashable;

        grouplist = lappend(grouplist, grpcl);
    }

    return grouplist;
}

// from PG's transformGroupClauseExpr 
static Index transform_group_clause_expr(List **flatresult, Bitmapset *seen_local, cypher_parsestate *cpstate, Node *gexpr, List **targetlist, List *sortClause, ParseExprKind exprKind, bool toplevel) {
    TargetEntry *tle = NULL;
    bool found = false;

    tle = find_target_list_entry(cpstate, gexpr, targetlist, exprKind);

    if (tle->ressortgroupref > 0) {
        ListCell *sl;

        /*
         * Eliminate duplicates (GROUP BY x, x) but only at local level.
         * (Duplicates in grouping sets can affect the number of returned
         * rows, so can't be dropped indiscriminately.)
         *
         * Since we don't care about anything except the sortgroupref, we can
         * use a bitmapset rather than scanning lists.
         */
        if (bms_is_member(tle->ressortgroupref, seen_local))
            return 0;

        /*
         * If we're already in the flat clause list, we don't need to consider
         * adding ourselves again.
         */
        found = targetIsInSortList(tle, InvalidOid, *flatresult);
        if (found)
            return tle->ressortgroupref;

        /*
         * If the GROUP BY tlist entry also appears in ORDER BY, copy operator
         * info from the (first) matching ORDER BY item.  This means that if
         * you write something like "GROUP BY foo ORDER BY foo USING <<<", the
         * GROUP BY operation silently takes on the equality semantics implied
         * by the ORDER BY.  There are two reasons to do this: it improves the
         * odds that we can implement both GROUP BY and ORDER BY with a single
         * sort step, and it allows the user to choose the equality semantics
         * used by GROUP BY, should she be working with a datatype that has
         * more than one equality operator.
         *
         * If we're in a grouping set, though, we force our requested ordering
         * to be NULLS LAST, because if we have any hope of using a sorted agg
         * for the job, we're going to be tacking on generated NULL values
         * after the corresponding groups. If the user demands nulls first,
         * another sort step is going to be inevitable, but that's the
         * planner's problem.
         */
         foreach(sl, sortClause) {
             SortGroupClause *sc = (SortGroupClause *) lfirst(sl);

             if (sc->tleSortGroupRef == tle->ressortgroupref) {
                 SortGroupClause *grpc = copyObject(sc);

                 if (!toplevel)
                     grpc->nulls_first = false;
                 *flatresult = lappend(*flatresult, grpc);
                 found = true;
                 break;
             }
         }
    }

    /*
     * If no match in ORDER BY, just add it to the result using default
     * sort/group semantics.
     */
    if (!found)
        *flatresult = add_target_to_group_list(cpstate, tle, *flatresult, *targetlist, exprLocation(gexpr));

    // _something_ must have assigned us a sortgroupref by now... 

    return tle->ressortgroupref;
}

/*
 * Transform a list of expressions within a GROUP BY clause or grouping set.
 *
 * The list of expressions belongs to a single clause within which duplicates
 * can be safely eliminated.
 *
 * Returns an integer list of ressortgroupref values.
 *
 * flatresult   reference to flat list of SortGroupClause nodes
 * pstate               ParseState
 * list                 nodes to transform
 * targetlist   reference to TargetEntry list
 * sortClause   ORDER BY clause (SortGroupClause nodes)
 * exprKind             expression kind
 * useSQL99             SQL99 rather than SQL92 syntax
 * toplevel             false if within any grouping set
 */
static List * transformGroupClauseList(List **flatresult, ParseState *pstate, List *list, List **targetlist,
                               List *sortClause, ParseExprKind exprKind, bool useSQL99, bool toplevel) {
    Bitmapset *seen_local = NULL;
    List *result = NIL;
    ListCell *gl;

    foreach(gl, list)
    {
        Node *gexpr = (Node *) lfirst(gl);

        Index ref = transform_group_clause_expr(flatresult, seen_local, pstate, gexpr, targetlist, sortClause, exprKind, toplevel);

        if (ref > 0) {
            seen_local = bms_add_member(seen_local, ref);
            result = lappend_int(result, ref);
        }
    }

    return result;
}

/*
 * Transform a grouping set and (recursively) its content.
 *
 * The grouping set might be a GROUPING SETS node with other grouping sets
 * inside it, but SETS within SETS have already been flattened out before
 * reaching here.
 *
 * Returns the transformed node, which now contains SIMPLE nodes with lists
 * of ressortgrouprefs rather than expressions.
 *
 * flatresult   reference to flat list of SortGroupClause nodes
 * pstate               ParseState
 * gset                 grouping set to transform
 * targetlist   reference to TargetEntry list
 * sortClause   ORDER BY clause (SortGroupClause nodes)
 * exprKind             expression kind
 * useSQL99             SQL99 rather than SQL92 syntax
 * toplevel             false if within any grouping set
 */
static Node *
transformGroupingSet(List **flatresult, ParseState *pstate, GroupingSet *gset, List **targetlist, List *sortClause,
                     ParseExprKind exprKind, bool useSQL99, bool toplevel) {
    ListCell *gl;
    List *content = NIL;

    Assert(toplevel || gset->kind != GROUPING_SET_SETS);

    foreach(gl, gset->content)
    {
        Node *n = lfirst(gl);

        if (IsA(n, List))
        {
            List *l = transformGroupClauseList(flatresult, pstate, (List *) n, targetlist, sortClause,
                                               exprKind, useSQL99, false);

            content = lappend(content, makeGroupingSet(GROUPING_SET_SIMPLE, l, exprLocation(n)));
        }
        else if (IsA(n, GroupingSet))
        {
            GroupingSet *gset2 = (GroupingSet *) lfirst(gl);

            content = lappend(content, transformGroupingSet(flatresult, pstate, gset2, targetlist, sortClause,
                                                            exprKind, useSQL99, false));
        }
        else
        {
            Index ref = transform_group_clause_expr(flatresult, NULL, pstate, n, targetlist, sortClause,
                                     exprKind, false);

            content = lappend(content, makeGroupingSet(GROUPING_SET_SIMPLE, list_make1_int(ref), exprLocation(n)));
        }
    }

    /* Arbitrarily cap the size of CUBE, which has exponential growth */
    if (gset->kind == GROUPING_SET_CUBE)
    {
        if (list_length(content) > 12)
             ereport(ERROR, (errcode(ERRCODE_TOO_MANY_COLUMNS),
                             errmsg("CUBE is limited to 12 elements"),
                             parser_errposition(pstate, gset->location)));
    }

    return (Node *) makeGroupingSet(gset->kind, content, gset->location);
}

static List * transform_group_clause(cypher_parsestate *cpstate, List *grouplist, List **groupingSets,
                                     List **targetlist, List *sortClause, ParseExprKind exprKind) {
    ParseState *pstate = (ParseState *)cpstate;
    List *result = NIL;
    List *flat_grouplist;
    List *gsets = NIL;
    ListCell *gl;
    bool hasGroupingSets = false;
    Bitmapset *seen_local = NULL;

    /*
     * Recursively flatten implicit RowExprs. (Technically this is only needed
     * for GROUP BY, per the syntax rules for grouping sets, but we do it
     * anyway.)
     */
    flat_grouplist = (List *) flatten_grouping_sets((Node *) grouplist, true, &hasGroupingSets);

    /*
     * If the list is now empty, but hasGroupingSets is true, it's because we
     * elided redundant empty grouping sets. Restore a single empty grouping
     * set to leave a canonical form: GROUP BY ()
     */

    if (flat_grouplist == NIL && hasGroupingSets)
        flat_grouplist = list_make1(makeGroupingSet(GROUPING_SET_EMPTY, NIL, exprLocation((Node *) grouplist)));


    foreach(gl, flat_grouplist) {
        Node *gexpr = (Node *) lfirst(gl);

        if (IsA(gexpr, GroupingSet)) {
                        GroupingSet *gset = (GroupingSet *) gexpr;

            switch (gset->kind)
            {
            case GROUPING_SET_EMPTY:
                gsets = lappend(gsets, gset);
                break;
            case GROUPING_SET_SIMPLE:
                /* can't happen */
                Assert(false);
                break;
            case GROUPING_SET_SETS:
            case GROUPING_SET_CUBE:
            case GROUPING_SET_ROLLUP:
                gsets = lappend(gsets,
                    transformGroupingSet(&result, pstate, gset, targetlist, sortClause, exprKind, true, true));
            break;
        }

        } else {
            Index ref = transform_group_clause_expr(&result, seen_local, cpstate, gexpr, targetlist,
                                                    sortClause, exprKind, true);
            if (ref > 0) {
                seen_local = bms_add_member(seen_local, ref);
                if (hasGroupingSets) {
                    seen_local = bms_add_member(seen_local, ref);
                    if (hasGroupingSets)
                        gsets = lappend(gsets,
                            makeGroupingSet(GROUPING_SET_SIMPLE, list_make1_int(ref), exprLocation(gexpr)));
                
        }
            }
        }
    }


    if (groupingSets)
        *groupingSets = gsets;

    return result;
}

static Query *transform_cypher_return(cypher_parsestate *cpstate, cypher_clause *clause) {
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

/*                                                                
 * findWindowClause
 *              Find the named WindowClause in the list, or return NULL if not there
 */
static WindowClause *
findWindowClause(List *wclist, const char *name)
{
        ListCell   *l;

        foreach(l, wclist)                                        
        {
                WindowClause *wc = (WindowClause *) lfirst(l);

                if (wc->name && strcmp(wc->name, name) == 0)
                        return wc;
        }

        return NULL;
}


/*                            
 * checkExprIsVarFree
 *              Check that given expr has no Vars of the current query level
 *              (aggregates and window functions should have been rejected already).
 *
 * This is used to check expressions that have to have a consistent value
 * across all rows of the query, such as a LIMIT.  Arguably it should reject
 * volatile functions, too, but we don't do that --- whatever value the
 * function gives on first execution is what you get.
 *
 * constructName does not affect the semantics, but is used in error messages
 */
static void
checkExprIsVarFree(ParseState *pstate, Node *n, const char *constructName)
{
    if (contain_vars_of_level(n, 0))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                                errmsg("argument of %s must not contain variables", constructName),
                                 parser_errposition(pstate, locate_var_of_level(n, 0))));
    }
}


/*
 * transformFrameOffset
 *        Process a window frame offset expression
 *
 * In RANGE mode, rangeopfamily is the sort opfamily for the input ORDER BY
 * column, and rangeopcintype is the input data type the sort operator is
 * registered with.  We expect the in_range function to be registered with
 * that same type.  (In binary-compatible cases, it might be different from
 * the input column's actual type, so we can't use that for the lookups.)
 * We'll return the OID of the in_range function to *inRangeFunc.
 */
static Node *
transform_frame_offset(ParseState *pstate, int frameOptions,
                     Oid rangeopfamily, Oid rangeopcintype, Oid *inRangeFunc,
                     Node *clause)
{
    const char *constructName = NULL;
    Node       *node;

    *inRangeFunc = InvalidOid;    /* default result */

    /* Quick exit if no offset expression */
    if (clause == NULL)
        return NULL;

    if (frameOptions & FRAMEOPTION_ROWS)
    {
        /* Transform the raw expression tree */
        node = sql_transform_expr(pstate, clause, EXPR_KIND_WINDOW_FRAME_ROWS);

        /*
         * Like LIMIT clause, simply coerce to int8
         */
        constructName = "ROWS";
        node = coerce_to_specific_type(pstate, node, INT8OID, constructName);
    }
    else if (frameOptions & FRAMEOPTION_RANGE)
    {
        /*
         * We must look up the in_range support function that's to be used,
         * possibly choosing one of several, and coerce the "offset" value to
         * the appropriate input type.
         */
        Oid            nodeType;
        Oid            preferredType;
        int            nfuncs = 0;
        int            nmatches = 0;
        Oid            selectedType = InvalidOid;
        Oid            selectedFunc = InvalidOid;
        CatCList   *proclist;
        int            i;

        /* Transform the raw expression tree */
        node = sql_transform_expr(pstate, clause, EXPR_KIND_WINDOW_FRAME_RANGE);
        nodeType = exprType(node);

        /*
         * If there are multiple candidates, we'll prefer the one that exactly
         * matches nodeType; or if nodeType is as yet unknown, prefer the one
         * that exactly matches the sort column type.  (The second rule is
         * like what we do for "known_type operator unknown".)
         */
        preferredType = (nodeType != UNKNOWNOID) ? nodeType : rangeopcintype;

        /* Find the in_range support functions applicable to this case */
        proclist = SearchSysCacheList2(AMPROCNUM,
                                       ObjectIdGetDatum(rangeopfamily),
                                       ObjectIdGetDatum(rangeopcintype));
        for (i = 0; i < proclist->n_members; i++)
        {
            HeapTuple    proctup = &proclist->members[i]->tuple;
            Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);

            /* The search will find all support proc types; ignore others */
            if (procform->amprocnum != BTINRANGE_PROC)
                continue;
            nfuncs++;

            /* Ignore function if given value can't be coerced to that type */
            if (!can_coerce_type(1, &nodeType, &procform->amprocrighttype,
                                 COERCION_IMPLICIT))
                continue;
            nmatches++;

            /* Remember preferred match, or any match if didn't find that */
            if (selectedType != preferredType)
            {
                selectedType = procform->amprocrighttype;
                selectedFunc = procform->amproc;
            }
        }
        ReleaseCatCacheList(proclist);

        /*
         * Throw error if needed.  It seems worth taking the trouble to
         * distinguish "no support at all" from "you didn't match any
         * available offset type".
         */
        if (nfuncs == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("RANGE with offset PRECEDING/FOLLOWING is not supported for column type %s",
                            format_type_be(rangeopcintype)),
                     parser_errposition(pstate, exprLocation(node))));
        if (nmatches == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("RANGE with offset PRECEDING/FOLLOWING is not supported for column type %s and offset type %s",
                            format_type_be(rangeopcintype),
                            format_type_be(nodeType)),
                     errhint("Cast the offset value to an appropriate type."),
                     parser_errposition(pstate, exprLocation(node))));
        if (nmatches != 1 && selectedType != preferredType)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("RANGE with offset PRECEDING/FOLLOWING has multiple interpretations for column type %s and offset type %s",
                            format_type_be(rangeopcintype),
                            format_type_be(nodeType)),
                     errhint("Cast the offset value to the exact intended type."),
                     parser_errposition(pstate, exprLocation(node))));

        /* OK, coerce the offset to the right type */
        constructName = "RANGE";
        node = coerce_to_specific_type(pstate, node,
                                       selectedType, constructName);
        *inRangeFunc = selectedFunc;
    }
    else if (frameOptions & FRAMEOPTION_GROUPS)
    {
        /* Transform the raw expression tree */
        node = sql_transform_expr(pstate, clause, EXPR_KIND_WINDOW_FRAME_GROUPS);

        /*
         * Like LIMIT clause, simply coerce to int8
         */
        constructName = "GROUPS";
        node = coerce_to_specific_type(pstate, node, INT8OID, constructName);
    }
    else
    {
        Assert(false);
        node = NULL;
    }

    /* Disallow variables in frame offsets */
    checkExprIsVarFree(pstate, node, constructName);

    return node;
}

/*
 * transformWindowDefinitions -
 *        transform window definitions (WindowDef to WindowClause)
 */
List *
transform_window_definitions(ParseState *pstate, List *windowdefs, List **targetlist)
{
    List *result = NIL;
    Index winref = 0;
    ListCell *lc;

    foreach(lc, windowdefs)
    {
        WindowDef *windef = (WindowDef *) lfirst(lc);
        WindowClause *refwc = NULL;
        List *partitionClause;
        List *orderClause;
        Oid rangeopfamily = InvalidOid;
        Oid rangeopcintype = InvalidOid;
        WindowClause *wc;

        winref++;

        /*
         * Check for duplicate window names.
         */
        if (windef->name && findWindowClause(result, windef->name) != NULL)
            ereport(ERROR, (errcode(ERRCODE_WINDOWING_ERROR),
                     errmsg("window \"%s\" is already defined", windef->name),
                     parser_errposition(pstate, windef->location)));

        /*
         * If it references a previous window, look that up.
         */
        if (windef->refname)
        {
            refwc = findWindowClause(result, windef->refname);
            if (refwc == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_OBJECT),
                         errmsg("window \"%s\" does not exist",
                                windef->refname),
                         parser_errposition(pstate, windef->location)));
        }

        /*
         * Transform PARTITION and ORDER specs, if any.  These are treated
         * almost exactly like top-level GROUP BY and ORDER BY clauses,
         * including the special handling of nondefault operator semantics.
         */
        orderClause = transform_cypher_order_by(pstate, windef->orderClause, targetlist, EXPR_KIND_ORDER_BY);
        partitionClause = transform_group_clause(pstate, windef->partitionClause, NULL, targetlist,
                                                 orderClause, EXPR_KIND_GROUP_BY);

        /*
         * And prepare the new WindowClause.
         */
        wc = makeNode(WindowClause);
        wc->name = windef->name;
        wc->refname = windef->refname;

        /*
         * Per spec, a windowdef that references a previous one copies the
         * previous partition clause (and mustn't specify its own).  It can
         * specify its own ordering clause, but only if the previous one had
         * none.  It always specifies its own frame clause, and the previous
         * one must not have a frame clause.  Yeah, it's bizarre that each of
         * these cases works differently, but SQL:2008 says so; see 7.11
         * <window clause> syntax rule 10 and general rule 1.  The frame
         * clause rule is especially bizarre because it makes "OVER foo"
         * different from "OVER (foo)", and requires the latter to throw an
         * error if foo has a nondefault frame clause.  Well, ours not to
         * reason why, but we do go out of our way to throw a useful error
         * message for such cases.
         */
        if (refwc)
        {
            if (partitionClause)
                ereport(ERROR,
                        (errcode(ERRCODE_WINDOWING_ERROR),
                         errmsg("cannot override PARTITION BY clause of window \"%s\"", windef->refname),
                         parser_errposition(pstate, windef->location)));
            wc->partitionClause = copyObject(refwc->partitionClause);
        }
        else
            wc->partitionClause = partitionClause;
        if (refwc) {
            if (orderClause && refwc->orderClause)
                ereport(ERROR, (errcode(ERRCODE_WINDOWING_ERROR),
                         errmsg("cannot override ORDER BY clause of window \"%s\"", windef->refname),
                         parser_errposition(pstate, windef->location)));
            if (orderClause) {
                wc->orderClause = orderClause;
                wc->copiedOrder = false;
            } else {
                wc->orderClause = copyObject(refwc->orderClause);
                wc->copiedOrder = true;
            }
        } else {
            wc->orderClause = orderClause;
            wc->copiedOrder = false;
        }
        if (refwc && refwc->frameOptions != FRAMEOPTION_DEFAULTS) {
            /*
             * Use this message if this is a WINDOW clause, or if it's an OVER
             * clause that includes ORDER BY or framing clauses.  (We already
             * rejected PARTITION BY above, so no need to check that.)
             */
            if (windef->name || orderClause || windef->frameOptions != FRAMEOPTION_DEFAULTS)
                ereport(ERROR, (errcode(ERRCODE_WINDOWING_ERROR),
                         errmsg("cannot copy window \"%s\" because it has a frame clause", windef->refname),
                         parser_errposition(pstate, windef->location)));
            /* Else this clause is just OVER (foo), so say this: */
            ereport(ERROR, (errcode(ERRCODE_WINDOWING_ERROR),
                     errmsg("cannot copy window \"%s\" because it has a frame clause", windef->refname),
                     errhint("Omit the parentheses in this OVER clause."),
                     parser_errposition(pstate, windef->location)));
        }
        wc->frameOptions = windef->frameOptions;

        /*
         * RANGE offset PRECEDING/FOLLOWING requires exactly one ORDER BY
         * column; check that and get its sort opfamily info.
         */
        if ((wc->frameOptions & FRAMEOPTION_RANGE) &&
            (wc->frameOptions & (FRAMEOPTION_START_OFFSET | FRAMEOPTION_END_OFFSET)))
        {
            SortGroupClause *sortcl;
            Node *sortkey;
            int16 rangestrategy;

            if (list_length(wc->orderClause) != 1)
                ereport(ERROR,
                        (errcode(ERRCODE_WINDOWING_ERROR),
                         errmsg("RANGE with offset PRECEDING/FOLLOWING requires exactly one ORDER BY column"),
                         parser_errposition(pstate, windef->location)));
            sortcl = castNode(SortGroupClause, linitial(wc->orderClause));
            sortkey = get_sortgroupclause_expr(sortcl, *targetlist);
            /* Find the sort operator in pg_amop */
            if (!get_ordering_op_properties(sortcl->sortop,
                                            &rangeopfamily,
                                            &rangeopcintype,
                                            &rangestrategy))
                elog(ERROR, "operator %u is not a valid ordering operator",
                     sortcl->sortop);
            /* Record properties of sort ordering */
            wc->inRangeColl = exprCollation(sortkey);
            wc->inRangeAsc = (rangestrategy == BTLessStrategyNumber);
            wc->inRangeNullsFirst = sortcl->nulls_first;
        }

        /* Per spec, GROUPS mode requires an ORDER BY clause */
        if (wc->frameOptions & FRAMEOPTION_GROUPS)
        {
            if (wc->orderClause == NIL)
                ereport(ERROR,
                        (errcode(ERRCODE_WINDOWING_ERROR),
                         errmsg("GROUPS mode requires an ORDER BY clause"),
                         parser_errposition(pstate, windef->location)));
        }

        /* Process frame offset expressions */
        wc->startOffset = transform_frame_offset(pstate, wc->frameOptions,
                                               rangeopfamily, rangeopcintype,
                                               &wc->startInRangeFunc,
                                               windef->startOffset);
        wc->endOffset = transform_frame_offset(pstate, wc->frameOptions,
                                             rangeopfamily, rangeopcintype,
                                             &wc->endInRangeFunc,
                                             windef->endOffset);
        wc->winref = winref;

        result = lappend(result, wc);
    }

    return result;
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

static Query *transform_cypher_with(cypher_parsestate *cpstate, cypher_clause *clause) {
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


/*
 * transform_cypher_optional_match_clause
 *      Transform the previous clauses and OPTIONAL MATCH clauses to be LATERAL LEFT JOIN
 *   transform_cypher_optional_match_clause   to construct a result value.
 */
static RangeTblEntry *transform_cypher_optional_match_clause(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_clause *prevclause;
    RangeTblEntry *l_rte, *r_rte;
    ParseNamespaceItem *l_nsitem, *r_nsitem;
    ParseState *pstate = (ParseState *) cpstate;
    JoinExpr* j = makeNode(JoinExpr);
    List *res_colnames = NIL, *res_colvars = NIL;
    Alias *l_alias, *r_alias;
    ParseNamespaceItem *jnsitem;
    int i = 0;

    j->jointype = JOIN_LEFT;

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

    jnsitem = addRangeTableEntryForJoin(pstate, res_colnames, NULL, j->jointype, 0, res_colvars, NIL, NIL, j->alias, NULL, false);

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
    if (self->optional == true && clause->prev != NULL) {
        RangeTblEntry *rte = transform_cypher_optional_match_clause(cpstate, clause);

        query->targetList = make_target_list_from_join(pstate, rte);
        query->rtable = pstate->p_rtable;
        query->jointree = makeFromExpr(pstate->p_joinlist, NULL);
    } else if (clause->prev && (self->order_by)) {
        RangeTblEntry *rte = transform_cypher_match_clause(cpstate, clause);

        query->targetList = make_target_list_from_join(pstate, rte);
        query->rtable = pstate->p_rtable;
        query->jointree = makeFromExpr(pstate->p_joinlist, NULL);
    } else if (clause->prev) {
        RangeTblEntry *rte;
        int rtindex;
        ParseNamespaceItem *pnsi;

        pnsi = transform_prev_cypher_clause(cpstate, clause->prev, true);
        rte = pnsi->p_rte;
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate

        /*
         * add all the target entries in rte to the current target list to pass
         * all the variables that are introduced in the previous clause to the
         * next clause
         */
        pnsi = get_namespace_item(pstate, rte);
        query->targetList = expandNSItemAttrs(pstate, pnsi, 0, -1);
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
 * Transform a cypher sub pattern. This is put here because it is a sub clause.
 * This works in tandem with transform_Sublink in cypher_expr.c
 */
static Query *transform_cypher_sub_pattern(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_clause *c = NULL;
    Query *qry;
    cypher_sub_pattern *subpat = (cypher_sub_pattern*)clause->self;
    cypher_parsestate *child_parse_state = make_cypher_parsestate(cpstate);

    // Since the first clause in stmt is the innermost subquery, the order of the clauses is inverted.
    ListCell *lc;
    foreach (lc, subpat->pattern) {
        cypher_clause *next;

        next = palloc(sizeof(*next));
        next->next = NULL;
        next->self = lfirst(lc);
        next->prev = c;

        if (c != NULL)
            c->next = next;
        c = next;
    }

    //ParseExprKind old_expr_kind = pstate->p_expr_kind;
    child_parse_state->entities = list_concat(NIL, cpstate->entities);
     child_parse_state->pstate.p_expr_kind = EXPR_KIND_WHERE;
    qry = transform_cypher_clause(child_parse_state, c);



    free_cypher_parsestate(child_parse_state);

    return qry;
}
static Node *make_null_const(int location) {
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_Null;
    n->location = location;

    return (Node *)n;
}
static Node *make_int_const(int i, int location) {
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_Integer;
    n->val.val.ival = i;
    n->location = location;

    return (Node *)n;
}

static Node *make_string_const(char *s, int location) {
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_String;
    n->val.val.str = s;
    n->location = location;

    return (Node *)n;
}

static ParseNamespaceItem *add_vle_to_query(cypher_parsestate *cpstate, Node *n) {
    ParseState *pstate = &cpstate->pstate;
    RangeTblEntry *rte = NULL;
    List *namespace = NULL;
    int rtindex;

    n = transform_VLE_Function(cpstate, n, &rte, &rtindex, &namespace);
    Assert(n != NULL);

    checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);
    
    setNamespaceLateralState(namespace, true, true);

    pstate->p_joinlist = lappend(pstate->p_joinlist, n);
    pstate->p_namespace = list_concat(pstate->p_namespace, namespace);
        
    setNamespaceLateralState(pstate->p_namespace, false, true);
        
    return lfirst(list_head(namespace));
}

static FuncCall *make_vle_func_call(cypher_parsestate *cpstate, cypher_node *prev_node, cypher_relationship *rel, cypher_node *next_node)
{
    ColumnRef *cref;
    A_Indices *ai = (A_Indices *)rel->varlen;
    List *args = NIL;


    // start node    
    cref = makeNode(ColumnRef);
    cref->location = -1;
    cref->fields = list_make1(makeString(prev_node->name));
    args = lappend(args, cref);

    //end node
    Assert(next_node->name != NULL);

    cref = makeNode(ColumnRef);
    cref->location = -1;
    cref->fields = list_make1(makeString(next_node->name));
    args = lappend(args, cref);

    // lower bound
    if (ai == NULL || ai->lidx == NULL)
        args = lappend(args, make_null_const(-1));
    else
        args = lappend(args, ai->lidx);

    // upper bound
    if (ai == NULL || ai->uidx == NULL)
        args = lappend(args, make_null_const(-1));
    else
        args = lappend(args, ai->uidx);

    // direction
    args = lappend(args, make_int_const(rel->dir, -1));

    if (rel->label == NULL)
        args = lappend(args, make_null_const(-1));
    else 
        args = lappend(args, make_string_const(rel->label, -1));
    
    if (rel->props == NULL)
        args = lappend(args, make_null_const(-1));                          
    else                    
        args = lappend(args, rel->props);   

    return makeFuncCall(list_make1(makeString("vle")), args, COERCE_SQL_SYNTAX, -1);
}

static transform_entity *handle_vertex(cypher_parsestate *cpstate, Query *query,
                                       cypher_path *path, cypher_node *node, int i)
{
    Expr *expr = NULL;
    transform_entity *entity = NULL;

    if(node->name == NULL && !INCLUDE_NODE_IN_JOIN_TREE(path, node) && i + 1 < list_length(path->path)) {
        cypher_relationship *rel = list_nth(path->path, i + 1);

        if (rel->varlen != NULL)
            node->name = get_next_default_alias(cpstate);
    }

    // transform vertex
    expr = transform_cypher_node(cpstate, node, &query->targetList, list_length(path->path) == 1 ? true :INCLUDE_NODE_IN_JOIN_TREE(path, node));

    entity = make_transform_entity(cpstate, ENT_VERTEX, (Node *)node, expr, node->name);

    // transform properties if they exist 
    if (node->props)
    {
        Node *n = NULL;

        n = create_property_constraints(cpstate, entity, node->props);

        cpstate->property_constraint_quals = lappend(cpstate->property_constraint_quals, n);
    }

    return entity;
}

static transform_entity *handle_edge(cypher_parsestate *cpstate, Query *query, 
                                     cypher_path *path, cypher_relationship *rel,     
                                     int i, ListCell *lc, transform_entity *prev_entity) 
{
    transform_entity *entity = NULL;                            
    Expr *expr = NULL;

    /*                                                               
     * In the case where the MATCH is one edge and two vertices, the 
     * edge is bidirectional, and neither vertex is included in the  
     * join tree, we need to force one of the vertices into the join 
     * tree to ensure the output is generated correctly.             
     */                                                              
    if (list_length(path->path) == 3 && rel->dir == CYPHER_REL_DIR_NONE && !prev_entity->in_join_tree) {                                                                
        cypher_node *node = list_nth(path->path, 2);//(cypher_node *)lfirst(lnext(path->path, lc));        
                                                                                 
        if (!INCLUDE_NODE_IN_JOIN_TREE(path, node))                  
        {                                                            
            /*                                                       
             * Assigning a variable name here will ensure that when  
             * the next vertex is processed, the vertex will be      
             * included in the join tree.                            
             */                                                      
            node->name = get_next_default_alias(cpstate);            
        }                                                            
    }                                                                
                                                                                 
    expr = transform_cypher_edge(cpstate, rel, &query->targetList);  
                                                                                 
    entity = make_transform_entity(cpstate, ENT_EDGE, (Node *)rel, expr, rel->name);                            
    if (rel->props) {                                                                
        Node *n = create_property_constraints(cpstate, entity, rel->props);

        cpstate->property_constraint_quals = lappend(cpstate->property_constraint_quals, n);          
    }                                                                
                                                 
   return entity; 
}

static List *insert_vle_entity(List *entities, transform_entity *next_entity, transform_entity *vle_entity) {
    entities = list_truncate(entities, list_length(entities) - 1);   
    entities = lappend(entities, vle_entity);                        
                                                                                 
    entities = lappend(entities, next_entity);   

    return entities;
}

static Node *transform_VLE_Function(cypher_parsestate *cpstate, Node *n, RangeTblEntry **top_rte, int *top_rti, List **namespace) {
    ParseState *pstate = &cpstate->pstate;

    Assert(IsA(n, RangeFunction));

    if (IsA(n, RangeFunction)) {
        RangeTblRef *rtr;
        RangeTblEntry *rte;
        ParseNamespaceItem *nsitem;
        int rtindex;

        nsitem = transform_RangeFunction(cpstate, (RangeFunction *) n);
        rte = nsitem->p_rte;
        rtindex = list_length(pstate->p_rtable);
        Assert(rte == rt_fetch(rtindex, pstate->p_rtable));
        *top_rte = rte;
        *top_rti = rtindex;
        *namespace = list_make1(nsitem);
        rtr = makeNode(RangeTblRef);
        rtr->rtindex = rtindex;
        return (Node *) rtr;
    }

    return NULL;
}

// setNamespaceLateralState - subroutine to update LATERAL flags in a namespace list.
static void setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok) {
    ListCell *lc;

    foreach(lc, namespace) {
        ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

        nsitem->p_lateral_only = lateral_only;
        nsitem->p_lateral_ok = lateral_ok;
    }
}

// transform a function call appearing in FROM
static ParseNamespaceItem *transform_RangeFunction(cypher_parsestate *cpstate, RangeFunction *r) {
    ParseState *pstate = NULL;
    List *funcexprs = NIL;
    List *funcnames = NIL;
    List *coldeflists = NIL;
    bool is_lateral = false;
    ListCell *lc = NULL;
    ParseNamespaceItem *pnsi;

    pstate = &cpstate->pstate;

    Assert(!pstate->p_lateral_active);
    pstate->p_lateral_active = true;

    // transform the raw expressions 
    foreach(lc, r->functions)
    {
        List *pair = (List*)lfirst(lc);
        Node *fexpr;
        List *coldeflist;
        Node *newfexpr;
        Node *last_srf;

        // Disassemble the function-call/column-def-list pairs 
        Assert(list_length(pair) == 2);
        fexpr = (Node*) linitial(pair);
        coldeflist = (List*) lsecond(pair);

        // normal case ... 
        last_srf = pstate->p_last_srf;

        // transform the function expression 
        newfexpr = transform_cypher_expr(cpstate, fexpr, EXPR_KIND_FROM_FUNCTION);

        // nodeFunctionscan.c requires SRFs to be at top level 
        if (pstate->p_last_srf != last_srf && pstate->p_last_srf != newfexpr)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("set-returning functions must appear at top level of FROM"),
                     parser_errposition(pstate, exprLocation(pstate->p_last_srf))));

        funcexprs = lappend(funcexprs, newfexpr);
        funcnames = lappend(funcnames, FigureColname(fexpr));

        if (coldeflist && r->coldeflist)
            ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("multiple column definition lists are not allowed for the same function"),
                     parser_errposition(pstate, exprLocation((Node *) r->coldeflist))));

        coldeflists = lappend(coldeflists, coldeflist);
    }

    pstate->p_lateral_active = false;

    /*
     * We must assign collations now so that the RTE exposes correct collation
     * info for Vars created from it.
     */
    assign_list_collations(pstate, funcexprs);

    // currently this is not used by the VLE 
    Assert(r->coldeflist == NULL);

    // mark the RTE as LATERAL 
    is_lateral = r->lateral || contain_vars_of_level((Node *) funcexprs, 0);

    // build an RTE for the function 
    pnsi = addRangeTableEntryForFunction(pstate, funcnames, funcexprs, coldeflists, r, is_lateral, true);

    return pnsi;
}

static void transform_match_pattern(cypher_parsestate *cpstate, Query *query, List *pattern, Node *where) {
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *lc;
    List *quals = NIL;
    Expr *q = NULL;
    Expr *expr = NULL;

    foreach (lc, pattern) {
        List *qual = NULL;
        cypher_path *path = (cypher_path *) lfirst(lc);

        qual = transform_match_path(cpstate, query, path);

        quals = list_concat(quals, qual);
    }

    if (quals != NIL) {
        q = makeBoolExpr(AND_EXPR, quals, -1);
        expr = (Expr *)sql_transform_expr(&cpstate->pstate, (Node *)q, EXPR_KIND_WHERE);
    }

    if (cpstate->property_constraint_quals != NIL) {
        Expr *prop_qual = makeBoolExpr(AND_EXPR, cpstate->property_constraint_quals, -1);

        if (expr == NULL)
            expr = prop_qual;
        else
            expr = makeBoolExpr(AND_EXPR, list_make2(expr, prop_qual), -1);
    }

    // transform the where clause quals and add to the quals,
    if (where != NULL) {
        Expr *where_qual;

        where_qual = (Expr *)transform_cypher_expr(cpstate, where, EXPR_KIND_WHERE);
        if (expr == NULL) {
            expr = where_qual;
        } else {
            /*
             * coerce the WHERE clause to a boolean before AND with the property
             * contraints, otherwise there could be evaluation issues.
             */
            where_qual = (Expr *)coerce_to_boolean(pstate, (Node *)where_qual, "WHERE");

            expr = makeBoolExpr(AND_EXPR, list_make2(expr, where_qual), -1);
        }
    }

    /*
     * Coerce to WHERE clause to a bool, denoting whether the constructed
     * clause is true or false.
     */
    if (expr != NULL)
        expr = (Expr *)coerce_to_boolean(pstate, (Node *)expr, "WHERE");

    query->rtable = cpstate->pstate.p_rtable;
    query->jointree = makeFromExpr(cpstate->pstate.p_joinlist, (Node *)expr);
}

/*
 * Creates a FuncCall node that will prevent an edge from being joined
 * to twice.
 */
static FuncCall *prevent_duplicate_edges(cypher_parsestate *cpstate, List *entities) {
    List *edges = NIL;
    ListCell *lc;
    List *qualified_function_name;
    Value *ag_catalog, *edge_fn;

    ag_catalog = makeString("postgraph");
    edge_fn = makeString("_ag_enforce_edge_uniqueness");

    qualified_function_name = list_make2(ag_catalog, edge_fn);

    bool has_vle =  false;
    foreach (lc, entities) {
        transform_entity *entity = lfirst(lc);

        if (entity->type == ENT_VLE_EDGE) {
            has_vle = true;
            break;
        }
    }

    /*
     * if the match clause uses the VLE code, we need to do the old slow way... for now.
     * that could probably be fixed by exposes the edges being scanned, but we will worry
     * about that later. For MATCH with no VLE edges, just use <> between each id in the path
     * let the optimizer do its magic.
     */
    if (has_vle) {
        // iterate through each entity, collecting the access node for each edge
        foreach (lc, entities)
        {
            transform_entity *entity = lfirst(lc);
            Node *edge;

            if (entity->type == ENT_EDGE) {
                edge = make_qual(cpstate, entity, AG_EDGE_COLNAME_ID);

                 edges = lappend(edges, edge);
            } else if (entity->type == ENT_VLE_EDGE) {
                edges = lappend(edges, entity->expr);
            }
        }

        return makeFuncCall(qualified_function_name, edges, COERCE_SQL_SYNTAX, -1);
    }

    for (int i = 0; i < list_length(entities); i++) {
        transform_entity *entity_i = (transform_entity *)list_nth_cell(entities, i)->ptr_value;

        if (entity_i->type == ENT_EDGE) {
            for (int j = i + 1; j < list_length(entities); j++) {
                transform_entity *entity_j = (transform_entity *)list_nth_cell(entities, j)->ptr_value;
                if (entity_j->type == ENT_EDGE) {
                    Node *left = make_qual(cpstate, entity_i, AG_EDGE_COLNAME_ID);
                    Node *right = make_qual(cpstate, entity_j, AG_EDGE_COLNAME_ID);

                    edges = lappend (edges, makeSimpleCypherA_Expr(AEXPR_OP, "<>", left, right, -1));
                }

            }
        }
    }
    return makeBoolExpr(AND_EXPR, edges, -1);
}

/*
 * For any given edge, the previous entity is joined with the edge
 * via the prev_qual node, and the next entity is join with the
 * next_qual node. If there is a filter on the previous vertex label,
 * create a filter, same with the next node.
 */
static List *make_directed_edge_join_conditions(cypher_parsestate *cpstate, transform_entity *prev_entity,
    transform_entity *next_entity, Node *prev_qual, Node *next_qual, char *prev_node_filter, char *next_node_filter) {
    List *quals = NIL;

    if (prev_entity->in_join_tree)
        quals = list_concat(quals, join_to_entity(cpstate, prev_entity, prev_qual, JOIN_SIDE_LEFT));

    if (next_entity->in_join_tree && next_entity->type != ENT_VLE_EDGE)
        quals = list_concat(quals, join_to_entity(cpstate, next_entity, next_qual, JOIN_SIDE_RIGHT));

    if (prev_node_filter != NULL && !IS_DEFAULT_LABEL_VERTEX(prev_node_filter)) {
        A_Expr *qual = filter_vertices_on_label_id(cpstate, prev_qual, prev_node_filter);

        quals = lappend(quals, qual);
    }

    if (next_node_filter != NULL && !IS_DEFAULT_LABEL_VERTEX(next_node_filter)) {
        A_Expr *qual;
        qual = filter_vertices_on_label_id(cpstate, next_qual, next_node_filter);

        quals = lappend(quals, qual);
    }

    return quals;
}

/*
 * The joins are driven by edges. Under specific conditions, it becomes
 * necessary to have knowledge about the previous edge and vertex and
 * the next vertex and edge.
 *
 * [prev_edge]-(prev_node)-[edge]-(next_node)-[next_edge]
 *
 * prev_edge and next_edge are allowed to be null.
 * prev_node and next_node are not allowed to be null.
 */
static List *make_join_condition_for_edge(cypher_parsestate *cpstate, transform_entity *prev_edge,
                                          transform_entity *prev_node, transform_entity *entity,
                                          transform_entity *next_node, transform_entity *next_edge) {
    char *next_label_name_to_filter = NULL;
    char *prev_label_name_to_filter = NULL;
    transform_entity *next_entity;
    transform_entity *prev_entity;

    if (entity->type == ENT_VLE_EDGE)
        return NIL;

    //  If the previous node is not in the join tree, set the previous label filter.
    if (!prev_node->in_join_tree)
        prev_label_name_to_filter = prev_node->entity.node->label;

    /*
     * If the next node is not in the join tree and there is not
     * another edge, set the label filter. When there is another
     * edge, we don't need to set it, because that edge will set the
     * filter for that node.
     */
    if (!next_node->in_join_tree && next_edge == NULL)
        next_label_name_to_filter = next_node->entity.node->label;

    /*
     * When the previous node is not in the join tree, and there
     * is a previous edge, set the previous entity to that edge.
     * Otherwise, use the previous node/
     */
    if (!prev_node->in_join_tree && prev_edge != NULL)
        prev_entity = prev_edge;
    else
        prev_entity = prev_node;

    /*
     * When the next node is not in the join tree, and there
     * is a next edge, set the next entity to that edge.
     * Otherwise, use the next node.
     */
    if (!next_node->in_join_tree && next_edge != NULL)
        next_entity = next_edge;
    else
        next_entity = next_node;

    switch (entity->entity.rel->dir)
    {
        case CYPHER_REL_DIR_RIGHT:
        {
            Node *prev_qual = make_qual(cpstate, entity, AG_EDGE_COLNAME_START_ID);
            Node *next_qual = make_qual(cpstate, entity, AG_EDGE_COLNAME_END_ID);

            return make_directed_edge_join_conditions(cpstate, prev_entity, next_node, prev_qual,
                                                      next_qual, prev_label_name_to_filter,
                                                      next_label_name_to_filter);
        }
        case CYPHER_REL_DIR_LEFT:
        {
            Node *prev_qual = make_qual(cpstate, entity, AG_EDGE_COLNAME_END_ID);
            Node *next_qual = make_qual(cpstate, entity, AG_EDGE_COLNAME_START_ID);

            return make_directed_edge_join_conditions(cpstate, prev_entity, next_node, prev_qual,
                                                      next_qual, prev_label_name_to_filter,
                                                      next_label_name_to_filter);
        }
        case CYPHER_REL_DIR_NONE:
        {
            /*
             * For undirected relationships, we can use the left directed
             * relationship OR'd by the right directed relationship.
             */
            Node *start_id_expr = make_qual(cpstate, entity, AG_EDGE_COLNAME_START_ID);
            Node *end_id_expr = make_qual(cpstate, entity, AG_EDGE_COLNAME_END_ID);
            List *first_join_quals = NIL, *second_join_quals = NIL;
            Expr *first_qual, *second_qual;
            Expr *or_qual;

            first_join_quals = make_directed_edge_join_conditions(cpstate, prev_entity, next_entity, start_id_expr,
                                                                  end_id_expr, prev_label_name_to_filter,
                                                                  next_label_name_to_filter);

            second_join_quals = make_directed_edge_join_conditions(cpstate, prev_entity, next_entity, end_id_expr,
                                                                   start_id_expr, prev_label_name_to_filter,
                                                                   next_label_name_to_filter);

            first_qual = makeBoolExpr(AND_EXPR, first_join_quals, -1);
            second_qual = makeBoolExpr(AND_EXPR, second_join_quals, -1);

            or_qual = makeBoolExpr(OR_EXPR, list_make2(first_qual, second_qual), -1);

            return list_make1(or_qual);
        }
        default:
            return NULL;
    }
}

/*
 * For the given entity, join it to the current edge, via the passed qual node. The side denotes if the entity is on the right
 * or left of the current edge. Which we will need to know if the passed entity is a directed edge.
 */
static List *join_to_entity(cypher_parsestate *cpstate, transform_entity *entity, Node *qual, enum transform_entity_join_side side) {
    ParseState *pstate = (ParseState *)cpstate;
    A_Expr *expr;
    List *quals = NIL;

    if (entity->type == ENT_VERTEX) {
        Node *id_qual = make_qual(cpstate, entity, AG_EDGE_COLNAME_ID);

        expr = makeSimpleCypherA_Expr(AEXPR_OP, "=", qual, (Node *)id_qual, -1);

        quals = lappend(quals, expr);
    } else if (entity->type == ENT_EDGE) {
        List *edge_quals = make_edge_quals(cpstate, entity, side);

        if (list_length(edge_quals) > 1)
            expr = makeSimpleCypherA_Expr(AEXPR_IN, "=", qual, (Node *)edge_quals, -1);
        else
            expr = makeSimpleCypherA_Expr(AEXPR_OP, "=", qual, linitial(edge_quals), -1);

        quals = lappend(quals, expr);
    } else {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("unknown entity type to join to")));
    }

    return quals;
}

// makes the quals neccessary when an edge is joining to another edge.
static List *make_edge_quals(cypher_parsestate *cpstate, transform_entity *edge, enum transform_entity_join_side side) {
    ParseState *pstate = (ParseState *)cpstate;
    char *left_dir;
    char *right_dir;

    Assert(edge->type == ENT_EDGE);

    switch (side) {
        case JOIN_SIDE_LEFT:
            left_dir = AG_EDGE_COLNAME_START_ID;
            right_dir = AG_EDGE_COLNAME_END_ID;
            break;
        case JOIN_SIDE_RIGHT:
            left_dir = AG_EDGE_COLNAME_END_ID;
            right_dir = AG_EDGE_COLNAME_START_ID;
            break;
        default:
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("unknown join type found"), parser_errposition(pstate, edge->entity.rel->location)));
    }

    switch (edge->entity.rel->dir) {
        case CYPHER_REL_DIR_LEFT:
            return list_make1(make_qual(cpstate, edge, left_dir));
        case CYPHER_REL_DIR_RIGHT:
            return list_make1(make_qual(cpstate, edge, right_dir));
        case CYPHER_REL_DIR_NONE:
            return list_make2(make_qual(cpstate, edge, left_dir), make_qual(cpstate, edge, right_dir));
        default:
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Unknown relationship direction")));
    }
    return NIL;
}

/*
 * Creates a node that will create a filter on the passed field node
 * that removes all labels that do not have the same label_id
 */
static A_Expr *filter_vertices_on_label_id(cypher_parsestate *cpstate, Node *id_field, char *label) {
    label_cache_data *lcd = search_label_name_graph_cache(label, cpstate->graph_oid);
    A_Const *n;
    FuncCall *fc;
    Value *catalog, *extract_label_id;
    int32 label_id = lcd->id;

    n = makeNode(A_Const);
    n->val.type = T_Integer;
    n->val.val.ival = label_id;
    n->location = -1;

    catalog = makeString(CATALOG_SCHEMA);
    extract_label_id = makeString("_extract_label_id");

    fc = makeFuncCall(list_make2(catalog, extract_label_id), list_make1(id_field), COERCE_EXPLICIT_CALL, -1);

    return makeSimpleCypherA_Expr(AEXPR_OP, "=", (Node *)fc, (Node *)n, -1);
}

/*
 * Creates the Contains operator to process property contraints for a vertex/
 * edge in a MATCH clause. creates the gtype @> with the enitity's properties
 * on the right and the contraints in the MATCH clause on the left.
 */
static Node *create_property_constraints(cypher_parsestate *cpstate, transform_entity *entity, Node *property_constraints) {
    ParseState *pstate = (ParseState *)cpstate;
    char *entity_name;
    ColumnRef *cr;
    Node *prop_expr, *const_expr;
    Node *last_srf = pstate->p_last_srf;
    ParseNamespaceItem *pnsi;

    cr = makeNode(ColumnRef);

    entity_name = get_entity_name(entity);

    cr->fields = list_make2(makeString(entity_name), makeString("properties"));

    // use Postgres to get the properties' transform node
    if ((pnsi = find_pnsi(cpstate, entity_name)))
        prop_expr = scanNSItemForColumn(pstate, pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1);
    else
        prop_expr = sql_transform_expr(pstate, (Node *)cr, EXPR_KIND_WHERE);

    // use cypher to get the constraints' transform node
    const_expr = transform_cypher_expr(cpstate, property_constraints, EXPR_KIND_WHERE);

    return (Node *)make_op(pstate, list_make2(makeString("postgraph"), makeString("@>")), prop_expr, const_expr, last_srf, -1);
}


/*
 * For the given path, transform each entity within the path, create
 * the path variable if needed, and construct the quals to enforce the
 * correct join tree, and enforce edge uniqueness.
 */
static List *transform_match_path(cypher_parsestate *cpstate, Query *query, cypher_path *path) {
    List *qual = NIL;
    List *entities = NIL;
    FuncCall *duplicate_edge_qual;
    List *join_quals;

    // transform the entities in the path
    entities = transform_match_entities(cpstate, query, path);

    // create the path variable, if needed.
    if (path->var_name != NULL) {
        TargetEntry *path_te;

        path_te = transform_match_create_path_variable(cpstate, path, entities);
        query->targetList = lappend(query->targetList, path_te);
    }

    // construct the quals for the join tree
    join_quals = make_path_join_quals(cpstate, entities);
    qual = list_concat(qual, join_quals);

    // construct the qual to prevent duplicate edges
    if (list_length(entities) > 3) {
        duplicate_edge_qual = prevent_duplicate_edges(cpstate, entities);
        qual = lappend(qual, duplicate_edge_qual);
    }

    return qual;
}

RangeFunction *make_range_function(FuncCall *func, Alias *alias, bool is_lateral, bool ordinality, bool is_rows_from) {
    RangeFunction *rf = makeNode(RangeFunction);

    rf->lateral = is_lateral;
    rf->ordinality = ordinality;
    rf->is_rowsfrom = is_rows_from;
    rf->functions = list_make1(list_make2(func, NIL));
    rf->alias = alias;

    return rf;
}

Alias *make_alias(char *name, List *colnames) {
    Alias *alias = makeNode(Alias);
    
    alias->aliasname = name;
    alias->colnames = colnames;

    return alias;
}


static transform_entity *transform_VLE_edge_entity(cypher_parsestate *cpstate, cypher_relationship *rel,
                                                   Query *query, FuncCall *func) {
    ParseState *pstate = (ParseState *)cpstate;

    if (!rel->name)
       rel->name = get_next_default_alias(cpstate);

    Alias *alias = make_alias(rel->name, NIL);
    RangeFunction *rf = make_range_function(func, alias, false, false, false);

    ParseNamespaceItem *pnsi = add_vle_to_query(cpstate, (Node*)rf);

    Node *var = scanNSItemForColumn(pstate, pnsi, 0, "edges", -1);

    TargetEntry *te = makeTargetEntry((Expr*)var, pstate->p_next_resno++, rel->name, false);
    query->targetList = lappend(query->targetList, te);

    transform_entity *entity = make_transform_entity(cpstate, ENT_VLE_EDGE, (Node *)rel, (Expr *)var, rel->name);

    return entity;
}

/*
 * Iterate through the path and construct all edges and necessary vertices
 */
static List *transform_match_entities(cypher_parsestate *cpstate, Query *query, cypher_path *path) {
    ListCell *lc = NULL;
    List *entities = NIL;
    transform_entity *prev_node_entity = NULL;

    /*
     * Iterate through every node in the path, construct the expr node
     * that is needed for the remaining steps
     */
    for (int i = 0; i < list_length(path->path); i++) {
        Expr *expr = NULL;
        transform_entity *entity = NULL;


        if (i % 2 == 0) {
            cypher_node *node = NULL;
            bool output_node = false;

            node = list_nth(path->path, i);

            entity = handle_vertex(cpstate, query, path, node, i);

            cpstate->entities = lappend(cpstate->entities, entity);
            entities = lappend(entities, entity);
            prev_node_entity = entity;
        } else {
            cypher_relationship *rel = NULL;

            rel = list_nth(path->path, i);
            
            if (!rel->varlen) {
                entity = handle_edge(cpstate, query, path, rel, i, lc, prev_node_entity);

                cpstate->entities = lappend(cpstate->entities, entity);
                entities = lappend(entities, entity);
            } else {
                transform_entity *vle_entity = NULL;
                transform_entity *next_entity = NULL;
                FuncCall *fnode = NULL;

                cypher_node *node = (cypher_node *)list_nth(path->path, i + 1);

                if (!INCLUDE_NODE_IN_JOIN_TREE(path, node))
                     node->name = get_next_default_alias(cpstate);

                next_entity = handle_vertex(cpstate, query, path, node, i + 1);

                cpstate->entities = lappend(cpstate->entities, next_entity);
               entities = lappend(entities, next_entity);

                fnode = make_vle_func_call(cpstate, prev_node_entity->entity.node, rel, node);

                vle_entity = transform_VLE_edge_entity(cpstate, rel, query, fnode);

                insert_vle_entity(cpstate->entities, next_entity, vle_entity);
                insert_vle_entity(entities, next_entity, vle_entity); 

                prev_node_entity = next_entity;

                i++;
            }
        }
    }
    return entities;
}

/*
 * Iterate through the list of entities setup the join conditions. Joins
 * are driven through edges. To correctly setup the joins, we must
 * aquire information about the previous edge and vertex, and the next
 * edge and vertex.
 */
static List *make_path_join_quals(cypher_parsestate *cpstate, List *entities) {
    transform_entity *prev_node = NULL, *prev_edge = NULL, *edge = NULL, *next_node = NULL, *next_edge = NULL;
    ListCell *lc;
    List *quals = NIL;
    List *join_quals;

    // for vertex only queries, there is no work to do
    if (list_length(entities) < 3)
        return NIL;

    lc = list_head(entities);
    for (;;) {
        /*
         * Initial setup, set the initial vertex as the previous vertex
         * and get the first edge
         */
        if (prev_node == NULL) {
            prev_node = lfirst(lc);
            lc = lnext(entities, lc);
            edge = lfirst(lc);
        }

        // Retrieve the next node and edge in the pattern.
        if (lnext(entities, lc) != NULL) {
            lc = lnext(entities, lc);
            next_node = lfirst(lc);

            if (lnext(entities, lc) != NULL) {
                lc = lnext(entities, lc);
                next_edge = lfirst(lc);
            }
        }

        // create the join quals for the node
        join_quals = make_join_condition_for_edge(cpstate, prev_edge, prev_node, edge, next_node, next_edge);

        quals = list_concat(quals, join_quals);

        /* Set the edge as the previous edge and the next edge as
         * the current edge. If there is not a new edge, exit the
         * for loop.
         */
        prev_edge = edge;
        prev_node = next_node;
        edge = next_edge;
        next_node = NULL;
        next_edge = NULL;

        if (edge == NULL)
            return quals;
    }
}

/*
 * Create the path variable. Takes the list of entities, extracts the variable
 * and passes as the argument list for the _gtype_build_path function.
 */
static TargetEntry *
transform_match_create_path_variable(cypher_parsestate *cpstate, cypher_path *path, List *entities) {
    ParseState *pstate = (ParseState *)cpstate;
    Oid build_path_oid;
    FuncExpr *fexpr;
    int resno;
    List *entity_exprs = NIL;
    ListCell *lc;

    // extract the expr for each entity
    foreach (lc, entities) {
        transform_entity *entity = lfirst(lc);

        if (entity->expr != NULL)
            entity_exprs = lappend(entity_exprs, entity->expr);
    }

    // get the oid for the path creation function
    build_path_oid = get_ag_func_oid("build_traversal", 1, ANYOID);

    // build the expr node for the function
    fexpr = makeFuncExpr(build_path_oid, TRAVERSALOID, entity_exprs, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

    resno = cpstate->pstate.p_next_resno++;

    // create the target entry
    return makeTargetEntry((Expr *)fexpr, resno, path->var_name, false);
}

/*
 * Maps a column name to the a function access name. In others word when
 * passed the name for the vertex's id column name, return the function name
 * for the vertex's gtype id element, etc.
 */
static char *get_accessor_function_name(enum transform_entity_type type, char *name) {
    if (type == ENT_VERTEX) {
        if (!strcmp(AG_VERTEX_COLNAME_ID, name))
            return AG_VERTEX_ACCESS_FUNCTION_ID;
        else if (!strcmp(AG_VERTEX_COLNAME_PROPERTIES, name))
            return AG_VERTEX_ACCESS_FUNCTION_PROPERTIES;
    }
    if (type == ENT_EDGE)
    {
        if (!strcmp(AG_EDGE_COLNAME_ID, name))
            return AG_EDGE_ACCESS_FUNCTION_ID;
        else if (!strcmp(AG_EDGE_COLNAME_START_ID, name))
            return AG_EDGE_ACCESS_FUNCTION_START_ID;
        else if (!strcmp(AG_EDGE_COLNAME_END_ID, name))
            return AG_EDGE_ACCESS_FUNCTION_END_ID;
        else if (!strcmp(AG_VERTEX_COLNAME_PROPERTIES, name))
            return AG_VERTEX_ACCESS_FUNCTION_PROPERTIES;
    }

    ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
             errmsg("column %s does not have an accessor function", name)));

    // keeps compiler silent
    return NULL;
}

static char *get_alias_colref_name(char *colname, char *name) {
        if (!strcmp(AG_EDGE_COLNAME_ID, colname))
            return make_id_alias(name);
        else if (!strcmp(AG_EDGE_COLNAME_START_ID, colname))
            return make_startid_alias(name);
        else if (!strcmp(AG_EDGE_COLNAME_END_ID, colname))
            return make_endid_alias(name);
        else if (!strcmp(AG_VERTEX_COLNAME_PROPERTIES, colname))
            return make_property_alias(name);

    ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
             errmsg("column %s does not have an accessor function", name)));

    // keeps compiler silent
    return NULL;
}

static Node *make_id_expr(cypher_parsestate *cpstate, FuncExpr *arg) {
    ParseState *pstate = (ParseState *)cpstate;

    Oid func_oid = get_ag_func_oid("id", 1, VERTEXOID);

    List *args = list_make1(arg);

    FuncExpr *func_expr = makeFuncExpr(func_oid, GTYPEOID, args, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    func_expr->location = -1;

    return (Node *)func_expr;
}

/*
 * For the given entity and column name, construct an expression that will
 * access the column or get the access function if the entity is a variable.
 */
static Node *make_qual(cypher_parsestate *cpstate, transform_entity *entity, char *col_name) {
    List *qualified_name, *args;
    Node *node;

    if (IsA(entity->expr, Var)) {
        char *function_name;

        function_name = get_accessor_function_name(entity->type, col_name);

        qualified_name = list_make2(makeString(CATALOG_SCHEMA), makeString(function_name));


        args = list_make1(entity->expr);
        node = (Node *)makeFuncCall(qualified_name, args, COERCE_EXPLICIT_CALL, -1);
    } else {
    int sublevels;
    char *entity_name;
        if (entity->type == ENT_EDGE)
            entity_name = entity->entity.node->name;
        else if (entity->type == ENT_VERTEX)
            entity_name = entity->entity.rel->name;
        else
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unknown entity type")));

    ParseNamespaceItem *pnsi = refnameNamespaceItem(cpstate, NULL, entity_name, -1, &sublevels);

        if (!pnsi) {
            Node *expr = colNameToVar(cpstate, get_alias_colref_name(col_name, entity_name), false, -1);
        
        if (expr)
            return expr;    
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("could not find ParseNamespaceItem %s here", get_alias_colref_name(col_name, entity_name))));
    }
        return scanNSItemForColumn(cpstate, pnsi, sublevels, col_name, -1);
    }

    return node;
}

static Expr *transform_cypher_edge(cypher_parsestate *cpstate, cypher_relationship *rel, List **target_list) {
    ParseState *pstate = (ParseState *)cpstate;
    char *schema_name;
    char *rel_name;
    RangeVar *label_range_var;
    Alias *alias;
    int resno;
    TargetEntry *te;
    Expr *expr;
    ParseNamespaceItem *pnsi;

    if (!rel->label) {
        rel->label = AG_DEFAULT_LABEL_EDGE;
    } else {
        label_cache_data *lcd = search_label_name_graph_cache(rel->label, cpstate->graph_oid);

        if (lcd == NULL)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("label %s does not exists", rel->label),
                            parser_errposition(pstate, rel->location)));

        if (lcd->kind != LABEL_KIND_EDGE)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("label %s is for vertices, not edges", rel->label),
                     parser_errposition(pstate, rel->location)));
    }

    if (rel->name != NULL) {
        TargetEntry *te;
        Node *expr;

        /*
         * If we are in a WHERE clause transform, we don't want to create new
         * variables, we want to use the existing ones. So, error if otherwise.
         */
        if (pstate->p_expr_kind == EXPR_KIND_WHERE) {
                        transform_entity *entity = find_transform_entity(cpstate, rel->name, ENT_EDGE);
            if (entity)
                return entity->expr;
            else
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("variable `%s` does not exist", rel->name),
                         parser_errposition(pstate, rel->location)));
        }

        te = findTarget(*target_list, rel->name);
        expr = colNameToVar(pstate, rel->name, false, rel->location);

        if (expr != NULL)
            return (Expr*)expr;

        if (te != NULL) {
            transform_entity *entity = find_variable(cpstate, rel->name);
            if (entity != NULL && (entity->type != ENT_EDGE || !IS_DEFAULT_LABEL_EDGE(rel->label) || rel->props))
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("variable %s already exists", rel->name),
                         parser_errposition(pstate, rel->location)));
            return te->expr;
        }
    }

    if (!rel->name)
        rel->name = get_next_default_alias(cpstate);

    schema_name = get_graph_namespace_name(cpstate->graph_name);
    rel_name = get_label_relation_name(rel->label, cpstate->graph_oid);
    label_range_var = makeRangeVar(schema_name, rel_name, -1);
    alias = makeAlias(rel->name, NIL);

    pnsi = addRangeTableEntry(pstate, label_range_var, alias, label_range_var->inh, true);
    Assert(pnsi != NULL);

    addNSItemToQuery(pstate, pnsi, true, true, false);

    resno = pstate->p_next_resno++;

    expr = (Expr *)make_edge_expr(cpstate, pnsi);

    if (rel->name) {
        te = makeTargetEntry(expr, resno, rel->name, false);
        *target_list = lappend(*target_list, te);
    }

    // id field
    Node *id = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_ID, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(id, resno, make_id_alias(rel->name), false);
    *target_list = lappend(*target_list, te);

    // startid field
    Node *startid = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_START_ID, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(startid, resno, make_startid_alias(rel->name), false);
    *target_list = lappend(*target_list, te);

    // endid field
    Node *endid = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_END_ID, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(startid, resno, make_endid_alias(rel->name), false);
    *target_list = lappend(*target_list, te);

    // property field
    Node *prop = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_PROPERTIES, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(startid, resno, make_property_alias(rel->name), false);
    *target_list = lappend(*target_list, te);

    return expr;
}

static char *make_property_alias(char *var_name) {
    char *str = palloc0(strlen(var_name) + 8);

    str[0] = '_';
    str[1] = 'p';
    str[2] = 'r';
    str[3] = '_';

    int i = 0;
    for (; i < strlen(var_name); i++)
        str[i + 4] = var_name[i];

    str[i + 5] = '_';
    str[i + 6] = '_';
    str[i + 7] = '\0';

    return str;
}

static char *make_id_alias(char *var_name) {
    char *str = palloc0(strlen(var_name) + 8);

    str[0] = '_';
    str[1] = 'i';
    str[2] = 'd';
    str[3] = '_';

    int i = 0;
    for (; i < strlen(var_name); i++)
        str[i + 4] = var_name[i];
    str[i + 4] = '_';
    str[i + 5] = '_';
    str[i + 6] = '_';
    str[i + 7] = '\0';

    return str;
}

static char *make_startid_alias(char *var_name) {
    char *str = palloc0(strlen(var_name) + 8);

    str[0] = '_';
    str[1] = 's';
    str[2] = 't';
    str[3] = '_';

    int i = 0;
    for (; i < strlen(var_name); i++)
        str[i + 4] = var_name[i];

    str[i + 5] = '_';
    str[i + 6] = '_';
    str[i + 7] = '\0';

    return str;
}

static char *make_endid_alias(char *var_name) {
    char *str = palloc0(strlen(var_name) + 8);

    str[0] = '_';
    str[1] = 'e';
    str[2] = 'n';
    str[3] = '_';

    int i = 0;
    for (; i < strlen(var_name); i++)
        str[i + 4] = var_name[i];

    str[i + 5] = '_';
    str[i + 6] = '_';
    str[i + 7] = '\0';

    return str;
}

static Expr *transform_cypher_node(cypher_parsestate *cpstate, cypher_node *node, List **target_list, bool output_node) {
    ParseState *pstate = (ParseState *)cpstate;
    char *schema_name;
    char *rel_name;
    RangeVar *label_range_var;
    Alias *alias;
    int resno;
    TargetEntry *te;
    Expr *expr;
    ParseNamespaceItem *pnsi;

    if (!node->label) {
        node->label = AG_DEFAULT_LABEL_VERTEX;
    } else {
        label_cache_data *lcd = search_label_name_graph_cache(node->label, cpstate->graph_oid);

        if (lcd == NULL)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("label %s does not exists", node->label),
                            parser_errposition(pstate, node->location)));

    if (lcd->kind != LABEL_KIND_VERTEX)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("label %s is for edges, not vertices", node->label),
                            parser_errposition(pstate, node->location)));
    }

    if (!output_node)
        return NULL;

    if (node->name) {
        TargetEntry *te;
        Node *expr;

        /*
         * If we are in a WHERE clause transform, we don't want to create new
         * variables, we want to use the existing ones. So, error if otherwise.
         */
        if (pstate->p_expr_kind == EXPR_KIND_WHERE) {
          transform_entity *entity = find_transform_entity(cpstate, node->name, ENT_VERTEX);
            if (entity)
                return entity->expr;
        }
        else {
        te = findTarget(*target_list, node->name);
        expr = colNameToVar(pstate, node->name, false, node->location);

        if (expr)
            return (Expr*)expr;

        if (te != NULL) {
            transform_entity *entity = find_variable(cpstate, node->name);
            if (entity != NULL && (entity->type != ENT_VERTEX || !IS_DEFAULT_LABEL_VERTEX(node->label) || node->props))
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("variable %s already exists", node->name),
                         parser_errposition(pstate, node->location)));

            return te->expr;
        }
        }
    } else {
        node->name = get_next_default_alias(cpstate);
    }

    schema_name = get_graph_namespace_name(cpstate->graph_name);
    rel_name = get_label_relation_name(node->label, cpstate->graph_oid);
    label_range_var = makeRangeVar(schema_name, rel_name, -1);
    alias = makeAlias(node->name, NIL);

    pnsi = addRangeTableEntry(pstate, label_range_var, alias, label_range_var->inh, true);
    Assert(pnsi != NULL);

    addNSItemToQuery(pstate, pnsi, true, true, true);

    resno = pstate->p_next_resno++;

    expr = (Expr *)make_vertex_expr(cpstate, pnsi);

    // make target entry and add it 
    te = makeTargetEntry(expr, resno, node->name, false);
    *target_list = lappend(*target_list, te);

    // id field
    Node *id = scanNSItemForColumn(pstate, pnsi, 0, AG_VERTEX_COLNAME_ID, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(id, resno, make_id_alias(node->name), false);
    *target_list = lappend(*target_list, te);

    /*
     * properties field
     */
    Node *props = scanNSItemForColumn(pstate, pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(props, resno, make_property_alias(node->name), false);
    *target_list = lappend(*target_list, te);


    return expr;
}

static Node *make_edge_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi) {
    ParseState *pstate = (ParseState *)cpstate;

    Oid func_oid = get_ag_func_oid("build_edge", 5, GRAPHIDOID, GRAPHIDOID, GRAPHIDOID, OIDOID, GTYPEOID);

    Node *id = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_ID, -1);

    Node *start_id = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_START_ID, -1);

    Node *end_id = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_END_ID, -1);

    Const *graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false, true);

    Node *props = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_PROPERTIES, -1);

    List *args = list_make5(id, start_id, end_id, graph_oid_const, props);

    FuncExpr *func_expr = makeFuncExpr(func_oid, EDGEOID, args, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    func_expr->location = -1;

    return (Node *)func_expr;
}

static Node *make_vertex_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi) {
    ParseState *pstate = (ParseState *)cpstate;

    Oid func_oid = get_ag_func_oid("build_vertex", 3, GRAPHIDOID, OIDOID, GTYPEOID);

    Node *id = scanNSItemForColumn(pstate, pnsi, 0, AG_VERTEX_COLNAME_ID, -1);

    Const *graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false, true);

    Node * props = scanNSItemForColumn(pstate, pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1);

    List *args = list_make3(id, graph_oid_const, props);
    
    FuncExpr *func_expr = makeFuncExpr(func_oid, VERTEXOID, args, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    func_expr->location = -1;

    return (Node *)func_expr;
}

static Query *transform_cypher_create(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_create *self = (cypher_create *)clause->self;
    cypher_create_target_nodes *target_nodes;
    Const *null_const;
    List *transformed_pattern;
    FuncExpr *func_expr;
    Query *query;
    TargetEntry *tle;

    target_nodes = make_ag_node(cypher_create_target_nodes);
    target_nodes->flags = CYPHER_CLAUSE_FLAG_NONE;
    target_nodes->graph_oid = cpstate->graph_oid;

    query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    if (clause->prev) {
        handle_prev_clause(cpstate, query, clause->prev, true);

        target_nodes->flags |= CYPHER_CLAUSE_FLAG_PREVIOUS_CLAUSE;
    }

    /*
     * Create the Const Node to hold the pattern. skip the parse node,
     * because we would not be able to control how our pointer to the
     * internal type is copied.
     */
    transformed_pattern = transform_cypher_create_pattern(cpstate, query, self->pattern);

    target_nodes->paths = transformed_pattern;
    if (!clause->next)
        target_nodes->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;


    func_expr = make_clause_func_expr(CREATE_CLAUSE_FUNCTION_NAME, (Node *)target_nodes);

    // Create the target entry
    tle = makeTargetEntry((Expr *)func_expr, pstate->p_next_resno++, "_create_clause", false);
    query->targetList = lappend(query->targetList, tle);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);
if (clause->next)
   return query;

{
    Query *topquery;
cypher_parsestate *new_cpstate = make_cypher_parsestate(cpstate);
    topquery = makeNode(Query);
    topquery->commandType = CMD_SELECT;
    topquery->targetList = NIL;

    ParseState *pstate = (ParseState *) cpstate;
    int rtindex;

     ParseNamespaceItem *pnsi = transform_cypher_clause_as_subquery_2(new_cpstate, clause, NULL, false, query);
        topquery->rtable = new_cpstate->pstate.p_rtable;
        topquery->jointree = makeFromExpr(new_cpstate->pstate.p_joinlist, NULL);

    return topquery;
}

}

static List *transform_cypher_create_pattern(cypher_parsestate *cpstate, Query *query, List *pattern) {
    ListCell *lc;
    List *transformed_pattern = NIL;

    foreach (lc, pattern) {
        cypher_create_path *transformed_path;

        transformed_path = transform_cypher_create_path(cpstate, &query->targetList, lfirst(lc));

        transformed_pattern = lappend(transformed_pattern, transformed_path);
    }

    return transformed_pattern;
}

static cypher_create_path *transform_cypher_create_path(cypher_parsestate *cpstate, List **target_list, cypher_path *path) {
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *lc;
    List *transformed_path = NIL;
    cypher_create_path *ccp = make_ag_node(cypher_create_path);
    bool in_path = path->var_name != NULL;

    ccp->path_attr_num = InvalidAttrNumber;

    foreach (lc, path->path) {
        if (is_ag_node(lfirst(lc), cypher_node)) {
            cypher_node *node = lfirst(lc);
            transform_entity *entity;

            cypher_target_node *rel =
                transform_create_cypher_node(cpstate, target_list, node);

            if (in_path)
                rel->flags |= CYPHER_TARGET_NODE_IN_PATH_VAR;

            transformed_path = lappend(transformed_path, rel);

            entity = make_transform_entity(cpstate, ENT_VERTEX, (Node *)node, NULL, NULL);

            cpstate->entities = lappend(cpstate->entities, entity);
        } else if (is_ag_node(lfirst(lc), cypher_relationship)) {
            cypher_relationship *edge = lfirst(lc);
            transform_entity *entity;

            cypher_target_node *rel = transform_create_cypher_edge(cpstate, target_list, edge);

            if (in_path)
                rel->flags |= CYPHER_TARGET_NODE_IN_PATH_VAR;

            transformed_path = lappend(transformed_path, rel);

            entity = make_transform_entity(cpstate, ENT_EDGE, (Node *)edge, NULL, NULL);

            cpstate->entities = lappend(cpstate->entities, entity);
        } else {
            ereport(ERROR, (errmsg_internal("unreconized node in create pattern")));
        }
    }

    ccp->target_nodes = transformed_path;

    /*
     * If this path a variable, create a placeholder entry that we can fill
     * in with during the execution phase.
     */
    if (path->var_name) {
        TargetEntry *te;

        if (list_length(transformed_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("paths require at least 1 vertex"),
                     parser_errposition(pstate, path->location)));

        te = placeholder_traversal(cpstate, path->var_name);

        ccp->path_attr_num = te->resno;

        *target_list = lappend(*target_list, te);
    }

    return ccp;
}

static cypher_target_node * transform_create_cypher_edge(cypher_parsestate *cpstate, List **target_list, cypher_relationship *edge) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_target_node *rel = make_ag_node(cypher_target_node);
    Expr *props;
    Relation label_relation;
    RangeVar *rv;
    RangeTblEntry *rte;
    TargetEntry *te;
    char *alias;
    AttrNumber resno;
    ParseNamespaceItem *pnsi;

    if (edge->label)
    {
        label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);

        if (lcd && lcd->kind != LABEL_KIND_EDGE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("label %s is for vertices, not edges", edge->label),
                     parser_errposition(pstate, edge->location)));
    }

    rel->type = LABEL_KIND_EDGE;
    rel->flags = CYPHER_TARGET_NODE_FLAG_INSERT;
    rel->label_name = edge->label;
    rel->resultRelInfo = NULL;

    if (edge->name) {
        /*
         * Variables can be declared in a CREATE clause, but not used if
         * it already exists.
         */
        if (variable_exists(cpstate, edge->name))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("variable %s already exists", edge->name)));

        rel->variable_name = edge->name;
        te = placeholder_edge(cpstate, edge->name);
        rel->tuple_position = te->resno;
        *target_list = lappend(*target_list, te);

        rel->flags |= CYPHER_TARGET_NODE_IS_VAR;
    } else {
        rel->variable_name = NULL;
        rel->tuple_position = 0;
    edge->name = get_next_default_alias(cpstate);
    }

    if (edge->dir == CYPHER_REL_DIR_NONE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("only directed relationships are allowed in CREATE"),
                 parser_errposition(&cpstate->pstate, edge->location)));

    rel->dir = edge->dir;

    if (!edge->label)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("relationships must be specify a label in CREATE."),
                 parser_errposition(&cpstate->pstate, edge->location)));

    // create the label entry if it does not exist
    if (!label_exists(edge->label, cpstate->graph_oid)) {
        List *parent;
        RangeVar *rv;

        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_EDGE);

        parent = list_make1(rv);

        create_label(cpstate->graph_name, edge->label, LABEL_TYPE_EDGE, parent);
    }

    // lock the relation of the label
    rv = makeRangeVar(cpstate->graph_name, edge->label, -1);
    label_relation = parserOpenTable(&cpstate->pstate, rv, RowExclusiveLock);

    // Store the relid
    rel->relid = RelationGetRelid(label_relation);

    pnsi = addRangeTableEntryForRelation((ParseState *)cpstate, label_relation, AccessShareLock, NULL, false, false);
    rte = pnsi->p_rte;
    rte->requiredPerms = ACL_INSERT;

    // Build Id expression, always use the default logic
    rel->id_expr = (Expr *)build_column_default(label_relation, Anum_ag_label_edge_table_id);

    // Build properties expression, if no map is given, use the default logic
    alias = make_property_alias(edge->name);//get_next_default_alias(cpstate);
    resno = pstate->p_next_resno++;

    props = cypher_create_properties(cpstate, rel, label_relation, edge->props, ENT_EDGE);

    rel->prop_attr_num = resno - 1;
    te = makeTargetEntry(props, resno, alias, false);

    *target_list = lappend(*target_list, te);


    // id field
    Node *id = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_ID, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(id, resno, make_id_alias(edge->name), false);
    *target_list = lappend(*target_list, te);

    // startid field
    Node *startid = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_START_ID, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(startid, resno, make_startid_alias(edge->name), false);
    *target_list = lappend(*target_list, te);

    // endid field
    Node *endid = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_END_ID, -1);
   resno = pstate->p_next_resno++;

    te = makeTargetEntry(startid, resno, make_endid_alias(edge->name), false);
    *target_list = lappend(*target_list, te);


    // Keep the lock
    table_close(label_relation, NoLock);

    return rel;
}

static bool variable_exists(cypher_parsestate *cpstate, char *name) {
    ParseState *pstate = (ParseState *)cpstate;
    Node *id;

    if (name == NULL)
        return false;

    ParseNamespaceItem *pnsi = find_pnsi(cpstate, PREV_CYPHER_CLAUSE_ALIAS);
    if (pnsi) {
        id = scanNSItemForColumn(pstate, pnsi, 0, name, -1);

        return id != NULL;
    }

    return false;
}

// transform nodes, check to see if the variable name already exists.
static cypher_target_node * transform_create_cypher_node(cypher_parsestate *cpstate, List **target_list, cypher_node *node) {
    ParseState *pstate = (ParseState *)cpstate;

    if (node->label) {
        label_cache_data *lcd = search_label_name_graph_cache(node->label, cpstate->graph_oid);

        if (lcd && lcd->kind != LABEL_KIND_VERTEX)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("label %s is for edges, not vertices", node->label),
                            parser_errposition(pstate, node->location)));
    }

    /*
     *  Check if the variable already exists, if so find the entity and
     *  setup the target node
     */
    if (node->name) {
        transform_entity *entity;

        entity = find_variable(cpstate, node->name);

        if (entity) {
            if (entity->type != ENT_VERTEX)
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("variable %s already exists", node->name),
                         parser_errposition(pstate, node->location)));

            return transform_create_cypher_existing_node(cpstate, target_list, entity->declared_in_current_clause, node);
        }
    }

    // otherwise transform the target node as a new node
    return transform_create_cypher_new_node(cpstate, target_list, node);
}

/*
* Returns the resno for the TargetEntry with the resname equal to the name
* passed. Returns -1 otherwise.
*/
static int get_target_entry_resno(cypher_parsestate *cpstate, List *target_list, char *name) {
    ListCell *lc;

    foreach (lc, target_list) {
        TargetEntry *te = (TargetEntry *)lfirst(lc);
 
        if (!strcmp(te->resname, name)) {
            enum transform_entity_type type = find_transform_entity_type(cpstate, name);
            if (type == ENT_VERTEX)
                te->expr = add_volatile_vertex_wrapper(te->expr);
            else if (type == ENT_EDGE)
                te->expr = add_volatile_edge_wrapper(te->expr);
            else if (type == ENT_VLE_EDGE)
                te->expr = add_volatile_vle_edge_wrapper(te->expr);
            else
                te->expr = add_volatile_wrapper(te->expr);
            return te->resno;
        }
    }

    return -1;
}

/*
* Transform logic for a previously declared variable in a CREATE clause.
* All we need from the variable node is its id, and whether we can skip
* some tests in the execution phase..
*/
static cypher_target_node *transform_create_cypher_existing_node(
    cypher_parsestate *cpstate, List **target_list, bool declared_in_current_clause, cypher_node *node) {
    cypher_target_node *rel = make_ag_node(cypher_target_node);

    rel->type = LABEL_KIND_VERTEX;
    rel->flags = CYPHER_TARGET_NODE_FLAG_NONE;
    rel->resultRelInfo = NULL;
    rel->variable_name = node->name;

    if (node->props)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("previously declared nodes in a create clause cannot have properties")));
    if (node->label)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("previously declared variables cannot have a label")));
    /*
     * When the variable is declared in the same clause this vertex is a part of
     * we can skip some expensive checks in the execution phase.
     */
    if (declared_in_current_clause)
        rel->flags |= EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE;

    /*
     * Get the AttrNumber the variable is stored in, so we can extract the id
     * later.
     */
    rel->tuple_position = get_target_entry_resno(cpstate, *target_list, node->name);

    return rel;
}


static Node *
make_int_placeholder(cypher_parsestate *cpstate) {
        
    Datum agt = integer_to_gtype(0);
        
    // typtypmod, typcollation, typlen, and typbyval of gtype are hard-coded.
    Const *c = makeConst(GTYPEOID, -1, InvalidOid, -1, agt, false, false);
    c->location = -1;

    return (Node *)c;
}       

/*
 * Transform logic for a node in a create clause that was not previously
 * declared.
 */
static cypher_target_node *transform_create_cypher_new_node(cypher_parsestate *cpstate, List **target_list, cypher_node *node) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_target_node *rel = make_ag_node(cypher_target_node);
    Relation label_relation;
    RangeVar *rv;
    RangeTblEntry *rte;
    TargetEntry *te;
    Expr *props;
    char *alias;
    int resno;
    ParseNamespaceItem *pnsi;

    rel->type = LABEL_KIND_VERTEX;
    rel->tuple_position = InvalidAttrNumber;
    rel->variable_name = NULL;
    rel->resultRelInfo = NULL;

    if (!node->label) {
        rel->label_name = "";
        /*
         *  If no label is specified, assign the generic label name that
         *  all labels are descendents of.
         */
        node->label = AG_DEFAULT_LABEL_VERTEX;
    } else {
        rel->label_name = node->label;
    }

    if (!node->name) {
        node->name = get_next_default_alias(cpstate);
    }


    // create the label entry if it does not exist
    if (!label_exists(node->label, cpstate->graph_oid))
    {
        List *parent;
        RangeVar *rv;

        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_VERTEX);

        parent = list_make1(rv);

        create_label(cpstate->graph_name, node->label, LABEL_TYPE_VERTEX, parent);
    }

    rel->flags = CYPHER_TARGET_NODE_FLAG_INSERT;

    rv = makeRangeVar(cpstate->graph_name, node->label, -1);
    label_relation = parserOpenTable(&cpstate->pstate, rv, RowExclusiveLock);

    // Store the relid
    rel->relid = RelationGetRelid(label_relation);

    pnsi = addRangeTableEntryForRelation((ParseState *)cpstate, label_relation, AccessShareLock, NULL, false, false);
    rte = pnsi->p_rte;
    rte->requiredPerms = ACL_INSERT;

    // id
    rel->id_expr = (Expr *)build_column_default(label_relation, Anum_ag_label_vertex_table_id);
    
    resno = pstate->p_next_resno++;
    te = makeTargetEntry(make_int_placeholder(cpstate), resno, make_id_alias(node->name), false);
    *target_list = lappend(*target_list, te);

    // properties
    alias = make_property_alias(node->name);//get_next_default_alias(cpstate);
    resno = pstate->p_next_resno++;

    props = cypher_create_properties(cpstate, rel, label_relation, node->props, ENT_VERTEX);

    rel->prop_attr_num = resno - 1;
    te = makeTargetEntry(props, resno, alias, false);
    *target_list = lappend(*target_list, te);

    table_close(label_relation, NoLock);

    if (node->name) {
        rel->variable_name = node->name;
        te = placeholder_vertex(cpstate, node->name);
        rel->tuple_position = te->resno;
        *target_list = lappend(*target_list, te);
        rel->flags |= CYPHER_TARGET_NODE_IS_VAR;
    } else {
        node->name = get_next_default_alias(cpstate);
    }
    // id field
    /*
     Node *id = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_ID, -1);
     resno = pstate->p_next_resno++;

    te = makeTargetEntry(id, resno, make_id_alias(edge->name), false);
    *target_list = lappend(*target_list, te);
    */
    return rel;
}


static TargetEntry *placeholder_edge(cypher_parsestate *cpstate, char *name) {
    ParseState *pstate = (ParseState *)cpstate;
    Expr *n;
    int resno;

    n = (Expr *)makeNullConst(EDGEOID, -1, InvalidOid);

    resno = pstate->p_next_resno++;

    return makeTargetEntry(n, resno, name, false);
}

static TargetEntry *placeholder_vertex(cypher_parsestate *cpstate, char *name) {
    ParseState *pstate = (ParseState *)cpstate;
    Expr *n;
    int resno;

    n = (Expr *)makeNullConst(VERTEXOID, -1, InvalidOid);

    resno = pstate->p_next_resno++;

    return makeTargetEntry(n, resno, name, false);
}

static TargetEntry *placeholder_traversal(cypher_parsestate *cpstate, char *name) {
    ParseState *pstate = (ParseState *)cpstate;
    Expr *n;
    int resno;

    n = (Expr *)makeNullConst(TRAVERSALOID, -1, InvalidOid);

    resno = pstate->p_next_resno++;

    return makeTargetEntry(n, resno, name, false);
}


/*
 * Build the target list for an entity that is not a previously declared
 * variable.
 */
static Expr *cypher_create_properties(cypher_parsestate *cpstate, cypher_target_node *rel,
                                      Relation label_relation, Node *props, enum transform_entity_type type) {
    Expr *properties;

    if (props != NULL && is_ag_node(props, cypher_param)) {
        ParseState *pstate = (ParseState *) cpstate;
        cypher_param *param = (cypher_param *)props;

        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("properties in a CREATE clause as a parameter is not supported"),
                 parser_errposition(pstate, param->location)));
    }

    if (props)
        properties = (Expr *)transform_cypher_expr(cpstate, props, EXPR_KIND_INSERT_TARGET);
    else if (type == ENT_VERTEX)
        properties = (Expr *)build_column_default(label_relation, Anum_ag_label_vertex_table_properties);
    else if (type == ENT_EDGE)
        properties = (Expr *)build_column_default(label_relation, Anum_ag_label_edge_table_properties);
    else
        ereport(ERROR, (errmsg_internal("unreconized entity type")));

    // add a volatile wrapper call to prevent the optimizer from removing it
    return (Expr *)add_volatile_wrapper(properties);
}

/*
 * This function is similar to transformFromClause() that is called with a
 * single RangeSubselect.
 */
static ParseNamespaceItem *
transform_cypher_clause_as_subquery_2(cypher_parsestate *cpstate, cypher_clause *clause, Alias *alias, bool add_rte_to_query, Query *query) {
    ParseState *pstate = (ParseState *)cpstate;
    ParseExprKind old_expr_kind = pstate->p_expr_kind;
    bool lateral = pstate->p_lateral_active;


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

    //if (add_rte_to_query)
        addNSItemToQuery(pstate, pnsi, true, false, true);

    return pnsi;
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

static TargetEntry *findTarget(List *targetList, char *resname) {
    ListCell *lt;
    TargetEntry *te = NULL;

    if (resname == NULL)
        return NULL;

    foreach (lt, targetList) {
        te = lfirst(lt);

        if (te->resjunk)
            continue;

        if (strcmp(te->resname, resname) == 0)
            return te;
    }

    return NULL;
}

/*
 * Wrap the expression with a volatile function, to prevent the optimer from
 * elimating the expression.
 */
static Expr *add_volatile_wrapper(Expr *node) {
    Oid oid = get_ag_func_oid("gtype_volatile_wrapper", 1, GTYPEOID);

    return (Expr *)makeFuncExpr(oid, GTYPEOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

static Expr *add_volatile_edge_wrapper(Expr *node) {
    Oid oid = get_ag_func_oid("gtype_volatile_wrapper", 1, EDGEOID);
    
    return (Expr *)makeFuncExpr(oid, EDGEOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}   

static Expr *add_volatile_vle_edge_wrapper(Expr *node) {
    Oid oid = get_ag_func_oid("gtype_volatile_wrapper", 1, VARIABLEEDGEOID);
            
    return (Expr *)makeFuncExpr(oid, VARIABLEEDGEOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}           
      
static Expr *add_volatile_vertex_wrapper(Expr *node) {
    Oid oid = get_ag_func_oid("gtype_volatile_wrapper", 1, VERTEXOID);

    return (Expr *)makeFuncExpr(oid, VERTEXOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

static Expr *add_volatile_traversal_wrapper(Expr *node) {
    Oid oid = get_ag_func_oid("gtype_volatile_wrapper", 1, TRAVERSALOID);

    return (Expr *)makeFuncExpr(oid, TRAVERSALOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}


/*
 * from postgresql parse_sub_analyze
 * Modified entry point for recursively analyzing a sub-statement in union.
 */
Query *cypher_parse_sub_analyze_union(cypher_clause *clause, cypher_parsestate *cpstate, CommonTableExpr *parentCTE,
                                bool locked_from_parent, bool resolve_unknowns) {
    cypher_parsestate *state = make_cypher_parsestate(cpstate);
    Query *query;

    state->pstate.p_parent_cte = parentCTE;
    state->pstate.p_locked_from_parent = locked_from_parent;
    state->pstate.p_resolve_unknowns = resolve_unknowns;

    query = transform_cypher_clause(state, clause);

    free_cypher_parsestate(state);

    return query;
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
 * Function for transforming MERGE.
 *
 * There are two cases for the form Query that is returned from here will
 * take:
 *
 * 1. If there is no previous clause, the query will have a subquery that
 * represents the path as a select staement, similar to match with a targetList
 * that is all declared variables and the FuncExpr that represents the MERGE
 * clause with its needed metadata information, that will be caught in the
 * planner phase and converted into a path.
 *
 * 2. If there is a previous clause then the query will have two subqueries.
 * The first query will be for the previous clause that we recursively handle.
 * The second query will be for the path that this MERGE clause defines. The
 * two subqueries will be joined together using a LATERAL LEFT JOIN with the
 * previous query on the left and the MERGE path subquery on the right. Like
 * case 1 the targetList will have all the decalred variables and a FuncExpr
 * that represents the MERGE clause with its needed metadata information, that
 * will be caught in the planner phase and converted into a path.
 *
 * This will allow us to be capable of handling the 2 cases that exist with a
 * MERGE clause correctly.
 *
 * Case 1: the path already exists. In this case we do not need to create
 * the path and MERGE will simply pass the tuple information up the execution
 * tree.
 *
 * Case 2: the path does not exist. In this case the LEFT part of the join
 * will not prevent the tuples from the previous clause from being emitted. We
 * can catch when this happens in the execution phase and create the missing
 * data, before passing up the execution tree.
 *
 * It should be noted that both cases can happen in the same query. If the
 * MERGE clause references a variable from a previous clause, it could be that
 * for one tuple the path exists (or there is multiple paths that exist and all
 * paths must be emitted) and for another the path does not exist. This is
 * similar to OPTIONAL MATCH, however with the added feature of creating the
 * path if not there, rather than just emiting NULL.
 */
static Query *transform_cypher_merge(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *) cpstate;
    cypher_clause *merge_clause_as_match;
    cypher_create_path *merge_path;
    cypher_merge *self = (cypher_merge *)clause->self;
    cypher_merge_information *merge_information;
    Query *query;
    FuncExpr *func_expr;
    TargetEntry *tle;

    Assert(is_ag_node(self->path, cypher_path));

    merge_information = make_ag_node(cypher_merge_information);

    query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    Const *null_const = makeNullConst(GTYPEOID, -1, InvalidOid);
    tle = makeTargetEntry((Expr *)null_const, pstate->p_next_resno++, "_null", false);
    query->targetList = lappend(query->targetList, tle);

    merge_information->flags = CYPHER_CLAUSE_FLAG_NONE;

    // make the merge node into a match node
    merge_clause_as_match = convert_merge_to_match(self);

    /*
     * If there is a previous clause we need to turn this query into a lateral
     * join. See the function transform_merge_make_lateral_join for details.
     */
    if (clause->prev != NULL) {
        merge_path = transform_merge_make_lateral_join(cpstate, query, clause, merge_clause_as_match);

        merge_information->flags |= CYPHER_CLAUSE_FLAG_PREVIOUS_CLAUSE;
    } else {
        // make the merge node into a match node
        cypher_clause *merge_clause_as_match = convert_merge_to_match(self);

        // Create the metadata needed for creating missing paths.
        merge_path = transform_cypher_merge_path(cpstate, &query->targetList, (cypher_path *)self->path);

        /*
         * If there is not a previous clause, then treat the MERGE's path
         * itself as the previous clause. We need to do this because if the
         * pattern exists, then we need to path all paths that match the
         * query patterns in the execution phase. WE way to do that by
         * converting the merge to a match and have the match logic create the
         * query. the merge execution phase will just pass the results up the
         * execution tree if the path exists.
         */
        handle_prev_clause(cpstate, query, merge_clause_as_match, false);

        /*
         * For the metadata need to create paths, find the tuple position that
         * will represent the entity in the execution phase.
         */
        transform_cypher_merge_mark_tuple_position(cpstate, query->targetList, merge_path);
    }

    merge_information->graph_oid = cpstate->graph_oid;
    merge_information->path = merge_path;

    if (!clause->next)
        merge_information->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;

    // Creates the function expression that the planner will find and convert to a MERGE path.
    func_expr = make_clause_func_expr(MERGE_CLAUSE_FUNCTION_NAME, (Node *)merge_information);

    // Create the target entry
    tle = makeTargetEntry((Expr *)func_expr, pstate->p_next_resno++, "_merge_clause", false);

    merge_information->merge_function_attr = tle->resno;
    query->targetList = lappend(query->targetList, tle);

    markTargetListOrigins(pstate, query->targetList);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    query->hasSubLinks = pstate->p_hasSubLinks;

    assign_query_collations(pstate, query);

    if (clause->next)
        return query;
    {
        Query *topquery;
        cypher_parsestate *new_cpstate = make_cypher_parsestate(cpstate);
        topquery = makeNode(Query);
        topquery->commandType = CMD_SELECT;
        topquery->targetList = NIL;

        ParseState *pstate = (ParseState *) cpstate;
        int rtindex;

        ParseNamespaceItem *pnsi = transform_cypher_clause_as_subquery_2(new_cpstate, clause, NULL, false, query);
        topquery->rtable = new_cpstate->pstate.p_rtable;
        topquery->jointree = makeFromExpr(new_cpstate->pstate.p_joinlist, NULL);

        return topquery;
    }
}

/*
 * This function does the heavy lifting of transforming a MERGE clause that has
 * a clause before it in the query of turning that into a lateral left join.
 * The previous clause will still be able to emit tuples if the path defined in
 * MERGE clause is not found. In that case variable assinged in the MERGE
 * clause will be emitted as NULL (same as OPTIONAL MATCH).
 */
static cypher_create_path *
transform_merge_make_lateral_join(cypher_parsestate *cpstate, Query *query, cypher_clause *clause,
                                  cypher_clause *isolated_merge_clause) {
    cypher_create_path *merge_path;
    ParseState *pstate = (ParseState *) cpstate;
    int i;
    Alias *l_alias;
    Alias *r_alias;
    RangeTblEntry *l_rte, *r_rte;
    ParseNamespaceItem *l_nsitem, *r_nsitem;
    JoinExpr *j = makeNode(JoinExpr);
    List *res_colnames = NIL, *res_colvars = NIL;
    ParseNamespaceItem *jnsitem;
    ParseExprKind tmp;
    cypher_merge *self = (cypher_merge *)clause->self;
    cypher_path *path;

    Assert(is_ag_node(self->path, cypher_path));

    path = (cypher_path *)self->path;

    r_alias = makeAlias(CYPHER_OPT_RIGHT_ALIAS, NIL);
    l_alias = makeAlias(PREV_CYPHER_CLAUSE_ALIAS, NIL);

    j->jointype = JOIN_LEFT;

    /*
     * transform the previous clause
     */
    j->larg = transform_clause_for_join(cpstate, clause->prev, &l_rte, &l_nsitem, l_alias);
    pstate->p_namespace = lappend(pstate->p_namespace, l_nsitem);

    /*
     * Get the merge path now. This is the only moment where it is simple
     * to know if a variable was declared in the MERGE clause or a previous
     * clause. Unlike create, we do not add these missing variables to the
     * targetList, we just create all the metadata necessary to make the
     * potentially missing parts of the path.
     */
    merge_path = transform_cypher_merge_path(cpstate, &query->targetList, path);

    /*
     * Transform this MERGE clause as a match clause, mark the parsestate
     * with the flag that a lateral join is active
     */
    pstate->p_lateral_active = true;
    tmp = pstate->p_expr_kind;
    pstate->p_expr_kind = EXPR_KIND_OTHER;

    // transform MERGE
    j->rarg = transform_clause_for_join(cpstate, isolated_merge_clause, &r_rte, &r_nsitem, r_alias);

    // deactivate the lateral flag
    pstate->p_lateral_active = false;

    pstate->p_namespace = NIL;

    /*
     * Resolve the column names and variables between the two subqueries,
     * in most cases, we can expect there to be overlap
     */
    get_res_cols(pstate, l_nsitem, r_nsitem, &res_colnames, &res_colvars);

    // make the RTE for the join
    jnsitem = addRangeTableEntryForJoin(pstate, res_colnames, NULL, j->jointype, 0, res_colvars, NIL,
                        NIL, j->alias, NULL, true);

    j->rtindex = jnsitem->p_rtindex;

    /*
     * The index of a node in the p_joinexpr list is expected to match the
     * rtindex the join expression is for. Add NULLs for all the previous
     * rtindexes and add the JoinExpr.
     */
    for (i = list_length(pstate->p_joinexprs) + 1; i < j->rtindex; i++)
        pstate->p_joinexprs = lappend(pstate->p_joinexprs, NULL);

    pstate->p_joinexprs = lappend(pstate->p_joinexprs, j);

    Assert(list_length(pstate->p_joinexprs) == j->rtindex);

    pstate->p_joinlist = lappend(pstate->p_joinlist, j);

    pstate->p_expr_kind = tmp;

    // add jnsitem to column namespace only 
    addNSItemToQuery(pstate, jnsitem, false, true, true);

    /*
     * Create the targetList from the joined subqueries, add everything.
     */
    query->targetList = list_concat(query->targetList, make_target_list_from_join(pstate, jnsitem->p_rte));

    /*
     * For the metadata need to create paths, find the tuple position that
     * will represent the entity in the execution phase.
     */
    transform_cypher_merge_mark_tuple_position(cpstate, query->targetList, merge_path);

    return merge_path;
}

/*
 * Iterate through the path and find the TargetEntry in the target_list
 * that each cypher_target_node is referencing. Add the volatile wrapper
 * function to keep the optimizer from removing the TargetEntry.
 */
static void transform_cypher_merge_mark_tuple_position(cypher_parsestate *cpstate, List *target_list, cypher_create_path *path) {
    ListCell *lc = NULL;

    if (path->var_name) {
        TargetEntry *te = findTarget(target_list, path->var_name);

        /*
         * Add the volatile wrapper function around the expression, this
         * ensures the optimizer will not remove the expression, if nothing
         * other than a private data structure needs it.
         */
        te->expr = add_volatile_traversal_wrapper(te->expr);
        // Mark the tuple position the target_node is for.
        path->path_attr_num = te->resno;
    }

    foreach (lc, path->target_nodes) {
        cypher_target_node *node = lfirst(lc);

        TargetEntry *te = findTarget(target_list, node->variable_name);
    enum transform_entity_type type = find_transform_entity_type(cpstate, node->variable_name);
        /*
         * Add the volatile wrapper function around the expression, this
         * ensures the optimizer will not remove the expression, if nothing
         * other than a private data structure needs it.
         */
    if (type == ENT_VERTEX) 
            te->expr = add_volatile_vertex_wrapper(te->expr);
        else if (type == ENT_EDGE)
            te->expr = add_volatile_edge_wrapper(te->expr);
    else if (type == ENT_VLE_EDGE)
            te->expr = add_volatile_vle_edge_wrapper(te->expr);
    else
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("rte must be last entry in p_rtable")));

        // Mark the tuple position the target_node is for.
        node->tuple_position = te->resno;
    }
}

/*
 * Creates the target nodes for a merge path. If MERGE has a path that doesn't
 * exist then in the MERGE clause we act like a CREATE clause. This function
 * sets up the metadata needed for that process.
 */
static cypher_create_path *transform_cypher_merge_path(cypher_parsestate *cpstate, List **target_list, cypher_path *path) {
    ListCell *lc;
    List *transformed_path = NIL;
    cypher_create_path *ccp = make_ag_node(cypher_create_path);
    bool in_path = path->var_name != NULL;

    ccp->path_attr_num = InvalidAttrNumber;

    foreach (lc, path->path) {
        if (is_ag_node(lfirst(lc), cypher_node)) {
            cypher_node *node = lfirst(lc);

            cypher_target_node *rel = transform_merge_cypher_node(cpstate, target_list, node);

            if (in_path)
                rel->flags |= CYPHER_TARGET_NODE_IN_PATH_VAR;

            transformed_path = lappend(transformed_path, rel);
        } else if (is_ag_node(lfirst(lc), cypher_relationship)) {
            cypher_relationship *edge = lfirst(lc);

            cypher_target_node *rel = transform_merge_cypher_edge(cpstate, target_list, edge);

            if (in_path)
                rel->flags |= CYPHER_TARGET_NODE_IN_PATH_VAR;

            transformed_path = lappend(transformed_path, rel);
        } else {
            ereport(ERROR, (errmsg_internal("unreconized node in create pattern")));
        }
    }

    // store the path's variable name
    if (path->var_name)
        ccp->var_name = path->var_name;

    ccp->target_nodes = transformed_path;

    return ccp;
}

/*
 * Transforms the parse cypher_relationship to a target_entry for merge.
 * All edges that have variables assigned in a merge must be declared in
 * the merge. Throw an error otherwise.
 */
static cypher_target_node * transform_merge_cypher_edge(cypher_parsestate *cpstate, List **target_list, cypher_relationship *edge) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_target_node *rel = make_ag_node(cypher_target_node);
    Relation label_relation;
    RangeVar *rv;
    RangeTblEntry *rte;
    ParseNamespaceItem *pnsi;

    if (edge->name != NULL) {
        transform_entity *entity = find_transform_entity(cpstate, edge->name, ENT_EDGE);

        // We found a variable with this variable name, throw an error.
    if (entity != NULL)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("variable %s already exists", edge->name),
                 parser_errposition(pstate, edge->location)));

        rel->flags |= CYPHER_TARGET_NODE_IS_VAR;
    } else {
        // assign a default variable name.
        edge->name = get_next_default_alias(cpstate);
    }

    rel->type = LABEL_KIND_EDGE;

    // all edges are marked with insert
    rel->flags |= CYPHER_TARGET_NODE_FLAG_INSERT;
    rel->label_name = edge->label;
    rel->variable_name = edge->name;
    rel->resultRelInfo = NULL;

    rel->dir = edge->dir;

    if (!edge->label)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("edges declared in a MERGE clause must have a label"),
                 parser_errposition(&cpstate->pstate, edge->location)));

    // check to see if the label exists, create the label entry if it does not.
    if (edge->label && !label_exists(edge->label, cpstate->graph_oid)) {
        List *parent;
        RangeVar *rv;

        // setup the default edge table as the parent table, that we will inherit from.
        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_EDGE);

        parent = list_make1(rv);

        // create the label
        create_label(cpstate->graph_name, edge->label, LABEL_TYPE_EDGE, parent);
    }

    // lock the relation of the label
    rv = makeRangeVar(cpstate->graph_name, edge->label, -1);
    label_relation = parserOpenTable(&cpstate->pstate, rv, RowExclusiveLock);

    // Store the relid
    rel->relid = RelationGetRelid(label_relation);

    pnsi = addRangeTableEntryForRelation((ParseState *)cpstate, label_relation, AccessShareLock, NULL, false, false);
    rte = pnsi->p_rte;
    rte->requiredPerms = ACL_INSERT;

    // Build Id expression, always use the default logic
    rel->id_expr = (Expr *)build_column_default(label_relation, Anum_ag_label_edge_table_id);

    rel->prop_expr = cypher_create_properties(cpstate, rel, label_relation, edge->props, ENT_EDGE);

    // Keep the lock
    table_close(label_relation, NoLock);

    return rel;
}

/*
 * Function for creating the metadata MERGE will need if MERGE does not find a path to exist
 */
static cypher_target_node *transform_merge_cypher_node(cypher_parsestate *cpstate, List **target_list, cypher_node *node) {
    cypher_target_node *rel = make_ag_node(cypher_target_node);
    Relation label_relation;
    RangeVar *rv;
    RangeTblEntry *rte;
    ParseNamespaceItem *pnsi;

    if (node->name) {
        transform_entity *entity = find_transform_entity(cpstate, node->name, ENT_VERTEX);

        // the vertex was previously declared, we do not need to do any setup to create the node.
        if (entity) {
            rel->type = LABEL_KIND_VERTEX;
            rel->tuple_position = InvalidAttrNumber;
            rel->variable_name = node->name;
            rel->resultRelInfo = NULL;

            rel->flags |= CYPHER_TARGET_NODE_MERGE_EXISTS;
            return rel;
        }
        rel->flags |= CYPHER_TARGET_NODE_IS_VAR;
    } else {
        // assign a default variable name.
        node->name = get_next_default_alias(cpstate);
    }

    rel->type = LABEL_KIND_VERTEX;
    rel->tuple_position = InvalidAttrNumber;
    rel->variable_name = node->name;
    rel->resultRelInfo = NULL;

    if (!node->label) {
        rel->label_name = "";
        node->label = AG_DEFAULT_LABEL_VERTEX;
    } else {
        rel->label_name = node->label;
    }

    // check to see if the label exists, create the label entry if it does not.
    if (node->label && !label_exists(node->label, cpstate->graph_oid)) {
        List *parent;
        RangeVar *rv;

        // setup the default vertex table as the parent table, that we will inherit from.
        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_VERTEX);

        parent = list_make1(rv);

        // create the label
        create_label(cpstate->graph_name, node->label, LABEL_TYPE_VERTEX, parent);
    }

    rel->flags |= CYPHER_TARGET_NODE_FLAG_INSERT;

    rv = makeRangeVar(cpstate->graph_name, node->label, -1);
    label_relation = parserOpenTable(&cpstate->pstate, rv, RowExclusiveLock);

    // Store the relid
    rel->relid = RelationGetRelid(label_relation);

    pnsi = addRangeTableEntryForRelation((ParseState *)cpstate, label_relation, AccessShareLock, NULL, false, false);
    rte = pnsi->p_rte;
    rte->requiredPerms = ACL_INSERT;

    // id
    rel->id_expr = (Expr *)build_column_default(label_relation, Anum_ag_label_vertex_table_id);

    rel->prop_expr = cypher_create_properties(cpstate, rel, label_relation, node->props, ENT_VERTEX);

    table_close(label_relation, NoLock);

    return rel;
}

// Takes a MERGE parse node and converts it to a MATCH parse node
static cypher_clause *convert_merge_to_match(cypher_merge *merge) {
    cypher_match *match = make_ag_node(cypher_match);
    cypher_clause *clause = palloc(sizeof(cypher_clause));

    // match supports multiple paths, whereas merge only supports one.
    match->pattern = list_make1(merge->path);
    // MERGE does not support where
    match->where = NULL;

    /*
     *  We do not want the transform logic to transform the previous clauses
     *  with this, just handle this one clause.
     */
    clause->prev = NULL;
    clause->self = (Node *)match;
    clause->next = NULL;

    return clause;
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

/*
 * Creates the function expression that represents the clause. Adds the
 * extensible node that represents the metadata that the clause needs to
 * handle the clause in the execution phase.
 */
static FuncExpr *make_clause_func_expr(char *function_name, Node *clause_information) {
    StringInfo str = makeStringInfo();
    /*
     * Serialize the clause_information data structure. In certain
     * cases (Prepared Statements and PL/pgsql), the MemoryContext that
     * it is stored in will be destroyed. We need to get it into a format
     * that Postgres' can copy between MemoryContexts. Just making it into
     * an ExtensibleNode does not work, because there are certain parts of
     * Postgres that cannot handle an ExtensibleNode in a function call.
     * So we serialize the data structure and place it into a Const node
     * that can handle these situations AND be copied correctly.
     */
    outNode(str, clause_information);

    Const *c = makeConst(INTERNALOID, -1, InvalidOid, str->len, PointerGetDatum(str->data), false, false);

    Oid func_oid = get_ag_func_oid(function_name, 1, INTERNALOID);

    return makeFuncExpr(func_oid, GTYPEOID, list_make1(c), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

/*
 * Utility function that helps a clause add the information needed to
 * the query from the previous clause.
 */
static void handle_prev_clause(cypher_parsestate *cpstate, Query *query, cypher_clause *clause, bool first_rte) {
    ParseState *pstate = (ParseState *) cpstate;
    int rtindex;
    ParseNamespaceItem *pnsi;

    pnsi = transform_prev_cypher_clause(cpstate, clause, true);

    rtindex = list_length(pstate->p_rtable);

    // rte is the first RangeTblEntry in pstate
    if (first_rte)
        Assert(rtindex == 1);

    // add all the rte's attributes to the current queries targetlist
    query->targetList = list_concat(query->targetList, expandNSItemAttrs(pstate, pnsi, 0, -1));
}

ParseNamespaceItem *find_pnsi(cypher_parsestate *cpstate, char *varname) {
    ParseState *pstate = (ParseState *) cpstate;
    ListCell *lc;

    foreach (lc, pstate->p_namespace) {
        ParseNamespaceItem *pnsi = (ParseNamespaceItem *)lfirst(lc);
        Alias *alias = pnsi->p_rte->alias;
        if (!alias)
            continue;

        if (!strcmp(alias->aliasname, varname))
            return pnsi;
    }

    return NULL;
}
