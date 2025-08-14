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

/*
 * When executing the children of the CREATE, SET, REMOVE, and
 * DELETE clasues, we need to alter the command id in the estate
 * and the snapshot. That way we can hide the modified tuples from
 * the sub clauses that should not know what their parent clauses are
 * doing.
 */
#define Increment_Estate_CommandId(estate) \
    estate->es_output_cid++; \
    estate->es_snapshot->curcid++;

#define Decrement_Estate_CommandId(estate) \
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
                              EState *estate);
static bool entity_exists(EState *estate, Oid graph_oid, graphid id);
static void begin_cypher_create(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *exec_cypher_create(CustomScanState *node);
static void end_cypher_create(CustomScanState *node);
static void rescan_cypher_create(CustomScanState *node);

static void insert_edge(cypher_create_custom_scan_state *css,
                        cypher_target_node *node, Datum prev_vertex_id,
                        ListCell *next, List *list);

static void insert_vertex(cypher_create_custom_scan_state *css,
                           cypher_target_node *node, ListCell *next, List *list);

static void process_pattern(cypher_create_custom_scan_state *css);


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

    /*
    if (!CYPHER_CLAUSE_IS_TERMINAL(css->flags)) {
        TupleDesc tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

        ExecAssignProjectionInfo(&node->ss.ps, tupdesc);
    }
    */

    if (list_length(css->pattern) != 1)
        ereport(ERROR, (errmsg_internal("executor create found a multi pattern")));

    cypher_create_path *path = linitial(css->pattern);



    css->vertex_ids = palloc(sizeof(graphid *) * list_length(css->pattern));
    css->vertex_ids[0] = palloc(sizeof(graphid) * ((list_length(path->target_nodes)/ 2) + 1) );

    if (path->target_nodes) {
        css->edge_ids = palloc(sizeof(graphid *) * list_length(css->pattern));
        css->edge_ids[0] = palloc(sizeof(graphid) * ((list_length(path->target_nodes)/ 2) ) );
    }   

    ListCell *lc1;
    int i = 0;
    foreach (lc1, path->target_nodes) {
        cypher_target_node *cypher_node = (cypher_target_node *)lfirst(lc1);

        Assert(cypher_node->id_expr);
        cypher_node->id_expr_state = ExecInitExpr(cypher_node->id_expr, (PlanState *)node);

        if (i % 2 == 1) {
            i++;
            continue;
        }
        
        Relation rel = table_open(cypher_node->relid, RowExclusiveLock);
        cypher_node->resultRelInfo = makeNode(ResultRelInfo);
        InitResultRelInfo(cypher_node->resultRelInfo, rel, list_length(estate->es_range_table), NULL, estate->es_instrument);
        
        // Open all indexes for the relation
        ExecOpenIndices(cypher_node->resultRelInfo, false);
    
        // Setup the relation's tuple slot
        cypher_node->elemTupleSlot = table_slot_create(rel, &estate->es_tupleTable); 

        Relation adj_rel = table_open(cypher_node->adj_relid, RowExclusiveLock);
        cypher_node->adj_resultRelInfo = makeNode(ResultRelInfo);
        InitResultRelInfo(cypher_node->adj_resultRelInfo, adj_rel, list_length(estate->es_range_table), NULL, estate->es_instrument);
        
        // Open all indexes for the relation
        ExecOpenIndices(cypher_node->adj_resultRelInfo, false);
    

        // Setup the relation's tuple slot
        cypher_node->adj_elemTupleSlot = table_slot_create(adj_rel, &estate->es_tupleTable); 

        i++;

    }

    if (estate->es_output_cid == 0)
        estate->es_output_cid = estate->es_snapshot->curcid;

    Increment_Estate_CommandId(estate);
}

/*
 * CREATE the vertices and edges for a CREATE clause pattern.
 */
static void process_pattern(cypher_create_custom_scan_state *css)
{
    ListCell *lc2;

    foreach (lc2, css->pattern)
    {
        cypher_create_path *path = lfirst(lc2);

        ListCell *lc = list_head(path->target_nodes);

        /*
         * Create the first vertex. The create_vertex function will
         * create the rest of the path, if necessary.
         */
        insert_vertex(css, lfirst(lc), lnext(path->target_nodes, lc), path->target_nodes);

        /*
         * If this path is a variable, take the list that was accumulated
         * in the vertex/edge creation, create a path datum, and add to the
         * scantuple slot.
         */
        if (path->path_attr_num != InvalidAttrNumber)
        {
            TupleTableSlot *scantuple;
            PlanState *ps;
            Datum result;

            ps = css->css.ss.ps.lefttree;
            scantuple = ps->ps_ExprContext->ecxt_scantuple;

            result = create_traversal(css->path_values);
            scantuple->tts_values[path->path_attr_num - 1] = result;
            scantuple->tts_isnull[path->path_attr_num - 1] = false;
        }

        css->path_values = NIL;
    }
}

static TupleTableSlot *exec_cypher_create(CustomScanState *csnode)
{
    cypher_create_custom_scan_state *css = (cypher_create_custom_scan_state *)csnode;

    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;

    TupleTableSlot *scanTupleSlot = econtext->ecxt_scantuple;

    if (list_length(css->pattern) != 1)
        ereport(ERROR, (errmsg_internal("executor create found a multi pattern")));

    cypher_create_path *path = linitial(css->pattern);

    ListCell *lc;
    int i = 1;
    foreach(lc, path->target_nodes) {
        cypher_target_node *node = (cypher_target_node *)lfirst(lc);
        bool isNull;
        if(i % 2 == 1)
            css->vertex_ids[0][i/2] = ExecEvalExpr(node->id_expr_state, econtext, &isNull);
        else {
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

        ExecClearTuple(elemTupleSlot);

        // get the next graphid for this vertex.
        elemTupleSlot->tts_values[0] = css->vertex_ids[0][i];
        elemTupleSlot->tts_isnull[0] = false;

        // get the properties for this vertex
        elemTupleSlot->tts_values[1] = NULL;
        elemTupleSlot->tts_isnull[1] = true;
        // Insert the new vertex
        insert_entity_tuple(resultRelInfo, elemTupleSlot, estate);

        if (list_length(path->target_nodes) > 1 && i < (list_length(path->target_nodes)/2)) {
            resultRelInfo = node->adj_resultRelInfo;
            elemTupleSlot = node->adj_elemTupleSlot;

            estate->es_result_relations = &resultRelInfo;

            ExecClearTuple(elemTupleSlot);

            // get the next graphid for this vertex.
            elemTupleSlot->tts_values[0] = css->edge_ids[0][i-1];
            elemTupleSlot->tts_isnull[0] = false;

            elemTupleSlot->tts_values[1] = css->vertex_ids[0][i];
            elemTupleSlot->tts_isnull[1] = false;
            
            elemTupleSlot->tts_values[2] = css->vertex_ids[0][i+1];
            elemTupleSlot->tts_isnull[2] = false;
            // get the properties for this vertex
            elemTupleSlot->tts_values[3] = NULL;
            elemTupleSlot->tts_isnull[3] = true;

            // Insert the new vertex
            insert_entity_tuple(resultRelInfo, elemTupleSlot, estate);
        }
        /* restore the old result relation info */
        estate->es_result_relations = old_estate_es_result_relations_info;
    }

    return NULL;
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

            if (!cypher_node->resultRelInfo)
                continue;

            ExecCloseIndices(cypher_node->resultRelInfo);
            table_close(cypher_node->resultRelInfo->ri_RelationDesc, RowExclusiveLock);
            ExecCloseIndices(cypher_node->adj_resultRelInfo);
            table_close(cypher_node->adj_resultRelInfo->ri_RelationDesc, RowExclusiveLock);



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
 * Creates the vertex entity, returns the vertex's id in case the caller is
 * the create_edge function.
 */
static void insert_vertex(cypher_create_custom_scan_state *css,
                           cypher_target_node *node, ListCell *next, List *list)
{
    bool isNull;
    Datum id;
    EState *estate = css->css.ss.ps.state;
    ExprContext *econtext = css->css.ss.ps.ps_ExprContext;
    ResultRelInfo *resultRelInfo = node->resultRelInfo;
    TupleTableSlot *elemTupleSlot = node->elemTupleSlot;
    TupleTableSlot *scanTupleSlot = econtext->ecxt_scantuple;

    /*
     * Vertices in a path might already exists. If they do get the id
     * to pass to the edges before and after it. Otherwise, insert the
     * new vertex into it's table and then pass the id along.
     */
    ResultRelInfo **old_estate_es_result_relations_info = NULL;

    old_estate_es_result_relations_info = estate->es_result_relations;

    estate->es_result_relations = &resultRelInfo;

    ExecClearTuple(elemTupleSlot);

    // get the next graphid for this vertex.
    id = ExecEvalExpr(node->id_expr_state, econtext, &isNull);
    elemTupleSlot->tts_values[0] = id;
    elemTupleSlot->tts_isnull[0] = isNull;

    // get the properties for this vertex
    elemTupleSlot->tts_values[1] = NULL;
    elemTupleSlot->tts_isnull[1] = true;
        
    // Insert the new vertex
    insert_entity_tuple(resultRelInfo, elemTupleSlot, estate);

    /* restore the old result relation info */
    estate->es_result_relations = old_estate_es_result_relations_info;
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
   // tuple = ExecFetchSlotHeapTuple(elemTupleSlot, true, NULL);

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
        ExecInsertIndexTuples(resultRelInfo, elemTupleSlot, estate, false, false, NULL, NIL);
    }

    return NULL;
}


/*
 * Find out if the entity still exists. This is for 'implicit' deletion
 * of an entity.
 */
bool entity_exists(EState *estate, Oid graph_oid, graphid id)
{
    label_cache_data *label;
    ScanKeyData scan_keys[1];
    TableScanDesc scan_desc;
    HeapTuple tuple;
    Relation rel;
    bool result = true;

    /*
     * Extract the label id from the graph id and get the table name
     * the entity is part of.
     */
    label = search_label_graph_oid_cache(graph_oid, GET_LABEL_ID(id));

    // Setup the scan key to be the graphid
    ScanKeyInit(&scan_keys[0], 1, BTEqualStrategyNumber,
                F_GRAPHIDEQ, GRAPHID_GET_DATUM(id));

    rel = table_open(label->relation, RowExclusiveLock);
    scan_desc = table_beginscan(rel, estate->es_snapshot, 1, scan_keys);

    tuple = heap_getnext(scan_desc, ForwardScanDirection);

    /*
     * If a single tuple was returned, the tuple is still valid, otherwise'
     * set to false.
     */
    if (!HeapTupleIsValid(tuple))
    {
        result = false;
    }

    table_endscan(scan_desc);
    table_close(rel, RowExclusiveLock);

    return result;
}

