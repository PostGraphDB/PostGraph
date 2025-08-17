
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

#include "ltree.h"

static Datum get_vertex(Oid graph_oid, int64 graphid)
{
    ScanKeyData scan_keys[1];
    Relation graph_vertex_label;
    TableScanDesc scan_desc;
    HeapTuple tuple;
    TupleDesc tupdesc;
    Datum properties, result;
    int32 labelid = (graphid >> ENTRY_ID_BITS);
    
    label_cache_data *lcd = search_label_graph_oid_cache(graph_oid, labelid);

    Snapshot snapshot = GetActiveSnapshot();

    ScanKeyInit(&scan_keys[0], 1, BTEqualStrategyNumber, F_OIDEQ, Int64GetDatum(graphid));

	
    graph_vertex_label = table_open(lcd->relation, ShareLock);
    scan_desc = table_beginscan(graph_vertex_label, snapshot, 1, scan_keys);
    tuple = heap_getnext(scan_desc, ForwardScanDirection);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("id %lu does not exist", graphid)));

    tupdesc = RelationGetDescr(graph_vertex_label);

    properties = column_get_datum(tupdesc, tuple, 1, "properties", GTYPEOID, true);

    table_endscan(scan_desc);
    table_close(graph_vertex_label, ShareLock);

    return properties;
}

PG_FUNCTION_INFO_V1(retrieve_vertex);
Datum retrieve_vertex(PG_FUNCTION_ARGS) {
    
	FuncCallContext *funcctx;
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(1);

		tupdesc->tdtypeid = RECORDOID;	/* not right, but we don't care */
		tupdesc->tdtypmod = -1;
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "properties",
						   GTYPEOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(oldcontext);

		funcctx = SRF_PERCALL_SETUP();
	

		Datum values[1];
		bool nulls[1];
		HeapTuple tuple1;
		gtype *graph_oid = GT_ARG_TO_INT4_DATUM(0);
		graphid id = AG_GETARG_GRAPHID(1);
   
		bool isnull;
		values[0] = get_vertex(graph_oid, id);
		nulls[0] = true;
		tuple1 = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple1));
	}

	SRF_RETURN_DONE(funcctx);
}


struct edge_search_cxt
{
    ScanKey scanKey;
	TableScanDesc scan_desc;
	Relation rel;
	TupleTableSlot *slot;
} edge_search_cxt;

PG_FUNCTION_INFO_V1(edge_search);
Datum edge_search(PG_FUNCTION_ARGS)
{
    gtype *graph_oid = GT_ARG_TO_INT4_DATUM(0);
    graphid id = AG_GETARG_GRAPHID(1);
   
    //lquery *label = PG_GETARG_LQUERY_P(1);
    //bool include_props = PG_GETARG_BOOL(2);
	FuncCallContext *funcctx;
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(4);

	tupdesc->tdtypeid = RECORDOID;	/* not right, but we don't care */
	tupdesc->tdtypmod = -1;
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "id",
						   GRAPHIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "startid",
						   GRAPHIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "endid",
						   GRAPHIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "properties",
						   GTYPEOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		struct edge_search_cxt *cxt= palloc(sizeof(struct edge_search_cxt));
		cxt->scanKey = palloc(sizeof(ScanKeyData));
		
	    int32 labelid = (id >> ENTRY_ID_BITS);
    	label_cache_data *lcd = search_label_graph_oid_cache(graph_oid, labelid);

		Snapshot snapshot = GetActiveSnapshot();
		
		ScanKeyInit(cxt->scanKey, 1, HTEqualStrategyNumber, F_GRAPHIDEQ, GRAPHID_GET_DATUM(id));

		cxt->rel = table_open(lcd->vertex_adjlist, ShareLock);
		cxt->scan_desc = table_beginscan(cxt->rel, snapshot, 1, cxt->scanKey);
		//VertexHeapScanDesc vertex_desc = cxt->scan_desc;
		List *empty = NIL;
		cxt->slot = table_slot_create(cxt->rel, &empty);
		//funcctx->tuple_desc = RelationBuildTupleDesc(cxt->rel);
		funcctx->user_fctx = cxt;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	struct edge_search_cxt *cxt = (struct edge_search_cxt *) funcctx->user_fctx;
	TableAmRoutine *vertex_am = GetVertexHeapamTableAmRoutine();
	
    if (!table_scan_getnextslot(cxt->scan_desc, ForwardScanDirection, cxt->slot)) {
		ReleaseTupleDesc(RelationGetDescr(cxt->rel));
		table_endscan(cxt->scan_desc);
    	table_close(cxt->rel, ShareLock);
		SRF_RETURN_DONE(funcctx);
	}
	
	
	Datum values[4];
	bool nulls[4];
	HeapTuple tuple1;
	cxt->slot->tts_ops->materialize(cxt->slot);
	//cxt->slot->tts_ops->getsomeattrs(cxt->slot, 10000);
	//memset(nulls, false, sizeof(nulls));
	//datum = heap_getattr(cxt->slot.tuple, 1,
	//					 RelationGetDescr(cxt->rel), &isnull);

	VertexHeapScanDesc vertex_desc = cxt->scan_desc;
	//vertex_desc->desc[0]
	bool isnull;
	//slot_getattr(cxt->slot, i, &isnull);
	values[0] = heap_getattr(cxt->slot->tts_ops->get_heap_tuple(cxt->slot), 1, RelationGetDescr(vertex_desc->desc[0]->rs_rd), &isnull);
	nulls[0] = isnull;
	//ereport(WARNING, errmsg("value %lu", DatumGetInt64(values[0])));
	values[1] = heap_getattr(cxt->slot->tts_ops->get_heap_tuple(cxt->slot), 2, RelationGetDescr(vertex_desc->desc[0]->rs_rd), &isnull);
	nulls[1] = isnull;
	//ereport(WARNING, errmsg("value %lu", DatumGetInt64(values[1])));
	values[2] = heap_getattr(cxt->slot->tts_ops->get_heap_tuple(cxt->slot), 3, RelationGetDescr(vertex_desc->desc[0]->rs_rd), &isnull);
	nulls[2] = isnull;
	//ereport(WARNING, errmsg("value %lu", DatumGetInt64(values[2])));	
	values[3] = heap_getattr(cxt->slot->tts_ops->get_heap_tuple(cxt->slot), 4, RelationGetDescr(vertex_desc->desc[0]->rs_rd), &isnull);
	nulls[3] = isnull;
	tuple1 = heap_form_tuple(funcctx->tuple_desc, values, nulls);
	//cxt->slot->tts_ops->get_heap_tuple(cxt->slot);

	//tuple1 = cxt->slot->tts_ops->copy_heap_tuple(cxt->slot);
	//tuple1->t_tableOid = RelationGetRelid(cxt->rel);
	//SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(cxt->slot->tts_ops->get_heap_tuple(cxt->slot)));
	//SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(cxt->slot->tts_ops->get_heap_tuple(cxt->slot)));
	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple1));
}
