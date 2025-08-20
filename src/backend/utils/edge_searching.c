
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

#include "ltree.h"

static Datum get_vertex(Oid graph_oid, int64 graphid, bool *isnull)
{
    label_cache_data *lcd = search_label_graph_oid_cache(graph_oid, (graphid >> ENTRY_ID_BITS));

	ScanKeyData scan_keys[1];
    ScanKeyInit(&scan_keys[0], 1, BTEqualStrategyNumber, F_OIDEQ, Int64GetDatum(graphid));

    Relation rel = table_open(lcd->relation, ShareLock);

    TableScanDesc scan_desc = table_beginscan(rel, GetActiveSnapshot(), 1, scan_keys);

	HeapTuple tuple;
    if (!HeapTupleIsValid(tuple = heap_getnext(scan_desc, ForwardScanDirection)))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("id %lu does not exist", graphid)));

	Datum properties = heap_getattr(tuple, 2, RelationGetDescr(rel), isnull);

    table_endscan(scan_desc);
    table_close(rel, ShareLock);

    return properties;
}



PG_FUNCTION_INFO_V1(retrieve_vertex);
Datum retrieve_vertex(PG_FUNCTION_ARGS) {
    
	FuncCallContext *funcctx;
	if (SRF_IS_FIRSTCALL()) {
		funcctx = SRF_FIRSTCALL_INIT();

		TupleDesc tupdesc = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(tupdesc, 1, "properties", GTYPEOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		funcctx = SRF_PERCALL_SETUP();
	
		Datum values[1];
		bool nulls[1];
		values[0] = get_vertex(GT_ARG_TO_INT4_DATUM(0), AG_GETARG_GRAPHID(1), &nulls[0]);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));
	}

	SRF_RETURN_DONE(funcctx);
}


typedef struct edge_search_cxt
{
    ScanKey scanKey;
	TableScanDesc scan_desc;
	Relation rel;
	TupleTableSlot *slot;
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

		funcctx->user_fctx = cxt;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	edge_search_cxt *cxt = (edge_search_cxt *) funcctx->user_fctx;
	
    if (!table_scan_getnextslot(cxt->scan_desc, ForwardScanDirection, cxt->slot)) {
		ReleaseTupleDesc(RelationGetDescr(cxt->rel));
		table_endscan(cxt->scan_desc);
    	/*
   		List *indexoidlist = RelationGetIndexList(cxt->rel);
		if(list_length(indexoidlist) == 1) {
			Oid idx = linitial_oid(indexoidlist);
			index_close(idx, RowExclusiveLock);
		}*/

		table_close(cxt->rel, ShareLock);

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

	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));
}
// Node for the linked list to handle collisions (chaining)
typedef struct HashSetNode {
    graphid key;
    struct HashSetNode* next;
} HashSetNode;

// The main Hash Set structure
typedef struct HashSet {
    HashSetNode** buckets; // Array of pointers to HashSetNodes (the buckets)
    int capacity;          // The total number of buckets
    int size;              // The current number of elements in the set
} HashSet;



/**
 * @brief Creates a new HashSetNode.
 * @param key The graphid key for the new node.
 * @return A pointer to the newly created HashSetNode.
 */
HashSetNode* createHashSetNode(graphid key) {
    HashSetNode* newNode = (HashSetNode*)palloc(sizeof(HashSetNode));
    newNode->key = key;
    newNode->next = NULL;
    return newNode;
}

/**
 * @brief Creates and initializes a new HashSet.
 * @param capacity The initial number of buckets for the hash set.
 * @return A pointer to the newly created HashSet, or NULL if capacity is invalid.
 */
HashSet* createHashSet(int capacity) {
    HashSet* set = (HashSet*)palloc(sizeof(HashSet));

    set->capacity = capacity;
    set->size = 0;
    set->buckets = (HashSetNode**)palloc0(capacity * sizeof(HashSetNode*));

    return set;
}

/**
 * @brief A simple hash function to map a key to a bucket index.
 * @param key The key to hash.
 * @param capacity The capacity of the hash set.
 * @return The calculated index for the key.
 */
int hashFunction(graphid key, int capacity) {
    return key % capacity;
}

/**
 * @brief Inserts a key into the hash set.
 * @param set A pointer to the HashSet.
 * @param key The key to insert.
 */
void insert(HashSet* set, graphid key) {
    if (set == NULL) return;

    int index = hashFunction(key, set->capacity);
    HashSetNode* currentNode = set->buckets[index];

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            return; 
        }
        currentNode = currentNode->next;
    }

    HashSetNode* newNode = createHashSetNode(key);
    newNode->next = set->buckets[index];
    set->buckets[index] = newNode;
    set->size++;
}

/**
 * @brief Checks if a key exists in the hash set.
 * @param set A pointer to the HashSet.
 * @param key The key to search for.
 * @return true if the key is found, false otherwise.
 */
bool contains(HashSet* set, graphid key) {
    if (set == NULL) return false;

    int index = hashFunction(key, set->capacity);
    HashSetNode* currentNode = set->buckets[index];

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            return true;
        }
        currentNode = currentNode->next;
    }

    return false;
}

/**
 * @brief Removes a key from the hash set.
 * @param set A pointer to the HashSet.
 * @param key The key to remove.
 * @return true if the key was found and removed, false otherwise.
 */
bool removeElement(HashSet* set, graphid key) {
    if (set == NULL) return false;

    int index = hashFunction(key, set->capacity);
    HashSetNode* currentNode = set->buckets[index];
    HashSetNode* prevNode = NULL;

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            if (prevNode == NULL) {
                set->buckets[index] = currentNode->next;
            } else {
                prevNode->next = currentNode->next;
            }
            // Use the environment-specific free function
            pfree(currentNode);
            set->size--;
            return true;
        }
        prevNode = currentNode;
        currentNode = currentNode->next;
    }

    return false;
}

/**
 * @brief Frees all memory associated with the hash set.
 * @param set A pointer to the HashSet to destroy.
 */
void destroyHashSet(HashSet* set) {
    if (set == NULL) return;

    for (int i = 0; i < set->capacity; i++) {
        HashSetNode* currentNode = set->buckets[i];
        while (currentNode != NULL) {
            HashSetNode* temp = currentNode;
            currentNode = currentNode->next;
            // Use the environment-specific free function
            pfree(temp);
        }
    }
    pfree(set->buckets);
    pfree(set);
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
	Oid graph_oid;
	HashSet *hashSet;
	variable_edge_stack *stack;
	graph_stack_count *current_path;
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
		graphid new_id = DATUM_GET_GRAPHID(
			heap_getattr(
				cxt->slot->tts_ops->get_heap_tuple(cxt->slot),
				3,
				RelationGetDescr(cxt->scan_desc->rs_rd),
				&isnull));

        if (!contains(cxt->hashSet, new_id)) {
            cxt->current_path[cxt->current_path_length].count++;
			variable_edge_stack_push(cxt->stack, new_id);
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

		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(tupdesc, 1, "edges", VARIABLEEDGEOID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "endid", GRAPHIDOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		variable_edge_search_cxt *cxt= palloc(sizeof(variable_edge_search_cxt));
		cxt->graph_oid = GT_ARG_TO_INT4_DATUM(0);
		cxt->min = GT_ARG_TO_INT4_DATUM(2);
		
		cxt->stack = palloc0(sizeof(variable_edge_stack));
		cxt->stack->array = palloc0(sizeof(graphid) * (1024));
		cxt->stack->top = 0;
		cxt->stack->capacity = 1024;

		cxt->current_path = palloc(sizeof(graph_stack_count) * (cxt->min + 1));
		cxt->current_path_length = 0;

		cxt->hashSet = createHashSet(1024);
		insert(cxt->hashSet, GRAPHID_GET_DATUM(id));
		scan_and_push_neighbors(cxt, id);
		funcctx->user_fctx = cxt;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	
	variable_edge_search_cxt *cxt = (variable_edge_search_cxt *) funcctx->user_fctx;

	while (cxt->hashSet->size > 0) {
		graphid id;


		if(!variable_edge_stack_pop(cxt->stack, &id)){
			//destroyHashSet(cxt->hashSet);
			SRF_RETURN_DONE(funcctx);
		}
/*
		if (cxt->current_path[++cxt->current_path_length].count == 0) {
			cxt->current_path_length--;
			variable_edge_stack_pop(cxt->stack, &cxt->current_path[cxt->current_path_length--]);
		}
*/
		cxt->current_path[++cxt->current_path_length].count = 0;
		cxt->current_path[cxt->current_path_length].count = id;

		
		insert(cxt->hashSet, GRAPHID_GET_DATUM(id));
	
		if (cxt->hashSet->size == cxt->min + 1) {
			Datum values[2];
			bool nulls[2];
			values[0] = NULL;
			nulls[0] = true;
			values[1] = GRAPHID_GET_DATUM(id);
			nulls[1] = false;
			removeElement(cxt->hashSet, id);
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));
		} else {
			scan_and_push_neighbors(cxt, id);
		}

		if (cxt->current_path[++cxt->current_path_length].count == 0) {
			variable_edge_stack_pop(cxt->stack, &cxt->current_path[cxt->current_path_length--]);
			removeElement(cxt->hashSet, id);
		}
	}
	//destroyHashSet(cxt->hashSet);
	SRF_RETURN_DONE(funcctx);


}
