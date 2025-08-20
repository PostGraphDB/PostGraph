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

static char *make_id_alias(char *var_name);
static char *make_property_alias(char *var_name);
static char *make_startid_alias(char *var_name);
static char *make_endid_alias(char *var_name);

static Node *make_vertex_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi);
static Node *make_edge_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi);
static Node *make_vertex_expr_with_edge(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi, ParseNamespaceItem *edge_pnsi);

List *
transform_window_definitions(ParseState *pstate, List *windowdefs, List **targetlist);

static char *make_vertex_adjlist_alias(char *var_name) {
    char *str = palloc0(strlen(var_name) + 8);

    str[0] = '_';
    str[1] = 'a';
    str[2] = 'd';
    str[3] = 'j';
    str[4] = '_';

    int i = 0;
    for (; i < strlen(var_name); i++)
        str[i + 5] = var_name[i];
    str[i + 5] = '\0';

    return str;
}
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




static FuncExpr *make_write_clause_function_placeholder(char *function_name, Node *clause_information) {
    StringInfo str = makeStringInfo();

    outNode(str, clause_information);

    Const *c = makeConst(INTERNALOID, -1, InvalidOid, str->len, PointerGetDatum(str->data), false, false);

    Oid func_oid = get_ag_func_oid(function_name, 1, INTERNALOID);

    return makeFuncExpr(func_oid, GTYPEOID, list_make1(c), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
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


static Node *make_vertex_expr_with_edge(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi, ParseNamespaceItem *edge_pnsi) {
    ParseState *pstate = (ParseState *)cpstate;

    Oid func_oid = get_ag_func_oid("build_vertex", 3, GRAPHIDOID, OIDOID, GTYPEOID);

    Node *id = scanNSItemForColumn(pstate, edge_pnsi, 0, "endid", -1);

    Const *graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false, true);

    Node * props = scanNSItemForColumn(pstate, pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1);

    List *args = list_make3(id, graph_oid_const, props);
    
    FuncExpr *func_expr = makeFuncExpr(func_oid, VERTEXOID, args, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    func_expr->location = -1;


    return (Node *)func_expr;
}



static Node *make_edge_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi) {
    ParseState *pstate = (ParseState *)cpstate;

    Oid func_oid = get_ag_func_oid("build_edge", 5, GRAPHIDOID, GRAPHIDOID, GRAPHIDOID, OIDOID, GTYPEOID);

    Node *id = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_ID, -1);

    Node *start_id = scanNSItemForColumn(pstate, pnsi, 0, "startid", -1);

    Node *end_id = scanNSItemForColumn(pstate, pnsi, 0, "endid", -1);

    Const *graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false, true);

    Node *props = scanNSItemForColumn(pstate, pnsi, 0, AG_EDGE_COLNAME_PROPERTIES, -1);

    List *args = list_make5(id, start_id, end_id, graph_oid_const, props);

    FuncExpr *func_expr = makeFuncExpr(func_oid, EDGEOID, args, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    func_expr->location = -1;

    return (Node *)func_expr;
}


static Node *
make_graphid_placeholder(cypher_parsestate *cpstate) {
     
    // typtypmod, typcollation, typlen, and typbyval of gtype are hard-coded.
    Const *c = makeConst(GRAPHIDOID, -1, InvalidOid, -1, 0, false, false);
    c->location = -1;

    return (Node *)c;
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
#include "rewrite/rewriteHandler.h"
static Expr *add_volatile_wrapper(Expr *node) {
    Oid oid = get_ag_func_oid("gtype_volatile_wrapper", 1, GTYPEOID);

    return (Expr *)makeFuncExpr(oid, GTYPEOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

static void validate_or_create_elabel(cypher_parsestate *cpstate, cypher_relationship *edge) {
    ParseState *pstate = (ParseState *)cpstate;
    label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);

    if (lcd && lcd->kind != LABEL_KIND_EDGE) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("label %s is for vertices, not edges", edge->label),
                        parser_errposition(pstate, edge->location)));
    } else if (!lcd)  {
        List *parent;
        RangeVar *rv;

        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_EDGE);

        parent = list_make1(rv);

        create_label(cpstate->graph_name, edge->label, LABEL_TYPE_EDGE, parent, NULL);
    }
}

static void validate_or_create_vlabel(cypher_parsestate *cpstate, cypher_node *node) {
    ParseState *pstate = (ParseState *)cpstate;
    label_cache_data *lcd = search_label_name_graph_cache(node->label, cpstate->graph_oid);

    if (lcd && lcd->kind != LABEL_KIND_VERTEX) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("label %s is for edges, not vertices", node->label),
                        parser_errposition(pstate, node->location)));
    } else if (!lcd)  {
        List *parent;
        RangeVar *rv;

        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_VERTEX);

        parent = list_make1(rv);

        create_label(cpstate->graph_name, node->label, LABEL_TYPE_VERTEX, parent, NULL);
    }
}


static Query *transform_cypher_create(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_create *self = (cypher_create *)clause->self;
    
    Query *query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    if (clause->prev != NULL)
        ereport(ERROR, (errmsg_internal("CREATE doesn't work with previous clauses")));

    if (clause->next)
        ereport(ERROR, (errmsg_internal("CREATE doesn't work with next clauses")));

    if (list_length(self->pattern) != 1)
        ereport(ERROR, (errmsg_internal("CREATE doesn't work with patterns")));


    cypher_create_target_nodes *target_nodes;
    target_nodes = make_ag_node(cypher_create_target_nodes);
    target_nodes->flags = CYPHER_CLAUSE_FLAG_NONE;
    target_nodes->graph_oid = cpstate->graph_oid;



    ListCell *lc;
    foreach (lc, self->pattern) {
        cypher_path *path = lfirst(lc);
        cypher_create_path *ccp = make_ag_node(cypher_create_path);

        if (path->var_name)
            ereport(ERROR, (errmsg_internal("CREATE doesn't work with traversals")));

        int i = 1;
        ListCell *lc2;
        foreach (lc2, path->path) {
            if (i % 2 == 1) {
                cypher_node *node = (cypher_node *)lfirst(lc2);

                cypher_target_node *target = make_ag_node(cypher_target_node);

                if (node->label) 
                    validate_or_create_vlabel(cpstate, node);
                else
                    node->label = AG_DEFAULT_LABEL_VERTEX;
                    
                if (node->name) {
                           ereport(ERROR, (errmsg_internal("nodes in CREATE cannot be a variable")));
  
                    target->variable_name = node->name;
                    target->id_attr_num = pstate->p_next_resno;
                    query->targetList = lappend(query->targetList,
                        makeTargetEntry(
                            make_graphid_placeholder(cpstate),
                            pstate->p_next_resno++,
                            make_id_alias(get_next_default_alias(cpstate)), 
                            false));
                        
                    
/*
                    query->targetList = lappend(query->targetList,
                        makeTargetEntry(
                            make_int_placeholder(cpstate),
                            pstate->p_next_resno++,
                            make_id_alias(get_next_default_alias(cpstate)), 
                            false));
                        }

                    target->props_attr_num = list_length(query->targetList);
*/
                } else
                    node->name = get_next_default_alias(cpstate);

                if (node->props) {
                    target->prop_attr_num = pstate->p_next_resno;
                    query->targetList = lappend(query->targetList,
                        makeTargetEntry(
                            (Expr *)add_volatile_wrapper(
                                transform_cypher_expr(cpstate, node->props, EXPR_KIND_INSERT_TARGET)),
                            pstate->p_next_resno++,
                            make_property_alias(node->name),
                            false));
    
                } else {
                    target->prop_attr_num = InvalidAttrNumber;
                }
        

                label_cache_data *lcd = search_label_name_graph_cache(node->label, cpstate->graph_oid);

                target->id_expr = (Expr *)build_column_default(RelationIdGetRelation(lcd->relation), 1);
                target->relid = lcd->relation;
                target->adj_relid = lcd->vertex_adjlist;

                ccp->target_nodes = lappend(ccp->target_nodes, target);
                
            } else {
                cypher_relationship *edge = lfirst(lc2);
                cypher_target_node *target = make_ag_node(cypher_target_node);
                if (edge->label) 
                    validate_or_create_elabel(cpstate, edge);
                else
                    edge->label = AG_DEFAULT_LABEL_EDGE;

                if (edge->name)
                    ereport(ERROR, (errmsg_internal("edges in CREATE cannot have variable names")));
                else
                    edge->name = get_next_default_alias(cpstate);
                
                if (edge->props) {
                    target->prop_attr_num = pstate->p_next_resno;
                    query->targetList = lappend(query->targetList,
                        makeTargetEntry(
                            (Expr *)add_volatile_wrapper(
                                transform_cypher_expr(cpstate, edge->props, EXPR_KIND_INSERT_TARGET)),
                            pstate->p_next_resno++,
                            make_property_alias(edge->name),
                            false));
                } else {
                    target->prop_attr_num = InvalidAttrNumber;
                }

                target->dir = edge->dir;
                if (edge->dir == CYPHER_REL_DIR_NONE)
                    ereport(ERROR, (errmsg_internal("edges in CREATE must have a direction")));


                label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);
                target->relid = lcd->relation;

                target->id_expr = (Expr *)build_column_default(RelationIdGetRelation(lcd->relation), 1);
                //target->prop_attr_num = InvalidAttrNumber;
                ccp->target_nodes = lappend(ccp->target_nodes, target);
                
            }

            i++;
        }

        target_nodes->paths = lappend(target_nodes->paths, ccp);
    }

    // Function for the set_rel_pathlist to capture
    FuncExpr *func_expr = make_write_clause_function_placeholder(CREATE_CLAUSE_FUNCTION_NAME, target_nodes);
    TargetEntry *te = makeTargetEntry((Expr *)func_expr, pstate->p_next_resno++, "_create_clause", false);
    query->targetList = lappend(query->targetList, te);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

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


static Node *transform_srf_function(cypher_parsestate *cpstate, Node *n, RangeTblEntry **top_rte, int *top_rti, List **namespace) ;
static void setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok);


static Node *make_null_const(int location) {
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_Null;
    n->location = location;

    return (Node *)n;
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



Alias *make_alias(char *name, List *colnames) {
    Alias *alias = makeNode(Alias);
    
    alias->aliasname = name;
    alias->colnames = colnames;

    return alias;
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

static ParseNamespaceItem *add_srf_to_query(cypher_parsestate *cpstate, Node *n, char *var_name) {
    RangeTblEntry *rte = NULL;
    RangeTblRef *rtr;
    List *namespace = NULL;
    int rtindex;
    ParseState *pstate = (ParseState *)cpstate;

    Alias *alias = make_alias(var_name, list_make4(makeString("id"), makeString("startid"), makeString("endid"), makeString("properties")));
    //Alias *alias = make_alias(var_name, NIL);
    RangeFunction *rf = make_range_function(n, alias, true, false, false);


    rtr = transform_srf_function(cpstate, rf, &rte, &rtindex, &namespace);
    Assert(rtr != NULL);

    checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);

    setNamespaceLateralState(namespace, true, true);
    //((ParseNamespaceItem *)lfirst(list_head(namespace)))->p_names = alias;//list_make4(makeString("id"), makeString("startid"), makeString("endid"), makeString("properties"));
    pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
    pstate->p_namespace = list_concat(pstate->p_namespace, namespace);

    setNamespaceLateralState(pstate->p_namespace, true, true);

    return lfirst(list_head(namespace));
}



static ParseNamespaceItem *add_srf_to_query2(cypher_parsestate *cpstate, Node *n, char *var_name) {
    RangeTblEntry *rte = NULL;
    RangeTblRef *rtr;
    List *namespace = NULL;
    int rtindex;
    ParseState *pstate = (ParseState *)cpstate;

    Alias *alias = make_alias(var_name, list_make2(makeString("edges"), makeString("endid")));
    //Alias *alias = make_alias(var_name, NIL);
    RangeFunction *rf = make_range_function(n, alias, true, false, false);


    rtr = transform_srf_function(cpstate, rf, &rte, &rtindex, &namespace);
    Assert(rtr != NULL);

    checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);

    setNamespaceLateralState(namespace, true, true);
    //((ParseNamespaceItem *)lfirst(list_head(namespace)))->p_names = alias;//list_make4(makeString("id"), makeString("startid"), makeString("endid"), makeString("properties"));
    pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
    pstate->p_namespace = list_concat(pstate->p_namespace, namespace);

    setNamespaceLateralState(pstate->p_namespace, true, true);

    return lfirst(list_head(namespace));
}



// transform a function call appearing in FROM
static ParseNamespaceItem *transformRangeFunction(cypher_parsestate *cpstate, RangeFunction *r) {
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

static Node *transform_srf_function(cypher_parsestate *cpstate, Node *n, RangeTblEntry **top_rte, int *top_rti, List **namespace) {
    ParseState *pstate = (ParseState *)cpstate;
    
    Assert(IsA(n, RangeFunction));
    
    if (IsA(n, RangeFunction)) {
        RangeTblRef *rtr;
        RangeTblEntry *rte;
        ParseNamespaceItem *nsitem;
        int rtindex;
    
        nsitem = transformRangeFunction(cpstate, (RangeFunction *) n);
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

static Node *make_int_const(int i, int location) {
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_Integer;
    n->val.val.ival = i;
    n->location = location;

    return (Node *)n;
}

char *get_vertex_relation_name(cypher_parsestate *cpstate, char *label, bool is_default_label) {
                    // XXX: LTree Labeling Project, first code is here
    Datum label_lquery;
    if (is_default_label)
        label_lquery = DirectFunctionCall1(ltree_in, CStringGetDatum(label));
    else
        label_lquery = DirectFunctionCall2(ltree_addltree, 
                            DirectFunctionCall1(ltree_in, CStringGetDatum(AG_DEFAULT_LABEL_VERTEX)),
                            DirectFunctionCall1(ltree_in, CStringGetDatum(label)));



    int ltq_query_args[2];
    ltq_query_args[0] = LookupTypeNameOid(cpstate, makeTypeNameFromNameList(list_make2(makeString("public"), makeString("ltree"))), false);
    ltq_query_args[1] = LookupTypeNameOid(cpstate, makeTypeNameFromNameList(list_make2(makeString("public"), makeString("ltree"))), false);
                    
    Oid ltree_contains_oid = LookupFuncName(list_make2(makeString("public"), makeString("ltree_risparent")), 2, &ltq_query_args, false);
                    
    ScanKeyData scan_keys[1];
    ScanKeyInit(&scan_keys[0], Anum_ag_label_label_path, BTEqualStrategyNumber, ltree_contains_oid, label_lquery);
                    
    Relation label_catalog = table_open(ag_label_relation_id(), ShareLock);
    SysScanDesc scan_desc = systable_beginscan(label_catalog, ag_label_label_index_id(), true, NULL, 1, scan_keys);

    HeapTuple tuple = systable_getnext(scan_desc);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,(errcode(ERRCODE_UNDEFINED_SCHEMA),
                errmsg("not found %s", label)));
                    
    bool is_null;
    char *rel_name = heap_getattr(tuple, Anum_ag_label_name, RelationGetDescr(label_catalog), &is_null);

    systable_endscan(scan_desc);
    table_close(label_catalog, ShareLock);

    return rel_name;
}

static ParseNamespaceItem *
add_vertex_to_query(cypher_parsestate *cpstate, Query *query, cypher_node *node)
{
    ParseState *pstate = (ParseState *)cpstate;

    node->has_variable = false;
    if (node->name)
        node->has_variable = true;
    else
        node->name = get_next_default_alias(cpstate);

    node->is_default_label = true;
    if (node->label)
        node->is_default_label = false;
    else
        node->label = AG_DEFAULT_LABEL_VERTEX;

    node->in_join_tree = true;

    ParseNamespaceItem *pnsi = addRangeTableEntry(pstate, 
                                    makeRangeVar(get_graph_namespace_name(cpstate->graph_name),
                                                 get_vertex_relation_name(cpstate, node->label, node->is_default_label),
                                                 -1),
                                    makeAlias(node->name, list_make2(makeString("id"), makeString("properties"))), 
                                    true, 
                                    true);


    addNSItemToQuery(pstate, pnsi, true, true, true);

    return pnsi;
}

static ParseNamespaceItem *
add_vertex_retrieval_to_query(cypher_parsestate *cpstate, Query *query, cypher_node *node, ParseNamespaceItem *edge_pnsi)
{
    ParseState *pstate = (ParseState *)cpstate;

    node->has_variable = false;
    if (node->name)
        node->has_variable = true;
    else {
        node->in_join_tree = false;
        return NULL;
    }
     node->in_join_tree = true;   

    bool is_default_label = true;
    if (node->label)
        is_default_label = false;
    else
        node->label = AG_DEFAULT_LABEL_VERTEX;


    ParseNamespaceItem *pnsi = transformRangeFunction(cpstate, 
        make_range_function(
            makeFuncCall(
                list_make2(makeString("postgraph"), makeString("retrieve_vertex")),
                list_make2(
                    make_int_const(cpstate->graph_oid, -1), 
                    scanNSItemForColumn(cpstate, edge_pnsi, 0, "endid", -1)),
                COERCE_EXPLICIT_CALL, -1), 
            make_alias(node->name, 
                list_make1(makeString("properties"))), 
            true, 
            false, 
            false));

    addNSItemToQuery(pstate, pnsi, true, true, true);

    return pnsi;
}

static ParseNamespaceItem *
add_edge_to_query(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, ParseNamespaceItem *vertex_pnsi)
{
    ParseState *pstate = (ParseState *)cpstate;

    edge->has_variable = false;
    if (edge->name)
        edge->has_variable = true;
    else
        edge->name = get_next_default_alias(cpstate);

    bool is_default_label = true;
    if (edge->label)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("MATCH labels are not supported")));
    else
        edge->label = AG_DEFAULT_LABEL_EDGE;

    Node *id_field = scanNSItemForColumn(cpstate, vertex_pnsi, 0, AG_VERTEX_COLNAME_ID, -1);

    FuncCall *fc = makeFuncCall(
        list_make2(makeString("postgraph"), makeString("edge_search")),
        list_make4(make_int_const(cpstate->graph_oid, -1), id_field, make_null_const(-1), make_null_const(-1)),
        COERCE_EXPLICIT_CALL, -1);

    return add_srf_to_query(cpstate, fc, edge->name);
}


static ParseNamespaceItem *
add_variable_edge_to_query(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, ParseNamespaceItem *vertex_pnsi)
{
    ParseState *pstate = (ParseState *)cpstate;

    edge->has_variable = false;
    if (edge->name)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("MATCH variable edge variables are not supported")));        
    else
        edge->name = get_next_default_alias(cpstate);

    bool is_default_label = true;
    if (edge->label)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("MATCH labels are not supported")));
    else
        edge->label = AG_DEFAULT_LABEL_EDGE;

    Node *id_field = scanNSItemForColumn(cpstate, vertex_pnsi, 0, AG_VERTEX_COLNAME_ID, -1);
    
    A_Indices *idx= edge->varlen;

    FuncCall *fc = makeFuncCall(
        list_make2(makeString("postgraph"), makeString("variable_edge_search")),
        list_make4(make_int_const(cpstate->graph_oid, -1), id_field, idx->lidx, make_null_const(-1)),
        COERCE_EXPLICIT_CALL, -1);

    return add_srf_to_query2(cpstate, fc, edge->name);
}


static void add_all_fields_to_target_list(cypher_parsestate *cpstate, Query *query,
    cypher_node *left_vertex, cypher_relationship *edge, cypher_node *right_vertex) {
    ParseState *pstate = (ParseState *)cpstate;

   // left vertex id field
    if (left_vertex->in_join_tree) {
        query->targetList = lappend(query->targetList, 
                            makeTargetEntry(
                                edge->dir == CYPHER_REL_DIR_RIGHT ?
                                    scanNSItemForColumn(pstate, left_vertex->pnsi, 0, AG_VERTEX_COLNAME_ID, -1) :
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_END_ID, -1), 
                                pstate->p_next_resno++, 
                                make_id_alias(left_vertex->name), 
                                false));

        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, left_vertex->pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1), 
                                    pstate->p_next_resno++, 
                                    make_property_alias(left_vertex->name), 
                                    false));
        
        // Vertex expression
        if (left_vertex->has_variable)
            query->targetList = lappend(query->targetList, 
                                    makeTargetEntry(
                                        edge->dir == CYPHER_REL_DIR_LEFT ?
                                            (Expr *)make_vertex_expr_with_edge(cpstate, left_vertex->pnsi, edge->pnsi) :
                                            (Expr *)make_vertex_expr(cpstate, left_vertex->pnsi),
                                        pstate->p_next_resno++,
                                        left_vertex->name,
                                        false));

    }

    if (edge->varlen) {
        // end id field
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_END_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_endid_alias(edge->name), 
                                    false));
    } else {
        // id field
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_id_alias(edge->name), 
                                    false));

        // start id field
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_START_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_startid_alias(edge->name), 
                                    false));

        // end id field
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_END_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_endid_alias(edge->name), 
                                    false));

        // properties field
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_PROPERTIES, -1), 
                                    pstate->p_next_resno++, 
                                    make_property_alias(edge->name), 
                                    false));
    }

    // id field
    if (edge->has_variable)
        query->targetList = lappend(query->targetList, 
                                    makeTargetEntry(
                                        (Expr *)make_edge_expr(cpstate, edge->pnsi),
                                        pstate->p_next_resno++,
                                        edge->name,
                                        false));

    // properties field
    if (right_vertex->in_join_tree){
        query->targetList = lappend(query->targetList, 
                            makeTargetEntry(
                                edge->dir == CYPHER_REL_DIR_RIGHT ?
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_END_ID, -1) :
                                    scanNSItemForColumn(pstate, right_vertex->pnsi, 0, AG_VERTEX_COLNAME_ID, -1),
                                pstate->p_next_resno++, 
                                make_id_alias(right_vertex->name), 
                                false));

        query->targetList = lappend(query->targetList, 
                            makeTargetEntry(
                                scanNSItemForColumn(pstate, right_vertex->pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1), 
                                pstate->p_next_resno++, 
                                make_property_alias(right_vertex->name), 
                                false));

        // Vertex expression
        if (right_vertex->has_variable) 
            query->targetList = lappend(query->targetList, 
                                    makeTargetEntry(
                                        edge->dir == CYPHER_REL_DIR_RIGHT ?
                                            (Expr *)make_vertex_expr_with_edge(cpstate, right_vertex->pnsi, edge->pnsi) :
                                            (Expr *)make_vertex_expr(cpstate, right_vertex->pnsi),
                                        pstate->p_next_resno++, 
                                        right_vertex->name, 
                                        false));     
    }

}

static void transform_match_pattern(cypher_parsestate *cpstate, Query *query, List *pattern, Node *where) {
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *lc;
    List *quals = NIL;

    Expr *expr = NULL;


    if (list_length(pattern) !=1 )
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("MATCH found more than one pattern")));

    foreach (lc, pattern) {
        cypher_path *path = (cypher_path *) lfirst(lc);

        if (list_length(path->path) == 1) {
            cypher_node *node = linitial(path->path);

            node->pnsi = add_vertex_to_query(cpstate, query, node);
            // left vertex id field
            query->targetList = lappend(query->targetList, 
                                    makeTargetEntry(
                                        scanNSItemForColumn(pstate, node->pnsi, 0, AG_VERTEX_COLNAME_ID, -1), 
                                        pstate->p_next_resno++, 
                                        make_id_alias(node->name), 
                                        false));

            // Vertex expression
            if (node->has_variable) {
                query->targetList = lappend(query->targetList, 
                                        makeTargetEntry(
                                            scanNSItemForColumn(pstate, node->pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1), 
                                            pstate->p_next_resno++, 
                                            make_property_alias(node->name), 
                                            false));

                query->targetList = lappend(query->targetList, 
                                        makeTargetEntry(
                                            (Expr *)make_vertex_expr(cpstate, node->pnsi), 
                                            pstate->p_next_resno++, 
                                            node->name, 
                                            false));

            }
            continue;
        }

        if (list_length(path->path) != 3)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("MATCH paths only support 3 vertex")));

        if( ((cypher_relationship *)lsecond(path->path))->dir == CYPHER_REL_DIR_RIGHT) {
            ListCell *path_cell;

            // left vertex FROM item
            cypher_node *node = linitial(path->path);
            node->pnsi = add_vertex_to_query(cpstate, query, node);

            
            // edge FROM item 
            cypher_relationship *edge = lsecond(path->path);
            if (edge->varlen) {
                edge->pnsi = add_variable_edge_to_query(cpstate, query, edge, node->pnsi);
            } else {
                edge->pnsi = add_edge_to_query(cpstate, query, edge, node->pnsi);
            }
            // right vertex FROM item
            node = lthird(path->path);

            node->pnsi = add_vertex_retrieval_to_query(cpstate, query, node, edge->pnsi);

            // SELECT fields
            add_all_fields_to_target_list(cpstate, 
                                            query,
                                            (cypher_node *)linitial(path->path),
                                            (cypher_relationship *)lsecond(path->path), 
                                            (cypher_node *)lthird(path->path));
        
        } else if( ((cypher_relationship *)lsecond(path->path))->dir == CYPHER_REL_DIR_LEFT) {
            ListCell *path_cell;

            // left vertex FROM item
            cypher_node *node = lthird(path->path);
            node->pnsi = add_vertex_to_query(cpstate, query, node);

            
            // edge FROM item 
            cypher_relationship *edge = lsecond(path->path);
            edge->pnsi = add_edge_to_query(cpstate, query, edge, node->pnsi);

            // right vertex FROM item
            node = linitial(path->path);

            node->pnsi = add_vertex_retrieval_to_query(cpstate, query, node, edge->pnsi);

            // SELECT fields
            add_all_fields_to_target_list(cpstate, 
                                            query,
                                            (cypher_node *)linitial(path->path),
                                            (cypher_relationship *)lsecond(path->path), 
                                            (cypher_node *)lthird(path->path));
        } else {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("MATCH edge can't be bi directional")));
        }

        
    }

    // AND the quals for each path together
    Expr *q = NULL;
    
    if (quals != NIL) {
        q = makeBoolExpr(AND_EXPR, quals, -1);
        expr = (Expr *)sql_transform_expr(&cpstate->pstate, (Node *)q, EXPR_KIND_WHERE);
    }

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
    query->jointree = makeFromExpr(cpstate->pstate.p_joinlist, (Node *)expr);
    //query->jointree = makeFromExpr(cpstate->pstate.p_joinlist, (Node *)NULL);
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
