%{
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
 * For PostgreSQL Database Management System:
 * (formerly known as Postgres, then as Postgres95)
 *
 * Portions Copyright (c) 2020-2023, Apache Software Foundation
 * Portions Copyright (c) 2019-2020, Bitnine Global
 */

#include "postgraph.h"

#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "parser/parser.h"
#include "nodes/primnodes.h"

#include "nodes/ag_nodes.h"
#include "nodes/cypher_nodes.h"
#include "parser/ag_scanner.h"
#include "parser/cypher_gram.h"

// override the default action for locations
#define YYLLOC_DEFAULT(current, rhs, n) \
    do \
    { \
        if ((n) > 0) \
            current = (rhs)[1]; \
        else \
            current = -1; \
    } while (0)

#define YYMALLOC palloc
#define YYFREE pfree
%}

%locations
%name-prefix="cypher_yy"
%pure-parser

%lex-param {ag_scanner_t scanner}
%parse-param {ag_scanner_t scanner}
%parse-param {cypher_yy_extra *extra}

%union {
    /* types in cypher_yylex() */
    int integer;
    char *string;
    const char *keyword;

    /* extra types */
    bool boolean;
    Node *node;
    List *list;
}

%token <integer> INTEGER
%token <string> DECIMAL STRING

%token <string> IDENTIFIER
%token <string> INET
%token <string> PARAMETER
%token <string> OPERATOR

/* operators that have more than 1 character */
%token NOT_EQ LT_EQ GT_EQ DOT_DOT TYPECAST PLUS_EQ

/* keywords in alphabetical order */
%token <keyword> ALL AND AS ASC ASCENDING
                 BY
                 CALL CASE COALESCE CONTAINS CREATE CUBE CURRENT_DATE CURRENT_TIME CURRENT_TIMESTAMP
                 DATE DECADE DELETE DESC DESCENDING DETACH DISTINCT
                 ELSE END_P ENDS EXCEPT EXISTS EXTRACT
                 GROUP GROUPING
                 FALSE_P FROM
                 IN INTERSECT INTERVAL IS
                 LIKE LIMIT LOCALTIME LOCALTIMESTAMP
                 MATCH MERGE 
                 NOT NULL_P
                 OPTIONAL OR ORDER OVERLAPS
                 REMOVE RETURN ROLLUP
                 SET SETS SKIP STARTS
                 TIME THEN TIMESTAMP TRUE_P
                 UNION UNWIND
                 WHEN WHERE WITH WITHOUT
                 XOR
                 YIELD
                 ZONE

/* query */
%type <node> stmt
%type <list> single_query query_part_init query_part_last cypher_stmt
             reading_clause_list updating_clause_list_0 updating_clause_list_1
%type <node> reading_clause updating_clause

/* RETURN and WITH clause */
%type <node> empty_grouping_set cube_clause rollup_clause group_item return return_item sort_item skip_opt limit_opt with
%type <list> group_item_list return_item_list order_by_opt sort_item_list group_by_opt
%type <integer> order_opt 

/* MATCH clause */
%type <node> match cypher_varlen_opt cypher_range_opt cypher_range_idx
             cypher_range_idx_opt
%type <integer> Iconst
%type <boolean> optional_opt

/* CREATE clause */
%type <node> create

/* UNWIND clause */
%type <node> unwind

/* SET and REMOVE clause */
%type <node> set set_item remove remove_item
%type <list> set_item_list remove_item_list

/* DELETE clause */
%type <node> delete
%type <boolean> detach_opt

/* MERGE clause */
%type <node> merge

/* CALL ... YIELD clause */
%type <node> call_stmt yield_item
%type <list> yield_item_list

/* common */
%type <node> where_opt

/* pattern */
%type <list> pattern simple_path_opt_parens simple_path
%type <node> path anonymous_path
             path_node path_relationship path_relationship_body
             properties_opt
%type <string> label_opt

/* expression */
%type <node> expr expr_opt expr_atom expr_literal map list

%type <node> expr_case expr_case_when expr_case_default
%type <list> expr_case_when_list

%type <node> expr_var expr_func expr_func_norm expr_func_subexpr
%type <list> expr_list expr_list_opt map_keyval_list_opt map_keyval_list

/* names */
%type <string> property_key_name var_name var_name_opt label_name
%type <string> symbolic_name schema_name temporal_cast
%type <keyword> reserved_keyword safe_keywords conflicted_keywords
%type <list> func_name

/*set operations*/
%type <boolean> all_or_distinct

/* precedence: lowest to highest */
%left UNION INTERSECT EXCEPT
%left OR
%left AND
%left XOR
%right NOT
%left '=' NOT_EQ '<' LT_EQ '>' GT_EQ '~' '!'
%left OPERATOR
%left '+' '-'
%left '*' '/' '%'
%left '^' '&' '|'
%nonassoc IN IS
%right UNARY_MINUS
%nonassoc CONTAINS ENDS EQ_TILDE STARTS LIKE
%left '[' ']' '(' ')'
%left '.'
%left TYPECAST

%{
// logical operators
static Node *make_or_expr(Node *lexpr, Node *rexpr, int location);
static Node *make_and_expr(Node *lexpr, Node *rexpr, int location);
static Node *make_xor_expr(Node *lexpr, Node *rexpr, int location);
static Node *make_not_expr(Node *expr, int location);

// arithmetic operators
static Node *do_negate(Node *n, int location);
static void do_negate_float(Value *v);

// indirection
static Node *append_indirection(Node *expr, Node *selector);

// literals
static Node *make_int_const(int i, int location);
static Node *make_float_const(char *s, int location);
static Node *make_string_const(char *s, int location);
static Node *make_bool_const(bool b, int location);
static Node *make_inet_const(char *s, int location);
static Node *make_null_const(int location);

// typecast
static Node *make_typecast_expr(Node *expr, char *typecast, int location);

// sql value
static Node *makeSQLValueFunction(SQLValueFunctionOp op, int32 typmod, int location);
// setops
static Node *make_set_op(SetOperation op, bool all_or_distinct, List *larg, List *rarg);

// comparison
static bool is_A_Expr_a_comparison_operation(A_Expr *a);
%}
%%

/*
 * query
 */

stmt:
    cypher_stmt semicolon_opt
        {
            /*
             * If there is no transition for the lookahead token and the
             * clauses can be reduced to single_query, the parsing is
             * considered successful although it actually isn't.
             *
             * For example, when `MATCH ... CREATE ... MATCH ... ;` query is
             * being parsed, there is no transition for the second `MATCH ...`
             * because the query is wrong but `MATCH .. CREATE ...` is correct
             * so it will be reduced to query_part_last anyway even if there
             * are more tokens to read.
             *
             * Throw syntax error in this case.
             */
            if (yychar != YYEOF)
                yyerror(&yylloc, scanner, extra, "syntax error");

            extra->result = $1;
        }
    ;

cypher_stmt:
    single_query
        {
            $$ = $1;
        }
    | '(' cypher_stmt ')'
        {
            $$ = $2;
        }
    | cypher_stmt UNION all_or_distinct cypher_stmt
        {
            $$ = list_make1(make_set_op(SETOP_UNION, $3, $1, $4));
        }
    | cypher_stmt INTERSECT all_or_distinct cypher_stmt
        {
            $$ = list_make1(make_set_op(SETOP_INTERSECT, $3, $1, $4));
        }
    | cypher_stmt EXCEPT all_or_distinct cypher_stmt
        {
            $$ = list_make1(make_set_op(SETOP_EXCEPT, $3, $1, $4));
        }
    ;

call_stmt:
    CALL expr_func_norm AS var_name where_opt
        {
            cypher_call *call = make_ag_node(cypher_call);
            call->cck = CCK_FUNCTION;
            call->func = $2;
            call->yield_list = NIL;
            call->alias = $4;
            call->where = $5;
            call->cypher = NIL;
            call->query_tree = NULL;
            $$ = call;
       }
    | CALL expr_func_norm YIELD yield_item_list where_opt
        {
            cypher_call *call = make_ag_node(cypher_call);
            call->cck = CCK_FUNCTION;
            call->func = $2;
            call->yield_list = $3;
            call->alias = $4;
            call->where = $5;
            call->cypher = NIL;
            call->query_tree = NULL;
            $$ = call;

            ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("CALL... [YIELD] not supported yet"),
                     ag_scanner_errposition(@1, scanner)));
        }
    | CALL '{' cypher_stmt '}'
        {
            cypher_call *call = make_ag_node(cypher_call);
            call->cck = CCK_CYPHER_SUBQUERY;
            call->func = NULL;
            call->yield_list = NIL;
            call->alias = NULL;
            call->where = NULL;
            call->cypher = $3;
            call->query_tree = NULL;
            $$ = call;
        }
    ;

yield_item_list:
    yield_item
        {
            $$ = list_make1($1);
        }
    | yield_item_list ',' yield_item
        {
            $$ = lappend($1, $3);
        }
    ;

yield_item:
    expr AS var_name
        {
            ResTarget *n;

            n = makeNode(ResTarget);
            n->name = $3;
            n->indirection = NIL;
            n->val = $1;
            n->location = @1;

            $$ = (Node *)n;
        }
    | expr
        {
            ResTarget *n;

            n = makeNode(ResTarget);
            n->name = NULL;
            n->indirection = NIL;
            n->val = $1;
            n->location = @1;

            $$ = (Node *)n;
        }
    | '*'
        {
            ColumnRef *cr;
            ResTarget *rt;

            cr = makeNode(ColumnRef);
            cr->fields = list_make1(makeNode(A_Star));
            cr->location = @1;

            rt = makeNode(ResTarget);
            rt->name = NULL;
            rt->indirection = NIL;
            rt->val = (Node *)cr;
            rt->location = @1;

            $$ = (Node *)rt;
        }
    ;


semicolon_opt:
    /* empty */
    | ';'
    ;

all_or_distinct:
    ALL
    {
        $$ = true;
    }
    | DISTINCT
    {
        $$ = false;
    }
    | /*EMPTY*/
    {
        $$ = false;
    }

/*
 * The overall structure of single_query looks like below.
 *
 * ( reading_clause* updating_clause* with )*
 * reading_clause* ( updating_clause+ | updating_clause* return )
 */
single_query:
    query_part_init query_part_last
        {
            $$ = list_concat($1, $2);
        }
    ;

query_part_init:
    /* empty */
        {
            $$ = NIL;
        }
    | query_part_init reading_clause_list updating_clause_list_0 with
        {
            $$ = lappend(list_concat(list_concat($1, $2), $3), $4);
        }
    ;

query_part_last:
    reading_clause_list updating_clause_list_1
        {
            $$ = list_concat($1, $2);
        }
    | reading_clause_list updating_clause_list_0 return
        {
            $$ = lappend(list_concat($1, $2), $3);
        }
    ;

reading_clause_list:
    /* empty */
        {
            $$ = NIL;
        }
    | reading_clause_list reading_clause
        {
            $$ = lappend($1, $2);
        }
    ;

reading_clause:
    match
    | unwind
    ;

updating_clause_list_0:
    /* empty */
        {
            $$ = NIL;
        }
    | updating_clause_list_1
    ;

updating_clause_list_1:
    updating_clause
        {
            $$ = list_make1($1);
        }
    | updating_clause_list_1 updating_clause
        {
            $$ = lappend($1, $2);
        }
    ;

updating_clause:
    create
    | set
    | remove
    | delete
    | merge
    | call_stmt
    ;

cypher_varlen_opt:
    '*' cypher_range_opt
        {
            A_Indices *n = (A_Indices *) $2;

            if (n->lidx == NULL)
                n->lidx = make_int_const(1, @2);

            if (n->uidx != NULL)
            {
                A_Const    *lidx = (A_Const *) n->lidx;
                A_Const    *uidx = (A_Const *) n->uidx;

                if (lidx->val.val.ival > uidx->val.val.ival)
                    ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                                    errmsg("invalid range"),
                                    ag_scanner_errposition(@2, scanner)));
            }
            $$ = (Node *) n;
        }
    | /* EMPTY */
        {
            $$ = NULL;
        }
    ;

cypher_range_opt:
    cypher_range_idx
        {
            A_Indices  *n;

            n = makeNode(A_Indices);
            n->lidx = copyObject($1);
            n->uidx = $1;
            $$ = (Node *) n;
        }
    | cypher_range_idx_opt DOT_DOT cypher_range_idx_opt
        {
            A_Indices  *n;

            n = makeNode(A_Indices);
            n->lidx = $1;
            n->uidx = $3;
            $$ = (Node *) n;
        }
    | /* EMPTY */
        {
            $$ = (Node *) makeNode(A_Indices);
        }
    ;

cypher_range_idx:
    Iconst
        {
            $$ = make_int_const($1, @1);
        }
    ;

cypher_range_idx_opt:
                        cypher_range_idx
                        | /* EMPTY */                   { $$ = NULL; }
                ;

Iconst: INTEGER

/*
 * RETURN and WITH clause
 */
group_by_opt:
    /* empty */
        {
            $$ = NIL;
        }
    | GROUP BY group_item_list
        {
            $$ = $3;
        }

group_item_list:
	group_item { $$ = list_make1($1); }
        | group_item_list ',' group_item { $$ = lappend($1, $3); }
;

group_item:
	expr { $$ = $1; }
        | cube_clause { $$ = $1; }
        | rollup_clause { $$ = $1; }
        | empty_grouping_set { $$ = $1; }
;

rollup_clause:
    ROLLUP '(' expr_list ')'
        {
            $$ = (Node *) makeGroupingSet(GROUPING_SET_ROLLUP, $3, @1);
        }
;

cube_clause:
     CUBE '(' expr_list ')'
     {
         $$ = (Node *) makeGroupingSet(GROUPING_SET_CUBE, $3, @1);
     }
;

empty_grouping_set:
    '(' ')'
    {
        $$ = (Node *) makeGroupingSet(GROUPING_SET_EMPTY, NIL, @1);
    }
;


return:
    RETURN DISTINCT return_item_list group_by_opt order_by_opt skip_opt limit_opt
        {
            cypher_return *n;

            n = make_ag_node(cypher_return);
            n->distinct = true;
            n->items = $3;
            n->real_group_clause = $4;
            n->order_by = $5;
            n->skip = $6;
            n->limit = $7;
            n->where = NULL;

            $$ = (Node *)n;
        }
    | RETURN return_item_list group_by_opt order_by_opt skip_opt limit_opt
        {
            cypher_return *n;

            n = make_ag_node(cypher_return);
            n->distinct = false;
            n->items = $2;
            n->real_group_clause = $3;
            n->order_by = $4;
            n->skip = $5;
            n->limit = $6;
            n->where = NULL;

            $$ = (Node *)n;
        }

    ;

return_item_list:
    return_item
        {
            $$ = list_make1($1);
        }
    | return_item_list ',' return_item
        {
            $$ = lappend($1, $3);
        }
    ;

return_item:
    expr AS var_name
        {
            ResTarget *n;

            n = makeNode(ResTarget);
            n->name = $3;
            n->indirection = NIL;
            n->val = $1;
            n->location = @1;

            $$ = (Node *)n;
        }
    | expr
        {
            ResTarget *n;

            n = makeNode(ResTarget);
            n->name = NULL;
            n->indirection = NIL;
            n->val = $1;
            n->location = @1;

            $$ = (Node *)n;
        }
    | '*'
        {
            ColumnRef *cr;
            ResTarget *rt;

            cr = makeNode(ColumnRef);
            cr->fields = list_make1(makeNode(A_Star));
            cr->location = @1;

            rt = makeNode(ResTarget);
            rt->name = NULL;
            rt->indirection = NIL;
            rt->val = (Node *)cr;
            rt->location = @1;

            $$ = (Node *)rt;
        }
    ;

order_by_opt:
    /* empty */
        {
            $$ = NIL;
        }
    | ORDER BY sort_item_list
        {
            $$ = $3;
        }
    ;

sort_item_list:
    sort_item
        {
            $$ = list_make1($1);
        }
    | sort_item_list ',' sort_item
        {
            $$ = lappend($1, $3);
        }
    ;

sort_item:
    expr order_opt
        {
            SortBy *n;

            n = makeNode(SortBy);
            n->node = $1;
            n->sortby_dir = $2;
            n->sortby_nulls = SORTBY_NULLS_DEFAULT;
            n->useOp = NIL;
            n->location = -1; // no operator

            $$ = (Node *)n;
        }
    ;

order_opt:
    /* empty */
        {
            $$ = SORTBY_DEFAULT;
        }
    | ASC
        {
            $$ = SORTBY_ASC;
        }
    | ASCENDING
        {
            $$ = SORTBY_ASC;
        }
    | DESC
        {
            $$ = SORTBY_DESC;
        }
    | DESCENDING
        {
            $$ = SORTBY_DESC;
        }
    ;

skip_opt:
    /* empty */
        {
            $$ = NULL;
        }
    | SKIP expr
        {
            $$ = $2;
        }
    ;

limit_opt:
    /* empty */
        {
            $$ = NULL;
        }
    | LIMIT expr
        {
            $$ = $2;
        }
    ;

with:
    WITH DISTINCT return_item_list where_opt group_by_opt order_by_opt skip_opt limit_opt
        {
            ListCell *li;
            cypher_with *n;

            // check expressions are aliased
            foreach (li, $3)
            {
                ResTarget *item = lfirst(li);

                // variable does not have to be aliased
                if (IsA(item->val, ColumnRef) || item->name)
                    continue;

                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("expression item must be aliased"),
                         errhint("Items can be aliased by using AS."),
                         ag_scanner_errposition(item->location, scanner)));
            }

            n = make_ag_node(cypher_with);
            n->distinct = true;
            n->items = $3;
            n->real_group_clause = $5;
            n->order_by = $6;
            n->skip = $7;
            n->limit = $8;
            n->where = $4;

            $$ = (Node *)n;
        }
    | WITH return_item_list where_opt group_by_opt order_by_opt skip_opt limit_opt
        {
            ListCell *li;
            cypher_with *n;

            // check expressions are aliased
            foreach (li, $2)
            {
                ResTarget *item = lfirst(li);

                // variable does not have to be aliased
                if (IsA(item->val, ColumnRef) || item->name)
                    continue;

                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("expression item must be aliased"),
                         errhint("Items can be aliased by using AS."),
                         ag_scanner_errposition(item->location, scanner)));
            }

            n = make_ag_node(cypher_with);
            n->distinct = false;
            n->items = $2;
            n->real_group_clause = $4;
            n->order_by = $5;
            n->skip = $6;
            n->limit = $7;
            n->where = $3;

            $$ = (Node *)n;
        }
    ;

/*
 * MATCH clause
 */

match:
    optional_opt MATCH pattern where_opt order_by_opt
        {
            cypher_match *n;

            n = make_ag_node(cypher_match);
            n->optional = $1;
            n->pattern = $3;
            n->where = $4;
            n->order_by = $5;

            $$ = (Node *)n;
        }
    ;

optional_opt:
    OPTIONAL
        {
            $$ = true;
        }
    | /* EMPTY */
        {
            $$ = false;
        }
    ;


unwind:
    UNWIND expr AS var_name
        {
            ResTarget  *res;
            cypher_unwind *n;

            res = makeNode(ResTarget);
            res->name = $4;
            res->val = (Node *) $2;
            res->location = @2;

            n = make_ag_node(cypher_unwind);
            n->target = res;
            $$ = (Node *) n;
        }

/*
 * CREATE clause
 */

create:
    CREATE pattern
        {
            cypher_create *n;

            n = make_ag_node(cypher_create);
            n->pattern = $2;

            $$ = (Node *)n;
        }
    ;

/*
 * SET and REMOVE clause
 */

set:
    SET set_item_list
        {
            cypher_set *n;

            n = make_ag_node(cypher_set);
            n->items = $2;
            n->is_remove = false;
            n->location = @1;

            $$ = (Node *)n;
        }
    ;

set_item_list:
    set_item
        {
            $$ = list_make1($1);
        }
    | set_item_list ',' set_item
        {
            $$ = lappend($1, $3);
        }
    ;

set_item:
    expr '=' expr
        {
            cypher_set_item *n;

            n = make_ag_node(cypher_set_item);
            n->prop = $1;
            n->expr = $3;
            n->is_add = false;
            n->location = @1;

            $$ = (Node *)n;
        }
   | expr PLUS_EQ expr
        {
            cypher_set_item *n;

            n = make_ag_node(cypher_set_item);
            n->prop = $1;
            n->expr = $3;
            n->is_add = true;
            n->location = @1;

            $$ = (Node *)n;
        }
    ;

remove:
    REMOVE remove_item_list
        {
            cypher_set *n;

            n = make_ag_node(cypher_set);
            n->items = $2;
            n->is_remove = true;
             n->location = @1;

            $$ = (Node *)n;
        }
    ;

remove_item_list:
    remove_item
        {
            $$ = list_make1($1);
        }
    | remove_item_list ',' remove_item
        {
            $$ = lappend($1, $3);
        }
    ;

remove_item:
    expr
        {
            cypher_set_item *n;

            n = make_ag_node(cypher_set_item);
            n->prop = $1;
            n->expr = make_null_const(-1);
            n->is_add = false;

            $$ = (Node *)n;
        }
    ;

/*
 * DELETE clause
 */

delete:
    detach_opt DELETE expr_list
        {
            cypher_delete *n;

            n = make_ag_node(cypher_delete);
            n->detach = $1;
            n->exprs = $3;
            n->location = @1;

            $$ = (Node *)n;
        }
    ;

detach_opt:
    DETACH
        {
            $$ = true;
        }
    | /* EMPTY */
        {
            $$ = false;
        }
    ;

/*
 * MERGE clause
 */
merge:
    MERGE path
        {
            cypher_merge *n;

            n = make_ag_node(cypher_merge);
            n->path = $2;

            $$ = (Node *)n;
        }
    ;

/*
 * common
 */

where_opt:
    /* empty */
        {
            $$ = NULL;
        }
    | WHERE expr
        {
            $$ = $2;
        }
    ;

/*
 * pattern
 */

/* pattern is a set of one or more paths */
pattern:
    path
        {
            $$ = list_make1($1);
        }
    | pattern ',' path
        {
            $$ = lappend($1, $3);
        }
    ;

/* path is a series of connected nodes and relationships */
path:
    anonymous_path
    | var_name '=' anonymous_path /* named path */
        {
            cypher_path *p;

            p = (cypher_path *)$3;
            p->var_name = $1;

            $$ = (Node *)p;
        }

    ;

anonymous_path:
    simple_path_opt_parens
        {
            cypher_path *n;

            n = make_ag_node(cypher_path);
            n->path = $1;
            n->var_name = NULL;
            n->location = @1;

            $$ = (Node *)n;
        }
    ;

simple_path_opt_parens:
    simple_path
    | '(' simple_path ')'
        {
            $$ = $2;
        }
    ;

simple_path:
    path_node
        {
            $$ = list_make1($1);
        }
    | simple_path path_relationship path_node
        {
            $$ = lappend(lappend($1, $2), $3);
        }
    ;

path_node:
    '(' var_name_opt label_opt properties_opt ')'
        {
            cypher_node *n;

            n = make_ag_node(cypher_node);
            n->name = $2;
            n->label = $3;
            n->props = $4;
            n->location = @1;

            $$ = (Node *)n;
        }
    ;

path_relationship:
    '-' path_relationship_body '-'
        {
            cypher_relationship *n = (cypher_relationship *)$2;

            n->dir = CYPHER_REL_DIR_NONE;
            n->location = @2;

            $$ = $2;
        }
    | '-' path_relationship_body '-' '>'
        {
            cypher_relationship *n = (cypher_relationship *)$2;

            n->dir = CYPHER_REL_DIR_RIGHT;
            n->location = @2;

            $$ = $2;
        }
    | '<' '-' path_relationship_body '-'
        {
            cypher_relationship *n = (cypher_relationship *)$3;

            n->dir = CYPHER_REL_DIR_LEFT;
            n->location = @3;

            $$ = $3;
        }
    ;

path_relationship_body:
    '[' var_name_opt label_opt cypher_varlen_opt properties_opt ']'
        {
            cypher_relationship *n;

            n = make_ag_node(cypher_relationship);
            n->name = $2;
            n->label = $3;
            n->varlen = $4;
            n->props = $5;

            $$ = (Node *)n;
        }
    |
    /* empty */
        {
            cypher_relationship *n;

            n = make_ag_node(cypher_relationship);
            n->name = NULL;
            n->label = NULL;
            n->varlen = NULL;
            n->props = NULL;

            $$ = (Node *)n;
        }
    ;

label_opt:
    /* empty */
        {
            $$ = NULL;
        }
    | ':' label_name
        {
            $$ = $2;
        }
    ;

properties_opt:
    /* empty */
        {
            $$ = NULL;
        }
    | map
    | PARAMETER
        {
            cypher_param *n;

            n = make_ag_node(cypher_param);
            n->name = $1;
            n->location = @1;

            $$ = (Node *)n;
        }

    ;

/*
 * expression
 */
expr:
    expr OR expr
        {
            $$ = make_or_expr($1, $3, @2);
        }
    | expr AND expr
        {
            $$ = make_and_expr($1, $3, @2);
        }
    | expr XOR expr
        {
            $$ = make_xor_expr($1, $3, @2);
        }
    | NOT expr
        {
            $$ = make_not_expr($2, @1);
        }
    | expr '=' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "=", $1, $3, @2);
        }
    | expr LIKE expr
        {   
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "~~", $1, $3, @2);
        }  
    | expr NOT_EQ expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "<>", $1, $3, @2);
        }
    | expr '<' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "<", $1, $3, @2);
        }
    | expr LT_EQ expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "<=", $1, $3, @2);
        }
    | expr '>' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, ">", $1, $3, @2);
        }
    | expr GT_EQ expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, ">=", $1, $3, @2);
        }
    | expr '+' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "+", $1, $3, @2);
        }
    | expr '-' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "-", $1, $3, @2);
        }
    | expr '*' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "*", $1, $3, @2);
        }
    | expr '/' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "/", $1, $3, @2);
        }
    | expr '%' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "%", $1, $3, @2);
        }
    | expr '^' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "^", $1, $3, @2);
        }
    | expr OPERATOR expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, $2, $1, $3, @2);
        }
    | OPERATOR expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, $1, NULL, $2, @1);
        }
    | expr '<' '-' '>' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "<->", $1, $5, @3);
        }
    | expr IN expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "@=", $1, $3, @2);
        }
    | expr IS NULL_P %prec IS
        {
            NullTest *n;

            n = makeNode(NullTest);
            n->arg = (Expr *)$1;
            n->nulltesttype = IS_NULL;
            n->location = @2;

            $$ = (Node *)n;
        }
    | expr IS NOT NULL_P %prec IS
        {
            NullTest *n;

            n = makeNode(NullTest);
            n->arg = (Expr *)$1;
            n->nulltesttype = IS_NOT_NULL;
            n->location = @2;

            $$ = (Node *)n;
        }
    | '-' expr %prec UNARY_MINUS
        {
            $$ = do_negate($2, @1);
        
        }
    | '~' expr %prec UNARY_MINUS
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "~", NULL, $2, @1);
        }
    | expr STARTS WITH expr %prec STARTS
        {
            cypher_string_match *n;

            n = make_ag_node(cypher_string_match);
            n->operation = CSMO_STARTS_WITH;
            n->lhs = $1;
            n->rhs = $4;
            n->location = @2;

            $$ = (Node *)n;
        }
    | expr ENDS WITH expr %prec ENDS
        {
            cypher_string_match *n;

            n = make_ag_node(cypher_string_match);
            n->operation = CSMO_ENDS_WITH;
            n->lhs = $1;
            n->rhs = $4;
            n->location = @2;

            $$ = (Node *)n;
        }
    | expr CONTAINS expr
        {
            cypher_string_match *n;

            n = make_ag_node(cypher_string_match);
            n->operation = CSMO_CONTAINS;
            n->lhs = $1;
            n->rhs = $3;
            n->location = @2;

            $$ = (Node *)n;
        }
    | expr '~' expr
        {
            $$ = (Node *)makeSimpleA_Expr(AEXPR_OP, "~", $1, $3, @2);
        }
    | expr '[' expr ']'  %prec '.'
        {
            A_Indices *i;

            i = makeNode(A_Indices);
            i->is_slice = false;
            i->lidx = NULL;
            i->uidx = $3;

            $$ = append_indirection($1, (Node *)i);
        }
    | expr '[' expr_opt DOT_DOT expr_opt ']'
        {
            A_Indices *i;

            i = makeNode(A_Indices);
            i->is_slice = true;
            i->lidx = $3;
            i->uidx = $5;

            $$ = append_indirection($1, (Node *)i);
        }
    | expr '.' expr
        {
            $$ = append_indirection($1, $3);
        }
    | expr TYPECAST schema_name
        {
            $$ = make_typecast_expr($1, $3, @2);
        }
    | temporal_cast expr %prec TYPECAST
        {
            $$ = make_typecast_expr($2, $1, @1);
        }
    | expr_atom
    ;

expr_opt:
    /* empty */
        {
            $$ = NULL;
        }
    | expr
    ;

expr_list:
    expr
        {
            $$ = list_make1($1);
        }
    | expr_list ',' expr
        {
            $$ = lappend($1, $3);
        }
    ;

expr_list_opt:
    /* empty */
        {
            $$ = NIL;
        }
    | expr_list
    ;

expr_func:
    expr_func_norm
    | expr_func_subexpr
    ;

expr_func_norm:
    func_name '(' ')'
        {
            $$ = (Node *)makeFuncCall($1, NIL, COERCE_SQL_SYNTAX, @1);
        }
    | func_name '(' expr_list ')'
        {
            $$ = (Node *)makeFuncCall($1, $3, COERCE_SQL_SYNTAX, @1);
        }
    /* borrowed from PG's grammar */
    | func_name '(' '*' ')'
        {
            /*
             * We consider AGGREGATE(*) to invoke a parameterless
             * aggregate.  This does the right thing for COUNT(*),
             * and there are no other aggregates in SQL that accept
             * '*' as parameter.
             *
             * The FuncCall node is also marked agg_star = true,
             * so that later processing can detect what the argument
             * really was.
             */
             FuncCall *n = (FuncCall *)makeFuncCall($1, NIL, COERCE_SQL_SYNTAX, @1);
             n->agg_star = true;
             $$ = (Node *)n;
         }
    | func_name '(' DISTINCT  expr_list ')'
        {
            FuncCall *n = (FuncCall *)makeFuncCall($1, $4, COERCE_SQL_SYNTAX, @1);
            n->agg_order = NIL;
            n->agg_distinct = true;
            $$ = (Node *)n;
        }
    ;

expr_func_subexpr:
    CURRENT_DATE
        {
            $$ = makeSQLValueFunction(SVFOP_CURRENT_DATE, -1, @1);
        }
    | CURRENT_TIME
        {
            $$ = makeSQLValueFunction(SVFOP_CURRENT_TIME, -1, @1);
        }
    | CURRENT_TIME '(' INTEGER ')'
        {
            $$ = makeSQLValueFunction(SVFOP_CURRENT_TIME, $3, @1);
        }
    | CURRENT_TIMESTAMP
        {
            $$ = makeSQLValueFunction(SVFOP_CURRENT_TIMESTAMP, -1, @1);
        }
    | CURRENT_TIMESTAMP '(' INTEGER ')'
        {
            $$ = makeSQLValueFunction(SVFOP_CURRENT_TIMESTAMP, $3, @1);
        }
    | COALESCE '(' expr_list ')'
        {
            CoalesceExpr *c;

            c = makeNode(CoalesceExpr);
            c->args = $3;
            c->location = @1;
            $$ = (Node *) c;
        }
    | LOCALTIME
        {
            $$ = makeSQLValueFunction(SVFOP_LOCALTIME, -1, @1);
        }
    | LOCALTIME '(' INTEGER ')'
        {
            $$ = makeSQLValueFunction(SVFOP_LOCALTIME, $3, @1);
        }
    | LOCALTIMESTAMP
        {
            $$ = makeSQLValueFunction(SVFOP_LOCALTIMESTAMP, -1, @1);
        }
    | LOCALTIMESTAMP '(' INTEGER ')'
        {
            $$ = makeSQLValueFunction(SVFOP_LOCALTIMESTAMP, $3, @1);
        }
    | EXISTS '(' anonymous_path ')'
        {
            cypher_sub_pattern *sub;
            SubLink    *n;

            sub = make_ag_node(cypher_sub_pattern);
            sub->kind = CSP_EXISTS;
            sub->pattern = list_make1($3);
            cypher_match *match = make_ag_node(cypher_match);
            match->pattern = list_make1($3);//subpat->pattern;
            match->where = NULL;
            sub->pattern = list_make1(match);
            n = makeNode(SubLink);
            n->subLinkType = EXISTS_SUBLINK;
            n->subLinkId = 0;
            n->testexpr = NULL;
            n->operName = NIL;
            n->subselect = (Node *) sub;
            n->location = @1;
            $$ = (Node *) n;
        }
    | EXISTS '(' cypher_stmt ')'
        {        
            cypher_sub_pattern *sub;
            SubLink    *n;
                 
            sub = make_ag_node(cypher_sub_pattern);
            sub->kind = CSP_EXISTS;
            sub->pattern = $3;
            
            n = makeNode(SubLink);
            n->subLinkType = EXISTS_SUBLINK;
            n->subLinkId = 0;
            n->testexpr = NULL;
            n->operName = NIL;
            n->subselect = (Node *) sub;
            n->location = @1;
            $$ = (Node *) n;
        }
    | EXTRACT '(' IDENTIFIER FROM expr ')'
        {
            $$ = (Node *)makeFuncCall(list_make1(makeString($1)),
                                      list_make2(make_string_const($3, @3), $5),
                                      COERCE_SQL_SYNTAX, @1);
        }
    | '(' expr ',' expr ')' OVERLAPS '(' expr ',' expr ')'
        {
            $$ = makeFuncCall(list_make1(makeString("overlaps")), list_make4($2, $4, $8, $10), COERCE_SQL_SYNTAX, @6);
        }
    ;

temporal_cast:
    TIMESTAMP
        {
            $$ = pnstrdup("timestamp", 9);
        }
    | TIMESTAMP WITHOUT TIME ZONE
        {
            $$ = pnstrdup("timestamp", 9);
        }
    | TIMESTAMP WITH TIME ZONE
        {
            $$ = pnstrdup("timestamptz", 11);
        }
    | DATE
        {
            $$ = pnstrdup("date", 4);
        }
    | TIME
        {
            $$ = pnstrdup("time", 4); 
        }
    | TIME WITHOUT TIME ZONE
        {
            $$ = pnstrdup("time", 4);
        }
    | TIME WITH TIME ZONE
        {
            $$ = pnstrdup("timetz", 6);
        }
    | INTERVAL
        {
            $$ = pnstrdup("interval", 8);
        }
    ;

expr_atom:
    expr_literal
    | PARAMETER
        {
            cypher_param *n;

            n = make_ag_node(cypher_param);
            n->name = $1;
            n->location = @1;

            $$ = (Node *)n;
        }
    | '(' expr ')'
        {
            $$ = $2;
        }
    | expr_case
    | expr_var
    | expr_func
    ;

expr_literal:
    INTEGER
        {
            $$ = make_int_const($1, @1);
        }
    | DECIMAL
        {
            $$ = make_float_const($1, @1);
        }
    | STRING
        {
            $$ = make_string_const($1, @1);
        }
    | TRUE_P
        {
            $$ = make_bool_const(true, @1);
        }
    | FALSE_P
        {
            $$ = make_bool_const(false, @1);
        }
    | NULL_P
        {
            $$ = make_null_const(@1);
        }
    | INET
        {
            $$ = make_inet_const($1, @1);
        }
    | map
    | list
    ;

map:
    '{' map_keyval_list_opt '}'
        {
            cypher_map *n;

            n = make_ag_node(cypher_map);
            n->keyvals = $2;

            $$ = (Node *)n;
        }
    ;

map_keyval_list_opt:
    /* empty */
        {
            $$ = NIL;
        }
    | map_keyval_list
    ;

map_keyval_list:
    property_key_name ':' expr
        {
            $$ = list_make2(makeString($1), $3);
        }
    | map_keyval_list ',' property_key_name ':' expr
        {
            $$ = lappend(lappend($1, makeString($3)), $5);
        }
    ;

list:
    '[' expr_list_opt ']'
        {
            cypher_list *n;

            n = make_ag_node(cypher_list);
            n->elems = $2;

            $$ = (Node *)n;
        }
    ;

expr_case:
    CASE expr expr_case_when_list expr_case_default END_P
        {
            CaseExpr *n;

            n = makeNode(CaseExpr);
            n->casetype = InvalidOid;
            n->arg = (Expr *) $2;
            n->args = $3;
            n->defresult = (Expr *) $4;
            n->location = @1;
            $$ = (Node *) n;
        }
    | CASE expr_case_when_list expr_case_default END_P
        {
            CaseExpr *n;

            n = makeNode(CaseExpr);
            n->casetype = InvalidOid;
            n->args = $2;
            n->defresult = (Expr *) $3;
            n->location = @1;
            $$ = (Node *) n;
        }
    ;

expr_case_when_list:
    expr_case_when
        {
            $$ = list_make1($1);
        }
    | expr_case_when_list expr_case_when
        {
            $$ = lappend($1, $2);
        }
    ;

expr_case_when:
    WHEN expr THEN expr
        {
            CaseWhen   *n;

            n = makeNode(CaseWhen);
            n->expr = (Expr *) $2;
            n->result = (Expr *) $4;
            n->location = @1;
            $$ = (Node *) n;
        }
    ;

expr_case_default:
    ELSE expr
        {
            $$ = $2;
        }
    | /* EMPTY */
        {
            $$ = NULL;
        }
    ;

expr_var:
    var_name
        {
            ColumnRef *n;

            n = makeNode(ColumnRef);
            n->fields = list_make1(makeString($1));
            n->location = @1;

            $$ = (Node *)n;
        }
    ;

/*
 * names
 */
func_name:
    symbolic_name
        {
            $$ = list_make1(makeString($1));
        }
    /*
     * symbolic_name '.' symbolic_name is already covered with the
     * rule expr '.' expr above. This rule is to allow most reserved
     * keywords to be used as well. So, it essentially makes the
     * rule schema_name '.' symbolic_name for func_name
     */
    | safe_keywords '.' symbolic_name
        {
            $$ = list_make2(makeString((char *)$1), makeString($3));
        }
    ;

property_key_name:
    schema_name
    ;

var_name:
    symbolic_name
    ;

var_name_opt:
    /* empty */
        {
            $$ = NULL;
        }
    | var_name
    ;

label_name:
    schema_name
    ;

symbolic_name:
    IDENTIFIER
    ;

schema_name:
    symbolic_name
    | reserved_keyword
        {
            /* we don't need to copy it, as it already has been */
            $$ = (char *) $1;
        }
    ;

reserved_keyword:
    safe_keywords
    | conflicted_keywords
    ;

/*
 * All keywords need to be copied and properly terminated with a null before
 * using them, pnstrdup effectively does this for us.
 */

safe_keywords:
    ALL          { $$ = pnstrdup($1, 3); }
    | AND        { $$ = pnstrdup($1, 3); }
    | AS         { $$ = pnstrdup($1, 2); }
    | ASC        { $$ = pnstrdup($1, 3); }
    | ASCENDING  { $$ = pnstrdup($1, 9); }
    | BY         { $$ = pnstrdup($1, 2); }
    | CALL       { $$ = pnstrdup($1, 4); }
    | CASE       { $$ = pnstrdup($1, 4); }
    | COALESCE   { $$ = pnstrdup($1, 8); }
    | CONTAINS   { $$ = pnstrdup($1, 8); }
    | CREATE     { $$ = pnstrdup($1, 6); }
    | DATE       { $$ = pnstrdup($1, 4); }
    | DELETE     { $$ = pnstrdup($1, 6); }
    | DESC       { $$ = pnstrdup($1, 4); }
    | DESCENDING { $$ = pnstrdup($1, 10); }
    | DETACH     { $$ = pnstrdup($1, 6); }
    | DISTINCT   { $$ = pnstrdup($1, 8); }
    | ELSE       { $$ = pnstrdup($1, 4); }
    | ENDS       { $$ = pnstrdup($1, 4); }
    | EXISTS     { $$ = pnstrdup($1, 6); }
    | INTERVAL   { $$ = pnstrdup($1, 8); }
    | IN         { $$ = pnstrdup($1, 2); }
    | IS         { $$ = pnstrdup($1, 2); }
    | LIMIT      { $$ = pnstrdup($1, 6); }
    | MATCH      { $$ = pnstrdup($1, 6); }
    | MERGE      { $$ = pnstrdup($1, 6); }
    | NOT        { $$ = pnstrdup($1, 3); }
    | OPTIONAL   { $$ = pnstrdup($1, 8); }
    | OR         { $$ = pnstrdup($1, 2); }
    | ORDER      { $$ = pnstrdup($1, 5); }
    | REMOVE     { $$ = pnstrdup($1, 6); }
    | RETURN     { $$ = pnstrdup($1, 6); }
    | SET        { $$ = pnstrdup($1, 3); }
    | SKIP       { $$ = pnstrdup($1, 4); }
    | STARTS     { $$ = pnstrdup($1, 6); }
    | TIME       { $$ = pnstrdup($1, 4); }
    | TIMESTAMP  { $$ = pnstrdup($1, 9); }
    | THEN       { $$ = pnstrdup($1, 4); }
    | UNION      { $$ = pnstrdup($1, 5); }
    | WHEN       { $$ = pnstrdup($1, 4); }
    | WHERE      { $$ = pnstrdup($1, 5); }
    | XOR        { $$ = pnstrdup($1, 3); }
    | YIELD      { $$ = pnstrdup($1, 5); }
    ;

conflicted_keywords:
    END_P     { $$ = pnstrdup($1, 5); }
    | FALSE_P { $$ = pnstrdup($1, 7); }
    | NULL_P  { $$ = pnstrdup($1, 6); }
    | TRUE_P  { $$ = pnstrdup($1, 6); }
    | WITH       { $$ = pnstrdup($1, 4); }
    ;

%%

/*
 * logical operators
 */

static Node *make_or_expr(Node *lexpr, Node *rexpr, int location)
{
    // flatten "a OR b OR c ..." to a single BoolExpr on sight
    if (IsA(lexpr, BoolExpr))
    {
        BoolExpr *bexpr = (BoolExpr *)lexpr;

        if (bexpr->boolop == OR_EXPR)
        {
            bexpr->args = lappend(bexpr->args, rexpr);

            return (Node *)bexpr;
        }
    }

    return (Node *)makeBoolExpr(OR_EXPR, list_make2(lexpr, rexpr), location);
}

static Node *make_and_expr(Node *lexpr, Node *rexpr, int location)
{
    // flatten "a AND b AND c ..." to a single BoolExpr on sight
    if (IsA(lexpr, BoolExpr))
    {
        BoolExpr *bexpr = (BoolExpr *)lexpr;

        if (bexpr->boolop == AND_EXPR)
        {
            bexpr->args = lappend(bexpr->args, rexpr);

            return (Node *)bexpr;
        }
    }

    return (Node *)makeBoolExpr(AND_EXPR, list_make2(lexpr, rexpr), location);
}

static Node *make_xor_expr(Node *lexpr, Node *rexpr, int location)
{
    Expr *aorb;
    Expr *notaandb;

    // XOR is (A OR B) AND (NOT (A AND B))
    aorb = makeBoolExpr(OR_EXPR, list_make2(lexpr, rexpr), location);

    notaandb = makeBoolExpr(AND_EXPR, list_make2(lexpr, rexpr), location);
    notaandb = makeBoolExpr(NOT_EXPR, list_make1(notaandb), location);

    return (Node *)makeBoolExpr(AND_EXPR, list_make2(aorb, notaandb), location);
}

static Node *make_not_expr(Node *expr, int location)
{
    return (Node *)makeBoolExpr(NOT_EXPR, list_make1(expr), location);
}

/*
 * arithmetic operators
 */

static Node *do_negate(Node *n, int location)
{
    if (IsA(n, A_Const))
    {
        A_Const *c = (A_Const *)n;

        // report the constant's location as that of the '-' sign
        c->location = location;

        if (c->val.type == T_Integer)
        {
            c->val.val.ival = -c->val.val.ival;
            return n;
        }
        else if (c->val.type == T_Float)
        {
            do_negate_float(&c->val);
            return n;
        }
    }

    return (Node *)makeSimpleA_Expr(AEXPR_OP, "-", NULL, n, location);
}

static void do_negate_float(Value *v)
{
    Assert(IsA(v, Float));

    if (v->val.str[0] == '-')
        v->val.str = v->val.str + 1; // just strip the '-'
    else
        v->val.str = psprintf("-%s", v->val.str);
}

/*
 * indirection
 */

static Node *append_indirection(Node *expr, Node *selector)
{
    A_Indirection *indir;

    if (IsA(expr, A_Indirection))
    {
        indir = (A_Indirection *)expr;
        indir->indirection = lappend(indir->indirection, selector);

        return expr;
    }
    else
    {
        indir = makeNode(A_Indirection);
        indir->arg = expr;
        indir->indirection = list_make1(selector);

        return (Node *)indir;
    }
}

/*
 * literals
 */

static Node *make_int_const(int i, int location)
{
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_Integer;
    n->val.val.ival = i;
    n->location = location;

    return (Node *)n;
}

static Node *make_float_const(char *s, int location)
{
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_Float;
    n->val.val.str = s;
    n->location = location;

    return (Node *)n;
}

static Node *make_string_const(char *s, int location)
{
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_String;
    n->val.val.str = s;
    n->location = location;

    return (Node *)n;
}

static Node *make_bool_const(bool b, int location)
{
    cypher_bool_const *n;

    n = make_ag_node(cypher_bool_const);
    n->boolean = b;
    n->location = location;

    return (Node *)n;
}

static Node *make_inet_const(char *s, int location)
{
    cypher_inet_const *n;

    n = make_ag_node(cypher_inet_const);
    n->inet = s;
    n->location = location;

    return (Node *)n;
}


static Node *make_null_const(int location)
{
    A_Const *n;

    n = makeNode(A_Const);
    n->val.type = T_Null;
    n->location = location;

    return (Node *)n;
}

static Node *make_typecast_expr(Node *expr, char *typecast, int location)
{
    cypher_typecast *node;

    node = make_ag_node(cypher_typecast);
    node->expr = expr;
    node->typecast = typecast;
    node->location = location;

    return (Node *)node;
}

static Node *
makeSQLValueFunction(SQLValueFunctionOp op, int32 typmod, int location)
{
        SQLValueFunction *svf = makeNode(SQLValueFunction);

        svf->op = op;
        /* svf->type will be filled during parse analysis */
        svf->typmod = typmod;
        svf->location = location;
        return (Node *) svf;
}

/*set operation function node to make a set op node*/
static Node *make_set_op(SetOperation op, bool all_or_distinct, List *larg, List *rarg)
{
    cypher_return *n = make_ag_node(cypher_return);

    n->op = op;
    n->all_or_distinct = all_or_distinct;
    n->where = NULL;
    n->larg = (List *) larg;
    n->rarg = (List *) rarg;
    return (Node *) n;
}

