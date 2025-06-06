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
#include "postgraph.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/objectaddress.h"
#include "commands/defrem.h"
#include "commands/schemacmds.h"
#include "commands/tablecmds.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "parser/parser.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/builtins.h"

#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "commands/label_commands.h"
#include "utils/graphid.h"

/*
 * Schema name doesn't have to be graph name but the same name is used so
 * that users can find the backed schema for a graph only by its name.
 */
#define gen_graph_namespace_name(graph_name) (graph_name)

static Oid create_schema_for_graph(const char *graph_name);
static void drop_schema_for_graph(char *graph_name_str, const bool cascade);
static void remove_schema(Node *schema_name, DropBehavior behavior);
static void rename_graph(const Name graph_name, const Name new_name);

PG_FUNCTION_INFO_V1(create_graph_if_not_exists);

Datum create_graph_if_not_exists(PG_FUNCTION_ARGS)
{
    char *graph;
    Name graph_name;
    char *graph_name_str;
    Oid nsp_id;

    if (PG_ARGISNULL(0))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    }
    graph_name = PG_GETARG_NAME(0);

    graph_name_str = NameStr(*graph_name);
    if (graph_exists(graph_name_str))
    {
        PG_RETURN_VOID();
    }

    nsp_id = create_schema_for_graph(graph_name);

    insert_graph(graph_name, nsp_id);

    //Increment the Command counter before create the generic labels.
    CommandCounterIncrement();

    //Create the default label tables
    graph = graph_name->data;
    create_label(graph, AG_DEFAULT_LABEL_VERTEX, LABEL_TYPE_VERTEX, NIL, NULL);
    create_label(graph, AG_DEFAULT_LABEL_EDGE, LABEL_TYPE_EDGE, NIL, NULL);

    ereport(NOTICE,
            (errmsg("graph \"%s\" has been created", NameStr(*graph_name))));

    PG_RETURN_VOID();
}


// Function updates graph name in ag_graph table.
void update_session_graph_oid(const Name graph_oid)
{
    ScanKeyData scan_keys[1];
    Relation ag_graph;
    SysScanDesc scan_desc;
    HeapTuple cur_tuple;
    Datum repl_values[2];
    bool repl_isnull[2];
    bool do_replace[2];
    HeapTuple new_tuple;

    // open and scan ag_graph for graph name
    ScanKeyInit(&scan_keys[0], 1, BTEqualStrategyNumber,
                F_OIDEQ, Int32GetDatum(MyProcPid));

    ag_graph = table_open(session_graph_use(), RowExclusiveLock);
    scan_desc = systable_beginscan(ag_graph, session_graph_use_index(), true,
                                   NULL, 1, scan_keys);

    cur_tuple = systable_getnext(scan_desc);

    // modify (which creates a new tuple) the current tuple's graph name
    MemSet(repl_values, 0, sizeof(repl_values));
    MemSet(repl_isnull, false, sizeof(repl_isnull));
    MemSet(do_replace, false, sizeof(do_replace));

    repl_values[0] = Int32GetDatum(MyProcPid);
    repl_isnull[0] = false;
    do_replace[0] = true;

    repl_values[1] = Int32GetDatum(graph_oid);
    repl_isnull[1] = false;
    do_replace[1] = true;

    if (!HeapTupleIsValid(cur_tuple)) {
        new_tuple = heap_form_tuple(RelationGetDescr(ag_graph), repl_values, repl_isnull);
        CatalogTupleInsert(ag_graph, new_tuple);
    } else {
        new_tuple = heap_modify_tuple(cur_tuple, RelationGetDescr(ag_graph),
                                     repl_values, repl_isnull, do_replace);

        // update the current tuple with the new tuple
        CatalogTupleUpdate(ag_graph, &cur_tuple->t_self, new_tuple);
    }

    // end scan and close ag_graph
    systable_endscan(scan_desc);
    table_close(ag_graph, RowExclusiveLock);
}

PG_FUNCTION_INFO_V1(use_graph);

Datum use_graph(PG_FUNCTION_ARGS)
{
    char *graph;
    Name graph_name;
    char *graph_name_str;
    Oid nsp_id;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));

    graph_name_str =  TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!graph_exists(graph_name_str))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" already exists", graph_name_str)));

    //Increment the Command counter before create the generic labels.
    CommandCounterIncrement();

    update_session_graph_oid(get_graph_oid(graph_name_str));
    //PopActiveSnapshot();
    
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(create_graph);

Datum create_graph(PG_FUNCTION_ARGS)
{
    char *graph;
    Name graph_name;
    char *graph_name_str;
    Oid nsp_id;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));

    /*
    graph_name = PG_GETARG_NAME(0);

    graph_name_str = NameStr(*graph_name);*/
    graph_name_str =  TextDatumGetCString(PG_GETARG_DATUM(0));
    if (graph_exists(graph_name_str))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" already exists", graph_name_str)));

    nsp_id = create_schema_for_graph(graph_name_str);

    insert_graph(graph_name_str, nsp_id);

    //Increment the Command counter before create the generic labels.
    CommandCounterIncrement();

    //Create the default label tables
    graph = graph_name_str;//graph_name->data;
    create_label(graph, AG_DEFAULT_LABEL_VERTEX, LABEL_TYPE_VERTEX, NIL, NULL);
    create_label(graph, AG_DEFAULT_LABEL_EDGE, LABEL_TYPE_EDGE, NIL, NULL);

    ereport(NOTICE,
            (errmsg("graph \"%s\" has been created", graph_name_str)));

    //PopActiveSnapshot();
    
    PG_RETURN_VOID();
}

static Oid create_schema_for_graph(const char * graph_name)
{
    char *graph_name_str = graph_name;
    CreateSchemaStmt *schema_stmt;
    CreateSeqStmt *seq_stmt;
    TypeName *integer;
    DefElem *data_type;
    DefElem *maxvalue;
    DefElem *cycle;
    Oid nsp_id;

    /*
     * This is the same with running the following SQL statement.
     *
     * CREATE SCHEMA `graph_name`
     *   CREATE SEQUENCE `LABEL_ID_SEQ_NAME`
     *     AS integer
     *     MAXVALUE `LABEL_ID_MAX`
     *     CYCLE
     *
     * The sequence will be used to assign a unique id to a label in the graph.
     *
     * schemaname doesn't have to be graph_name but the same name is used so
     * that users can find the backed schema for a graph only by its name.
     *
     * ProcessUtilityContext of this command is PROCESS_UTILITY_SUBCOMMAND
     * so the event trigger will not be fired.
     */
    schema_stmt = makeNode(CreateSchemaStmt);
    schema_stmt->schemaname = gen_graph_namespace_name(graph_name_str);
    schema_stmt->authrole = NULL;
    seq_stmt = makeNode(CreateSeqStmt);
    seq_stmt->sequence = makeRangeVar(graph_name_str, LABEL_ID_SEQ_NAME, -1);
    integer = SystemTypeName("int4");
    data_type = makeDefElem("as", (Node *)integer, -1);
    maxvalue = makeDefElem("maxvalue", (Node *)makeInteger(LABEL_ID_MAX), -1);
    cycle = makeDefElem("cycle", (Node *)makeInteger(true), -1);
    seq_stmt->options = list_make3(data_type, maxvalue, cycle);
    seq_stmt->ownerId = InvalidOid;
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;
    schema_stmt->schemaElts = list_make1(seq_stmt);
    schema_stmt->if_not_exists = false;
    nsp_id = CreateSchemaCommand(schema_stmt,
                                 "(generated CREATE SCHEMA command)", -1, -1);
    // CommandCounterIncrement() is called in CreateSchemaCommand()

    return nsp_id;
}

PG_FUNCTION_INFO_V1(drop_graph);

Datum drop_graph(PG_FUNCTION_ARGS)
{
    Name graph_name;
    char *graph_name_str;
    bool cascade;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
                        
    graph_name = PG_GETARG_NAME(0);
    cascade = PG_GETARG_BOOL(1);

    graph_name_str = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!graph_exists(graph_name_str))
    {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist", graph_name_str)));
    }

    drop_schema_for_graph(graph_name_str, cascade);

    delete_graph(graph_name_str);
    CommandCounterIncrement();

    ereport(NOTICE, (errmsg("graph \"%s\" has been dropped", graph_name_str)));

    PG_RETURN_VOID();
}

static void drop_schema_for_graph(char *graph_name_str, const bool cascade)
{
    DropStmt *drop_stmt;
    Value *schema_name;
    List *label_id_seq_name;
    DropBehavior behavior;

    /*
     * ProcessUtilityContext of commands below is PROCESS_UTILITY_SUBCOMMAND
     * so the event triggers will not be fired.
     */

    // DROP SEQUENCE `graph_name_str`.`LABEL_ID_SEQ_NAME`
    drop_stmt = makeNode(DropStmt);
    schema_name = makeString(get_graph_namespace_name(graph_name_str));
    label_id_seq_name = list_make2(schema_name, makeString(LABEL_ID_SEQ_NAME));
    drop_stmt->objects = list_make1(label_id_seq_name);
    drop_stmt->removeType = OBJECT_SEQUENCE;
    drop_stmt->behavior = DROP_RESTRICT;
    drop_stmt->missing_ok = false;
    drop_stmt->concurrent = false;

    RemoveRelations(drop_stmt);
    // CommandCounterIncrement() is called in RemoveRelations()

    // DROP SCHEMA `graph_name_str` [ CASCADE ]
    behavior = cascade ? DROP_CASCADE : DROP_RESTRICT;
    remove_schema((Node *)schema_name, behavior);
    // CommandCounterIncrement() is called in performDeletion()
}

// See RemoveObjects() for more details.
static void remove_schema(Node *schema_name, DropBehavior behavior)
{
    ObjectAddress address;
    Relation relation;

    address = get_object_address(OBJECT_SCHEMA, schema_name, &relation,
                                 AccessExclusiveLock, false);
    // since the target object is always a schema, relation is NULL
    Assert(!relation);

    if (!OidIsValid(address.objectId))
    {
        // missing_ok is always false

        /*
         * before calling this function, this condition is already checked in
         * drop_graph()
         */
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ag_graph catalog is corrupted"),
                        errhint("Schema \"%s\" does not exist",
                                strVal(schema_name))));
    }

    // removeType is always OBJECT_SCHEMA

    /*
     * Check permissions. Since the target object is always a schema, the
     * original logic is simplified.
     */
    check_object_ownership(GetUserId(), OBJECT_SCHEMA, address, schema_name,
                           NULL);

    // the target schema is not temporary

    // the target object is always a schema

    /*
     * set PERFORM_DELETION_INTERNAL flag so that object_access_hook can ignore
     * this deletion
     */
    performDeletion(&address, behavior, PERFORM_DELETION_INTERNAL);
}

PG_FUNCTION_INFO_V1(alter_graph);

/*
 * Function alter_graph, invoked by the sql function -
 * alter_graph(graph_name name, operation cstring, new_value name)
 * NOTE: Currently only RENAME is supported.
 *       graph_name and new_value are case sensitive.
 *       operation is case insensitive.
 */
Datum alter_graph(PG_FUNCTION_ARGS)
{
    Name graph_name;
    Name new_value;
    char *operation;

    if (PG_ARGISNULL(0))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_name must not be NULL")));
    }
    if (PG_ARGISNULL(1))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("operation must not be NULL")));
    }
    if (PG_ARGISNULL(2))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("new_value must not be NULL")));
    }

    graph_name = PG_GETARG_NAME(0);
    operation = PG_GETARG_CSTRING(1);
    new_value = PG_GETARG_NAME(2);

    if (strcasecmp("RENAME", operation) == 0)
    {
        rename_graph(graph_name, new_value);
    }
    else
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid operation \"%s\"", operation),
                        errhint("valid operations: RENAME")));
    }

    PG_RETURN_VOID();
}

/*
 * Function to rename a graph by renaming the schema (which is also the
 * namespace) and updating the name in ag_graph
 */
static void rename_graph(const Name graph_name, const Name new_name)
{
    char *oldname = NameStr(*graph_name);
    char *newname = NameStr(*new_name);
    char *schema_name;

    /*
     * ProcessUtilityContext of this command is PROCESS_UTILITY_SUBCOMMAND
     * so the event trigger will not be fired.
     *
     * CommandCounterIncrement() does not have to be called after this.
     *
     * NOTE: If graph_name and schema_name are decoupled, this operation does
     *       not required.
     */
    schema_name = get_graph_namespace_name(oldname);
    RenameSchema(schema_name, newname);

    update_graph_name(graph_name, new_name);
    CommandCounterIncrement();

    ereport(NOTICE,
            (errmsg("graph \"%s\" renamed to \"%s\"", oldname, newname)));
}

// returns a list containing the name of every graph in the database
List *get_graphnames(void)
{
    TupleTableSlot *slot;
    Relation ag_graph;
    SysScanDesc scan_desc;
    HeapTuple tuple;
    List *graphnames = NIL;
    char *str;

    ag_graph = table_open(ag_graph_relation_id(), RowExclusiveLock);
    scan_desc = systable_beginscan(ag_graph, ag_graph_name_index_id(), true,
                                   NULL, 0, NULL);

    slot = MakeTupleTableSlot(RelationGetDescr(ag_graph), &TTSOpsHeapTuple);

    for (;;)
    {
        tuple = systable_getnext(scan_desc);
        if (!HeapTupleIsValid(tuple))
            break;

        ExecClearTuple(slot);
        ExecStoreHeapTuple(tuple, slot, false);

        slot_getallattrs(slot);

        str = DatumGetCString(slot->tts_values[Anum_ag_graph_name - 1]);
        graphnames = lappend(graphnames, str);
    }

    ExecDropSingleTupleTableSlot(slot);
    systable_endscan(scan_desc);
    table_close(ag_graph, RowExclusiveLock);

    return graphnames;
}

// deletes all the graphs in the list.
void drop_graphs(List *graphnames)
{
    ListCell *lc;

    foreach(lc, graphnames)
    {
        char *graphname = lfirst(lc);

        DirectFunctionCall2(
            drop_graph, CStringGetDatum(graphname), BoolGetDatum(true));
    }
}
