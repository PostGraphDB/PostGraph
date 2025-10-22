#include "postgraph.h"

#include "access/heapam.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/parsetree.h"

#include "parser/cypher_analyze.h"
#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "commands/label_commands.h"
#include "nodes/ag_nodes.h"
#include "nodes/cypher_nodes.h"
#include "parser/cypher_clause.h"
#include "parser/cypher_expr.h"
#include "parser/cypher_item.h"
#include "parser/cypher_parse_node.h"
#include "utils/ag_cache.h"
#include "utils/ag_func.h"
#include "utils/gtype.h"
#include "utils/graphid.h"
#include "utils/variable_edge.h"
#include "utils/vertex.h"
#include "utils/edge.h"

typedef Query *(*transform_method)(cypher_parsestate *cpstate, cypher_clause *clause);

// projection
static TargetEntry *find_target_list_entry(cypher_parsestate *cpstate, Node *node, List **target_list, ParseExprKind expr_kind);
static Node *transform_cypher_limit(cypher_parsestate *cpstate, Node *node, ParseExprKind expr_kind, const char *construct_name);
static Query *transform_cypher_clause_with_where(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause);
Query *transform_cypher_return(cypher_parsestate *cpstate, cypher_clause *clause);

// match clause
static Query *transform_cypher_match(cypher_parsestate *cpstate, cypher_clause *clause);
static void transform_match_pattern(cypher_parsestate *cpstate, Query *query, List *pattern, Node *where);
static Query *transform_cypher_match_pattern(cypher_parsestate *cpstate, cypher_clause *clause);
// match from clause functions
static void process_three_element_path(cypher_parsestate *cpstate, Query *query, cypher_path *path, List **quals);
static List *find_path_parts(cypher_path *path);
static void process_path_part(cypher_parsestate *cpstate, Query *query, cypher_path *path, path_parts *pp, List **quals, cypher_relationship **prev_edge);
static ParseNamespaceItem *add_vertex_to_query(cypher_parsestate *cpstate, Query *query, cypher_node *node, List **quals);
static ParseNamespaceItem *add_vertex_retrieval_to_query(cypher_parsestate *cpstate, Query *query, cypher_node *node, ParseNamespaceItem *edge_pnsi, List **quals);
static ParseNamespaceItem *add_edge_to_query(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, cypher_node *vertex);
static ParseNamespaceItem *add_edge_to_query_with_prev_edge(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, ParseNamespaceItem *prev_pnsi, char *prev_name);
static ParseNamespaceItem *add_variable_edge_to_query(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, ParseNamespaceItem *vertex_pnsi);
// match select clause functions
static void add_all_fields_to_target_list(cypher_parsestate *cpstate, Query *query, cypher_node *left_vertex, cypher_relationship *edge, cypher_node *right_vertex);
static void add_new_fields_to_target_list(cypher_parsestate *cpstate, Query *query, cypher_node *left_vertex, cypher_relationship *edge, cypher_node *right_vertex);
// match expression logic
static Node *transform_srf_function(cypher_parsestate *cpstate, Node *n, List **namespace);
static A_Expr *makeSimpleCypherA_Expr(A_Expr_Kind kind, char *name, Node *lexpr, Node *rexpr, int location);
static ParseNamespaceItem *add_srf_to_query(cypher_parsestate *cpstate, Node *n, char *var_name, List *colnames);
static void setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok);
static Node *transform_srf_function(cypher_parsestate *cpstate, Node *n, List **namespace);
static ParseNamespaceItem *transformRangeFunction(cypher_parsestate *cpstate, RangeFunction *r);
RangeFunction *make_range_function(FuncCall *func, Alias *alias, bool is_lateral, bool ordinality, bool is_rows_from);
static void get_res_cols(ParseState *pstate, ParseNamespaceItem *l_pnsi, ParseNamespaceItem *r_pnsi, List **res_colnames, List **res_colvars);
static Node *make_vertex_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi);
static Node *make_edge_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi);
static Node *make_vertex_expr_with_edge(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi, ParseNamespaceItem *edge_pnsi);

// create clause
static Query *transform_cypher_create(cypher_parsestate *cpstate, cypher_clause *clause);
static void process_create_vertex(cypher_parsestate *cpstate, Query *query, cypher_node *node, cypher_create_path *ccp);
static void process_create_edge(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, cypher_create_path *ccp);

// merge
static Query *transform_cypher_merge(cypher_parsestate *cpstate, cypher_clause *clause);
static cypher_create_path *transform_merge_make_lateral_join(cypher_parsestate *cpstate, Query *query, cypher_clause *clause, cypher_clause *isolated_merge_clause);
static cypher_create_path *transform_cypher_merge_path(cypher_parsestate *cpstate, Query *query, cypher_path *path);
static cypher_target_node *transform_merge_cypher_node(cypher_parsestate *cpstate, Query *query, cypher_node *node);
static cypher_target_node *transform_merge_cypher_edge(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge);
static Node *transform_clause_for_join(cypher_parsestate *cpstate, cypher_clause *clause, RangeTblEntry **rte, ParseNamespaceItem **nsitem, Alias* alias);
static cypher_clause *convert_merge_to_match(cypher_merge *merge);
static void transform_cypher_merge_mark_tuple_position(cypher_parsestate *cpstate, List *target_list, cypher_create_path *path);

// transform
#define PREV_CYPHER_CLAUSE_ALIAS    "_"
#define CYPHER_OPT_RIGHT_ALIAS      "_R"
#define transform_prev_cypher_clause(cpstate, prev_clause, add_rte_to_query) \
    transform_cypher_clause_as_subquery(cpstate, transform_cypher_clause, prev_clause, NULL, add_rte_to_query)
static ParseNamespaceItem *transform_cypher_clause_as_subquery(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause, Alias *alias, bool add_rte_to_query);
static Query *analyze_cypher_clause(transform_method transform, cypher_clause *clause, cypher_parsestate *parent_cpstate);
static List *make_target_list_from_join(ParseState *pstate, RangeTblEntry *rte);
static void setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok);
static void transform_cypher_clause_as_subquery_2(cypher_parsestate *cpstate, Query *query);

// utility functions
// label validation
static void validate_or_create_elabel(cypher_parsestate *cpstate, cypher_relationship *edge);
static void validate_or_create_vlabel(cypher_parsestate *cpstate, cypher_node *node);
// write clause function placeholder
static FuncExpr *make_write_clause_function_placeholder(char *function_name, Node *clause_information);
//empty placeholders
static Node *make_graphid_placeholder(cypher_parsestate *cpstate);
static Node *make_gtype_placeholder(cypher_parsestate *cpstate);
static Node *make_edge_placeholder(cypher_parsestate *cpstate);
static Node *make_vertex_placeholder(cypher_parsestate *cpstate);
// volatile wrappers
static Expr *add_volatile_wrapper(Expr *node);
static Expr *add_volatile_edge_wrapper(Expr *node);
static Expr *add_volatile_vle_edge_wrapper(Expr *node);
static Expr *add_volatile_vertex_wrapper(Expr *node);
// aliasing
static char *make_id_alias(char *var_name);
static char *make_property_alias(char *var_name);
static char *make_startid_alias(char *var_name);
static char *make_endid_alias(char *var_name);
static char *make_vertex_adjlist_alias(char *var_name);
Alias *make_alias(char *name, List *colnames);

static Node *make_int_const(int i, int location);
static Node *make_null_const(int location);



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
    } else if (is_ag_node(self, cypher_merge)) {
        result = transform_cypher_merge(cpstate, clause);
    } else {
        ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));
    }
    
    result->querySource = QSRC_ORIGINAL;
    result->canSetTag = true;

    return result;
}


/*
 * This function is similar to transformFromClause() that is called with a
 * single RangeSubselect.
 */
static void
transform_cypher_clause_as_subquery_2(cypher_parsestate *cpstate, Query *query) {
    ParseState *pstate = (ParseState *)cpstate;
    ParseExprKind old_expr_kind = pstate->p_expr_kind;
    bool lateral = pstate->p_lateral_active;

    ParseNamespaceItem *pnsi = addRangeTableEntryForSubquery(pstate, query, makeAlias(PREV_CYPHER_CLAUSE_ALIAS, NIL), lateral, true);

    addNSItemToQuery(pstate, pnsi, true, false, true);

    //query->targetList = list_concat(query->targetList, expandNSItemAttrs(pstate, pnsi, 0, -1));
}


/*
 * validate_or_create_elabel
 *
 * This function ensures that an edge label exists and is valid. If the label
 * does not exist, it is created automatically.
 *
 * Parameters:
 *   cpstate - The current Cypher parser state.
 *   edge    - The `cypher_relationship` whose label is being validated.
 *
 * Behavior:
 *   1. It searches the graph's label cache for the label name specified in the edge.
 *   2. If the label is found, it verifies that it is an edge label. If it is
 *      a vertex label, it throws an error.
 *   3. If the label is not found, it calls `create_label` to create a new
 *      edge label with the specified name. The new label inherits from the
 *      default edge label.
 */
static void validate_or_create_elabel(cypher_parsestate *cpstate, cypher_relationship *edge) {
    label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);

    if (lcd && lcd->kind != LABEL_KIND_EDGE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("label %s is for vertices, not edges", edge->label),
                        parser_errposition(cpstate, edge->location)));
    else if (!lcd)  
        create_label(cpstate->graph_name,
            edge->label,
            LABEL_TYPE_EDGE,
            list_make1(get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_EDGE)),
            NULL);
}

/*
 * validate_or_create_vlabel
 *
 * This function ensures that a vertex label exists and is valid. If the label
 * does not exist, it is created automatically.
 *
 * Parameters:
 *   cpstate - The current Cypher parser state.
 *   node    - The `cypher_node` whose label is being validated.
 *
 * Behavior:
 *   1. It searches the graph's label cache for the label name specified in the node.
 *   2. If the label is found, it verifies that it is a vertex label. If it is
 *      an edge label, it throws an error.
 *   3. If the label is not found, it calls `create_label` to create a new
 *      vertex label with the specified name. The new label inherits from the
 *      default vertex label.
 */
static void validate_or_create_vlabel(cypher_parsestate *cpstate, cypher_node *node) {
    label_cache_data *lcd = search_label_name_graph_cache(node->label, cpstate->graph_oid);

    if (lcd && lcd->kind != LABEL_KIND_VERTEX) 
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("label %s is for edges, not vertices", node->label),
                        parser_errposition(cpstate, node->location)));
    else if (!lcd)  
        create_label(cpstate->graph_name,
            node->label,
            LABEL_TYPE_VERTEX,
            list_make1(get_label_range_var(cpstate->graph_name, cpstate->graph_oid, AG_DEFAULT_LABEL_VERTEX)),
            NULL);
}

static TargetEntry * get_target_entry(cypher_parsestate *cpstate, List *target_list, char *name) {
    ListCell *lc;

    foreach (lc, target_list) {
        TargetEntry *te = (TargetEntry *)lfirst(lc);
 
        if (!strcmp(te->resname, name)) {
            //te->expr = add_volatile_wrapper(te->expr);
            return te;
        }
    }

    return NULL;
}
static int get_target_entry_resno(cypher_parsestate *cpstate, List *target_list, char *name) {
    ListCell *lc;

    foreach (lc, target_list) {
        TargetEntry *te = (TargetEntry *)lfirst(lc);
 
        if (!strcmp(te->resname, name)) {
            //te->expr = add_volatile_wrapper(te->expr);
            return te->resno;
        }
    }

    return -1;
}

/*
 * process_create_vertex
 *
 * This function transforms a `cypher_node` from a CREATE clause into the
 * necessary structures for execution. It handles both creating new vertices and
 * referencing existing ones within the same query.
 *
 * Parameters:
 *   cpstate - The current Cypher parser state.
 *   query   - The Query node being constructed.
 *   node    - The `cypher_node` from the CREATE pattern to be processed.
 *   ccp     - The `cypher_create_path` struct that will hold the transformation's output.
 *
 * Behavior:
 *   1. Handles Named vs. Anonymous Nodes:
 *      - If the node has a variable name, it checks if the variable has been
 *        previously defined in the query (e.g., in a MATCH clause).
 *      - If it's an anonymous node, a default internal name is generated.
 *
 *   2. Handles Existing Variables:
 *      - If a named variable already exists, it flags the node as a reference
 *        to an existing entity.
 *      - It validates that one cannot add new properties or labels to an
 *        already-existing vertex within a CREATE clause.
 *
 *   3. Handles New Vertices:
 *      - For new vertices (named or anonymous), it adds placeholder target
 *        entries to the query's target list for the new vertex's ID, properties,
 *        and the full vertex object itself. This ensures the created vertex's
 *        data is available to subsequent clauses.
 *      - It transforms any provided property maps.
 *
 *   4. Handles Labels and Storage:
 *      - It validates the vertex's label, creating it if it doesn't exist.
 *      - It looks up the label's metadata to determine the underlying storage
 *        relation (`relid`) and its corresponding adjacency list relation
 *        (`adj_relid`).
 *
 *   5. Output:
 *      - It populates a `cypher_target_node` with all the collected information
 *        (relation OIDs, attribute numbers in the target list, flags, etc.) and
 *        appends it to the `cypher_create_path`.
 */
static void
process_create_vertex(
    cypher_parsestate *cpstate,
    Query *query,
    cypher_node *node,
    cypher_create_path *ccp)
{
    cypher_target_node *target = make_ag_node(cypher_target_node);


    if (node->name) {
        Var *var = NULL;
        if ((var = colNameToVar(cpstate, node->name, false, -1)) ||
        (target->id_attr_num = get_target_entry_resno(cpstate, query->targetList, make_id_alias(node->name))) != -1) {
        
            if (var && var->vartype != VERTEXOID)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("CREATE vertex variable %s already exists", node->name)));
        
            if (node->props)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("previously declared nodes in a create clause cannot have properties")));
            if (node->label)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("previously declared variables cannot have a label")));
      
            target->flags |= EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE;
    
            target->id_attr_num = get_target_entry_resno(cpstate, query->targetList, make_id_alias(node->name));
    
            ccp->target_nodes = lappend(ccp->target_nodes, target);
            return;
        }

        query->targetList = lappend(query->targetList,
            makeTargetEntry(
                make_graphid_placeholder(cpstate),
                target->id_attr_num = pstate->p_next_resno++,
                make_id_alias(target->variable_name = node->name),
                false));

        if (node->props) {
            query->targetList = lappend(query->targetList,
                makeTargetEntry(
                    add_volatile_wrapper(transform_cypher_expr(cpstate, node->props, EXPR_KIND_INSERT_TARGET)),
                    target->prop_attr_num = pstate->p_next_resno++,
                    make_property_alias(node->name),
                    false));
        } else {
            target->prop_attr_num = InvalidAttrNumber;
        }

        query->targetList = lappend(query->targetList,
                makeTargetEntry(
                    make_vertex_placeholder(cpstate),
                    target->tuple_position = pstate->p_next_resno++,
                    node->name,
                    false));

    } else {
        target->id_attr_num = InvalidAttrNumber;
        target->tuple_position = InvalidAttrNumber;

        if (node->props) {
            query->targetList = lappend(query->targetList,
                makeTargetEntry(
                    (Expr *)add_volatile_wrapper(
                        transform_cypher_expr(cpstate, node->props, EXPR_KIND_INSERT_TARGET)),
                    target->prop_attr_num = pstate->p_next_resno++,
                    make_property_alias(node->name = get_next_default_alias(cpstate)),
                    false));
        } else {
            target->prop_attr_num = InvalidAttrNumber;
        }

    }
    
    if (node->label)
        validate_or_create_vlabel(cpstate, node);
    else
        node->label = AG_DEFAULT_LABEL_VERTEX;

    label_cache_data *lcd = search_label_name_graph_cache(node->label, cpstate->graph_oid);
    Relation *rel = RelationIdGetRelation(lcd->relation);
    target->id_expr = (Expr *)build_column_default(rel, 1);
    target->relid = lcd->relation;
    table_close(rel, NoLock);

    target->adj_relid = lcd->vertex_adjlist;

    ccp->target_nodes = lappend(ccp->target_nodes, target);
}

/*
 * process_create_edge
 *
 * This function transforms a `cypher_relationship` from a CREATE clause into
 * the necessary structures for execution. It is a helper function for
 * `transform_cypher_create`.
 *
 * Parameters:
 *   cpstate - The current Cypher parser state.
 *   query   - The Query node being constructed, which this function modifies.
 *   edge    - The `cypher_relationship` from the CREATE pattern to be processed.
 *   ccp     - The `cypher_create_path` struct that will hold the transformation's output.
 *
 * Behavior:
 *   1. Handles Named vs. Anonymous Edges:
 *      - If the edge has a variable name, it validates that the name is not
 *        already in use.
 *      - If it's an anonymous edge, a default internal name is generated.
 *
 *   2. Adds Placeholders to Target List:
 *      - For new edges, it adds placeholder target entries to the query's
 *        target list for the new edge's ID, properties, and the full edge
 *        object itself. This ensures the created edge's data is available to
 *        subsequent clauses.
 *      - It transforms any provided property maps.
 *
 *   3. Handles Labels and Storage:
 *      - It validates the edge's label, creating it if it doesn't exist.
 *      - It looks up the label's metadata to determine the underlying storage
 *        relation (`relid`).
 *
 *   4. Output:
 *      - It populates a `cypher_target_node` with all the collected information
 *        (relation OID, attribute numbers in the target list, direction, etc.)
 *        and appends it to the `cypher_create_path`.
 */
static void
process_create_edge(
    cypher_parsestate *cpstate,
    Query *query,
    cypher_relationship *edge,
    cypher_create_path *ccp)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_target_node *target = make_ag_node(cypher_target_node);

    if (edge->label)
        validate_or_create_elabel(cpstate, edge);
    else
        edge->label = AG_DEFAULT_LABEL_EDGE;


    if (edge->name) {
        if (colNameToVar(cpstate, edge->name, false, -1))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("CREATE edge variable %s already exists", edge->name)));

        target->variable_name = edge->name;
        query->targetList = lappend(query->targetList,
            makeTargetEntry(
                make_graphid_placeholder(cpstate),
                target->id_attr_num = pstate->p_next_resno++,
                make_id_alias(edge->name),
                false));

        if (edge->props) {
            query->targetList = lappend(query->targetList,
                makeTargetEntry(
                    transform_cypher_expr(cpstate, edge->props, EXPR_KIND_INSERT_TARGET),
                    target->prop_attr_num = pstate->p_next_resno++,
                    make_property_alias(edge->name),
                    false));
        } else {
            target->prop_attr_num = InvalidAttrNumber;
        }

        query->targetList = lappend(query->targetList,
                makeTargetEntry(
                    make_edge_placeholder(cpstate),
                    target->tuple_position = pstate->p_next_resno++,
                    edge->name,
                    false));

    } else {
        edge->name = get_next_default_alias(cpstate);
        target->id_attr_num = InvalidAttrNumber;
        target->tuple_position = InvalidAttrNumber;

        if (edge->props) {
            query->targetList = lappend(query->targetList,
                makeTargetEntry(
                    (Expr *)add_volatile_wrapper(
                        transform_cypher_expr(cpstate, edge->props, EXPR_KIND_INSERT_TARGET)),
                    target->prop_attr_num = pstate->p_next_resno++,
                    make_property_alias(edge->name),
                    false));
        } else {
            target->prop_attr_num = InvalidAttrNumber;
        }
    }

    target->dir = edge->dir;
    if (edge->dir == CYPHER_REL_DIR_NONE)
        ereport(ERROR, (errmsg_internal("edges in CREATE must have a direction")));

    label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);
    target->relid = lcd->relation;

    Relation *rel = RelationIdGetRelation(lcd->relation);
    target->id_expr = (Expr *)build_column_default(rel, 1);

    table_close(rel, NoLock);

    ccp->target_nodes = lappend(ccp->target_nodes, target);
}

/*
 * transform_cypher_create
 *
 * This function transforms a Cypher `CREATE` clause into a `Query` node. It
 * orchestrates the processing of vertices and edges to be created and packages
 * the creation logic into a placeholder function call for later execution.
 *
 * Parameters:
 *   cpstate - The current Cypher parser state.
 *   clause  - The `cypher_clause` containing the `cypher_create` node.
 *
 * Behavior:
 *   1. Initializes a new `SELECT` query.
 *   2. If the `CREATE` follows another clause (e.g., `MATCH`), it transforms
 *      the preceding clause into a subquery and adds its output columns to the
 *      current query's target list. This makes variables from the `MATCH`
 *      available to the `CREATE`.
 *   3. It iterates through each path pattern in the `CREATE` clause. For each
 *      path, it calls `process_create_vertex` for each node and
 *      `process_create_edge` for each relationship. These helpers add
 *      placeholder target entries for the new entities and gather metadata.
 *   4. It aggregates all the metadata from the helpers into a
 *      `cypher_create_target_nodes` structure.
 *   5. It serializes this structure and embeds it in a placeholder function
 *      call (e.g., `_cypher_create_clause`), which is added as the final
 *      entry in the query's target list. The actual creation logic is deferred
 *      to the execution stage.
 *   6. If `CREATE` is the final clause in the statement, it wraps the generated
 *      query in an outer `SELECT` query to produce a clean final result set.
 *
 * Returns:
 *   A `Query` node representing the transformed `CREATE` clause.
 */
static Query *transform_cypher_create(cypher_parsestate *cpstate, cypher_clause *clause) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_create *self = (cypher_create *)clause->self;
    
    Query *query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    cypher_create_target_nodes *target_nodes;
    target_nodes = make_ag_node(cypher_create_target_nodes);
    target_nodes->flags = CYPHER_CLAUSE_FLAG_NONE;
    target_nodes->graph_oid = cpstate->graph_oid;


    if (clause->prev) {
        query->targetList = list_concat(query->targetList,
            expandNSItemAttrs(get_parse_state(cpstate),
                transform_prev_cypher_clause(cpstate, clause->prev, true),
                0,
                -1));

        target_nodes->flags |= CYPHER_CLAUSE_FLAG_PREVIOUS_CLAUSE;
    }

    if (list_length(self->pattern) != 1)
        ereport(ERROR, (errmsg_internal("CREATE doesn't work with patterns")));



    if (!clause->next)
        target_nodes->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;

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
                process_create_vertex(cpstate, query, node, ccp);
            } else {
                cypher_relationship *edge = lfirst(lc2);
                process_create_edge(cpstate, query, edge, ccp);
            }

            i++;
        }

        target_nodes->paths = lappend(target_nodes->paths, ccp);
    }

    query->targetList = lappend(query->targetList, 
        makeTargetEntry(
            (Expr *)make_write_clause_function_placeholder(CREATE_CLAUSE_FUNCTION_NAME, target_nodes),
            pstate->p_next_resno++, 
            "_create_clause", 
            false));

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    if (clause->next)
        return query;


    {
        cypher_parsestate *new_cpstate = make_cypher_parsestate(cpstate);
        Query *topquery = makeNode(Query);
        topquery->commandType = CMD_SELECT;
        topquery->targetList = NIL;

        transform_cypher_clause_as_subquery_2(new_cpstate, query);
        
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
        query->groupClause = transformGroupClause(cpstate, self->real_group_clause, &query->groupingSets,
                                                    &query->targetList,
                                                    query->sortClause, EXPR_KIND_GROUP_BY);
    else if (groupClause != NIL) // auto GROUP BY
        query->groupClause = transformGroupClause(cpstate, groupClause, &query->groupingSets, &query->targetList,
                                                    query->sortClause, EXPR_KIND_GROUP_BY);
    else 
        query->groupClause = NULL;

    if (self->having) 
        query->havingQual = transform_cypher_expr(cpstate, self->having, EXPR_KIND_HAVING);
    
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

    if (pstate->p_windowdefs != NIL)
        query->windowClause = transformWindowDefinitions(pstate, pstate->p_windowdefs, &query->targetList);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, expr);
    query->hasWindowFuncs = pstate->p_hasWindowFuncs;
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;
    query->hasSubLinks = pstate->p_hasSubLinks;
    query->hasAggs = pstate->p_hasAggs;

    assign_query_collations(pstate, query);

    // this must be done after collations, for reliable comparison of exprs 
    if (pstate->p_hasAggs || query->groupClause || query->groupingSets || query->havingQual)
        parseCheckAggregates(pstate, query);

    return query;
}


static TargetEntry *find_target_list_entry(cypher_parsestate *cpstate, Node *node, List **target_list,
                                           ParseExprKind expr_kind) {
    ListCell *lt;

    Node *expr = transform_cypher_expr(cpstate, node, expr_kind);

    foreach (lt, *target_list) {
        TargetEntry *te = lfirst(lt);
        Node *te_expr = strip_implicit_coercions((Node *)te->expr);

        if (equal(expr, te_expr))
            return te;
    }

    TargetEntry *te = transform_cypher_item(cpstate, node, expr, expr_kind, NULL, true);

    *target_list = lappend(*target_list, te);

    return te;
}

// see transformSortClause()
List *transform_cypher_order_by(cypher_parsestate *cpstate, List *sort_items,List **target_list,
                                       ParseExprKind expr_kind) {
    List *sort_list = NIL;
    ListCell *li;

    foreach (li, sort_items) {
        SortBy *sort_by = lfirst(li);
        sort_list = addTargetToSortList(cpstate, find_target_list_entry(cpstate, sort_by->node, target_list, expr_kind), sort_list, *target_list, sort_by);
    }

    return sort_list;
}

// see transformLimitClause()
static Node *transform_cypher_limit(cypher_parsestate *cpstate, Node *node, ParseExprKind expr_kind,
                                    const char *construct_name) {
    if (!node)
        return NULL;

    Node *qual = coerce_to_specific_type(cpstate, transform_cypher_expr(cpstate, node, expr_kind), INT8OID, construct_name);

    // LIMIT can't refer to any variables of the current query.
    if (contain_vars_of_level(qual, 0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                 errmsg("argument of %s must not contain variables", construct_name),
                 parser_errposition(cpstate, locate_var_of_level(qual, 0))));

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
                                                 cypher_clause *clause) {
    Query *query;
    cypher_match *self = (cypher_match *)clause->self;
    Node *where = self->where;

    if (where) {
        query = makeNode(Query);
        query->commandType = CMD_SELECT;

        ParseNamespaceItem *pnsi = transform_cypher_clause_as_subquery(cpstate, transform, clause, NULL, true);

        query->targetList = expandNSItemAttrs(cpstate, pnsi, 0, -1);

        markTargetListOrigins(cpstate, query->targetList);

        query->rtable = get_parse_state(cpstate)->p_rtable;
        query->jointree = makeFromExpr(get_parse_state(cpstate)->p_joinlist, NULL);

        assign_query_collations(cpstate, query);
    } else {
        query = transform(cpstate, clause);
    }

    query->hasSubLinks = get_parse_state(cpstate)->p_hasSubLinks;
    query->hasTargetSRFs = get_parse_state(cpstate)->p_hasTargetSRFs;
    query->hasAggs = get_parse_state(cpstate)->p_hasAggs;

    return query;
}

static Query *transform_cypher_match(cypher_parsestate *cpstate, cypher_clause *clause) {
    return transform_cypher_clause_with_where(cpstate, transform_cypher_match_pattern, clause);
}

static Node *transform_clause_for_join(cypher_parsestate *cpstate, cypher_clause *clause,
                                       RangeTblEntry **rte, ParseNamespaceItem **nsitem, Alias* alias) {
    *nsitem = transform_cypher_clause_as_subquery(cpstate, transform_cypher_clause, clause, alias, false);
    *rte = (*nsitem)->p_rte;

    RangeTblRef *rtr = makeNode(RangeTblRef);
    rtr->rtindex = (*nsitem)->p_rtindex;

    return (Node *) rtr;
}

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
    ParseNamespaceItem *jnsitem;
    int i = 0;

    j->jointype = JOIN_INNER;


    j->larg = transform_clause_for_join(cpstate, clause->prev, &l_rte, &l_nsitem, makeAlias(PREV_CYPHER_CLAUSE_ALIAS, NIL));
    pstate->p_namespace = lappend(pstate->p_namespace, l_nsitem);
        

    prevclause = clause->prev;
    clause->prev = NULL;
    pstate->p_lateral_active = true;

    j->rarg = transform_clause_for_join(cpstate, clause, &r_rte, &r_nsitem, makeAlias(CYPHER_OPT_RIGHT_ALIAS, NIL));

    pstate->p_lateral_active = false;
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

    addNSItemToQuery(pstate, jnsitem, true, false, true);

    return jnsitem->p_rte;
}

static Query *transform_cypher_match_pattern(cypher_parsestate *cpstate, cypher_clause *clause) {
    cypher_match *self = (cypher_match *)clause->self;
    Query *query;
    Node *where = self->where;
    Node *order_by = self->order_by;
    self->order_by = NULL;
    
    query = makeNode(Query);
    query->commandType = CMD_SELECT;

    if (clause->prev)
        query->targetList = expandNSItemAttrs(cpstate, transform_prev_cypher_clause(cpstate, clause->prev, true), 0, -1);

    transform_match_pattern(cpstate, query, self->pattern, where);
    // ORDER BY
    query->sortClause = transform_cypher_order_by(cpstate, order_by, &query->targetList, EXPR_KIND_ORDER_BY);

    markTargetListOrigins(cpstate, query->targetList);

    query->hasSubLinks = get_parse_state(cpstate)->p_hasSubLinks;
    query->hasWindowFuncs = get_parse_state(cpstate)->p_hasWindowFuncs;
    query->hasTargetSRFs = get_parse_state(cpstate)->p_hasTargetSRFs;
    query->hasAggs = get_parse_state(cpstate)->p_hasAggs;

    assign_query_collations(cpstate, query);

    return query;
}

static Node *make_null_const(int location) {
    A_Const *n = makeNode(A_Const);
    n->val.type = T_Null;
    n->location = location;

    return (Node *)n;
}

static A_Expr *
makeSimpleCypherA_Expr(A_Expr_Kind kind, char *name,
				 Node *lexpr, Node *rexpr, int location)
{
	A_Expr *a = makeNode(A_Expr);

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

static ParseNamespaceItem *add_srf_to_query(cypher_parsestate *cpstate, Node *n, char *var_name, List *colnames) {
    List *namespace = NULL;

    RangeTblRef *rtr = transform_srf_function(
        cpstate, 
        make_range_function(n, make_alias(var_name, colnames), true, false, false),
        &namespace);

    setNamespaceLateralState(namespace, false, true);

    get_parse_state(cpstate)->p_joinlist = lappend(get_parse_state(cpstate)->p_joinlist, rtr);
    get_parse_state(cpstate)->p_namespace = list_concat(get_parse_state(cpstate)->p_namespace, namespace);

    return linitial(namespace);
}

static ParseNamespaceItem *transformRangeFunction(cypher_parsestate *cpstate, RangeFunction *r) {
    List *funcexprs = NIL;
    List *funcnames = NIL;
    List *coldeflists = NIL;
    ListCell *lc;

    Assert(!get_parse_state(cpstate)->p_lateral_active);
    get_parse_state(cpstate)->p_lateral_active = true;

    foreach(lc, r->functions) {
        List *pair = (List*)lfirst(lc);

        Assert(list_length(pair) == 2);
        Node *fexpr = (Node*) linitial(pair);
        List *coldeflist = (List*) lsecond(pair);

        Node *last_srf = get_parse_state(cpstate)->p_last_srf;
        Node *newfexpr = transform_cypher_expr(cpstate, fexpr, EXPR_KIND_FROM_FUNCTION);

        if (get_parse_state(cpstate)->p_last_srf != last_srf && get_parse_state(cpstate)->p_last_srf != newfexpr)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("set-returning functions must appear at top level of FROM"),
                     parser_errposition(cpstate, exprLocation(get_parse_state(cpstate)->p_last_srf))));

        funcexprs = lappend(funcexprs, newfexpr);
        funcnames = lappend(funcnames, FigureColname(fexpr));

        if (coldeflist && r->coldeflist)
            ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("multiple column definition lists are not allowed for the same function"),
                     parser_errposition(cpstate, exprLocation((Node *) r->coldeflist))));

        coldeflists = lappend(coldeflists, coldeflist);
    }

    get_parse_state(cpstate)->p_lateral_active = false;

    assign_list_collations(get_parse_state(cpstate), funcexprs);

    Assert(r->coldeflist == NULL);

    return addRangeTableEntryForFunction(cpstate, funcnames, funcexprs, coldeflists, r,  r->lateral || contain_vars_of_level((Node *) funcexprs, 0), true);
}

static Node *transform_srf_function(cypher_parsestate *cpstate, Node *n, List **namespace) {
    ParseNamespaceItem *pnsi = transformRangeFunction(cpstate, (RangeFunction *) n);

    RangeTblEntry *rte = pnsi->p_rte;

    *namespace = list_make1(pnsi);
    RangeTblRef *rtr = makeNode(RangeTblRef);
    rtr->rtindex = list_length(get_parse_state(cpstate)->p_rtable);

    return (Node *) rtr;
}

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

// only needed for this function
// TODO move
#include "access/nbtree.h"
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
add_vertex_to_query(cypher_parsestate *cpstate, Query *query, cypher_node *node, List **quals)
{
    node->has_variable = false;
    node->declared_in_previous_clause = false;
    if (node->name) {
        node->has_variable = true;
        if (colNameToVar(cpstate, node->name, false, -1)) {
            node->declared_in_previous_clause = true;
            return refnameNamespaceItem(cpstate, NULL, PREV_CYPHER_CLAUSE_ALIAS, -1, NULL);
        }

        int sublevels_up;
        ParseNamespaceItem *pnsi = refnameNamespaceItem(cpstate, NULL, node->name, -1, NULL);
        if (pnsi) {
            node->in_join_tree = true;
            return pnsi;
        }
    }

    else
        node->name = get_next_default_alias(cpstate);

    node->is_default_label = true;
    if (node->label)
        node->is_default_label = false;
    else
        node->label = AG_DEFAULT_LABEL_VERTEX;

    node->in_join_tree = true;

    ParseNamespaceItem *pnsi = addRangeTableEntry(cpstate, 
                                    makeRangeVar(get_graph_namespace_name(cpstate->graph_name),
                                                 get_vertex_relation_name(cpstate, node->label, node->is_default_label),
                                                 -1),
                                    makeAlias(node->name, list_make2(makeString("id"), makeString("properties"))), 
                                    true, 
                                    true);
    setNamespaceLateralState(pnsi, false, true);

    addNSItemToQuery(cpstate, pnsi, true, true, true);

    return pnsi;
}

/*
 * add_vertex_retrieval_to_query
 *
 * This function handles adding the end vertex of a relationship to a MATCH
 * query. It determines how to retrieve or reference the vertex based on whether
 * it's a new variable, an existing variable from a previous clause, or a
 * variable already present in the current join tree.
 *
 * Parameters:
 *   cpstate   - The current Cypher parser state.
 *   query     - The Query node being constructed.
 *   node      - The cypher_node representing the vertex to be added.
 *   edge_pnsi - The ParseNamespaceItem for the incoming edge, used to get the
 *               end vertex's ID.
 *   quals     - A list of qualifications (WHERE clause conditions) to be updated.
 *
 * Behavior:
 *   1. If the vertex variable already exists (from a previous clause or earlier
 *      in the current path), it adds a qualification to join the edge's 'endid'
 *      with the existing vertex's 'id'. It then returns the namespace item for
 *      the existing variable.
 *   2. If the vertex is new, it adds a call to the `retrieve_vertex`
 *      set-returning function (SRF) to the query's FROM clause. This function
 *      fetches the vertex's properties using the 'endid' from the edge.
 *   3. If the vertex is anonymous (no variable name), it does nothing and
 *      returns NULL, as there's no need to retrieve or reference it.
 *
 * Returns:
 *   The ParseNamespaceItem for the vertex, which can be either a new SRF entry
 *   or a reference to an existing entry in the query's namespace. Returns NULL
 *   for anonymous vertices.
 */
static ParseNamespaceItem *
add_vertex_retrieval_to_query(cypher_parsestate *cpstate, Query *query, cypher_node *node, ParseNamespaceItem *edge_pnsi, List **quals)
{
    node->has_variable = false;
    if (node->name) {
        node->has_variable = true;

        /*
         * If the vertex variable has already been defined, create a join
         * qualification to link the incoming edge's end ID to the existing
         * vertex's ID.
         */
        Var *var;
        if (var = colNameToVar(cpstate, make_id_alias(node->name), false, -1)) {
            node->declared_in_previous_clause = true;

            *quals = lappend(*quals,
                        makeSimpleCypherA_Expr(AEXPR_OP, "=",
                            (Node *)scanNSItemForColumn(cpstate, edge_pnsi, 0, "endid", -1),
                            (Node *)var, -1));

            return refnameNamespaceItem(cpstate, NULL, PREV_CYPHER_CLAUSE_ALIAS, -1, NULL);
        }

        ParseNamespaceItem *pnsi = refnameNamespaceItem(cpstate, NULL, node->name, -1, NULL);
        if (pnsi) {
            node->in_join_tree = false;
            node->pnsi = pnsi;
            *quals = lappend(*quals,
                        makeSimpleCypherA_Expr(AEXPR_OP, "=",
                            (Node *)scanNSItemForColumn(cpstate, edge_pnsi, 0, "endid", -1),
                            (Node *)scanNSItemForColumn(cpstate, pnsi, 0, "id", -1), -1));

            return pnsi;
        }
    } else {
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
            make_alias(node->name, list_make1(makeString("properties"))), 
            true, 
            false, 
            false));
    setNamespaceLateralState(pnsi, false, true);
    addNSItemToQuery(cpstate, pnsi, true, true, true);

    return pnsi;
}

/*
 * add_edge_to_query
 *
 * This function adds a fixed-length edge to a MATCH query. It does this by
 * constructing a call to the `edge_search` set-returning function (SRF) and
 * adding it to the query's FROM clause as a lateral join.
 *
 * Parameters:
 *   cpstate - The current Cypher parser state.
 *   query   - The Query node being constructed.
 *   edge    - The cypher_relationship representing the edge to be added.
 *   vertex  - The cypher_node representing the start vertex for the edge search.
 *
 * Behavior:
 *   1. Validates that if the edge has a variable name, it has not been
 *      previously defined in the current scope. Assigns a default alias if
 *      unnamed.
 *   2. Processes the edge's label. If a label is specified, it looks up the
 *      label's ID and creates a filter constant. If no label is given, it
 *      defaults to the standard edge label.
 *   3. Constructs a `FuncCall` to the `edge_search` SRF. The arguments include
 *      the graph OID, the ID of the start `vertex`, and the label filter.
 *   4. Adds the SRF to the query's join tree using `add_srf_to_query`. The
 *      SRF is defined to return `id`, `startid`, `endid`, and `properties`.
 *
 * Returns:
 *   The ParseNamespaceItem for the newly added SRF, allowing its output
 *   columns to be referenced in subsequent transformations.
 */
static ParseNamespaceItem *
add_edge_to_query(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, cypher_node *vertex)
{
    edge->has_variable = false;
    if (edge->name) {
        if (colNameToVar(cpstate, make_id_alias(edge->name), false, -1) || refnameNamespaceItem(cpstate, NULL, edge->name, -1, NULL)) 
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("MATCH edge variable %s already exists", edge->name)));

        edge->has_variable = true;

    } else
        edge->name = get_next_default_alias(cpstate);

    bool is_default_label = true;
    Node *label_filter = NULL;
    if (edge->label) {
        is_default_label = false;
        label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);
        if (!lcd)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("Edge label \"%s\" does not exist", edge->label)));

            label_filter = make_int_const(lcd->id, -1);
    }else {
        edge->label = AG_DEFAULT_LABEL_EDGE;
    }

    return add_srf_to_query(
        cpstate, 
        makeFuncCall(
            list_make2(makeString("postgraph"), makeString("edge_search")),
            list_make4(
                make_int_const(cpstate->graph_oid, -1),
                vertex->declared_in_previous_clause ? 
                    colNameToVar(cpstate, make_id_alias(vertex->name), false, -1) : 
                    scanNSItemForColumn(cpstate, vertex->pnsi, 0, AG_VERTEX_COLNAME_ID, -1), 
                !is_default_label ? (Node *)label_filter : make_null_const(-1) , 
                make_null_const(-1)
            ),
            COERCE_EXPLICIT_CALL, -1), 
        edge->name, 
        list_make4(makeString("id"), makeString("startid"), makeString("endid"), makeString("properties")));
}

static ParseNamespaceItem *
add_edge_to_query_with_prev_edge(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, ParseNamespaceItem *prev_pnsi, char *prev_name)
{
    edge->has_variable = false;
    if (edge->name){
        if (colNameToVar(cpstate, make_id_alias(edge->name), false, -1) || refnameNamespaceItem(cpstate, NULL, edge->name, -1, NULL)) 
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("MATCH edge variable %s already exists", edge->name)));

        edge->has_variable = true;
    } else
        edge->name = get_next_default_alias(cpstate);

    bool is_default_label = true;
    Node *label_filter = NULL;
    if (edge->label) {
        is_default_label = false;
        label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);
        if (!lcd)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("Edge label \"%s\" does not exist", edge->label)));

        label_filter = make_int_const(lcd->id, -1);
    }else {
        edge->label = AG_DEFAULT_LABEL_EDGE;
    }

    if(edge->dir == CYPHER_REL_DIR_NONE) 
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("MATCH edge can't be bi directional")));

    return add_srf_to_query(
        cpstate, 
        makeFuncCall(
            list_make2(makeString("postgraph"), makeString("edge_search")),
            list_make4(
                make_int_const(cpstate->graph_oid, -1), 
                scanNSItemForColumn(cpstate, prev_pnsi, 0, "endid", -1), 
                !is_default_label ? (Node *)label_filter : make_null_const(-1), 
                make_null_const(-1)
            ),
            COERCE_EXPLICIT_CALL, -1), 
        edge->name, 
        list_make4(makeString("id"), makeString("startid"), makeString("endid"), makeString("properties")));
}



/*
 * add_variable_edge_to_query
 *
 * Adds a variable-length edge traversal to the query for Cypher MATCH clauses.
 * This function constructs a set-returning function (SRF) call to the
 * 'variable_edge_search' function, which finds all paths between vertices
 * with a variable number of edges (e.g., for patterns like (a)-[*1..3]->(b)).
 *
 * Parameters:
 *   cpstate    - Cypher parser state containing context for parsing.
 *   query      - The Query node being constructed.
 *   edge       - The cypher_relationship structure representing the edge.
 *   vertex_pnsi- The ParseNamespaceItem for the starting vertex.
 *
 * Behavior:
 *   - Ensures the edge does not have a variable name or label (not supported).
 *   - Assigns a default alias and label if necessary.
 *   - Extracts the variable-length bounds from the edge's varlen field.
 *   - Constructs a FuncCall node for 'variable_edge_search', passing:
 *       * graph_oid (the graph's OID)
 *       * vertex id (from the starting vertex)
 *       * lower bound (lidx) or NULL
 *       * upper bound (uidx) or NULL
 *   - Adds the SRF to the query with the edge's alias and expected output columns:
 *       * "edges"   - the list of traversed edge IDs
 *       * "endid"   - the ID of the ending vertex
 *       * "hashset" - a set of visited edge IDs for cycle prevention
 *
 * Returns:
 *   The ParseNamespaceItem for the SRF, which can be used to reference its output columns.
 */
static ParseNamespaceItem *
add_variable_edge_to_query(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge, ParseNamespaceItem *vertex_pnsi) {
    edge->has_variable = false;
    if (edge->name)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("MATCH variable edge variables are not supported")));
    else
        edge->name = get_next_default_alias(cpstate);

    bool is_default_label = true;
    Node *label_filter = NULL;
    if (edge->label) {
        is_default_label = false;
        label_cache_data *lcd = search_label_name_graph_cache(edge->label, cpstate->graph_oid);
        if (!lcd)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("Edge label \"%s\" does not exist", edge->label)));

            label_filter = make_int_const(lcd->id, -1);
    }else {
        edge->label = AG_DEFAULT_LABEL_EDGE;
    }
   
    A_Indices *varlen = (A_Indices *)edge->varlen;

    return add_srf_to_query(
        cpstate, 
        makeFuncCall(
            list_make2(makeString("postgraph"), makeString("variable_edge_search")),
            list_make5(
                make_int_const(cpstate->graph_oid, -1), 
                scanNSItemForColumn(cpstate, vertex_pnsi, 0, AG_VERTEX_COLNAME_ID, -1), 
                varlen->lidx ? varlen->lidx : make_null_const(-1), 
                varlen->uidx ? varlen->uidx : make_null_const(-1),
                !is_default_label ? (Node *)label_filter : make_null_const(-1)
            ),
            COERCE_EXPLICIT_CALL, -1),
        edge->name,
        list_make3(makeString("edges"), makeString("endid"), makeString("hashset")));
}

/*
 * add_all_fields_to_target_list
 *
 * This function is responsible for adding all the necessary fields from a
 * three-element path (left_vertex, edge, right_vertex) to the query's
 * target list. This is typically called for the first segment of a path in a
 * MATCH clause, ensuring that all variables and their properties are available
 * for subsequent clauses.
 *
 * For each element (vertex or edge) that has a variable, it adds entries for
 * its id, properties, and the complete entity expression (vertex or edge object)
 * to the target list.
 *
 * Special handling is included for relationship direction and variable length
 * edges.
 */
static void add_all_fields_to_target_list(cypher_parsestate *cpstate, Query *query,
    cypher_node *left_vertex, cypher_relationship *edge, cypher_node *right_vertex) {
    ParseState *pstate = (ParseState *)cpstate;

   // left vertex id
    if (left_vertex->in_join_tree && !left_vertex->declared_in_previous_clause) {
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
        // end id
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_END_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_endid_alias(edge->name), 
                                    false));
    } else {
        // id
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_id_alias(edge->name), 
                                    false));

        // start id
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_START_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_startid_alias(edge->name), 
                                    false));

        // end id
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_END_ID, -1), 
                                    pstate->p_next_resno++, 
                                    make_endid_alias(edge->name), 
                                    false));

        // properties
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_PROPERTIES, -1), 
                                    pstate->p_next_resno++, 
                                    make_property_alias(edge->name), 
                                    false));
    }

    // id
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
        if (right_vertex->has_variable && !right_vertex->declared_in_previous_clause) 
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

/*
 * add_new_fields_to_target_list
 *
 * This function is responsible for adding fields from a path segment to the
 * query's target list. It is intended for use on subsequent segments of a
 * multi-hop path (e.g., for the `(b)-[r2]->(c)` part of `MATCH (a)-[r1]->(b)-[r2]->(c)`).
 *
 * Unlike `add_all_fields_to_target_list`, this function assumes the left_vertex
 * of the segment has already been processed and its fields are present in the
 * target list. It only adds the fields for the new edge and the new right_vertex.
 *
 * For the edge, it adds its id, start/end ids, and properties. For the right
 * vertex, it adds its id and properties. If either has a variable, the full
 * entity expression is also added.
 */
static void add_new_fields_to_target_list(cypher_parsestate *cpstate, Query *query,
    cypher_node *left_vertex, cypher_relationship *edge, cypher_node *right_vertex) {
    ParseState *pstate = (ParseState *)cpstate;

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

/*
 * process_three_element_path
 *
 * This function handles the transformation of a simple Cypher path of length three,
 * such as (a)-[r]->(b), into its corresponding SQL query components within a
 * MATCH clause.
 *
 * It performs the following steps:
 * 1. Determines the start and end vertices based on the relationship's direction.
 * 2. Adds the start vertex to the query's FROM clause, either as a new table
 *    scan or by referencing an existing variable.
 * 3. Adds the edge to the query as a set-returning function (SRF)
 *    call (e.g., edge_search or variable_edge_search), which is joined to the
 *    start vertex.
 * 4. Adds the end vertex by either retrieving it through a function call or by
 *    adding qualifications to match an existing variable.
 * 5. Populates the query's target list with all necessary fields (id, properties,
 *    etc.) from all three path elements, making them available for subsequent
 *    clauses.
 */
static void
process_three_element_path(cypher_parsestate *cpstate, Query *query, cypher_path *path, List **quals)
{
    cypher_node *start_vertex;
    cypher_node *end_vertex;
    cypher_relationship *edge = lsecond(path->path);

    if (edge->dir == CYPHER_REL_DIR_RIGHT) {
        start_vertex = linitial(path->path);
        end_vertex = lthird(path->path);
    } else if (edge->dir == CYPHER_REL_DIR_LEFT) {
        start_vertex = lthird(path->path);
        end_vertex = linitial(path->path);
    } else {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("MATCH edge can't be bi directional")));
    }

    start_vertex->pnsi = add_vertex_to_query(cpstate, query, start_vertex, quals);
    if (edge->varlen)
        edge->pnsi = add_variable_edge_to_query(cpstate, query, edge, start_vertex->pnsi);
    else
        edge->pnsi = add_edge_to_query(cpstate, query, edge, start_vertex);

    end_vertex->pnsi = add_vertex_retrieval_to_query(cpstate, query, end_vertex, edge->pnsi, quals);

    // SELECT fields
    add_all_fields_to_target_list(cpstate, query, linitial(path->path), edge, lthird(path->path));
}

typedef struct path_parts {
    int start;
    int end;
    cypher_rel_dir dir;
} path_parts;

/*
 * find_path_parts
 *
 * This function analyzes a Cypher MATCH path and breaks it down into
 * continuous, single-direction segments. A complex path with changing
 * directions, such as (a)-[]->(b)<-[]-(c), is not processed directly. Instead,
 * this function identifies the points where the direction changes and divides
 * the path into "parts," where each part has a consistent direction (either
 * all right-directed `->` or all left-directed `<-`).
 *
 * It iterates through the relationships in the path, creating a new `path_parts`
 * segment each time the direction of a relationship differs from the previous one.
 *
 * It also validates that no relationship is directionless (`-[]-`), as this is
 * not supported in a MATCH clause, yet.
 *
 * Returns:
 *   A List of `path_parts` structs. Each struct defines a segment with a
 *   `start` index, an `end` index (within the original path list), and a `dir`
 *   (direction). This allows the parser to process complex paths one simple,
 *   unidirectional segment at a time.
 */
static List *find_path_parts(cypher_path *path) {
    cypher_relationship *start = list_nth(path->path,1);

    if (start->dir == CYPHER_REL_DIR_NONE)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("MATCH edge can't be bi directional")));

    path_parts *pp = palloc(sizeof(path_parts));
    pp->start = 1;
    pp->dir = start->dir;

    List *parts = list_make1(pp);
    int i;
    for (i = 3; i < list_length(path->path); i += 2) {
        cypher_relationship *rel = list_nth(path->path, i);

        if (rel->dir == CYPHER_REL_DIR_NONE)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("MATCH edge can't be bi directional")));

        if (pp->dir != rel->dir) {
            pp->end = i;

            pp = palloc(sizeof(path_parts));
            pp->start = i;
            
            pp->dir = rel->dir;
            parts = lappend(parts, pp);
        } else
            pp->end = i;
    }
    pp->end = i;

    return parts;
}

/*
 * process_path_part
 *
 * This function transforms a continuous, single-direction segment of a Cypher
 * MATCH path into its corresponding SQL query components. A complex path with
 * changing directions (e.g., (a)-->(b)<--(c)) is broken down into "parts" of
 * consistent direction, and this function processes one such part at a time.
 *
 * It iterates through the vertices and edges of the path segment, building up
 * the query by:
 *
 * 1. Adding Vertices and Edges:
 *    - For the starting vertex of the segment, it adds a table scan.
 *    - For each edge, it adds a set-returning function call (e.g., `edge_search`)
 *      to the FROM clause, chaining it to the previous element in the path.
 *    - For each subsequent vertex, it adds a function call to retrieve it or
 *      a join condition if the vertex variable already exists.
 *
 * 2. Adding Qualifications:
 *    - It generates join conditions to link the end of one path segment to the
 *      start of another (e.g., to join `(a)->(b)` and `(c)->(b)` on `b`).
 *    - It adds cycle-prevention qualifications to ensure that no edge is
 *      traversed more than once within the same MATCH clause.
 *
 * 3. Populating the Target List:
 *    - It calls `add_all_fields_to_target_list` or `add_new_fields_to_target_list`
 *      to project all necessary columns (id, properties, etc.) for the new
 *      elements, making them available to subsequent query clauses.
 *
 * The function handles both right-directed (`->`) and left-directed (`<-`)
 * path segments by adjusting its iteration order accordingly.
 */
static void
process_path_part(
    cypher_parsestate *cpstate,
    Query *query,
    cypher_path *path,
    path_parts *pp,
    List **quals,
    cypher_relationship **prev_edge)
{
    ParseState *pstate = (ParseState *)cpstate;

    if (pp->dir == CYPHER_REL_DIR_RIGHT) {
        cypher_node *left_vertex = list_nth(path->path, pp->start - 1);
        left_vertex->pnsi = add_vertex_to_query(cpstate, query, left_vertex, quals);

        for (int i = pp->start; i < pp->end; i += 2) {
            cypher_node *left_vertex = list_nth(path->path, i - 1);
            cypher_relationship *edge = list_nth(path->path, i);
            cypher_node *right_vertex = list_nth(path->path, i + 1);

            if (edge->varlen)
                edge->pnsi = add_variable_edge_to_query(cpstate, query, edge, left_vertex->pnsi);
            else if (i == pp->start)
                edge->pnsi = add_edge_to_query(cpstate, query, edge, left_vertex);
            else {
                cypher_relationship *prev = list_nth(path->path, i - 2);
                edge->pnsi = add_edge_to_query_with_prev_edge(cpstate, query, edge, prev->pnsi, make_endid_alias(prev->name));
            }
            right_vertex->pnsi = add_vertex_retrieval_to_query(cpstate, query, right_vertex, edge->pnsi, quals);

            if (i == pp->start && *prev_edge)
                *quals = lappend(*quals,
                    makeSimpleCypherA_Expr(AEXPR_OP, "=",
                        (Node *)scanNSItemForColumn(cpstate, (*prev_edge)->pnsi, 0, "endid", -1),
                        (Node *)scanNSItemForColumn(cpstate, edge->pnsi, 0, "endid", -1), -1));

            for (int j = 1; j < i; j += 2) {
                cypher_relationship *earlier_edge = list_nth(path->path, j);
                if (earlier_edge->varlen && !edge->varlen)
                    *quals = lappend(*quals,
                        makeSimpleCypherA_Expr(AEXPR_OP, "@>",
                            (Node *)scanNSItemForColumn(cpstate, earlier_edge->pnsi, 0, "hashset", -1),
                            (Node *)scanNSItemForColumn(cpstate, edge->pnsi, 0, "id", -1), -1));
                else
                    *quals = lappend(*quals,
                        makeSimpleCypherA_Expr(AEXPR_OP, "<>",
                            (Node *)scanNSItemForColumn(cpstate, earlier_edge->pnsi, 0, "id", -1),
                            (Node *)scanNSItemForColumn(cpstate, edge->pnsi, 0, "id", -1), -1));
            }

            if (pp->start == i)
                add_all_fields_to_target_list(cpstate, query, left_vertex, edge, right_vertex);
            else
                add_new_fields_to_target_list(cpstate, query, left_vertex, edge, right_vertex);

            *prev_edge = edge;
        }
    } else if (pp->dir == CYPHER_REL_DIR_LEFT) {
        cypher_node *right_vertex = list_nth(path->path, pp->end - 1);
        right_vertex->pnsi = add_vertex_to_query(cpstate, query, right_vertex, quals);

        for (int i = pp->end; i > pp->start; i -= 2) {
            cypher_node *left_vertex = list_nth(path->path, i - 3);
            cypher_relationship *edge = list_nth(path->path, i - 2);
            cypher_node *right_vertex = list_nth(path->path, i - 1);

            if (edge->varlen)
                edge->pnsi = add_variable_edge_to_query(cpstate, query, edge, right_vertex->pnsi);
            else if (i == pp->end)
                edge->pnsi = add_edge_to_query(cpstate, query, edge, right_vertex);
            else {
                cypher_relationship *prev = list_nth(path->path, i);
                edge->pnsi = add_edge_to_query_with_prev_edge(cpstate, query, edge, prev->pnsi, make_endid_alias(prev->name));
            }
            right_vertex->pnsi = add_vertex_retrieval_to_query(cpstate, query, left_vertex, edge->pnsi, quals);

            if (i == pp->end && *prev_edge != NULL) {
                *quals = lappend(*quals,
                    makeSimpleCypherA_Expr(AEXPR_OP, "=",
                        (Node *)scanNSItemForColumn(cpstate, (*prev_edge)->pnsi, 0, "endid", -1),
                        (Node *)scanNSItemForColumn(cpstate, edge->pnsi, 0, "endid", -1), -1));
            }

            for (int p = 1; p < pp->start; p += 2) {
                cypher_relationship *prev = list_nth(path->path, p);
                *quals = lappend(*quals,
                    makeSimpleCypherA_Expr(AEXPR_OP, "<>",
                        (Node *)scanNSItemForColumn(cpstate, prev->pnsi, 0, "id", -1),
                        (Node *)scanNSItemForColumn(cpstate, edge->pnsi, 0, "id", -1), -1));
            }

            for (int k = pp->end; k > i; k -= 2) {
                cypher_relationship *prev = list_nth(path->path, k - 2);
                *quals = lappend(*quals,
                    makeSimpleCypherA_Expr(AEXPR_OP, "<>",
                        (Node *)scanNSItemForColumn(cpstate, prev->pnsi, 0, "id", -1),
                        (Node *)scanNSItemForColumn(cpstate, edge->pnsi, 0, "id", -1), -1));
            }

            if (pp->start == i)
                add_all_fields_to_target_list(cpstate, query, left_vertex, edge, right_vertex);
            else
                add_new_fields_to_target_list(cpstate, query, right_vertex, edge, left_vertex);

            *prev_edge = edge;
        }
    }
}

static void transform_match_pattern(cypher_parsestate *cpstate, Query *query, List *pattern, Node *where) {
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *lc;
    List *quals = NIL;


    if (list_length(pattern) !=1 )
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("MATCH found more than one pattern")));

    foreach (lc, pattern) {
        cypher_path *path = (cypher_path *) lfirst(lc);

        if (list_length(path->path) == 1) {
            cypher_node *node = linitial(path->path);

            node->pnsi = add_vertex_to_query(cpstate, query, node, &quals);
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

        
        if (list_length(path->path) == 3) {
            process_three_element_path(cpstate, query, path, &quals);
        } else {
            List *parts = find_path_parts(path);
            ListCell *lc;
            cypher_relationship *prev_edge = NULL;
            foreach (lc, parts) {
                path_parts *pp = (path_parts *) lfirst(lc);
                process_path_part(cpstate, query, path, pp, &quals, &prev_edge);
            }
        }
    }
    
    // AND the quals for each path together
    Expr *expr = NULL;
    if (quals != NIL) 
        expr = (Expr *)transformExpr(cpstate, (Node *)makeBoolExpr(AND_EXPR, quals, -1), EXPR_KIND_WHERE);

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
}

static List *make_target_list_from_join(ParseState *pstate, RangeTblEntry *rte) {
    List *targetlist = NIL;
    ListCell *lt;
    ListCell *ln;

    AssertArg(rte->rtekind == RTE_JOIN);

    forboth(lt, rte->joinaliasvars, ln, rte->eref->colnames) {
        targetlist = lappend(targetlist,
            makeTargetEntry((Expr *) lfirst(lt), (AttrNumber) pstate->p_next_resno++, strVal(lfirst(ln)), false));
    }

    return targetlist;
}

static ParseNamespaceItem *
transform_cypher_clause_as_subquery(cypher_parsestate *cpstate, transform_method transform, cypher_clause *clause,
                                    Alias *alias, bool add_rte_to_query) {
    /*
     * As these are all sub queries, if this is just of type NONE, note it as a
     * SUBSELECT. Other types will be dealt with as needed.
     */
    if (get_parse_state(cpstate)->p_expr_kind == EXPR_KIND_NONE) {
        get_parse_state(cpstate)->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;
    } else if (get_parse_state(cpstate)->p_expr_kind == EXPR_KIND_OTHER) {
        // this is a lateral subselect for the MERGE
        get_parse_state(cpstate)->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;
        get_parse_state(cpstate)->p_lateral_active = true;
    }
    
    ParseExprKind old_expr_kind = get_parse_state(cpstate)->p_expr_kind;
    Query *query = analyze_cypher_clause(transform, clause, cpstate);
    get_parse_state(cpstate)->p_expr_kind = old_expr_kind;

    if (!alias)
        alias = makeAlias(PREV_CYPHER_CLAUSE_ALIAS, NIL);

    ParseNamespaceItem *pnsi = addRangeTableEntryForSubquery(cpstate, query, alias, get_parse_state(cpstate)->p_lateral_active, true);

    if (add_rte_to_query)
        addNSItemToQuery(cpstate, pnsi, true, false, true);

    return pnsi;
}

static Query *analyze_cypher_clause(transform_method transform, cypher_clause *clause, cypher_parsestate *parent_cpstate) {
    cypher_parsestate *cpstate = make_cypher_parsestate(parent_cpstate);

    get_parse_state(cpstate)->p_expr_kind = get_parse_state(parent_cpstate)->p_expr_kind;

    Query *query = transform(cpstate, clause);

    free_cypher_parsestate(cpstate);

    return query;
}

Query *cypher_parse_sub_analyze(Node *parseTree, cypher_parsestate *cpstate, CommonTableExpr *parentCTE,
                                bool locked_from_parent, bool resolve_unknowns) {
    ParseState *pstate = make_cypher_parsestate((ParseState*)cpstate);

    pstate->p_parent_cte = parentCTE;
    pstate->p_locked_from_parent = locked_from_parent;
    pstate->p_resolve_unknowns = resolve_unknowns;

    cypher_clause *clause = palloc0(sizeof(cypher_clause));
    clause->self = parseTree;
    Query *query = transform_cypher_clause(pstate, clause);

    free_cypher_parsestate(pstate);

    return query;
}

/*
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
        //cypher_clause *merge_clause_as_match = convert_merge_to_match(self);

        // Create the metadata needed for creating missing paths.
        merge_path = transform_cypher_merge_path(cpstate, query, (cypher_path *)self->path);

        /*
         * If there is not a previous clause, then treat the MERGE's path
         * itself as the previous clause. We need to do this because if the
         * pattern exists, then we need to path all paths that match the
         * query patterns in the execution phase. WE way to do that by
         * converting the merge to a match and have the match logic create the
         * query. the merge execution phase will just pass the results up the
         * execution tree if the path exists.
         */
        query->targetList = list_concat(query->targetList,
            expandNSItemAttrs(get_parse_state(cpstate),
                transform_prev_cypher_clause(cpstate, merge_clause_as_match, true),
                0,
                -1));
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
    query->targetList = lappend(query->targetList, 
        makeTargetEntry(
            (Expr *)make_write_clause_function_placeholder(MERGE_CLAUSE_FUNCTION_NAME, merge_information),
            merge_information->merge_function_attr = pstate->p_next_resno++, 
            "_merge_clause", 
            false));

    
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

        transform_cypher_clause_as_subquery_2(new_cpstate, query);
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
    merge_path = transform_cypher_merge_path(cpstate, query, path);

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
    /*if (path->var_name) {
        TargetEntry *te = findTarget(target_list, path->var_name);
        te->expr = add_volatile_traversal_wrapper(te->expr);
        path->path_attr_num = te->resno;
    }*/

    ListCell *lc;
    foreach (lc, path->target_nodes) {
        cypher_target_node *node = lfirst(lc);
        
        TargetEntry *te  = get_target_entry(cpstate, target_list, node->variable_name);
        if (!IsA(te->expr, Var))
            continue;
        /*
         * Add the volatile wrapper function around the expression, this
         * ensures the optimizer will not remove the expression, if nothing
         * other than a private data structure needs it.
         */
        if (((Var *)te->expr)->vartype == VERTEXOID) 
            te->expr = add_volatile_vertex_wrapper(te->expr);
        else if (((Var *)te->expr)->vartype == EDGEOID)
            te->expr = add_volatile_edge_wrapper(te->expr);
        else if (((Var *)te->expr)->vartype == VARIABLEEDGEOID)
            te->expr = add_volatile_vle_edge_wrapper(te->expr);
        else
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("internal error: unsupported type for MERGE clause")));

        // Mark the tuple position the target_node is for.
        node->tuple_position = te->resno;
    }
}

/*
 * Creates the target nodes for a merge path. If MERGE has a path that doesn't
 * exist then in the MERGE clause we act like a CREATE clause. This function
 * sets up the metadata needed for that process.
 */
static cypher_create_path *transform_cypher_merge_path(cypher_parsestate *cpstate, Query *query, cypher_path *path) {
    cypher_create_path *ccp = make_ag_node(cypher_create_path);

    ccp->target_nodes = NIL;
    ListCell *lc;
    foreach (lc, path->path) {
        if (is_ag_node(lfirst(lc), cypher_node)) 
            ccp->target_nodes = lappend(ccp->target_nodes,
                transform_merge_cypher_node(cpstate, query, lfirst(lc)));
        else
            ccp->target_nodes = lappend(ccp->target_nodes, 
                transform_merge_cypher_edge(cpstate, query, lfirst(lc)));
    }

    ccp->path_attr_num = InvalidAttrNumber;

    return ccp;
}

/*
 * Transforms the parse cypher_relationship to a target_entry for merge.
 * All edges that have variables assigned in a merge must be declared in
 * the merge. Throw an error otherwise.
 */
static cypher_target_node *transform_merge_cypher_edge(cypher_parsestate *cpstate, Query *query, cypher_relationship *edge) {
    ParseState *pstate = (ParseState *)cpstate;
    cypher_target_node *target_node = make_ag_node(cypher_target_node);

    if (edge->name != NULL) 
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("MERGE does not support variables"),
                 parser_errposition(get_parse_state(cpstate), edge->location)));
    else 
        edge->name = get_next_default_alias(cpstate);

    target_node->type = LABEL_KIND_EDGE;
    target_node->flags |= CYPHER_TARGET_NODE_FLAG_INSERT;
    target_node->label_name = edge->label;
    target_node->variable_name = edge->name;
    target_node->resultRelInfo = NULL;
    target_node->dir = edge->dir;

    if (edge->label) {
        validate_or_create_elabel(cpstate, edge);
        target_node->label_name = edge->label;
    } else {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("edges declared in a MERGE clause must have a label"),
                 parser_errposition(get_parse_state(cpstate), edge->location)));
    }

    // id
    label_cache_data *lcd = search_label_name_graph_cache(edge->label ? edge->label : AG_DEFAULT_LABEL_EDGE , cpstate->graph_oid);
    Relation rel = RelationIdGetRelation(lcd->relation);
    target_node->id_expr = (Expr *)build_column_default(rel, 1);
    target_node->relid = lcd->relation;
    table_close(rel, NoLock);

    // props
    if (edge->props) {
      /*  query->targetList = lappend(query->targetList,
            makeTargetEntry(//BlackPink
                add_volatile_wrapper(transform_cypher_expr(cpstate, edge->props, EXPR_KIND_INSERT_TARGET)),
                target_node->prop_attr_num = get_parse_state(cpstate)->p_next_resno++,
                make_property_alias(edge->name),
                false));
*/
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    add_volatile_wrapper(scanNSItemForColumn(pstate, edge->pnsi, 0, AG_EDGE_COLNAME_PROPERTIES, -1)), 
                                    target_node->prop_attr_num = get_parse_state(cpstate)->p_next_resno++,
                                    make_property_alias(edge->name), 
                                    false));
    } else {
        target_node->prop_attr_num = InvalidAttrNumber;
    }

    return target_node;
}


static cypher_target_node *transform_merge_cypher_node(cypher_parsestate *cpstate, Query *query, cypher_node *node) {
    cypher_target_node *target_node = make_ag_node(cypher_target_node);

    target_node->type = LABEL_KIND_VERTEX;
    

    if (node->name) {
       /* ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("MERGE does not support variables"),
                 parser_errposition(get_parse_state(cpstate), node->location)));
*/
        // /ereport(ERROR, (errmsg_internal("nodes in CREATE cannot be a variable")));
        Var *var = NULL;
        if ((var = colNameToVar(cpstate, node->name, false, -1)) ||
        (target_node->id_attr_num = get_target_entry_resno(cpstate, query->targetList, make_id_alias(node->name))) != -1) {
        
            if (var && var->vartype != VERTEXOID)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("CREATE vertex variable %s already exists", node->name)));
        
            if (node->props)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("previously declared nodes in a create clause cannot have properties")));
            if (node->label)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("previously declared variables cannot have a label")));
      
            target_node->flags |= EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE;
    
            target_node->id_attr_num = get_target_entry_resno(cpstate, query->targetList, make_id_alias(node->name));
   
            return target_node;
        }
    } else {
        target_node->tuple_position = InvalidAttrNumber;
        node->name = get_next_default_alias(cpstate);
    }

    target_node->variable_name = node->name;

    if (node->label) {
        validate_or_create_vlabel(cpstate, node);
        target_node->label_name = node->label;
    } else {
        target_node->label_name = "";
        //node->label = AG_DEFAULT_LABEL_VERTEX;
    }

    target_node->flags |= CYPHER_TARGET_NODE_FLAG_INSERT;

    // id
    label_cache_data *lcd = search_label_name_graph_cache(node->label ? node->label : AG_DEFAULT_LABEL_VERTEX, cpstate->graph_oid);
    Relation rel = RelationIdGetRelation(lcd->relation);
    target_node->id_expr = (Expr *)build_column_default(rel, 1);
    target_node->relid = lcd->relation;
    table_close(rel, NoLock);
    target_node->adj_relid = lcd->vertex_adjlist;
    // props
    if (node->props) {
       /* query->targetList = lappend(query->targetList,
            makeTargetEntry(
                add_volatile_wrapper(transform_cypher_expr(cpstate, node->props, EXPR_KIND_INSERT_TARGET)),
                target_node->prop_attr_num = get_parse_state(cpstate)->p_next_resno++,
                make_property_alias(node->name),
                false));*/
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    colNameToVar(cpstate, make_property_alias(node->name), false, -1), 
                                    target_node->prop_attr_num = get_parse_state(cpstate)->p_next_resno++,
                                    make_property_alias(node->name), 
                                    false));
                                    /*
        query->targetList = lappend(query->targetList, 
                                makeTargetEntry(
                                    add_volatile_wrapper(scanNSItemForColumn(get_parse_state(cpstate), refnameNamespaceItem(cpstate, NULL, node->name, -1, NULL), 0, AG_VERTEX_COLNAME_PROPERTIES, -1)), 
                                    target_node->prop_attr_num = get_parse_state(cpstate)->p_next_resno++,
                                    make_property_alias(node->name), 
                                    false));

colNameToVar(cpstate, node->name, false, -1))
*/


    } else {
        target_node->prop_attr_num = InvalidAttrNumber;
    }

    return target_node;
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


static FuncExpr *make_write_clause_function_placeholder(char *function_name, Node *clause_information) {
    StringInfo str = makeStringInfo();
    outNode(str, clause_information);

    return makeFuncExpr(
        get_ag_func_oid(function_name, 1, INTERNALOID), 
        GTYPEOID,
        list_make1(makeConst(INTERNALOID, -1, InvalidOid, str->len, PointerGetDatum(str->data), false, false)),
        InvalidOid,
        InvalidOid,
        COERCE_EXPLICIT_CALL);
}

static Node *make_vertex_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi) {
    return makeFuncExpr(
        get_ag_func_oid("build_vertex", 3, GRAPHIDOID, OIDOID, GTYPEOID),
        VERTEXOID,
        list_make3(
            scanNSItemForColumn(cpstate, pnsi, 0, AG_VERTEX_COLNAME_ID, -1),
            makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(cpstate->graph_oid), false, true), 
            scanNSItemForColumn(cpstate, pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1)),
        InvalidOid,
        InvalidOid,
        COERCE_EXPLICIT_CALL);

}

static Node *make_vertex_expr_with_edge(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi, ParseNamespaceItem *edge_pnsi) {
    return makeFuncExpr(
        get_ag_func_oid("build_vertex", 3, GRAPHIDOID, OIDOID, GTYPEOID),
        VERTEXOID,
        list_make3(
            scanNSItemForColumn(cpstate, edge_pnsi, 0, "endid", -1),
            makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(cpstate->graph_oid), false, true), 
            scanNSItemForColumn(cpstate, pnsi, 0, AG_VERTEX_COLNAME_PROPERTIES, -1)),
        InvalidOid,
        InvalidOid,
        COERCE_EXPLICIT_CALL);
}

static Node *make_edge_expr(cypher_parsestate *cpstate, ParseNamespaceItem *pnsi) {
    return makeFuncExpr(
        get_ag_func_oid("build_edge", 5, GRAPHIDOID, GRAPHIDOID, GRAPHIDOID, OIDOID, GTYPEOID),
        EDGEOID,
        list_make5(
            scanNSItemForColumn(cpstate, pnsi, 0, AG_EDGE_COLNAME_ID, -1), 
            scanNSItemForColumn(cpstate, pnsi, 0, "startid", -1), 
            scanNSItemForColumn(cpstate, pnsi, 0, "endid", -1),
            makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(cpstate->graph_oid), false, true), 
            scanNSItemForColumn(cpstate, pnsi, 0, AG_EDGE_COLNAME_PROPERTIES, -1)), 
        InvalidOid,
        InvalidOid,
        COERCE_EXPLICIT_CALL);
}

static Node *
make_graphid_placeholder(cypher_parsestate *cpstate) {
    return makeConst(GRAPHIDOID, -1, InvalidOid, sizeof(graphid), GRAPHID_GET_DATUM(0), false, true);
} 

static Node *
make_gtype_placeholder(cypher_parsestate *cpstate) {
    return makeConst(GTYPEOID, -1, InvalidOid, -1, integer_to_gtype(0), false, false);
} 

static Node *
make_edge_placeholder(cypher_parsestate *cpstate) {
    return makeConst(EDGEOID, -1, InvalidOid, -1, create_edge(0, 0, 0, cpstate->graph_oid, NULL), false, false);
} 

static Node *
make_vertex_placeholder(cypher_parsestate *cpstate) {
    return makeConst(VERTEXOID, -1, InvalidOid, -1, create_vertex(0, cpstate->graph_oid, NULL), false, false);
}


static char *make_vertex_adjlist_alias(char *var_name) {
    char *str = palloc(strlen(var_name) + 8);

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



static Expr *add_volatile_wrapper(Expr *node) {
    return (Expr *)makeFuncExpr(get_ag_func_oid("gtype_volatile_wrapper", 1, GTYPEOID), GTYPEOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
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
/*
static Expr *add_volatile_traversal_wrapper(Expr *node) {
    Oid oid = get_ag_func_oid("gtype_volatile_wrapper", 1, TRAVERSALOID);

    return (Expr *)makeFuncExpr(oid, TRAVERSALOID, list_make1(node), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}*/