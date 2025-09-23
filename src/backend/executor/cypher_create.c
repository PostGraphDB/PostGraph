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

typedef struct cypher_create_custom_scan_state
{
    CustomScanState css;
    CustomScan *cs;
    List *pattern;
    List *path_values;
    uint32 flags;
    TupleTableSlot *slot;
    TupleTableSlot *slot_adj_relid;
    Oid graph_oid;
    graphid **vertex_ids;
    graphid **edge_ids;
} cypher_create_custom_scan_state;

static HeapTuple insert_entity_tuple(ResultRelInfo *resultRelInfo,
                              TupleTableSlot *elemTupleSlot,
                              EState *estate);\
static void begin_cypher_create(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *exec_cypher_create(CustomScanState *node);
static void end_cypher_create(CustomScanState *node);
static void rescan_cypher_create(CustomScanState *node);

const CustomExecMethods cypher_create_exec_methods = {CREATE_SCAN_STATE_NAME,
                                                      begin_cypher_create,
                                                      exec_cypher_create,
                                                      end_cypher_create,
                                                      rescan_cypher_create,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL};

static void
append_to_buffer(StringInfo buffer, const char *data, int len) {
    int offset = reserve_from_buffer(buffer, len);
    memcpy(buffer->data + offset, data, len);
}


static Datum create_traversal(List *entities) {
    ListCell *lc;
    StringInfoData buffer;
    initStringInfo(&buffer);

    // header
    reserve_from_buffer(&buffer, VARHDRSZ);

    // length
    reserve_from_buffer(&buffer, sizeof(pentry));

    foreach(lc, entities) {
	Datum d = lfirst(lc);
        append_to_buffer(&buffer, d, VARSIZE_ANY(d));
    }

    traversal *p = (traversal *)buffer.data;

    p->children[0] = list_length(entities);
    SET_VARSIZE(p, buffer.len);

    return TRAVERSAL_GET_DATUM(p);
}

#include "commands/label_commands.h"

static void begin_cypher_create(CustomScanState *node, EState *estate, int eflags) {
    cypher_create_custom_scan_state *css = (cypher_create_custom_scan_state *)node;
    ListCell *lc;
    Plan *subplan;

    Assert(list_length(css->cs->custom_plans) == 1);

    subplan = linitial(css->cs->custom_plans);
    node->ss.ps.lefttree = ExecInitNode(subplan, estate, eflags);

    ExecAssignExprContext(estate, &node->ss.ps);

    ExecInitScanTupleSlot(estate, &node->ss,
                          ExecGetResultType(node->ss.ps.lefttree),
                          &TTSOpsHeapTuple);

    
    if (!CYPHER_CLAUSE_IS_TERMINAL(css->flags)) {
        TupleDesc tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

        ExecAssignProjectionInfo(&node->ss.ps, tupdesc);
    }
    

    if (list_length(css->pattern) != 1)
        ereport(ERROR, (errmsg_internal("executor create found a multi pattern")));

    cypher_create_path *path = linitial(css->pattern);

    css->vertex_ids = palloc(sizeof(graphid *) * list_length(css->pattern));
    css->vertex_ids[0] = palloc(sizeof(graphid) * ((list_length(path->target_nodes)/ 2) + 1) );

    if (path->target_nodes) {
        css->edge_ids = palloc(sizeof(graphid *) * list_length(css->pattern));
        css->edge_ids[0] = palloc(sizeof(graphid) * ((list_length(path->target_nodes) / 2) ) );
    }

    ListCell *lc1;
    int i = 0;
    foreach (lc1, path->target_nodes) {
        cypher_target_node *cypher_node = (cypher_target_node *)lfirst(lc1);

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

static TupleTableSlot *exec_cypher_create(CustomScanState *csnode)
{
    cypher_create_custom_scan_state *css = (cypher_create_custom_scan_state *)csnode;

    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;

    while (true) {
        RollbackCmdId(estate);
        TupleTableSlot *return_slot = ExecProcNode(csnode->ss.ps.lefttree);
        AdvanceCmdId(estate);

        if (TupIsNull(return_slot))
            return return_slot;

        econtext->ecxt_scantuple =
                csnode->ss.ps.lefttree->ps_ProjInfo->pi_exprContext->ecxt_scantuple;
        TupleTableSlot *slot = econtext->ecxt_scantuple;

        if (list_length(css->pattern) != 1)
            ereport(ERROR, (errmsg_internal("executor create found a multi pattern")));

        cypher_create_path *path = linitial(css->pattern);

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
                    elemTupleSlot->tts_values[1] = slot->tts_values[node->prop_attr_num - 1];
                    elemTupleSlot->tts_isnull[1] = slot->tts_isnull[node->prop_attr_num - 1];
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

        if(!CYPHER_CLAUSE_IS_TERMINAL(css->flags)) {
            econtext->ecxt_scantuple = ExecProject(csnode->ss.ps.lefttree->ps_ProjInfo);
            return ExecProject(csnode->ss.ps.ps_ProjInfo);
        }

    }
}

static void end_cypher_create(CustomScanState *node)
{
    cypher_create_custom_scan_state *css =
        (cypher_create_custom_scan_state *)node;

    CommandCounterIncrement();

    ExecEndNode(node->ss.ps.lefttree);

    ListCell *lc;
    foreach (lc, css->pattern) {
        cypher_create_path *path = lfirst(lc);
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
    }
}

static void rescan_cypher_create(CustomScanState *node)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cypher create clause cannot be rescaned"),
                    errhint("its unsafe to use joins in a query with a Cypher CREATE clause")));
}

Node *create_cypher_create_plan_state(CustomScan *cscan)
{
    cypher_create_custom_scan_state *cypher_css =
        palloc0(sizeof(cypher_create_custom_scan_state));
    cypher_create_target_nodes *target_nodes;
    char *serialized_data;
    Const *c;

    cypher_css->cs = cscan;

    // get the serialized data structure from the Const and deserialize it.
    c = linitial(cscan->custom_private);
    serialized_data = (char *)c->constvalue;
    target_nodes = stringToNode(serialized_data);

    Assert(is_ag_node(target_nodes, cypher_create_target_nodes));

    cypher_css->path_values = NIL;
    cypher_css->pattern = target_nodes->paths;
    cypher_css->flags = target_nodes->flags;
    cypher_css->graph_oid = target_nodes->graph_oid;

    cypher_css->css.ss.ps.type = T_CustomScanState;
    cypher_css->css.methods = &cypher_create_exec_methods;

    return (Node *)cypher_css;
}

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
