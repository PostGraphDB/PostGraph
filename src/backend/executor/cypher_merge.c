/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "utils/rel.h"


#include "catalog/ag_label.h"
#include "executor/cypher_executor.h"
#include "nodes/cypher_nodes.h"
#include "utils/gtype.h"
#include "utils/graphid.h"
#include "utils/vertex.h"
#include "utils/edge.h"
#include "utils/ag_cache.h"
#include "utils/traversal.h"
#define AdvanceCmdId(estate) \
    estate->es_output_cid++; \
    estate->es_snapshot->curcid++;

#define RollbackCmdId(estate) \
    estate->es_output_cid--; \
    estate->es_snapshot->curcid--;
typedef struct cypher_merge_custom_scan_state
{
    CustomScanState css;
    CustomScan *cs;
    cypher_merge_information *merge_information;
    int flags;
    cypher_create_path *path;
    List *path_values;
    Oid graph_oid;
    AttrNumber merge_function_attr;
    bool created_new_path;
    bool found_a_path;
    graphid **vertex_ids;
    graphid **edge_ids;
} cypher_merge_custom_scan_state;

static void begin_cypher_merge(CustomScanState *node, EState *estate,
                               int eflags);
static TupleTableSlot *exec_cypher_merge(CustomScanState *node);
static void end_cypher_merge(CustomScanState *node);
static void rescan_cypher_merge(CustomScanState *node);
static Datum merge_vertex(cypher_merge_custom_scan_state *css,
                          cypher_target_node *node, ListCell *next);
static void merge_edge(cypher_merge_custom_scan_state *css,
                       cypher_target_node *node, Datum prev_vertex_id,
                       ListCell *next);
static void process_simple_merge(CustomScanState *node);
static bool check_path(cypher_merge_custom_scan_state *css,
                       TupleTableSlot *slot);
static void process_path(cypher_merge_custom_scan_state *css);
static void mark_tts_isnull(TupleTableSlot *slot);

const CustomExecMethods cypher_merge_exec_methods = {"Merge Scan State",
                                                     begin_cypher_merge,
                                                     exec_cypher_merge,
                                                     end_cypher_merge,
                                                     rescan_cypher_merge,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     NULL};



/*
 * Insert the edge/vertex tuple into the table and indices. Check that the
 * table's constraints have not been violated.
 */
HeapTuple insert_entity_tuple(ResultRelInfo *resultRelInfo,
                                  TupleTableSlot *elemTupleSlot,
                                  EState *estate)
{
    HeapTuple tuple = NULL;

    ExecStoreVirtualTuple(elemTupleSlot);
    //tuple = ExecFetchSlotHeapTuple(elemTupleSlot, true, NULL);

    /* Check the constraints of the tuple */
    //tuple->t_tableOid = resultRelInfo->ri_RelationDesc->rd_id;
   /* if (resultRelInfo->ri_RelationDesc->rd_att->constr != NULL)
    {
        ExecConstraints(resultRelInfo, elemTupleSlot, estate);
    }*/

    // Insert the tuple normally
    table_tuple_insert(resultRelInfo->ri_RelationDesc, elemTupleSlot, estate->es_output_cid, 0, NULL);

    // Insert index entries for the tuple
    if (resultRelInfo->ri_NumIndices > 0)
    {
        Oid oid = elemTupleSlot->tts_tableOid;
        elemTupleSlot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);// RelationGetRelid(heapRelation)

        ExecInsertIndexTuples(resultRelInfo, elemTupleSlot, estate, false, false, NULL, NIL);
        elemTupleSlot->tts_tableOid = oid;
    }

    return NULL;
}

                                                     
/*
 * Initializes the MERGE Execution Node at the begginning of the execution
 * phase.
 */
static void begin_cypher_merge(CustomScanState *node, EState *estate,
                               int eflags)
{
    cypher_merge_custom_scan_state *css =
        (cypher_merge_custom_scan_state *)node;
    ListCell *lc;
    Plan *subplan;
css->found_a_path = false;
    Assert(list_length(css->cs->custom_plans) == 1);

    // initialize the subplan
    subplan = linitial(css->cs->custom_plans);
    node->ss.ps.lefttree = ExecInitNode(subplan, estate, eflags);

    ExecAssignExprContext(estate, &node->ss.ps);

    ExecInitScanTupleSlot(estate, &node->ss,
                          ExecGetResultType(node->ss.ps.lefttree),
                          &TTSOpsHeapTuple);

    /*
     * When MERGE is not the last clause in a cypher query. Setup projection
     * information to pass to the parent execution node.
     */
    if (!CYPHER_CLAUSE_IS_TERMINAL(css->flags))
    {
        TupleDesc tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

        ExecAssignProjectionInfo(&node->ss.ps, tupdesc);
    }

    cypher_create_path *path = css->path;

    css->vertex_ids = palloc(sizeof(graphid *) * 1);
    css->vertex_ids[0] = palloc(sizeof(graphid) * ((list_length(path->target_nodes)/ 2) + 1) );

    if (path->target_nodes) {
        css->edge_ids = palloc(sizeof(graphid *) * 1);
        css->edge_ids[0] = palloc(sizeof(graphid) * ((list_length(path->target_nodes) / 2) ) );
    }

    ListCell *lc1;
    int i = 0;
    foreach (lc1, path->target_nodes) {
        cypher_target_node *cypher_node = (cypher_target_node *)lfirst(lc1);

        if (cypher_node->prop_expr != NULL)
            cypher_node->prop_expr_state = ExecInitExpr(cypher_node->prop_expr, (PlanState *)node);

        if (cypher_node->flags & EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE) {
            i++;
            continue;
        }
        
        cypher_node->id_expr_state = ExecInitExpr(cypher_node->id_expr, (PlanState *)node);

        if (i % 2 == 1) {
            i++;
            continue;
        }
        
        cypher_node->rel = table_open(cypher_node->relid, RowExclusiveLock);
        cypher_node->resultRelInfo = makeNode(ResultRelInfo);
        InitResultRelInfo(cypher_node->resultRelInfo, cypher_node->rel, list_length(estate->es_range_table), NULL, estate->es_instrument);
        
        // Open all indexes for the relation
        ExecOpenIndices(cypher_node->resultRelInfo, false);
    
        // Setup the relation's tuple slot
        cypher_node->elemTupleSlot = table_slot_create(cypher_node->rel, &estate->es_tupleTable); 

        cypher_node->adj_rel = table_open(cypher_node->adj_relid, RowExclusiveLock);
        cypher_node->adj_resultRelInfo = makeNode(ResultRelInfo);
        InitResultRelInfo(cypher_node->adj_resultRelInfo, cypher_node->adj_rel, list_length(estate->es_range_table), NULL, estate->es_instrument);
        
        ExecOpenIndices(cypher_node->adj_resultRelInfo, false);
    
        // Setup the relation's tuple slot
        cypher_node->adj_elemTupleSlot = table_slot_create(cypher_node->adj_rel, &estate->es_tupleTable); 

        i++;

    }

    if (estate->es_output_cid == 0)
        estate->es_output_cid = estate->es_snapshot->curcid;

    AdvanceCmdId(estate);
}



/*
 * Checks the subtree to see if the lateral join representing the MERGE path
 * found results. Returns true if the path does not exist and must be created,
 * false otherwise.
 */
static bool check_path(cypher_merge_custom_scan_state *css,
                       TupleTableSlot *slot)
{
    cypher_create_path *path = css->path;
    ListCell *lc;

    foreach(lc, path->target_nodes)
    {
        cypher_target_node *node = lfirst(lc);

        /*
         * If target_node as a valid attribute number and is a node not
         * declared in a previous clause, check the tuple position in the
         * slot. If the slot is null, the path was not found. The rules
         * state that if one part of the path does not exists, the whold
         * path must be created.
         */
        if (node->tuple_position != InvalidAttrNumber ||
            ((node->flags & CYPHER_TARGET_NODE_MERGE_EXISTS) == 0))
        {
            /*
             * Attribute number is 1 indexed and tts_values is 0 indexed,
             * offset by 1.
             */
            if (slot->tts_isnull[node->tuple_position - 1])
            {
                return true;
            }
        }

    }

    return false;
}
static void process_path(cypher_merge_custom_scan_state *css)
{
    cypher_create_path *path = css->path;
    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    TupleTableSlot *slot = econtext->ecxt_scantuple;
    
    ListCell *lc;
    int i = 1;
    foreach(lc, path->target_nodes) {
            cypher_target_node *node = (cypher_target_node *)lfirst(lc);
            bool isNull;
            if(i % 2 == 1) {
                if(node->flags & EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE)
                    css->vertex_ids[0][i/2] = DATUM_GET_GRAPHID(slot->tts_values[node->id_attr_num - 1]);
                else
                    css->vertex_ids[0][i/2] = ExecEvalExpr(node->id_expr_state, econtext, &isNull);
            } else {
                css->edge_ids[0][(i-1)/2] = ExecEvalExpr(node->id_expr_state, econtext, &isNull);
            }  
            i++;
    }

    for (int i = 0; i < list_length(path->target_nodes)/2 + 1; i++) {
            cypher_target_node *node = (cypher_target_node *)list_nth(path->target_nodes, i * 2);
                    
            ResultRelInfo *resultRelInfo = node->resultRelInfo;
            TupleTableSlot *elemTupleSlot = node->elemTupleSlot;

            ResultRelInfo **old_estate_es_result_relations_info = NULL;

            /* save the old result relation info */
            old_estate_es_result_relations_info = estate->es_result_relations;

            estate->es_result_relations = &resultRelInfo;
            if(!(node->flags & EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE)){
                ExecClearTuple(elemTupleSlot);

                // get the next graphid for this vertex.
                elemTupleSlot->tts_values[0] = css->vertex_ids[0][i];
                elemTupleSlot->tts_isnull[0] = false;
                if (node->id_attr_num != InvalidAttrNumber) {
                    slot->tts_values[node->id_attr_num - 1] = GRAPHID_GET_DATUM(css->vertex_ids[0][i]);
                    slot->tts_isnull[node->id_attr_num - 1] = false;
                }

                // get the properties for this vertex
                if (node->prop_attr_num == InvalidAttrNumber) {
                    elemTupleSlot->tts_values[1] = NULL;
                    elemTupleSlot->tts_isnull[1] = true;
                } else {
                    bool isNull;
                    elemTupleSlot->tts_values[1] = slot->tts_values[node->id_attr_num - 1] = ExecEvalExpr(node->prop_expr_state, econtext, &isNull);
                    elemTupleSlot->tts_isnull[1] = slot->tts_isnull[node->id_attr_num - 1] = isNull;
                }

                if (node->tuple_position != InvalidAttrNumber) {
                    slot->tts_values[node->tuple_position - 1] = VERTEX_GET_DATUM(create_vertex(
                        slot->tts_values[node->id_attr_num - 1],
                        css->graph_oid,
                        elemTupleSlot->tts_isnull[1] ? NULL : DATUM_GET_GTYPE_P(elemTupleSlot->tts_values[1])));
                    slot->tts_isnull[node->tuple_position - 1] = false;
                }

                // Insert the new vertex
                insert_entity_tuple(resultRelInfo, elemTupleSlot, estate);
            }
            

            if (list_length(path->target_nodes) > 1 && i < (list_length(path->target_nodes)/2)) {
                cypher_target_node *start_vertex;
                if (node->dir == CYPHER_REL_DIR_RIGHT) {
                    start_vertex = list_nth(path->target_nodes, i * 2);
                } else {
                    start_vertex = list_nth(path->target_nodes, (i * 2) + 2);
                }

                if(start_vertex->flags & EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE){
                    label_cache_data *lcd = search_label_graph_oid_cache(
                        css->graph_oid,
                        DATUM_GET_GRAPHID(slot->tts_values[start_vertex->id_attr_num - 1]) >> ENTRY_ID_BITS);
                    start_vertex->adj_relid = lcd->relation;

                    Relation adj_rel = table_open(start_vertex->adj_relid, RowExclusiveLock);

                    start_vertex->adj_resultRelInfo = makeNode(ResultRelInfo);
                    InitResultRelInfo(start_vertex->adj_resultRelInfo, adj_rel, list_length(estate->es_range_table), NULL, estate->es_instrument);
                    ExecOpenIndices(start_vertex->adj_resultRelInfo, false);

                    start_vertex->adj_elemTupleSlot = table_slot_create(adj_rel, &estate->es_tupleTable); 
                }

                resultRelInfo = start_vertex->adj_resultRelInfo;
                elemTupleSlot = start_vertex->adj_elemTupleSlot;

                estate->es_result_relations = &resultRelInfo;

                ExecClearTuple(elemTupleSlot);
                cypher_target_node *node = (cypher_target_node *)list_nth(path->target_nodes, (i * 2) + 1);
                // get the next graphid for this vertex.
                elemTupleSlot->tts_values[0] = css->edge_ids[0][i];
                elemTupleSlot->tts_isnull[0] = false;
            
                elemTupleSlot->tts_values[1] = css->vertex_ids[0][node->dir == CYPHER_REL_DIR_RIGHT ? i : i + 1];
                elemTupleSlot->tts_isnull[1] = false;
                    
                elemTupleSlot->tts_values[2] = css->vertex_ids[0][node->dir == CYPHER_REL_DIR_RIGHT ? i + 1 : i];
                elemTupleSlot->tts_isnull[2] = false;


                if (node->prop_attr_num == InvalidAttrNumber) {
                    elemTupleSlot->tts_values[3] = NULL;
                    elemTupleSlot->tts_isnull[3] = true;
                } else {
                    elemTupleSlot->tts_values[3] = slot->tts_values[node->prop_attr_num - 1];
                    elemTupleSlot->tts_isnull[3] = slot->tts_isnull[node->prop_attr_num - 1];
                }

                // Insert the new vertex
                insert_entity_tuple(resultRelInfo, elemTupleSlot, estate);

                if (node->tuple_position != InvalidAttrNumber) {
                    slot->tts_values[node->tuple_position - 1] = EDGE_GET_DATUM(create_edge(
                            css->edge_ids[0][i],
                            css->vertex_ids[0][node->dir == CYPHER_REL_DIR_RIGHT ? i : i + 1],
                            css->vertex_ids[0][node->dir == CYPHER_REL_DIR_RIGHT ? i + 1 : i],
                            css->graph_oid,
                            elemTupleSlot->tts_isnull[3] ?  NULL : DATUM_GET_GTYPE_P(elemTupleSlot->tts_values[3])));
                    slot->tts_isnull[node->tuple_position - 1] = false;
                }

                if(start_vertex->flags & EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE){
                    ExecCloseIndices(start_vertex->adj_resultRelInfo);
                    table_close(start_vertex->adj_resultRelInfo->ri_RelationDesc, RowExclusiveLock);
                }
            }
            
            // restore the old result relation info
            estate->es_result_relations = old_estate_es_result_relations_info;
        }

}

/*
 * Function that handles the case where MERGE is the only clause in the query.
 */
static void process_simple_merge(CustomScanState *node)
{
    cypher_merge_custom_scan_state *css =
        (cypher_merge_custom_scan_state *)node;
    EState *estate = css->css.ss.ps.state;

    RollbackCmdId(estate);
    TupleTableSlot *slot = ExecProcNode(node->ss.ps.lefttree);
    AdvanceCmdId(estate);

    if (TupIsNull(slot))
    {
        ExprContext *econtext = node->ss.ps.ps_ExprContext;

        /* setup the scantuple that the process_path needs */
        econtext->ecxt_scantuple = node->ss.ps.lefttree->ps_ProjInfo->pi_state.resultslot;
        //csnode->ss.ps.lefttree->ps_ProjInfo->pi_exprContext->ecxt_scantuple;// node->ss.ps.lefttree->ps_ResultTupleSlot;
        //econtext->ecxt_scantuple->tts_isempty = false;

        process_path(css);
    }
}

/*
 * Iterate through the TupleTableSlot's tts_values and marks the isnull field
 * with true.
 */
static void mark_tts_isnull(TupleTableSlot *slot)
{
    int numberOfAttributes = slot->tts_tupleDescriptor->natts;
    int i;

    for (i = 0; i < numberOfAttributes; i++)
    {
        Datum val;

        val = slot->tts_values[i];

        if (val == (Datum)NULL)
        {
            slot->tts_isnull[i] = true;
        }
    }
}

/*
 * Function that is called mid-execution. This function will call
 * its subtree in the execution tree, and depending on the results
 * create the new path, and depending on the the context of the MERGE
 * within the query pass data to the parent execution node.
 *
 * Returns a TupleTableSlot with the next tuple to it parent or
 * Returns NULL when MERGE has no more tuples to emit.
 */
static TupleTableSlot *exec_cypher_merge(CustomScanState *node)
{
    cypher_merge_custom_scan_state *css =
        (cypher_merge_custom_scan_state *)node;
    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    TupleTableSlot *slot;
    bool terminal = CYPHER_CLAUSE_IS_TERMINAL(css->flags);

    /*
     * There are three cases that dictate the flow of the execution logic.
     *
     * 1. MERGE is not the first clause in the cypher query.
     * 2. MERGE is the first clause and there are no following clauses.
     * 3. MERGE is the first clause and there are following clauses.
     * CYPHER_CLAUSE_FLAG_PREVIOUS_CLAUSE
     */
    if (CYPHER_CLAUSE_HAS_PREVIOUS_CLAUSE(css->flags))
    {
        /*
         * Case 1: MERGE is not the first clause in the cypher query.
         *
         * For this case, we need to process all tuples give to us by the
         * previous clause. When we receive a tuple from the previous clause:
         * check to see if the left lateral join found the pattern already. If
         * it did, we don't need to create the pattern. If the lateral join did
         * not find the whole path, create the whole path.
         *
         * If this is a terminal clause, process all tuples. If not, pass the
         * tuple to up the execution tree.
         */
        do
        {
            /*Process the subtree first */
            RollbackCmdId(estate);
            slot = ExecProcNode(node->ss.ps.lefttree);
            AdvanceCmdId(estate);

            /*
             * We are done processing the subtree, mark as terminal
             * so the function returns NULL.
             */
            if (TupIsNull(slot))
            {
                terminal = true;
                break;
            }

            /* setup the scantuple that the process_path needs */
            econtext->ecxt_scantuple =
                node->ss.ps.lefttree->ps_ProjInfo->pi_exprContext->ecxt_scantuple;

            if (check_path(css, econtext->ecxt_scantuple))
            {
                process_path(css);
            }

        } while (terminal);

        /* if this was a terminal MERGE just return NULL */
        if (terminal)
            return NULL;

        econtext->ecxt_scantuple = ExecProject(node->ss.ps.lefttree->ps_ProjInfo);
        return ExecProject(node->ss.ps.ps_ProjInfo);
    }
    else if (terminal)
    {
        /*
         * Case 2: MERGE is the first clause and there are no following clauses
         *
         * For case 2, check to see if we found the pattern, if not create it.
         * Return NULL in either cases, because no rows are expected.
         */
        process_simple_merge(node);

        /*
         * Case 2 always returns NULL the first time exec_cypher_merge is
         * called.
         */
        return NULL;
    }
    else
    {
        /*
         * Case 3: MERGE is the first clause and there are following clauses.
         *
         * Case three has two subcases:
         *
         * 1. The already path exists.
         * 2. The path does not exist.
         */

        /*
         * Part of Case 2.
         *
         * If created_new_path is marked as true. The path did not exist and
         * MERGE created it. We have already passed that information up the
         * execution tree, and now we tell MERGE's parents in the execution
         * tree there is no more tuples to pass.
         */
        if (css->created_new_path)
        {
            /*
             * If the created_new_path is set to true. Then MERGE should not
             * have found a path, because this should only be set to true if
             * merge found sub-case 1
             */
            Assert(css->found_a_path == false);

            return NULL;
        }

        /*
         * Process the subtree. The subtree will only consist of the MERGE
         * path.
         */
        RollbackCmdId(estate);
        slot = ExecProcNode(node->ss.ps.lefttree);
        AdvanceCmdId(estate);

        if (!TupIsNull(slot))
        {
            /*
             * Part of Sub-Case 1.
             *
             * If we found a path, mark the found_a_path flag to true and
             * pass the tuple to the next execution tree. When the path
             * exists, we don't need to create/modify anything.
             */
            css->found_a_path = true;

            return node->ss.ps.lefttree->ps_ResultTupleSlot;
        }
        else if (TupIsNull(slot) && css->found_a_path)
        {
            /*
             * Part of Sub-Case 2.
             *
             * MERGE found the path(s) that already exists and we are done passing
             * all the found path(s) up the execution tree.
             */
            return NULL;
        }
        else
        {
            /*
             * Part of Sub-Case 1.
             *
             * MERGE could not find the path in memory and the sub-node in the
             * execution tree returned NULL. We need to create the path and
             * pass the tuple to the next execution node. The subtrees will
             * begin its cleanup process when there are no more tuples found.
             * So we will need to create a TupleTableSlot and populate with the
             * information from the newly created path that the query needs.
             */
            ExprContext *econtext = node->ss.ps.ps_ExprContext;
            SubqueryScanState *sss = (SubqueryScanState *)node->ss.ps.lefttree;
            HeapTuple heap_tuple;

            /*
             * Our child execution node is always a subquery. If not there
             * is an issue.
             */
            Assert(IsA(sss, SubqueryScanState));

            /*
             * found_a_path should only be set to true if MERGE is following
             * sub-case 2.
             */
            Assert(css->found_a_path == false);

            /*
             * This block of sub-case 1 should only be exectuted once. To
             * create the single path if the path does not exist. If we find
             * ourselves here again, the internal state of the MERGE execution
             * node was incorrectly altered.
             */
            Assert(css->created_new_path == false);

            /*
             *  Postgres cleared the child tuple table slot, we need to remake
             *  it.
             */
            ExecInitScanTupleSlot(estate, &sss->ss,
                                  ExecGetResultType(sss->subplan),&TTSOpsHeapTuple);


            /* setup the scantuple that the process_path needs */
            econtext->ecxt_scantuple = sss->ss.ss_ScanTupleSlot;

            // create the path
            process_path(css);

            // mark the create_new_path flag to true.
            css->created_new_path = true;

            /*
             *  find the tts_values that process_path did not populate and
             *  mark as null.
             */
            mark_tts_isnull(econtext->ecxt_scantuple);

            // create the physical heap tuple
            /*heap_tuple = heap_form_tuple(
                                econtext->ecxt_scantuple->tts_tupleDescriptor,
                                econtext->ecxt_scantuple->tts_values,
                                econtext->ecxt_scantuple->tts_isnull);
*/
 
            // store the heap tuble
            ExecStoreVirtualTuple(econtext->ecxt_scantuple);

            /*
             * make the subquery's projection scan slot be the tuple table we
             * created and run the projection logic.
             */
            sss->ss.ps.ps_ProjInfo->pi_exprContext->ecxt_scantuple =
                                                        econtext->ecxt_scantuple;

            // assign this to be our scantuple
            econtext->ecxt_scantuple = ExecProject(node->ss.ps.lefttree->ps_ProjInfo);

            /*
             *  run the merge's projection logic and pass to its parent
             *  execution node
             */
            return ExecProject(node->ss.ps.ps_ProjInfo);
        }
    }
}


/*
 * Function called at the end of the execution phase to cleanup
 * MERGE.
 */
static void end_cypher_merge(CustomScanState *node)
{
    cypher_merge_custom_scan_state *css =
        (cypher_merge_custom_scan_state *)node;
    cypher_create_path *path = css->path;

    // increment the command counter
    CommandCounterIncrement();

    ExecEndNode(node->ss.ps.lefttree);

    ListCell *lc;
    //foreach (lc, css->pattern) {
        ListCell *lc2;

        foreach (lc2, path->target_nodes) {
            cypher_target_node *cypher_node = (cypher_target_node *)lfirst(lc2);

            if (!cypher_node->resultRelInfo || node->flags & EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE)
                continue;

            ExecCloseIndices(cypher_node->resultRelInfo);
            table_close(cypher_node->rel, RowExclusiveLock);
            ExecCloseIndices(cypher_node->adj_resultRelInfo);
            table_close(cypher_node->adj_rel, RowExclusiveLock);
            //RelationDecrementReferenceCount(cypher_node->relation);
            //RelationDecrementReferenceCount(lcd->relation);
        }
    //}
}

/*
 * Rescan is mostly used by join execution nodes, and several others.
 * Since we are creating data here its not safe to rescan the node. Throw
 * an error and try to help the uer understand what went wrong.
 */
static void rescan_cypher_merge(CustomScanState *node)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cypher merge clause cannot be rescaned"),
                    errhint("its unsafe to use joins in a query with a Cypher MERGE clause")));
}

/*
 * Extracts the metadata information that MERGE needs from the
 * merge_custom_scan node and creates the cypher_merge_custom_scan_state
 * for the execution phase.
 */
Node *create_cypher_merge_plan_state(CustomScan *cscan)
{
    cypher_merge_custom_scan_state *cypher_css =
        palloc0(sizeof(cypher_merge_custom_scan_state));
    cypher_merge_information *merge_information;
    char *serialized_data;
    Const *c;

    cypher_css->cs = cscan;

    // get the serialized data structure from the Const and deserialize it.
    c = linitial(cscan->custom_private);
    serialized_data = (char *)c->constvalue;
    merge_information = stringToNode(serialized_data);

    Assert(is_ag_node(merge_information, cypher_merge_information));

    cypher_css->merge_information = merge_information;
    cypher_css->flags = merge_information->flags;
    cypher_css->merge_function_attr = merge_information->merge_function_attr;
    cypher_css->path = merge_information->path;
    cypher_css->created_new_path = false;
    cypher_css->found_a_path = false;
    cypher_css->graph_oid = merge_information->graph_oid;

    cypher_css->css.ss.ps.type = T_CustomScanState;
    cypher_css->css.methods = &cypher_merge_exec_methods;

    return (Node *)cypher_css;
}

