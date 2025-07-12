
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

static WindowClause *
findWindowClause(List *wclist, const char *name);
 static Node *flatten_grouping_sets(Node *expr, bool toplevel, bool *hasGroupingSets);
static Index transform_group_clause_expr(List **flatresult, Bitmapset *seen_local, cypher_parsestate *cpstate, Node *gexpr, List **targetlist, List *sortClause, ParseExprKind exprKind, bool toplevel);
static List *add_target_to_group_list(cypher_parsestate *cpstate, TargetEntry *tle, List *grouplist, List *targetlist, int location);
static Node *
transformGroupingSet(List **flatresult, ParseState *pstate, GroupingSet *gset, List **targetlist, List *sortClause,
                     ParseExprKind exprKind, bool useSQL99, bool toplevel);
 static Node *
transform_frame_offset(ParseState *pstate, int frameOptions,
                     Oid rangeopfamily, Oid rangeopcintype, Oid *inRangeFunc,
                     Node *clause);
static void
checkExprIsVarFree(ParseState *pstate, Node *n, const char *constructName);

 List * transform_group_clause(cypher_parsestate *cpstate, List *grouplist, List **groupingSets,
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
