/*
 * Copyright (C) 2023 PostGraphDB
 *  
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 * 
 * Portions Copyright (c) 2020-2023, Apache Software Foundation
 * Portions Copyright (c) 1996-2010, Bitnine Global
 */

#ifndef AG_CYPHER_NODE_H
#define AG_CYPHER_NODE_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

#include "nodes/ag_nodes.h"

/* cypher sub patterns */
typedef enum csp_kind
{
        CSP_EXISTS,
        CSP_ALL,
        CSP_SIZE,
        CSP_EXPR,
        CSP_FINDPATH /* shortestpath, allshortestpaths, dijkstra */
} csp_kind;

typedef struct cypher_sub_pattern
{
        ExtensibleNode extensible;
        csp_kind kind;
        List *pattern;
} cypher_sub_pattern;

/*
 * clauses
 */
typedef struct cypher_group_by
{
    ExtensibleNode extensible;
    List *items; // a list of ResTargets
} cypher_group_by;

typedef struct cypher_return
{
    ExtensibleNode extensible;
    bool distinct;
    List *items; // a list of ResTargets
    List *real_group_clause;
    cypher_group_by *group_by;
    Node *having;
    List *order_by;
    List *window_clause;
    Node *skip;
    Node *limit;
    Node *where;
    bool all_or_distinct;
    SetOperation op;
    List *larg; // lefthand argument of the unions
    List *rarg; // righthand argument of the unions
} cypher_return;

typedef struct cypher_with
{
    ExtensibleNode extensible;
    bool distinct;
    List *items; // a list of ResTargets
    List *real_group_clause;
    cypher_group_by *group_by;
    Node *having;
    List *order_by;
    List *window_clause;
    Node *skip;
    Node *limit;
    Node *where;
} cypher_with;

typedef struct cypher_match
{
    ExtensibleNode extensible;
    List *pattern; // a list of cypher_paths
    Node *where; // optional WHERE subclause (expression)
    List *order_by;
    bool optional; // OPTIONAL MATCH
} cypher_match;

typedef struct cypher_create
{
    ExtensibleNode extensible;
    List *pattern; // a list of cypher_paths
} cypher_create;

typedef struct cypher_set
{
    ExtensibleNode extensible;
    List *items; // a list of cypher_set_items
    bool is_remove; // true if this is REMOVE clause
    int location;
} cypher_set;

typedef struct cypher_set_item
{
    ExtensibleNode extensible;
    Node *prop; // LHS
    Node *expr; // RHS
    bool is_add; // true if this is +=
    int location;
} cypher_set_item;

typedef struct cypher_delete
{
    ExtensibleNode extensible;
    bool detach; // true if DETACH is specified
    List *exprs; // targets of this deletion
    int location;
} cypher_delete;

typedef struct cypher_unwind
{
    ExtensibleNode extensible;
    ResTarget *target;
    Node *where;
    RangeFunction *rf;
} cypher_unwind;

typedef struct cypher_merge
{
    ExtensibleNode extensible;
    Node *path;
} cypher_merge;

typedef enum cypher_call_kind
{
    CCK_FUNCTION,
    CCK_CYPHER_SUBQUERY,
	CCK_SQL_SUBQUERY
} cypher_call_kind; 

typedef struct cypher_call
{
    ExtensibleNode extensible;
    cypher_call_kind cck;
    // Function
    Node *func;
    List *yield_list;
    Node *where;
    char *alias;
    // cypher Subquery
    List *cypher;
    // sql subquery
    Node *query_tree;
} cypher_call;

typedef struct cypher_load_csv
{
    ExtensibleNode extensible;
    char *file;
} cypher_load_csv;


/*
 * pattern
 */

typedef struct cypher_path
{
    ExtensibleNode extensible;
    List *path; // [ node ( , relationship , node , ... ) ]
    char *var_name;
    int location;
} cypher_path;

// ( name :label props )
typedef struct cypher_node
{
    ExtensibleNode extensible;
    char *name;
    char *label;
    Node *props; // map or parameter
    int location;
} cypher_node;

typedef enum
{
    CYPHER_REL_DIR_NONE,
    CYPHER_REL_DIR_LEFT,
    CYPHER_REL_DIR_RIGHT
} cypher_rel_dir;

// -[ name :label props ]-
typedef struct cypher_relationship
{
    ExtensibleNode extensible;
    char *name;
    char *label;
    Node *props; // map or parameter
    Node *varlen; // variable length relationships (A_Indices)
    cypher_rel_dir dir;
    int location;
} cypher_relationship;

/*
 * DDL Commands
 */
typedef struct cypher_create_graph
{
    ExtensibleNode extensible;
    char *graph_name;
} cypher_create_graph;

typedef struct cypher_use_graph
{
    ExtensibleNode extensible;
    char *graph_name;
} cypher_use_graph;

typedef struct cypher_drop_graph
{
    ExtensibleNode extensible;
    char *graph_name;
    bool cascade;
} cypher_drop_graph;

/*
 * expression
 */

typedef struct cypher_bool_const
{
    ExtensibleNode extensible;
    bool boolean;
    int location;
} cypher_bool_const;

typedef struct cypher_inet_const
{
    ExtensibleNode extensible;
    char *inet;
    int location;
} cypher_inet_const;


typedef struct cypher_integer_const
{
    ExtensibleNode extensible;
    int64 integer;
    int location;
} cypher_integer_const;

typedef struct cypher_param
{
    ExtensibleNode extensible;
    char *name;
    int location;
} cypher_param;

typedef struct cypher_map
{
    ExtensibleNode extensible;
    List *keyvals;
    int location;
} cypher_map;

typedef struct cypher_list
{
    ExtensibleNode extensible;
    List *elems;
    int location;
} cypher_list;

enum cypher_string_match_op
{
    CSMO_STARTS_WITH,
    CSMO_ENDS_WITH,
    CSMO_CONTAINS
};

typedef struct cypher_string_match
{
    ExtensibleNode extensible;
    enum cypher_string_match_op operation;
    Node *lhs;
    Node *rhs;
    int location;
} cypher_string_match;

typedef struct cypher_create_target_nodes
{
    ExtensibleNode extensible;
    List *paths;
    uint32 flags;
    uint32 graph_oid;
} cypher_create_target_nodes;

typedef struct cypher_create_path
{
    ExtensibleNode extensible;
    List *target_nodes;
    AttrNumber path_attr_num;
    char *var_name;
} cypher_create_path;

#define CYPHER_CLAUSE_FLAG_NONE 0x0000
#define CYPHER_CLAUSE_FLAG_TERMINAL 0x0001
#define CYPHER_CLAUSE_FLAG_PREVIOUS_CLAUSE 0x0002

#define CYPHER_CLAUSE_IS_TERMINAL(flags) \
    (flags & CYPHER_CLAUSE_FLAG_TERMINAL)

#define CYPHER_CLAUSE_HAS_PREVIOUS_CLAUSE(flags) \
    (flags & CYPHER_CLAUSE_FLAG_PREVIOUS_CLAUSE)

/*
 * Structure that contains all information to create
 * a new entity in the create clause, or where to access
 * this information if it doesn't need to be created.
 *
 * NOTE: This structure may be used for the MERGE clause as
 * well
 */
typedef struct cypher_target_node
{
    ExtensibleNode extensible;
    // 'v' for vertex or 'e' for edge
    char type;
    // flags defined below, prefaced with CYPHER_TARGET_NODE_FLAG_*
    uint32 flags;
    // if an edge, denotes direction
    cypher_rel_dir dir;
    /*
     * Used to create the id for the vertex/edge,
     * if the CYPHER_TARGET_NODE_FLAG_INSERT flag
     * is set. Doing it this way will protect us when
     * rescan gets implemented. By calling the function
     * that creates the id ourselves, we won't have an
     * issue where the id could be created then not used.
     * Since there is a limited number of ids available, we
     * don't want to waste them.
     */
    Expr *id_expr;
    ExprState *id_expr_state;

    Expr *prop_expr;
    ExprState *prop_expr_state;
    /*
     * Attribute Number that this entity's properties
     * are stored in the CustomScanState's child TupleTableSlot
     */
    AttrNumber prop_attr_num;
    // RelInfo for the table this entity will be stored in
    ResultRelInfo *resultRelInfo;
    // elemTupleSlot used to insert the entity into its table
    TupleTableSlot *elemTupleSlot;
    // relid that the label stores its entity
    Oid relid;
    // label this entity belongs to.
    char *label_name;
    // variable name for this entity
    char *variable_name;
    /*
     * Attribute number this entity needs to be stored in
     * for parent execution nodes to reference it. If the
     * entity is a varaible (CYPHER_TARGET_NODE_IS_VAR).
     */
    AttrNumber tuple_position;
} cypher_target_node;

#define CYPHER_TARGET_NODE_FLAG_NONE 0x0000
// node must insert data
#define CYPHER_TARGET_NODE_FLAG_INSERT 0x0001
/*
 * Flag that denotes if this target node is referencing
 * a variable that was already created AND created in the
 * same clause.
 */
#define EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE 0x0002

//node is the first instance of a declared variable
#define CYPHER_TARGET_NODE_IS_VAR 0x0004
// node is an element in a path variable
#define CYPHER_TARGET_NODE_IN_PATH_VAR 0x0008

#define CYPHER_TARGET_NODE_MERGE_EXISTS 0x0010

#define CYPHER_TARGET_NODE_OUTPUT(flags) \
    (flags & (CYPHER_TARGET_NODE_IS_VAR | CYPHER_TARGET_NODE_IN_PATH_VAR))

#define CYPHER_TARGET_NODE_IN_PATH(flags) \
    (flags & CYPHER_TARGET_NODE_IN_PATH_VAR)

#define CYPHER_TARGET_NODE_IS_VARIABLE(flags) \
    (flags & CYPHER_TARGET_NODE_IS_VAR)

/*
 * When a vertex is created and is reference in the same clause
 * later. We don't need to check to see if the vertex still exists.
 */
#define SAFE_TO_SKIP_EXISTENCE_CHECK(flags) \
    (flags & EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE)

#define CYPHER_TARGET_NODE_INSERT_ENTITY(flags) \
    (flags & CYPHER_TARGET_NODE_FLAG_INSERT)

#define UPDATE_CLAUSE_SET "SET"
#define UPDATE_CLAUSE_REMOVE "REMOVE"

/* Data Structures that contain information about a vertices and edges the need to be updated */
typedef struct cypher_update_information
{
    ExtensibleNode extensible;
    List *set_items;
    int flags;
    AttrNumber tuple_position;
    char *graph_name;
    Oid graph_oid;
    char *clause_name;
} cypher_update_information;

typedef struct cypher_update_item
{
    ExtensibleNode extensible;
    AttrNumber prop_position;
    AttrNumber entity_position;
    char *var_name;
    char *prop_name;
    List *qualified_name;
    bool remove_item;
} cypher_update_item;

typedef struct cypher_delete_information
{
    ExtensibleNode extensible;
    List *delete_items;
    int flags;
    char *graph_name;
    uint32 graph_oid;
    bool detach;
} cypher_delete_information;

typedef struct cypher_delete_item
{
    ExtensibleNode extensible;
    Value *entity_position;
    char *var_name;
} cypher_delete_item;

typedef struct cypher_merge_information
{
    ExtensibleNode extensible;
    int flags;
    uint32 graph_oid;
    AttrNumber merge_function_attr;
    cypher_create_path *path;
} cypher_merge_information;

/* grammar node for typecasts */
typedef struct cypher_typecast
{
    ExtensibleNode extensible;
    Node *expr;
    char *typecast;
    int location;
} cypher_typecast;

typedef struct cypher_label_tree_node
{
    ExtensibleNode extensible;
    Node *larg;
    Node *rarg;
    char *label_name;
} cypher_label_tree_node;

#endif
