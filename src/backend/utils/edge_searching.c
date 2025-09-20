
#include "postgres.h"

#include <math.h>

#include "access/table.h"
#include "access/tableam.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "portability/instr_time.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

#include "utils/graphid.h"
#include "utils/gtype.h"
#include "utils/edge.h"
#include "utils/variable_edge.h"
#include "utils/vector.h"
#include "utils/vertex.h"
#include "utils/gtype.h"
#include "utils/gtype_parser.h"
#include "utils/gtype_typecasting.h"
#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "utils/graphid.h"
#include "utils/numeric.h"

#include "access/vertex.h"
#include "utils/ag_cache.h"
#include "utils/hashset.h"

#include "ltree.h"

PG_FUNCTION_INFO_V1(retrieve_vertex);
Datum retrieve_vertex(PG_FUNCTION_ARGS) {
	Oid graph_oid = GT_ARG_TO_INT4_DATUM(0);
	int64 graphid = AG_GETARG_GRAPHID(1);
	bool isnull;

    label_cache_data *lcd = search_label_graph_oid_cache(graph_oid, (graphid >> ENTRY_ID_BITS));

	ScanKeyData scan_keys[1];
    ScanKeyInit(&scan_keys[0], 1, BTEqualStrategyNumber, F_OIDEQ, Int64GetDatum(graphid));

    Relation rel = table_open(lcd->relation, ShareLock);

    TableScanDesc scan_desc = table_beginscan(rel, GetActiveSnapshot(), 1, scan_keys);

	HeapTuple tuple;
    if (!HeapTupleIsValid(tuple = heap_getnext(scan_desc, ForwardScanDirection)))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("id %lu does not exist", graphid)));

	Datum properties = heap_getattr(tuple, 2, RelationGetDescr(rel), &isnull);

    table_endscan(scan_desc);
    table_close(rel, ShareLock);

	if (isnull) 
		PG_RETURN_NULL();
	
	AG_RETURN_GTYPE_P(properties);

}

typedef struct edge_search_cxt
{
    ScanKey scanKey;
	TableScanDesc scan_desc;
	Relation rel;
	TupleTableSlot *slot;
	List *label_filter;
} edge_search_cxt;

PG_FUNCTION_INFO_V1(edge_search);
Datum edge_search(PG_FUNCTION_ARGS)
{ 
	FuncCallContext *funcctx;
	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;
		TupleDesc tupdesc;
        graphid id = AG_GETARG_GRAPHID(1);



		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);



		tupdesc = CreateTemplateTupleDesc(4);

		TupleDescInitEntry(tupdesc, 1, "id", GRAPHIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "startid", GRAPHIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "endid", GRAPHIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "properties", GTYPEOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		edge_search_cxt *cxt= palloc(sizeof(edge_search_cxt));

    	label_cache_data *lcd = search_label_graph_oid_cache(GT_ARG_TO_INT4_DATUM(0), (id >> ENTRY_ID_BITS));

		cxt->scanKey = palloc(sizeof(ScanKeyData));		
		ScanKeyInit(cxt->scanKey, 1, BTEqualStrategyNumber, F_GRAPHIDEQ, GRAPHID_GET_DATUM(id));

		cxt->rel = table_open(lcd->vertex_adjlist, ShareLock);
		cxt->scan_desc = table_beginscan(cxt->rel, GetActiveSnapshot(), 1, cxt->scanKey);

		List *empty = NIL;
		cxt->slot = table_slot_create(cxt->rel, &empty);

        cxt->label_filter = NIL;
        if (!PG_ARGISNULL(2)) {
			cxt->label_filter = lappend_oid(cxt->label_filter, GT_ARG_TO_INT4_DATUM(2));
        }

		funcctx->user_fctx = cxt;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	edge_search_cxt *cxt = (edge_search_cxt *) funcctx->user_fctx;
	
	while (table_scan_getnextslot(cxt->scan_desc, ForwardScanDirection, cxt->slot)) {
		cxt->slot->tts_ops->materialize(cxt->slot);
		HeapTuple heap_tuple = cxt->slot->tts_ops->get_heap_tuple(cxt->slot);
		Relation rel = cxt->scan_desc->rs_rd;


        if (cxt->label_filter != NIL) {
            bool isnull;
            Oid edge_label_oid = (DATUM_GET_GRAPHID(heap_getattr(heap_tuple, 1, RelationGetDescr(rel), &isnull)) >> ENTRY_ID_BITS);

            bool found = false;
            ListCell *lc;
            foreach(lc, cxt->label_filter) {
                if (edge_label_oid == lfirst_oid(lc)) {
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
        }

		Datum values[4];
		bool nulls[4];
		values[0] = heap_getattr(heap_tuple, 1, RelationGetDescr(rel), &nulls[0]);
		values[1] = heap_getattr(heap_tuple, 2, RelationGetDescr(rel), &nulls[1]);
		values[2] = heap_getattr(heap_tuple, 3, RelationGetDescr(rel), &nulls[2]);
		values[3] = heap_getattr(heap_tuple, 4, RelationGetDescr(rel), &nulls[3]);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));
	}

	ReleaseTupleDesc(RelationGetDescr(cxt->rel));
	table_endscan(cxt->scan_desc);
	table_close(cxt->rel, ShareLock);
	SRF_RETURN_DONE(funcctx);
/*
    if (!table_scan_getnextslot(cxt->scan_desc, ForwardScanDirection, cxt->slot)) {
		ReleaseTupleDesc(RelationGetDescr(cxt->rel));


		SRF_RETURN_DONE(funcctx);
	}
	
	Datum values[4];
	bool nulls[4];
	VertexHeapScanDesc vertex_desc = cxt->scan_desc;
	cxt->slot->tts_ops->materialize(cxt->slot);
	HeapTuple heap_tuple = cxt->slot->tts_ops->get_heap_tuple(cxt->slot);
	Relation rel = vertex_desc->desc[0]->rs_rd;
	values[0] = heap_getattr(heap_tuple, 1, RelationGetDescr(rel), &nulls[0]);
	values[1] = heap_getattr(heap_tuple, 2, RelationGetDescr(rel), &nulls[1]);
	values[2] = heap_getattr(heap_tuple, 3, RelationGetDescr(rel), &nulls[2]);
	values[3] = heap_getattr(heap_tuple, 4, RelationGetDescr(rel), &nulls[3]);
	//ereport(WARNING, (errmsg("edge_search sending graphid %lu", DATUM_GET_GRAPHID(values[2]))));
	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));*/
}

typedef struct variable_edge_stack
{
	graphid *array;
	int top;
	int capacity;
} variable_edge_stack;

typedef struct graph_stack_count {
	int count;
	graphid id;
} graph_stack_count;

typedef struct variable_edge_search_cxt
{
    ScanKey scanKey;
	TableScanDesc scan_desc;
	Relation rel;
	TupleTableSlot *slot;
	int min;
	int max;
	Oid graph_oid;
	HashSetValue *hashSet;
	variable_edge_stack *stack;
	graph_stack_count *current_path;
	variable_edge_stack *edge_stack;
	int current_path_length;
} variable_edge_search_cxt;

static void
variable_edge_stack_push(variable_edge_stack *stack, graphid value)
{

    if (stack->top >= stack->capacity)
    {
        int new_capacity = stack->capacity * 2;
        stack->array = repalloc(stack->array, sizeof(graphid) * (new_capacity + 1));
        stack->capacity = new_capacity;
    }
    stack->array[stack->top++] = value;
   // ereport(WARNING, (errmsg("pushing to stack %i, %i, %lu, %lu", stack->top, stack->capacity, value, stack->array[stack->top-1])));
}

static bool
variable_edge_stack_pop(variable_edge_stack *stack, graphid *result)
{
   // ereport(WARNING, (errmsg("popping from stack %i, %i ", stack->top, stack->capacity)));
    if (stack->top == 0)
        return false;
    *result = stack->array[--stack->top];
	return true;
}

static void
scan_and_push_neighbors(variable_edge_search_cxt *cxt, graphid id)
{
    label_cache_data *lcd = search_label_graph_oid_cache(cxt->graph_oid, (id >> ENTRY_ID_BITS));
	//ereport(WARNING, (errmsg("here")));
    cxt->scanKey = palloc(sizeof(ScanKeyData));
    ScanKeyInit(cxt->scanKey, 1, BTEqualStrategyNumber, F_GRAPHIDEQ, GRAPHID_GET_DATUM(id));

    cxt->rel = table_open(lcd->vertex_adjlist, ShareLock);
    cxt->scan_desc = table_beginscan(cxt->rel, GetActiveSnapshot(), 1, cxt->scanKey);
    List *empty = NIL;
    cxt->slot = table_slot_create(cxt->rel, &empty);

    while (table_scan_getnextslot(cxt->scan_desc, ForwardScanDirection, cxt->slot)) {
        cxt->slot->tts_ops->materialize(cxt->slot);

		bool isnull;
		graphid edge_id = DATUM_GET_GRAPHID(
			heap_getattr(
				cxt->slot->tts_ops->get_heap_tuple(cxt->slot),
				1,
				RelationGetDescr(cxt->scan_desc->rs_rd),
				&isnull));	
		graphid new_id = DATUM_GET_GRAPHID(
			heap_getattr(
				cxt->slot->tts_ops->get_heap_tuple(cxt->slot),
				3,
				RelationGetDescr(cxt->scan_desc->rs_rd),
				&isnull));

        if (!contains(cxt->hashSet, edge_id)) {
            cxt->current_path[cxt->current_path_length].count++;
			variable_edge_stack_push(cxt->stack, new_id);
			variable_edge_stack_push(cxt->edge_stack, edge_id);
        }
    }
    ReleaseTupleDesc(RelationGetDescr(cxt->rel));
    table_endscan(cxt->scan_desc);
    table_close(cxt->rel, ShareLock);
}

PG_FUNCTION_INFO_V1(variable_edge_search);
Datum variable_edge_search(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;
		TupleDesc tupdesc;
        graphid id = AG_GETARG_GRAPHID(1);

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(3);
		TupleDescInitEntry(tupdesc, 1, "edges", VARIABLEEDGEOID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "endid", GRAPHIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "hset", HASHSETOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		variable_edge_search_cxt *cxt= palloc(sizeof(variable_edge_search_cxt));
		cxt->graph_oid = GT_ARG_TO_INT4_DATUM(0);

		if (PG_ARGISNULL(2))
			cxt->min = 0;
		else
			cxt->min = GT_ARG_TO_INT4_DATUM(2);		

		if (PG_ARGISNULL(3))
			cxt->max = 1024;
		else
			cxt->max = GT_ARG_TO_INT4_DATUM(3);

		cxt->stack = palloc0(sizeof(variable_edge_stack));
		cxt->stack->array = palloc0(sizeof(graphid) * (1024));
		cxt->stack->top = 0;
		cxt->stack->capacity = 1024;

		cxt->edge_stack = palloc0(sizeof(variable_edge_stack));
		cxt->edge_stack->array = palloc0(sizeof(graphid) * (1024));
		cxt->edge_stack->top = 0;
		cxt->edge_stack->capacity = 1024;

		if (cxt->max == -1)
			cxt->current_path = palloc(sizeof(graph_stack_count) * (cxt->min + 1));
		else
			cxt->current_path = palloc(sizeof(graph_stack_count) * (cxt->max + 1));

		cxt->current_path_length = 0;

		cxt->hashSet = createHashSet(1024);
		scan_and_push_neighbors(cxt, id);
		funcctx->user_fctx = cxt;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	
	variable_edge_search_cxt *cxt = (variable_edge_search_cxt *) funcctx->user_fctx;

	while (cxt->stack->top > 0) {
		graphid id;
		graphid edge_id;

		if(!variable_edge_stack_pop(cxt->stack, &id) || !variable_edge_stack_pop(cxt->edge_stack, &edge_id))
			SRF_RETURN_DONE(funcctx);

		cxt->current_path[++cxt->current_path_length].count = 0;
		cxt->current_path[cxt->current_path_length].count = id;

		
		insert(cxt->hashSet, edge_id);
		if (cxt->hashSet->size >= cxt->min) {
			Datum values[3];
			bool nulls[3];
			values[0] = NULL;
			nulls[0] = true;
			values[1] = GRAPHID_GET_DATUM(id);
			nulls[1] = false;

			hashset *hset = palloc(sizeof(hashset) + getHashSetSize(cxt->hashSet));
			hset->data_size = getHashSetSize(cxt->hashSet);
			serializeHashSetToBuffer(cxt->hashSet, hset->data, getHashSetSize(cxt->hashSet));
			SET_VARSIZE(hset, VARHDRSZ + sizeof(size_t) + hset->data_size);

			values[2] = HASHSET_P_GET_DATUM(hset);
			nulls[2] = false;
			if (cxt->max == -1 || cxt->current_path_length < cxt->max) {
				scan_and_push_neighbors(cxt, id);
			} else {
				removeElement(cxt->hashSet, edge_id);
			}

			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));
		} else if (cxt->max == -1 || cxt->current_path_length < cxt->max) {
			scan_and_push_neighbors(cxt, id);
		}

		if (cxt->current_path[++cxt->current_path_length].count == 0) {
			variable_edge_stack_pop(cxt->stack, &cxt->current_path[cxt->current_path_length--]);
			variable_edge_stack_pop(cxt->edge_stack, &edge_id);
		 	removeElement(cxt->hashSet, edge_id);
		}
	}
	//destroyHashSetValue(cxt->hashSet);
	SRF_RETURN_DONE(funcctx);
}
