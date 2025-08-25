/*
 * Copyright (C) 2023-2025 PostGraphDB
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
 * Portions Copyright (c) 1996-2010, Bitnine Global
 * Portions Copyright (c) 1996-2010, The PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 */

#include "postgraph.h"

#include "catalog/pg_type.h"
#include "catalog/pg_aggregate.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/cypher_clause.h"
#include "parser/parse_clause.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "commands/label_commands.h"
#include "nodes/cypher_nodes.h"
#include "parser/cypher_expr.h"
#include "parser/cypher_item.h"
#include "parser/cypher_parse_node.h"
#include "utils/ag_func.h"
#include "utils/gtype.h"
#include "utils/gtype_typecasting.h"

#define is_a_slice(node) \
    (IsA((node), A_Indices) && ((A_Indices *)(node))->is_slice)

static Node *transform_cypher_expr_recurse(cypher_parsestate *cpstate, Node *expr);
static Node *transform_a_const(cypher_parsestate *cpstate, A_Const *ac);
static Node *transform_column_ref(cypher_parsestate *cpstate, ColumnRef *cref);
static Node *transform_a_indirection(cypher_parsestate *cpstate, A_Indirection *a_ind);
static Node *transform_a_expr_op(cypher_parsestate *cpstate, A_Expr *a);
static Node *transform_bool_expr(cypher_parsestate *cpstate, BoolExpr *expr);
static Node *transform_cypher_bool_const(cypher_parsestate *cpstate, cypher_bool_const *bc);
static Node *transform_cypher_inet_const(cypher_parsestate *cpstate, cypher_inet_const *inet);
static Node *transform_cypher_integer_const(cypher_parsestate *cpstate, cypher_integer_const *ic);
static Node *transform_cypher_param(cypher_parsestate *cpstate, cypher_param *cp);
static Node *transform_cypher_map(cypher_parsestate *cpstate, cypher_map *cm);
static Node *transform_cypher_list(cypher_parsestate *cpstate, cypher_list *cl);
static Node *transform_cypher_string_match(cypher_parsestate *cpstate, cypher_string_match *csm_node);
static Node *transform_cypher_typecast(cypher_parsestate *cpstate, cypher_typecast *ctypecast);
static Node *transform_case_expr(cypher_parsestate *cpstate, CaseExpr *cexpr);
static Node *transform_coalesce_expr(cypher_parsestate *cpstate, CoalesceExpr *cexpr);
static Node *transform_sub_link(cypher_parsestate *cpstate, SubLink *sublink);
static Node *transform_func_call(cypher_parsestate *cpstate, FuncCall *fn);
static Node *transform_column_ref_for_indirection(cypher_parsestate *cpstate, ColumnRef *cr);
static Node *transformSQLValueFunction(cypher_parsestate *cpstate, SQLValueFunction *svf);
static List *make_qualified_function_name(cypher_parsestate *cpstate, List *lst, List *targs);

static char *make_property_alias(char *var_name) {
    char *str = palloc(strlen(var_name) + 8);

    str[0] = '_';
    str[1] = 'p';
    str[2] = 'r';
    str[3] = '_';

    int i = 0;
    for (; i < strlen(var_name); i++)
        str[i + 4] = var_name[i];

    str[i + 5] = '_';
    str[i + 6] = '_';
    str[i + 7] = '\n';

    return str;
}


Node *
transform_cypher_expr(cypher_parsestate *cpstate, Node *expr, ParseExprKind expr_kind) {
    ParseExprKind old_expr_kind = get_parse_state(cpstate)->p_expr_kind;
    get_parse_state(cpstate)->p_expr_kind = expr_kind;

    Node *result = transform_cypher_expr_recurse(cpstate, expr);

    get_parse_state(cpstate)->p_expr_kind = old_expr_kind;

    return result;
}

static Node *
transform_cypher_expr_recurse(cypher_parsestate *cpstate, Node *expr) {
    if (!expr)
        return NULL;

    check_stack_depth();

    switch (nodeTag(expr))
    {
    case T_A_Const:
        return transform_a_const(cpstate, (A_Const *)expr);
    case T_ColumnRef:
        return transform_column_ref(cpstate, (ColumnRef *)expr);
    case T_A_Indirection:
        return transform_a_indirection(cpstate, (A_Indirection *)expr);
    case T_A_Expr:
    {
        A_Expr *a = (A_Expr *)expr;

        switch (a->kind)
        {
        case AEXPR_OP:
            return transform_a_expr_op(cpstate, a);
        default:
            ereport(ERROR, (errmsg_internal("unrecognized A_Expr kind: %d", a->kind)));
        }
    }
    case T_BoolExpr:
        return transform_bool_expr(cpstate, (BoolExpr *)expr);
    case T_NullTest:
    {
        NullTest *n = (NullTest *)expr;

        n->arg = (Expr *)transform_cypher_expr_recurse(cpstate, (Node *)n->arg);
        n->argisrow = type_is_rowtype(exprType((Node *)n->arg));

        return expr;
    }
    case T_CaseExpr:
        return transform_case_expr(cpstate, (CaseExpr *) expr);
    case T_CaseTestExpr:
        return expr;
    case T_CoalesceExpr:
        return transform_coalesce_expr(cpstate, (CoalesceExpr *) expr);
    case T_ExtensibleNode:
        if (is_ag_node(expr, cypher_bool_const))
            return transform_cypher_bool_const(cpstate, (cypher_bool_const *)expr);
        if (is_ag_node(expr, cypher_inet_const))
            return transform_cypher_inet_const(cpstate, (cypher_inet_const *)expr);
        if (is_ag_node(expr, cypher_integer_const))
            return transform_cypher_integer_const(cpstate, (cypher_integer_const *)expr);
        if (is_ag_node(expr, cypher_param))
            return transform_cypher_param(cpstate, (cypher_param *)expr);
        if (is_ag_node(expr, cypher_map))
            return transform_cypher_map(cpstate, (cypher_map *)expr);
        if (is_ag_node(expr, cypher_list))
            return transform_cypher_list(cpstate, (cypher_list *)expr);
        if (is_ag_node(expr, cypher_string_match))
            return transform_cypher_string_match(cpstate, (cypher_string_match *)expr);
        if (is_ag_node(expr, cypher_typecast))
            return transform_cypher_typecast(cpstate, (cypher_typecast *)expr);

        ereport(ERROR, (errmsg_internal("unrecognized ExtensibleNode: %s",
                        ((ExtensibleNode *)expr)->extnodename)));

    case T_FuncCall:
        return transform_func_call(cpstate, (FuncCall *)expr);
    case T_SubLink:
        return transform_sub_link(cpstate, (SubLink *)expr);
    case T_SQLValueFunction:
        return transformSQLValueFunction(cpstate, (SQLValueFunction *)expr);
    case T_String:
        return makeConst(GTYPEOID, -1, InvalidOid, -1, string_to_gtype(strVal(expr)), false, false);
    case T_Var:
			{
				return (Node *) expr;
			}
    default:
        ereport(ERROR, (errmsg_internal("unrecognized node type: %d", nodeTag(expr))));
    }

    return NULL;
}

static Node *
transform_a_const(cypher_parsestate *cpstate, A_Const *ac) {
    Value *v = &ac->val;
    Datum d = (Datum)0;
    bool is_null = false;
    Const *c;

    switch (nodeTag(v))
    {
    case T_Integer:
        d = integer_to_gtype((int64)intVal(v));
        break;
    case T_Float:
        {
            char *n = strVal(v);
            int64 i;

            if (scanint8(n, true, &i)) {
                d = integer_to_gtype(i);
            } else {
                float8 f = float8in_internal(n, NULL, "double precision", n);

                d = float_to_gtype(f);
            }
        }
        break;
    case T_String:
        d = string_to_gtype(strVal(v));
        break;
    case T_Null:
        is_null = true;
        break;
    default:
        ereport(ERROR,
                (errmsg_internal("unrecognized node type: %d", nodeTag(v))));
        return NULL;
    }

    // typtypmod, typcollation, typlen, and typbyval of gtype are hard-coded.
    c = makeConst(GTYPEOID, -1, InvalidOid, -1, d, is_null, false);
    c->location = ac->location;
    return (Node *)c;
}


static Node *
transform_column_ref(cypher_parsestate *cpstate, ColumnRef *cref) {
    ParseState *pstate = (ParseState *)cpstate;
    Node *field1 = NULL;
    Node *field2 = NULL;
    char *colname = NULL;
    char *nspname = NULL;
    char *relname = NULL;
    Node *node = NULL;
    int levels_up;
    ParseNamespaceItem *pnsi;

   if (list_length(cref->fields) == 1) {
        
        field1 = (Node*)linitial(cref->fields);

        Assert(IsA(field1, String));
        colname = strVal(field1);

        node = colNameToVar(pstate, colname, false, cref->location);

        if (node != NULL)
            return node;

        {
          //  ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
         //                   errmsg("could not find rte for %s", colname),
         //                   parser_errposition(pstate, cref->location)));
        }
        //if (!node)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("variable `%s` does not exist", colname),
                    parser_errposition(pstate, cref->location)));

        return node;
   }

    Node *last_srf = pstate->p_last_srf;
       
        field1 = (Node*)linitial(cref->fields);

        Assert(IsA(field1, String));
        colname = strVal(field1);

        node = colNameToVar(pstate, colname, false, cref->location);
  
        if (!node)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("variable `%s` does not exist", colname),
                 parser_errposition(pstate, cref->location)));

    for (int i = 1; i < list_length(cref->fields); i++) {
        field2 = (Node*)list_nth(cref->fields, i);

        Node *rexpr;
        if (IsA(field2, A_Indices)) {
            A_Indices *indices = field2;

            if (indices->is_slice) {
                List *args = list_make1(node);

                // lower bound
                if (!indices->lidx)
                    args = lappend(args, makeConst(GTYPEOID, -1, InvalidOid, -1, (Datum)NULL, true, false));
                else
                    args = lappend(args, transform_cypher_expr_recurse(cpstate, indices->lidx));

                // upper bound
                if (!indices->uidx)
                    args = lappend(args, makeConst(GTYPEOID, -1, InvalidOid, -1, (Datum)NULL, true, false));
                else
                    args = lappend(args, transform_cypher_expr_recurse(cpstate, indices->uidx));

                Oid oid = get_ag_func_oid("gtype_access_slice", 3, GTYPEOID, GTYPEOID, GTYPEOID);
                FuncExpr *func_expr = makeFuncExpr(oid, GTYPEOID, args, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
                func_expr->location = exprLocation(node);

               return func_expr;
            }
            rexpr = transform_cypher_expr_recurse(cpstate, field2);
        } else if (IsA(field2, String))
            rexpr = transform_cypher_expr_recurse(cpstate, field2);
        else    
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("ColumnRef expects a String"),
                 parser_errposition(pstate, cref->location)));

        node = (Node *)make_op(pstate, list_make2(makeString("postgraph"), makeString("->")), node, rexpr, last_srf, cref->location);
    }
   

    return node;
}

/*
 * There are some operators where the result of referencing
 * the verties's or edge's properties or referencing the
 * vertex or edge itself will yield the same results, at the
 * cost of longer runtine and loss of index visibility for the
 * later way. For those operators, we wanna just reference the 
 * vertex or edge properties directly, when its possible.
 */
static bool
is_a_valid_operator(List *lst) {
    if (list_length(lst) != 1 || !IsA(linitial(lst), String))
        return false;

    char *str = strVal(linitial(lst));

    if (!strcmp(str, "?") || !strcmp(str, "?|") || !strcmp(str, "?&"))
        return true;

    return false;
}

static Node *
transform_a_expr_op(cypher_parsestate *cpstate, A_Expr *a) {
    ParseState *pstate = (ParseState *)cpstate;
    Node *lexpr = NULL;

    Node *last_srf = pstate->p_last_srf;

    if (a->lexpr != NULL && is_a_valid_operator(a->name) && IsA(a->lexpr, ColumnRef)) {
        ColumnRef *cr = a->lexpr;
        ParseNamespaceItem *pnsi;
        Node *field1 = linitial(cr->fields);
        char *relname;
        int levels_up = 0;

        field1 = linitial(cr->fields);
        Assert(IsA(field1, String));
        relname = strVal(field1);

        // locate the referenced RTE
        pnsi = refnameNamespaceItem(pstate, NULL, relname, cr->location, &levels_up);
        if (pnsi)
            lexpr = scanNSItemForColumn(pstate, pnsi, 0, "properties", cr->location);
    }

    if (lexpr == NULL)
         lexpr = transform_cypher_expr_recurse(cpstate, a->lexpr);

    Node *rexpr = transform_cypher_expr_recurse(cpstate, a->rexpr);

    return (Node *)make_op(pstate, a->name, lexpr, rexpr, last_srf, a->location);
}

static Node *
transform_bool_expr(cypher_parsestate *cpstate, BoolExpr *expr) {
    ParseState *pstate = (ParseState *)cpstate;
    List *args = NIL;
    const char *opname;

    switch (expr->boolop)
    {
    case AND_EXPR:
        opname = "AND";
        break;
    case OR_EXPR:
        opname = "OR";
        break;
    case NOT_EXPR:
        opname = "NOT";
        break;
    default:
        ereport(ERROR, (errmsg_internal("unrecognized boolop: %d",
                                        (int)expr->boolop)));
        return NULL;
    }

    ListCell *la;
    foreach (la, expr->args)
    {
        Node *arg = lfirst(la);

        arg = transform_cypher_expr_recurse(cpstate, arg);
        arg = coerce_to_boolean(pstate, arg, opname);

        args = lappend(args, arg);
    }

    return (Node *)makeBoolExpr(expr->boolop, args, expr->location);
}

static Node *
transform_cypher_bool_const(cypher_parsestate *cpstate, cypher_bool_const *b) {
    return (Node *)makeConst(GTYPEOID, -1, InvalidOid, -1, boolean_to_gtype(b->boolean), false, false);
}

static Node *
transform_cypher_inet_const(cypher_parsestate *cpstate, cypher_inet_const *inet) {
    return makeConst(GTYPEOID, -1, InvalidOid, -1, _gtype_toinet(GTYPE_P_GET_DATUM(string_to_gtype(inet->inet))), false, false);    
}

static Node *
transform_cypher_integer_const(cypher_parsestate *cpstate, cypher_integer_const *i) {
    return makeConst(GTYPEOID, -1, InvalidOid, -1, integer_to_gtype(i->integer), false, false);
}

static Node *
transform_cypher_param(cypher_parsestate *cpstate, cypher_param *p) {
    if (!cpstate->params)
        ereport( ERROR,
            (errcode(ERRCODE_UNDEFINED_PARAMETER),
             errmsg("parameters argument is missing from cypher() function call"),
             parser_errposition(cpstate, p->location)));

    return (Node *)make_op(
        cpstate, 
        list_make1(makeString("->")), 
        (Node *)cpstate->params, 
        (Node *)makeConst(GTYPEOID, -1, InvalidOid, -1, string_to_gtype(p->name), false, false), 
        get_parse_state(cpstate)->p_last_srf, 
         -1);
}

static Node *
transform_cypher_map(cypher_parsestate *cpstate, cypher_map *cm) {
    List *newkeyvals = NIL;
    ListCell *le;
    FuncExpr *fexpr;
    Oid func_oid;

    Assert(list_length(cm->keyvals) % 2 == 0);

    le = list_head(cm->keyvals);
    while (le != NULL) {
        Node *key = lfirst(le);
        le = lnext(cm->keyvals, le);

        Node *val = lfirst(le);
        le = lnext(cm->keyvals, le);

        Node *newval = transform_cypher_expr_recurse(cpstate, val);

        // typtypmod, typcollation, typlen, and typbyval of gtype are hard-coded.
        Const *newkey = makeConst(TEXTOID, -1, InvalidOid, -1, CStringGetTextDatum(strVal(key)), false, false);

        newkeyvals = lappend(lappend(newkeyvals, newkey), newval);
    }

    if (list_length(newkeyvals) == 0)
        func_oid = get_ag_func_oid("gtype_build_map", 0);
    else
        func_oid = get_ag_func_oid("gtype_build_map", 1, ANYOID);

    fexpr = makeFuncExpr(func_oid, GTYPEOID, newkeyvals, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    fexpr->location = cm->location;

    return (Node *)fexpr;
}

static Node *
transform_cypher_list(cypher_parsestate *cpstate, cypher_list *cl) {
    List *args = NIL;
    ListCell *lc;
    foreach (lc, cl->elems) {
        args = lappend(args, transform_cypher_expr_recurse(cpstate, lfirst(lc)));
    }

    return makeFuncExpr(
        list_length(args) == 0 ?
            get_ag_func_oid("gtype_build_list", 0) :
            get_ag_func_oid("gtype_build_list", 1, ANYOID),
        GTYPEOID, args, InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

/*
 * Transforms a column ref for indirection. Try to find the rte that the
 * columnRef is references and pass the properties of that rte as what the
 * columnRef is referencing. Otherwise, reference the Var
 */
static Node *
transform_column_ref_for_indirection(cypher_parsestate *cpstate, ColumnRef *cr) {
    ParseState *pstate = (ParseState *)cpstate;
    ParseNamespaceItem *pnsi;
    Node *field1 = linitial(cr->fields);
    char *relname;
    Node *node;
    int levels_up = 0;

    field1 = linitial(cr->fields);
    Assert(IsA(field1, String));
    relname = strVal(field1);

    // locate the referenced RTE
    pnsi = refnameNamespaceItem(pstate, NULL, relname, cr->location, &levels_up);

    if (!pnsi)
        return transform_cypher_expr_recurse(cpstate, (Node *)cr);

    node = scanNSItemForColumn(pstate, pnsi, 0, "properties", cr->location);

    if (!node)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("could not find rte for %s", relname)));

    return node;
}

static Node *
transform_a_indirection(cypher_parsestate *cpstate, A_Indirection *a_ind) {
    ParseState *pstate = (ParseState *)cpstate;

    /*
     * we can mark the last srf here, because if any part of this function finds another
     * srf, its a problem, no need to track this throughout
     *
     * XXX: There might be a use case that allows us to use SRFs with Indirection... leave it
     * as a potential todo for now.
     */
    Node *last_srf = pstate->p_last_srf;

    Assert(a_ind != NULL && list_length(a_ind->indirection) > 0);

    Node *cur;
    if (IsA(a_ind->arg, ColumnRef))
        cur = transform_column_ref_for_indirection(cpstate, (ColumnRef *)a_ind->arg);
    else
        cur = transform_cypher_expr_recurse(cpstate, a_ind->arg);

    ListCell *lc;
    foreach (lc, a_ind->indirection)
    {
        Node *node = lfirst(lc);

        if (is_a_slice(node)) {
            A_Indices *indices = (A_Indices *)node;

            cur =  makeFuncExpr(
                get_ag_func_oid("gtype_access_slice", 3, GTYPEOID, GTYPEOID, GTYPEOID),
                GTYPEOID,
                list_make3(
                    cur,
                    indices->lidx ? transform_cypher_expr_recurse(cpstate, indices->lidx) : makeConst(GTYPEOID, -1, InvalidOid, -1, (Datum)NULL, true, false),
                    indices->uidx ? transform_cypher_expr_recurse(cpstate, indices->uidx) : makeConst(GTYPEOID, -1, InvalidOid, -1, (Datum)NULL, true, false)
                ),
                InvalidOid,
                InvalidOid,
                COERCE_EXPLICIT_CALL);
        } else if (IsA(node, A_Indices)) {
            // a[i]
            A_Indices *indices = (A_Indices *)node;

            cur = (Node *)make_op(
                pstate,
                list_make1(makeString("->")),
                cur,
                transform_cypher_expr_recurse(cpstate, indices->uidx), pstate->p_last_srf,  -1);
        } else if (IsA(node, ColumnRef)) {
            // a.i
            ColumnRef *cr = (ColumnRef *)node;
            cur = (Node *)make_op(
                pstate,
                list_make1(makeString("->")),
                cur,
                makeConst(GTYPEOID, -1, InvalidOid, -1, string_to_gtype(strVal(linitial(cr->fields))), false, false),
                pstate->p_last_srf,
                -1
            );

        } else {
            ereport(ERROR, (errmsg("invalid indirection node %d", nodeTag(node))));
        }

        if (pstate->p_last_srf != last_srf)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("set-returning functions are not allowed in -> operator"),
                errhint("Use UNWIND to expand the result set of the function first"),
                parser_errposition(pstate, exprLocation(pstate->p_last_srf))));

    }
    
    return cur;
}

static Node *
transform_cypher_string_match(cypher_parsestate *cpstate, cypher_string_match *csm_node) {
    const char *func_name;
    switch (csm_node->operation)
    {
    case CSMO_STARTS_WITH:
        func_name = "gtype_string_match_starts_with";
        break;
    case CSMO_ENDS_WITH:
        func_name = "gtype_string_match_ends_with";
        break;
    case CSMO_CONTAINS:
        func_name = "gtype_string_match_contains";
        break;
    default:
        ereport(ERROR, (errmsg_internal("unknown Cypher string match operation")));
    }

    return makeFuncExpr(
        get_ag_func_oid(func_name, 2, GTYPEOID, GTYPEOID), 
        GTYPEOID,
        list_make2(transform_cypher_expr_recurse(cpstate, csm_node->lhs),
                   transform_cypher_expr_recurse(cpstate, csm_node->rhs)),
        InvalidOid,
        InvalidOid,
        COERCE_EXPLICIT_CALL);
}

/*
 * Function to create a typecasting node
 */
static Node *
transform_cypher_typecast(cypher_parsestate *cpstate, cypher_typecast *ctypecast) {
    List *fname;

    /* verify input parameter */
    Assert (cpstate != NULL);
    Assert (ctypecast != NULL);

    /* create the qualified function name, schema first */
    fname = list_make1(makeString(CATALOG_SCHEMA));

    /* append the name of the requested typecast function */
    if (pg_strcasecmp(ctypecast->typecast, "numeric") == 0)
        fname = lappend(fname, makeString("tonumeric"));
    else if (pg_strcasecmp(ctypecast->typecast, "float") == 0)
        fname = lappend(fname, makeString("tofloat"));
    else if (pg_strcasecmp(ctypecast->typecast, "int") == 0 || pg_strcasecmp(ctypecast->typecast, "integer") == 0)
        fname = lappend(fname, makeString("tointeger"));
    else if (pg_strcasecmp(ctypecast->typecast, "timestamp") == 0)
        fname = lappend(fname, makeString("totimestamp"));
    else if (pg_strcasecmp(ctypecast->typecast, "timestamptz") == 0)
        fname = lappend(fname, makeString("totimestamptz"));
    else if (pg_strcasecmp(ctypecast->typecast, "date") == 0)
        fname = lappend(fname, makeString("todate"));
    else if (pg_strcasecmp(ctypecast->typecast, "time") == 0)
        fname = lappend(fname, makeString("totime"));
    else if (pg_strcasecmp(ctypecast->typecast, "timetz") == 0)
        fname = lappend(fname, makeString("totimetz"));
    else if (pg_strcasecmp(ctypecast->typecast, "interval") == 0)
        fname = lappend(fname, makeString("tointerval"));
    else if (pg_strcasecmp(ctypecast->typecast, "vector") == 0)
        fname = lappend(fname, makeString("tovector"));
    else if (pg_strcasecmp(ctypecast->typecast, "inet") == 0)
        fname = lappend(fname, makeString("toinet"));
    else if (pg_strcasecmp(ctypecast->typecast, "cidr") == 0)
        fname = lappend(fname, makeString("tocidr"));
    else if (pg_strcasecmp(ctypecast->typecast, "macaddr") == 0)
        fname = lappend(fname, makeString("tomacaddr"));
    else if (pg_strcasecmp(ctypecast->typecast, "macaddr8") == 0)
        fname = lappend(fname, makeString("tomacaddr8"));
    else if (pg_strcasecmp(ctypecast->typecast, "point") == 0)
        fname = lappend(fname, makeString("topoint"));
    else if (pg_strcasecmp(ctypecast->typecast, "line") == 0)
        fname = lappend(fname, makeString("toline"));
    else if (pg_strcasecmp(ctypecast->typecast, "lseg") == 0)
        fname = lappend(fname, makeString("tolseg"));
    else if (pg_strcasecmp(ctypecast->typecast, "path") == 0)
        fname = lappend(fname, makeString("topath"));
    else if (pg_strcasecmp(ctypecast->typecast, "polygon") == 0)
        fname = lappend(fname, makeString("topolygon"));
    else if (pg_strcasecmp(ctypecast->typecast, "circle") == 0)
        fname = lappend(fname, makeString("tocircle"));
    else if (pg_strcasecmp(ctypecast->typecast, "box") == 0)
        fname = lappend(fname, makeString("tobox"));
    else if (pg_strcasecmp(ctypecast->typecast, "box2d") == 0)
        fname = lappend(fname, makeString("tobox2d"));
    else if (pg_strcasecmp(ctypecast->typecast, "box3d") == 0)
        fname = lappend(fname, makeString("tobox3d"));
    else if (pg_strcasecmp(ctypecast->typecast, "spheroid") == 0)
        fname = lappend(fname, makeString("tospheroid"));
    else if (pg_strcasecmp(ctypecast->typecast, "geometry") == 0)
        fname = lappend(fname, makeString("togeometry"));
    else if (pg_strcasecmp(ctypecast->typecast, "tsvector") == 0)
        fname = lappend(fname, makeString("totsvector"));
    else if (pg_strcasecmp(ctypecast->typecast, "tsquery") == 0)
        fname = lappend(fname, makeString("totsquery"));
    else
        ereport(ERROR, (errmsg_internal("typecast \'%s\' not supported", ctypecast->typecast)));


    return transform_func_call(cpstate, makeFuncCall(fname, list_make1(ctypecast->expr), COERCE_SQL_SYNTAX, ctypecast->location));
}


static List *
make_qualified_function_name(cypher_parsestate *cpstate, List *lst, List *targs) {
    char *name = ((Value*)linitial(lst))->val.str;
    int pnlen = strlen(name);
    char *ag_name = palloc(pnlen + 1);
    int i;
        
    /*
     * change all functions to lower case, this is mostly uncessary, but some
     * functions are keywords in SQL, we need to escape those functions in the sql
     * script to a lower case version, do that for all functions to make life
     * easier.
     */ 
    for (i = 0; i < pnlen; i++)
        ag_name[i] = tolower(name[i]);
        
    // Add NULL at the end
    ag_name[i] = '\0';

    // add the catalog schema and create list        
    List *fname;
    if (strcmp(ag_name, "now") == 0 || strcmp(ag_name, "transaction_timestamp") == 0 ||
        strcmp(ag_name, "statement_timestamp") == 0 ||  strcmp(ag_name, "clock_timestamp") == 0 ||
        strcmp(ag_name, "timeofday") == 0 || strcmp(ag_name, "row_number") == 0 ||
        strcmp(name, "rank") == 0 || strcmp(name, "dense_rank") == 0 ||
        strcmp(name, "percent_rank") == 0 || strcmp(name, "cume_dist") == 0 || 
        strcmp(name, "lag") == 0 || strcmp(name, "lead") == 0 || strcmp(name, "first_value") == 0 ||
        strcmp(name, "last_value") == 0 || strcmp(name, "ntile") == 0 || strcmp(name, "nth_value") == 0) 
        fname = list_make2(makeString("pg_catalog"), makeString(ag_name));
    else
        fname = list_make2(makeString(CATALOG_SCHEMA), makeString(ag_name));

    // Some functions need the graph name passed to them in order to work
    if (strcmp("vle", ag_name) == 0) {
        char *graph_name = cpstate->graph_name;
        Datum d = string_to_gtype(graph_name);
        Const *c = makeConst(GTYPEOID, -1, InvalidOid, -1, d, false, false);

        targs = lcons(c, targs);
    }

    return fname;

}

static Node *
transform_func_call(cypher_parsestate *cpstate, FuncCall *fn) {
    ParseState *pstate = (ParseState *)cpstate;
    Node *last_srf = pstate->p_last_srf;
    ListCell *lc;

    // transform args
    List *args = NIL;
    foreach(lc, fn->args) {
        args = lappend(args, transform_cypher_expr_recurse(cpstate, (Node *)lfirst(lc)));
    }

    // if the function name is not qualified assume the function is in our catalog.
    List *fname;
    if (list_length(fn->funcname) == 1)
        fname = make_qualified_function_name(cpstate, fn->funcname, args);
    else
        fname = fn->funcname;

    // Passed our new fname to the normal function transform logic
    Node *retval = ParseFuncOrColumn(pstate, fname, args, last_srf, fn, false, fn->location);


    if (list_length(fn->funcname) == 1) {
        char *name = strVal(linitial(fname));
    if (strcmp(name, "now") == 0 || strcmp(name, "transaction_timestamp") == 0 ||
        strcmp(name, "statement_timestamp") == 0 || strcmp(name, "clock_timestamp") == 0 ||
        strcmp(name, "timeofday") == 0 || strcmp(name, "row_number") == 0 ||
        strcmp(name, "rank") == 0 || strcmp(name, "dense_rank") == 0 ||
        strcmp(name, "percent_rank") == 0 || strcmp(name, "cume_dist") == 0) {
            Node *result;
            Oid inputType;
            Oid targetType;
            int32 targetTypmod;
    
            typenameTypeIdAndMod(cpstate, makeTypeName("gtype"), &targetType, &targetTypmod);

            inputType = exprType(retval);

            result = coerce_to_target_type(cpstate, retval, inputType, targetType, targetTypmod,
                                           COERCION_EXPLICIT, COERCE_EXPLICIT_CAST, -1);

            if (result == NULL)
                ereport(ERROR, (errcode(ERRCODE_CANNOT_COERCE),
                        errmsg("cannot cast type %s to %s", format_type_be(inputType), format_type_be(targetType)),
                        parser_coercion_errposition(cpstate, -1, retval)));

            retval = result;
    }
    }
    // flag that an aggregate was found
    if (retval != NULL && retval->type == T_Aggref)
        cpstate->exprHasAgg = true;

    return retval;
}

static Node *
transform_coalesce_expr(cypher_parsestate *cpstate, CoalesceExpr *cexpr) {
    ParseState *pstate = (ParseState *)cpstate;
    Node *last_srf = pstate->p_last_srf;
    ListCell *lc;

    CoalesceExpr *newcexpr = makeNode(CoalesceExpr);

    // transform each argument
    List *newargs = NIL;
    foreach(lc, cexpr->args) {
        Node *newe = transform_cypher_expr_recurse(cpstate, (Node *)lfirst(lc));

        newargs = lappend(newargs, newe);
    }

    newcexpr->coalescetype = select_common_type(pstate, newargs, "COALESCE", NULL);

    if (newcexpr->coalescetype != GTYPEOID)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("COALESCE in a cypher query expects to return gtype"),
                 parser_errposition(pstate, exprLocation((Node*)cexpr))));


    // coearce each agruement to gtype
    List *newcoercedargs = NIL;
    foreach(lc, newargs) {
        Node *e = (Node *)lfirst(lc);
        Node *newe = coerce_to_common_type(pstate, e, newcexpr->coalescetype, "COALESCE");

        newcoercedargs = lappend(newcoercedargs, newe);
    }

    if (pstate->p_last_srf != last_srf)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-returning functions are not allowed in COALESCE"),
                 parser_errposition(pstate, exprLocation(pstate->p_last_srf))));

    newcexpr->args = newcoercedargs;
    newcexpr->location = cexpr->location;
    return (Node *) newcexpr;
}

static Node *
transform_case_expr(cypher_parsestate *cpstate, CaseExpr *cexpr) {
    ParseState *pstate = (ParseState *)cpstate;
    Node *last_srf = pstate->p_last_srf;
    ListCell *lc;

    CaseExpr *newcexpr = makeNode(CaseExpr);

    // transform the test expression
    Node *arg = transform_cypher_expr_recurse(cpstate, (Node *) cexpr->arg);

    // generate placeholder for test expression
    CaseTestExpr *placeholder;
    if (arg) {
        if (exprType(arg) == UNKNOWNOID)
            arg = coerce_to_common_type(pstate, arg, GTYPEOID, "CASE");

        assign_expr_collations(pstate, arg);

        placeholder = makeNode(CaseTestExpr);
        placeholder->typeId = exprType(arg);
        placeholder->typeMod = exprTypmod(arg);
        placeholder->collation = exprCollation(arg);
    } else {
        placeholder = NULL;
    }

    newcexpr->arg = (Expr *) arg;

    // transform the case statements
    List *newargs = NIL;
    List *resultexprs = NIL;
    foreach(lc, cexpr->args) {
        CaseWhen *w = lfirst(lc);
        CaseWhen *neww = makeNode(CaseWhen);
        Node *warg = (Node *)w->expr;

        //shorthand form was specified, so default to equality check
        if (placeholder)
            warg = (Node *) makeSimpleA_Expr(AEXPR_OP, "=", (Node *) placeholder, warg, w->location);

        neww->expr = (Expr *) transform_cypher_expr_recurse(cpstate, warg);

        neww->expr = (Expr *) coerce_to_boolean(pstate, (Node *) neww->expr, "CASE/WHEN");

        warg = (Node *) w->result;
        neww->result = (Expr *) transform_cypher_expr_recurse(cpstate, warg);
        neww->location = w->location;

        newargs = lappend(newargs, neww);
        resultexprs = lappend(resultexprs, neww->result);
    }

    newcexpr->args = newargs;

    // transform the default clause
    Node *defresult = (Node *) cexpr->defresult;
    if (!defresult) {
        A_Const *n = makeNode(A_Const);

        n->val.type = T_Null;
        n->location = -1;
        defresult = (Node *) n;
    }

    newcexpr->defresult = (Expr *) transform_cypher_expr_recurse(cpstate, defresult);

    resultexprs = lcons(newcexpr->defresult, resultexprs);

    Oid ptype = select_common_type(pstate, resultexprs, "CASE", NULL);

    Assert(OidIsValid(ptype));

    newcexpr->casetype = ptype;

    // Convert default result clause, if necessary
    newcexpr->defresult = (Expr *)
        coerce_to_common_type(pstate, (Node *) newcexpr->defresult, ptype, "CASE/ELSE");

    /* Convert when-clause results, if necessary */
    foreach(lc, newcexpr->args) {
        CaseWhen *w = (CaseWhen *) lfirst(lc);

        w->result = (Expr *) coerce_to_common_type(pstate, (Node *) w->result, ptype, "CASE/WHEN");
    }

    if (pstate->p_last_srf != last_srf)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-returning functions are not allowed in CASE"),
                 parser_errposition(pstate, exprLocation(pstate->p_last_srf))));

    newcexpr->location = cexpr->location;

    return (Node *) newcexpr;
}

/*
 * Transform a "row compare-op row" construct
 *
 * The inputs are lists of already-transformed expressions.
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 *
 * The output may be a single OpExpr, an AND or OR combination of OpExprs,
 * or a RowCompareExpr.  In all cases it is guaranteed to return boolean.
 * The AND, OR, and RowCompareExpr cases further imply things about the
 * behavior of the operators (ie, they behave as =, <>, or < <= > >=).
 */
static Node *
make_row_comparison_op(ParseState *pstate, List *opname, List *largs, List *rargs, int location)
{
    RowCompareExpr *rcexpr;
    RowCompareType rctype;
    List *opexprs;
    List *opnos;
    List *opfamilies;
    ListCell *l, *r;
    List **opinfo_lists;
    Bitmapset  *strats;
    int nopers;
    int i;

    nopers = list_length(largs);
    if (nopers != list_length(rargs))
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("unequal number of entries in row expressions"),
                        parser_errposition(pstate, location)));

    /*
     * We can't compare zero-length rows because there is no principled basis
     * for figuring out what the operator is.
     */
    if (nopers == 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot compare rows of zero length"),
                        parser_errposition(pstate, location)));
    /*
     * Identify all the pairwise operators, using make_op so that behavior is
     * the same as in the simple scalar case.
     */
    opexprs = NIL;
    forboth(l, largs, r, rargs)
    {
        Node *larg = (Node *) lfirst(l);
        Node *rarg = (Node *) lfirst(r);
        OpExpr *cmp;

        cmp = castNode(OpExpr, make_op(pstate, opname, larg, rarg, pstate->p_last_srf, location));

        /*
         * We don't use coerce_to_boolean here because we insist on the
         * operator yielding boolean directly, not via coercion.  If it
         * doesn't yield bool it won't be in any index opfamilies...
         */
        if (cmp->opresulttype != BOOLOID)
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                            errmsg("row comparison operator must yield type boolean, " "not type %s",
                                   format_type_be(cmp->opresulttype)),
                            parser_errposition(pstate, location)));
        if (expression_returns_set((Node *) cmp))
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                            errmsg("row comparison operator must not return a set"),
                            parser_errposition(pstate, location)));
        opexprs = lappend(opexprs, cmp);
    }
    /*
     * If rows are length 1, just return the single operator.  In this case we
     * don't insist on identifying btree semantics for the operator (but we
     * still require it to return boolean).
     */
    if (nopers == 1)
                return (Node *) linitial(opexprs);

    /*
     * Now we must determine which row comparison semantics (= <> < <= > >=)
     * apply to this set of operators.  We look for btree opfamilies
     * containing the operators, and see which interpretations (strategy
     * numbers) exist for each operator.
     */
    opinfo_lists = (List **) palloc(nopers * sizeof(List *));
    strats = NULL;
    i = 0;
    foreach(l, opexprs)
    {
        Oid opno = ((OpExpr *) lfirst(l))->opno;
        Bitmapset *this_strats;
        ListCell *j;

        opinfo_lists[i] = get_op_btree_interpretation(opno);

        /*
         * convert strategy numbers into a Bitmapset to make the intersection
         * calculation easy.
         */
        this_strats = NULL;
        foreach(j, opinfo_lists[i])
        {
            OpBtreeInterpretation *opinfo = lfirst(j);

            this_strats = bms_add_member(this_strats, opinfo->strategy);
        }
        if (i == 0)
            strats = this_strats;
        else
            strats = bms_int_members(strats, this_strats);
        i++;
    }
    /*
     * If there are multiple common interpretations, we may use any one of
     * them ... this coding arbitrarily picks the lowest btree strategy
     * number.
     */
    i = bms_first_member(strats);
    if (i < 0)
    {
        /* No common interpretation, so fail */
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("could not determine interpretation of row comparison operator %s", strVal(llast(opname))),
            errhint("Row comparison operators must be associated with btree operator families."),
            parser_errposition(pstate, location)));
    }
    rctype = (RowCompareType) i;

    /*
     * For = and <> cases, we just combine the pairwise operators with AND or
     * OR respectively.
     */
    if (rctype == ROWCOMPARE_EQ)
        return (Node *) makeBoolExpr(AND_EXPR, opexprs, location);
    if (rctype == ROWCOMPARE_NE)
        return (Node *) makeBoolExpr(OR_EXPR, opexprs, location);

    /*
     * Otherwise we need to choose exactly which opfamily to associate with
     * each operator.
     */
    opfamilies = NIL;
    for (i = 0; i < nopers; i++)
    {
        Oid opfamily = InvalidOid;
        ListCell *j;

        foreach(j, opinfo_lists[i])
        {
             OpBtreeInterpretation *opinfo = lfirst(j);

             if (opinfo->strategy == rctype)
             {
                 opfamily = opinfo->opfamily_id;
                 break;
             }
        }
        if (OidIsValid(opfamily))
            opfamilies = lappend_oid(opfamilies, opfamily);
        else /* should not happen */
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("could not determine interpretation of row comparison operator %s",
                                   strVal(llast(opname))),
                            errdetail("There are multiple equally-plausible candidates."),
                            parser_errposition(pstate, location)));
    }

    /*
     * Now deconstruct the OpExprs and create a RowCompareExpr.
     *
     * Note: can't just reuse the passed largs/rargs lists, because of
     * possibility that make_op inserted coercion operations.
     */
    opnos = NIL;
    largs = NIL;
    rargs = NIL;
    foreach(l, opexprs)
    {
        OpExpr *cmp = (OpExpr *) lfirst(l);

        opnos = lappend_oid(opnos, cmp->opno);
        largs = lappend(largs, linitial(cmp->args));
        rargs = lappend(rargs, lsecond(cmp->args));
    }

    rcexpr = makeNode(RowCompareExpr);
    rcexpr->rctype = rctype;
    rcexpr->opnos = opnos;
    rcexpr->opfamilies = opfamilies;
    rcexpr->inputcollids = NIL; /* assign_expr_collations will fix this */
    rcexpr->largs = largs;
    rcexpr->rargs = rargs;

    return (Node *) rcexpr;
}

/*
 * Transform a FOR [KEY] UPDATE/SHARE clause
 *
 * This basically involves replacing names by integer relids.
 *
 * NB: if you need to change this, see also markQueryForLocking()
 * in rewriteHandler.c, and isLockedRefname() in parse_relation.c.
 */
static void
transformLockingClause(ParseState *pstate, Query *qry, LockingClause *lc,
					   bool pushedDown)
{
	List	   *lockedRels = lc->lockedRels;
	ListCell   *l;
	ListCell   *rt;
	Index		i;
	LockingClause *allrels;

	CheckSelectLocking(qry, lc->strength);

	/* make a clause we can pass down to subqueries to select all rels */
	allrels = makeNode(LockingClause);
	allrels->lockedRels = NIL;	/* indicates all rels */
	allrels->strength = lc->strength;
	allrels->waitPolicy = lc->waitPolicy;

	if (lockedRels == NIL)
	{
		/*
		 * Lock all regular tables used in query and its subqueries.  We
		 * examine inFromCl to exclude auto-added RTEs, particularly NEW/OLD
		 * in rules.  This is a bit of an abuse of a mostly-obsolete flag, but
		 * it's convenient.  We can't rely on the namespace mechanism that has
		 * largely replaced inFromCl, since for example we need to lock
		 * base-relation RTEs even if they are masked by upper joins.
		 */
		i = 0;
		foreach(rt, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			++i;
			if (!rte->inFromCl)
				continue;
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					applyLockingClause(qry, i, lc->strength, lc->waitPolicy,
									   pushedDown);
					rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
					break;
				case RTE_SUBQUERY:
					applyLockingClause(qry, i, lc->strength, lc->waitPolicy,
									   pushedDown);

					/*
					 * FOR UPDATE/SHARE of subquery is propagated to all of
					 * subquery's rels, too.  We could do this later (based on
					 * the marking of the subquery RTE) but it is convenient
					 * to have local knowledge in each query level about which
					 * rels need to be opened with RowShareLock.
					 */
					transformLockingClause(pstate, rte->subquery,
										   allrels, true);
					break;
				default:
					/* ignore JOIN, SPECIAL, FUNCTION, VALUES, CTE RTEs */
					break;
			}
		}
	}
	else
	{
		/*
		 * Lock just the named tables.  As above, we allow locking any base
		 * relation regardless of alias-visibility rules, so we need to
		 * examine inFromCl to exclude OLD/NEW.
		 */
		foreach(l, lockedRels)
		{
			RangeVar   *thisrel = (RangeVar *) lfirst(l);

			/* For simplicity we insist on unqualified alias names here */
			if (thisrel->catalogname || thisrel->schemaname)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("%s must specify unqualified relation names",
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));

			i = 0;
			foreach(rt, qry->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);
				char	   *rtename;

				++i;
				if (!rte->inFromCl)
					continue;

				/*
				 * A join RTE without an alias is not visible as a relation
				 * name and needs to be skipped (otherwise it might hide a
				 * base relation with the same name), except if it has a USING
				 * alias, which *is* visible.
				 */
				if (rte->rtekind == RTE_JOIN && rte->alias == NULL)
				{
					if (rte->join_using_alias == NULL)
						continue;
					rtename = rte->join_using_alias->aliasname;
				}
				else
					rtename = rte->eref->aliasname;

				if (strcmp(rtename, thisrel->relname) == 0)
				{
					switch (rte->rtekind)
					{
						case RTE_RELATION:
							applyLockingClause(qry, i, lc->strength,
											   lc->waitPolicy, pushedDown);
							rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
							break;
						case RTE_SUBQUERY:
							applyLockingClause(qry, i, lc->strength,
											   lc->waitPolicy, pushedDown);
							/* see comment above */
							transformLockingClause(pstate, rte->subquery,
												   allrels, true);
							break;
						case RTE_JOIN:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a join",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_FUNCTION:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_TABLEFUNC:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a table function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_VALUES:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to VALUES",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_CTE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a WITH query",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_NAMEDTUPLESTORE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a named tuplestore",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;

							/* Shouldn't be possible to see RTE_RESULT here */

						default:
							elog(ERROR, "unrecognized RTE type: %d",
								 (int) rte->rtekind);
							break;
					}
					break;		/* out of foreach loop */
				}
			}
			if (rt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("relation \"%s\" in %s clause not found in FROM clause",
								thisrel->relname,
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));
		}
	}
}


/*
 * parse_sub_analyze
 *		Entry point for recursively analyzing a sub-statement.
 */
Query *
sql_parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
				  CommonTableExpr *parentCTE,
				  bool locked_from_parent,
				  bool resolve_unknowns)
{
	ParseState *pstate = make_parsestate(parentParseState);
	Query	   *query;

	pstate->p_parent_cte = parentCTE;
	pstate->p_locked_from_parent = locked_from_parent;
	pstate->p_resolve_unknowns = resolve_unknowns;

	//query = transformStmt(pstate, parseTree);
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
                    elog(ERROR, "unexpected non-SELECT command in SubLink");
					//query = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE) {
                    parsing_cypher = false;
                    query = transformSelectStmt(pstate, n);
                    parsing_cypher = true;
                } else
                    elog(ERROR, "unexpected non-SELECT command in SubLink");
					//query = transformSetOperationStmt(pstate, n);
			}
	query->querySource = QSRC_ORIGINAL;
	query->canSetTag = true;
	free_parsestate(pstate);

	return query;
}

static Node *
transform_sub_link(cypher_parsestate *cpstate, SubLink *sublink) {
    ParseState *pstate = (ParseState*)cpstate;
    const char *err;
    /*
     * Check to see if the sublink is in an invalid place within the query. We
     * allow sublinks everywhere in SELECT/INSERT/UPDATE/DELETE, but generally
     * not in utility statements.
     */
    err = NULL;
    switch (pstate->p_expr_kind)
    {
        case EXPR_KIND_OTHER: /* Accept sublink here; caller must throw error if wanted */
        case EXPR_KIND_JOIN_ON:
        case EXPR_KIND_JOIN_USING:
        case EXPR_KIND_FROM_SUBSELECT:
        case EXPR_KIND_FROM_FUNCTION:
        case EXPR_KIND_WHERE:
        case EXPR_KIND_POLICY:
        case EXPR_KIND_HAVING:
        case EXPR_KIND_FILTER:
        case EXPR_KIND_WINDOW_PARTITION:
        case EXPR_KIND_WINDOW_ORDER:
        case EXPR_KIND_WINDOW_FRAME_RANGE:
        case EXPR_KIND_WINDOW_FRAME_ROWS:
        case EXPR_KIND_WINDOW_FRAME_GROUPS:
        case EXPR_KIND_SELECT_TARGET:
        case EXPR_KIND_INSERT_TARGET:
        case EXPR_KIND_UPDATE_SOURCE:
        case EXPR_KIND_UPDATE_TARGET:
        case EXPR_KIND_GROUP_BY:
        case EXPR_KIND_ORDER_BY:
        case EXPR_KIND_DISTINCT_ON:
        case EXPR_KIND_LIMIT:
        case EXPR_KIND_OFFSET:
        case EXPR_KIND_RETURNING:
        case EXPR_KIND_VALUES:
        case EXPR_KIND_VALUES_SINGLE:
        case EXPR_KIND_CYCLE_MARK:
            break;
        case EXPR_KIND_CHECK_CONSTRAINT:
        case EXPR_KIND_DOMAIN_CHECK:
            err = _("cannot use subquery in check constraint");
            break;
        case EXPR_KIND_COLUMN_DEFAULT:
        case EXPR_KIND_FUNCTION_DEFAULT:
            err = _("cannot use subquery in DEFAULT expression");
            break;
        case EXPR_KIND_INDEX_EXPRESSION:
            err = _("cannot use subquery in index expression");
            break;
        case EXPR_KIND_INDEX_PREDICATE:
            err = _("cannot use subquery in index predicate");
            break;
        case EXPR_KIND_STATS_EXPRESSION:
            err = _("cannot use subquery in statistics expression");
            break;
        case EXPR_KIND_ALTER_COL_TRANSFORM:
            err = _("cannot use subquery in transform expression");
            break;
        case EXPR_KIND_EXECUTE_PARAMETER:
            err = _("cannot use subquery in EXECUTE parameter");
            break;
        case EXPR_KIND_TRIGGER_WHEN:
            err = _("cannot use subquery in trigger WHEN condition");
            break;
        case EXPR_KIND_PARTITION_BOUND:
            err = _("cannot use subquery in partition bound");
            break;
        case EXPR_KIND_PARTITION_EXPRESSION:
            err = _("cannot use subquery in partition key expression");
            break;
        case EXPR_KIND_CALL_ARGUMENT:
            err = _("cannot use subquery in CALL argument");
            break;
        case EXPR_KIND_COPY_WHERE:
            err = _("cannot use subquery in COPY FROM WHERE condition");
            break;
        case EXPR_KIND_GENERATED_COLUMN:
            err = _("cannot use subquery in column generation expression");
            break;
    }
    if (err)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg_internal("%s", err),
                        parser_errposition(pstate, sublink->location)));

    pstate->p_hasSubLinks = true;


    Query *query;
    if (IsA(sublink->subselect, SelectStmt)) {
	    parsing_cypher = false;
        query = sql_parse_sub_analyze(sublink->subselect, pstate, NULL, false, true);
        parsing_cypher = true;
    } else
        query = cypher_parse_sub_analyze(sublink->subselect, cpstate, NULL, false, true);

    if (!IsA(query, Query) || query->commandType != CMD_SELECT)
        elog(ERROR, "unexpected non-SELECT command in SubLink");

    sublink->subselect = (Node *)query;

    if (sublink->subLinkType == EXISTS_SUBLINK) {
        sublink->testexpr = NULL;
        sublink->operName = NIL;
    }  else if (sublink->subLinkType == EXPR_SUBLINK ||
			    sublink->subLinkType == ARRAY_SUBLINK) {
		/*
		 * Make sure the subselect delivers a single column (ignoring resjunk
		 * targets).
		 */
		if (count_nonjunk_tlist_entries(query->targetList) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery must return only one column"),
					 parser_errposition(pstate, sublink->location)));

		/*
		 * EXPR and ARRAY need no test expression or combining operator. These
		 * fields should be null already, but make sure.
		 */
		sublink->testexpr = NULL;
		sublink->operName = NIL;
	} else if (sublink->subLinkType == MULTIEXPR_SUBLINK) {
        /* Same as EXPR case, except no restriction on number of columns */
        sublink->testexpr = NULL;
        sublink->operName = NIL;
    } else {
        /* ALL, ANY, or ROWCOMPARE: generate row-comparing expression */
        Node *lefthand;
        List *left_list;
        List *right_list;
        ListCell *l;

        /*
         * If the source was "x IN (select)", convert to "x = ANY (select)".
         */
        if (sublink->operName == NIL)
            sublink->operName = list_make2(makeString("postgraph"), makeString("="));

        /*
         * Transform lefthand expression, and convert to a list
         */
        lefthand = transform_cypher_expr_recurse(pstate, sublink->testexpr);
        if (lefthand && IsA(lefthand, RowExpr))
            left_list = ((RowExpr *) lefthand)->args;
        else
            left_list = list_make1(lefthand);

        /*
         * Build a list of PARAM_SUBLINK nodes representing the output columns
         * of the subquery.
         */
        right_list = NIL;
        foreach(l, query->targetList)
        {
            TargetEntry *tent = (TargetEntry *) lfirst(l);
            Param      *param;

            if (tent->resjunk)
                 continue;

            param = makeNode(Param);
            param->paramkind = PARAM_SUBLINK;
            param->paramid = tent->resno;
            param->paramtype = exprType((Node *) tent->expr);
            param->paramtypmod = exprTypmod((Node *) tent->expr);
            param->paramcollid = exprCollation((Node *) tent->expr);
            param->location = -1;

            right_list = lappend(right_list, param);
        }

        /*
         * We could rely on make_row_comparison_op to complain if the list
         * lengths differ, but we prefer to generate a more specific error
         * message.
         */
        if (list_length(left_list) < list_length(right_list))
            ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("subquery has too many columns"),
                            parser_errposition(pstate, sublink->location)));
        if (list_length(left_list) > list_length(right_list))
            ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("subquery has too few columns"),
                            parser_errposition(pstate, sublink->location)));

        /*
         * Identify the combining operator(s) and generate a suitable
         * row-comparison expression.
         */
        sublink->testexpr = make_row_comparison_op(pstate, sublink->operName, left_list, right_list, sublink->location);
    }

    return (Node *)sublink;
}

static Node *
transformSQLValueFunction(cypher_parsestate *cpstate, SQLValueFunction *svf)
{
    /*
     * All we need to do is insert the correct result type and (where needed)
     * validate the typmod, so we just modify the node in-place.
     */
    switch (svf->op)
    {
        case SVFOP_CURRENT_DATE:
            svf->type = DATEOID;
            break;
        case SVFOP_CURRENT_TIME:
            svf->type = TIMETZOID;
            break;
        case SVFOP_CURRENT_TIME_N:
            svf->type = TIMETZOID;
            svf->typmod = anytime_typmod_check(true, svf->typmod);
            break;
        case SVFOP_CURRENT_TIMESTAMP:
            svf->type = TIMESTAMPTZOID;
            break;
        case SVFOP_CURRENT_TIMESTAMP_N:
            svf->type = TIMESTAMPTZOID;
            svf->typmod = anytimestamp_typmod_check(true, svf->typmod);
            break;
        case SVFOP_LOCALTIME:
            svf->type = TIMEOID;
            break;
        case SVFOP_LOCALTIME_N:
            svf->type = TIMEOID;
            svf->typmod = anytime_typmod_check(false, svf->typmod);
            break;
        case SVFOP_LOCALTIMESTAMP:
            svf->type = TIMESTAMPOID;
            break;
        case SVFOP_LOCALTIMESTAMP_N:
            svf->type = TIMESTAMPOID;
            svf->typmod = anytimestamp_typmod_check(false, svf->typmod);
            break;
        case SVFOP_CURRENT_ROLE:
        case SVFOP_CURRENT_USER:
        case SVFOP_USER:
        case SVFOP_SESSION_USER:
        case SVFOP_CURRENT_CATALOG:
        case SVFOP_CURRENT_SCHEMA:
            svf->type = NAMEOID;
            break;
    }

    Node *result;
    Node *expr = (Node *) svf;
    Oid inputType;
    Oid targetType;
    int32 targetTypmod;

    typenameTypeIdAndMod(cpstate, makeTypeName("gtype"), &targetType, &targetTypmod);

    inputType = exprType(expr);
    if (inputType == InvalidOid)
            return expr;

    result = coerce_to_target_type(cpstate, expr, inputType, targetType, targetTypmod,
                                    COERCION_EXPLICIT, COERCE_EXPLICIT_CAST, -1);

    if (result == NULL)
        ereport(ERROR, (errcode(ERRCODE_CANNOT_COERCE),
                        errmsg("cannot cast type %s to %s", format_type_be(inputType), format_type_be(targetType)),
                        parser_coercion_errposition(cpstate, -1, expr)));

    return result;
}

Node *cypher_columnref_hook (ParseState *pstate, ColumnRef *cref) {
    return NULL;
}