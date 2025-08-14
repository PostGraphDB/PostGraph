/*-------------------------------------------------------------------------
 *
 * vertex_heapam_handler.c
 *      vertex heap access method code
 *
 * Portions Copyright (c) 2025, PostGraphDB
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/access/vertex_heap/vertex_heapam_handler.c
 *
 *
 * NOTES
 *      This files wires up the lower level heapam.c et al routines with the
 *      tableam abstraction.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/syncscan.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "access/heapam_xlog.h"
#include "access/visibilitymapdefs.h"
#include "access/valid.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "access/nbtree.h"
#include "nodes/nodeFuncs.h"

#include "optimizer/plancat.h"
#include "utils/inval.h"
#include "access/visibilitymap.h"
#include "utils/lsyscache.h"
#include "access/table.h"


#include "access/vertex.h"
#include "utils/graphid.h"
#include "utils/gtype.h"

#define HEAP_OVERHEAD_BYTES_PER_TUPLE \
        (MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))
#define HEAP_USABLE_BYTES_PER_PAGE \
        (BLCKSZ - SizeOfPageHeaderData)

static void GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
										   uint16 *new_infomask2);


/* ------------------------------------------------------------------------
 * Slot related callbacks for vertex heap AM
 * ------------------------------------------------------------------------
 */

static const TupleTableSlotOps *
vertex_heapam_slot_callbacks(Relation relation)
{
    return &TTSOpsBufferHeapTuple;
}

/* ------------------------------------------------------------------------
 * Functions related to scaning
 * ------------------------------------------------------------------------
 */

static char *make_vertex_adjlist_alias(char *var_name) {
    char *str = palloc0(strlen(var_name) + 8);

    str[0] = '_';
    str[1] = 'a';
    str[2] = 'd';
    str[3] = 'j';
    str[4] = '_';

    int i = 0;
    for (; i < strlen(var_name); i++)
        str[i + 5] = var_name[i];
    str[i + 5] = '\0';

    return str;
}

TableScanDesc vertex_scan_begin(Relation relation, Snapshot snapshot, int nkeys,
                struct ScanKeyData *key, 
                ParallelTableScanDesc parallel_scan, uint32 flags) {
	VertexScanDescData *vertex_desc = palloc(sizeof(VertexScanDescData));
	vertex_desc->rs_base.rs_rd = relation;
	vertex_desc->rs_base.rs_snapshot = snapshot;
	vertex_desc->rs_base.rs_nkeys = nkeys;
	vertex_desc->rs_base.rs_key = key;
	//vertex_desc->rs_base.rs_mintid = NULL;
	//vertex_desc->rs_base.rs_maxtid = NULL;
	vertex_desc->rs_base.rs_flags = flags;
	vertex_desc->rs_base.rs_parallel = NULL;
	vertex_desc->ndesc = 1;
	TableAmRoutine *tableam = GetHeapamTableAmRoutine();
	Oid oid = get_relname_relid(make_vertex_adjlist_alias(get_rel_name(RelationGetRelid(relation))), get_rel_namespace(RelationGetRelid(relation)));
	Relation rel = RelationIdGetRelation(oid);

	TableScanDesc *desc = tableam->scan_begin(rel, snapshot, nkeys, key, parallel_scan, flags);
	vertex_desc->desc = palloc(sizeof (TableScanDesc *) * vertex_desc->ndesc);
	vertex_desc->desc[0] = desc;
	return vertex_desc;
}

void vertex_scan_end(TableScanDesc sscan) {
	VertexScanDescData *vertex_desc = sscan;
	RelationDecrementReferenceCount( ((HeapScanDesc)vertex_desc->desc[0])->rs_base.rs_rd);

	GetHeapamTableAmRoutine()->scan_end(vertex_desc->desc[0]);
}

/*
 * Restart relation scan.  If set_params is set to true, allow_{strat,
 * sync, pagemode} (see scan_begin) changes should be taken into account.
 */
void vertex_scan_rescan(TableScanDesc scan, struct ScanKeyData *key,
            bool set_params, bool allow_strat,
            bool allow_sync, bool allow_pagemode) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_scan_rescan not implemented")));
    
}

/*
 * Return next tuple from `scan`, store in slot.
 */
bool vertex_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                TupleTableSlot *slot) {
	VertexScanDescData *vertex_desc = sscan;

	TableAmRoutine *tableam = GetHeapamTableAmRoutine();
	
	return tableam->scan_getnextslot(vertex_desc->desc[0], direction, slot);
}

/* ------------------------------------------------------------------------
 * Parallel table scan related functions.
 * ------------------------------------------------------------------------
 */

/*
 * Estimate the size of shared memory needed for a parallel scan of this
 * relation. The snapshot does not need to be accounted for.
 */
Size vertex_parallelscan_estimate(Relation rel) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_estimate not implemented")));
    
    return -1;
}

/*
 * Initialize ParallelTableScanDesc for a parallel scan of this relation.
 * `pscan` will be sized according to parallelscan_estimate() for the same
 * relation.
 */
Size vertex_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_initialize not implemented")));
    
    return -1;
}

/*
 * Reinitialize `pscan` for a new scan. `rel` will be the same relation as
 * when `pscan` was initialized by parallelscan_initialize.
 */
void vertex_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    
}


/* ------------------------------------------------------------------------
 * Index Scan Callbacks
 * ------------------------------------------------------------------------
 */

/*
 * Prepare to fetch tuples from the relation, as needed when fetching
 * tuples for an index scan.  The callback has to return an
 * IndexFetchTableData, which the AM will typically embed in a larger
 * structure with additional information.
 *
 * Tuples for an index scan can then be fetched via index_fetch_tuple.
 */
struct IndexFetchTableData *vertex_index_fetch_begin(Relation rel) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    
    return NULL;
}

/*
 * Reset index fetch. Typically this will release cross index fetch
 * resources held in IndexFetchTableData.
 */
void vertex_index_fetch_reset(struct IndexFetchTableData *data) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
}

/*
 * Release resources and deallocate index fetch.
 */
void vertex_index_fetch_end(struct IndexFetchTableData *data) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
}

/*
 * Fetch tuple at `tid` into `slot`, after doing a visibility test
 * according to `snapshot`. If a tuple was found and passed the visibility
 * test, return true, false otherwise.
 *
 * Note that AMs that do not necessarily update indexes when indexed
 * columns do not change, need to return the current/correct version of
 * the tuple that is visible to the snapshot, even if the tid points to an
 * older version of the tuple.
 *
 * *call_again is false on the first call to index_fetch_tuple for a tid.
 * If there potentially is another tuple matching the tid, *call_again
 * needs to be set to true by index_fetch_tuple, signaling to the caller
 * that index_fetch_tuple should be called again for the same tid.
 *
 * *all_dead, if all_dead is not NULL, should be set to true by
 * index_fetch_tuple iff it is guaranteed that no backend needs to see
 * that tuple. Index AMs can use that to av-oid returning that tid in
 * future searches.
 */
bool vertex_index_fetch_tuple(struct IndexFetchTableData *scan,
                              ItemPointer tid, Snapshot snapshot,
                              TupleTableSlot *slot,
                              bool *call_again, bool *all_dead) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    return false;
}


/* -----------------------------------------------------------------------
 * Callbacks for non-modifying operations on individual tuples
 * ------------------------------------------------------------------------
 */

/*
 * Fetch tuple at `tid` into `slot`, after doing a visibility test
 * according to `snapshot`. If a tuple was found and passed the visibility
 * test, returns true, false otherwise.
 */
bool vertex_tuple_fetch_row_version(Relation relation, ItemPointer tid,
                                    Snapshot snapshot, TupleTableSlot *slot) {
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buffer;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	bslot->base.tupdata.t_self = *tid;
	if (heap_fetch(relation, snapshot, &bslot->base.tupdata, &buffer))
	{
		/* store in slot, transferring existing pin */
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot, buffer);
		slot->tts_tableOid = RelationGetRelid(relation);

		return true;
	}

	return false;
}

/*
 * Is tid valid for a scan of this relation.
 */
bool vertex_tuple_tid_valid(TableScanDesc scan, ItemPointer tid) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    

    return false;
}

/*
 * Return the latest version of the tuple at `tid`, by updating `tid` to
 * point at the newest version.
 */
void vertex_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    

}

/*
 * Does the tuple in `slot` satisfy `snapshot`?  The slot needs to be of
 * the appropriate type for the AM.
 */
bool vertex_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
                         Snapshot snapshot) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    
     return false;
}

/* see table_index_delete_tuples() */
TransactionId vertex_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    

    return InvalidTransactionId;
}



/* ------------------------------------------------------------------------
 * Manipulations of physical tuples.
 * ------------------------------------------------------------------------
 */

/* see table_tuple_insert() for reference about parameters */
void vertex_tuple_insert(Relation relation, TupleTableSlot *slot,
                         CommandId cid, int options,
                         struct BulkInsertStateData *bistate) {
	TableAmRoutine *tableam = GetHeapamTableAmRoutine();
	Oid oid = get_relname_relid(make_vertex_adjlist_alias(get_rel_name(RelationGetRelid(relation))), get_rel_namespace(RelationGetRelid(relation)));
	Relation rel = table_open(oid, RowExclusiveLock);
	tableam->tuple_insert(rel, slot, cid, options, bistate);
	table_close(rel, RowExclusiveLock);
}

/* see table_tuple_insert_speculative() for reference about parameters */
void vertex_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
                     CommandId cid, int options,
                     struct BulkInsertStateData *bistate,
                     uint32 specToken) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_tuple_insert_speculative not implemented")));
}

/* see table_tuple_complete_speculative() for reference about parameters */
void vertex_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
                       uint32 specToken, bool succeeded) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
}

/* see table_multi_insert() for reference about parameters */
void vertex_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
             CommandId cid, int options, struct BulkInsertStateData *bistate) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
}

/* see table_tuple_delete() for reference about parameters */
TM_Result vertex_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
                  Snapshot snapshot, Snapshot crosscheck, bool wait,
                  TM_FailureData *tmfd, bool changingPart) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_tuple_delete not implemented")));
}

/* see table_tuple_update() for reference about parameters */
TM_Result vertex_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                  CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                  bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                  bool *update_indexes) {
	return TM_Invisible;
}

/* see table_tuple_lock() for reference about parameters */
TM_Result vertex_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
                TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
                LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    return 0;
}

/* ------------------------------------------------------------------------
 * DDL related functionality.
 * ------------------------------------------------------------------------
 */

/*
 * This callback needs to create a new relation filenode for `rel`, with
 * appropriate durability behaviour for `persistence`.
 *
 * Note that only the subset of the relcache filled by
 * RelationBuildLocalRelation() can be relied upon and that the relation's
 * catalog entries will either not yet exist (new relation), or will still
 * reference the old relfilenode.
 *
 * As output *freezeXid, *minmulti must be set to the values appropriate
 * for pg_class.{relfrozenxid, relminmxid}. For AMs that don't need those
 * fields to be filled they can be set to InvalidTransactionId and
 * InvalidMultiXactId, respectively.
 *
 * See also table_relation_set_new_filenode().
 */
void vertex_relation_set_new_filenode(Relation rel, const RelFileNode *newrnode,
                      char persistence, TransactionId *freezeXid,
                      MultiXactId *minmulti) {

    if(persistence != RELPERSISTENCE_PERMANENT || rel->rd_rel->relkind != RELKIND_RELATION)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg_internal("vertex am can only work on permenant tables")));   
}

/*
 * This callback needs to remove all contents from `rel`'s current
 * relfilenode. No provisions for transactional behaviour need to be made.
 * Often this can be implemented by truncating the underlying storage to
 * its minimal size.
 *
 * See also table_relation_nontransactional_truncate().
 */
void vertex_relation_nontransactional_truncate(Relation rel) {


}

/*
 * See table_relation_copy_data().
 *
 * This can typically be implemented by directly copying the underlying
 * storage, unless it contains references to the tablespace internally.
 */
void vertex_relation_copy_data(Relation rel, const RelFileNode *newrnode) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    


}

/* See table_relation_copy_for_cluster() */
void vertex_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
                      Relation OldIndex, bool use_sort,
                      TransactionId OldestXmin, TransactionId *xid_cutoff,
                      MultiXactId *multi_cutoff, double *num_tuples,
                      double *tups_vacuumed, double *tups_recently_dead) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
}

/*
 * React to VACUUM command on the relation. The VACUUM can be triggered by
 * a user or by autovacuum. The specific actions performed by the AM will
 * depend heavily on the individual AM.
 *
 * On entry a transaction is already established, and the relation is
 * locked with a ShareUpdateExclusive lock.
 *
 * Note that neither VACUUM FULL (and CLUSTER), nor ANALYZE go through
 * this routine, even if (for ANALYZE) it is part of the same VACUUM
 * command.
 *
 * There probably, in the future, needs to be a separate callback to
 * integrate with autovacuum's scheduling.
 */
void vertex_relation_vacuum(Relation rel, struct VacuumParams *params,
                            BufferAccessStrategy bstrategy) {

    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
}

/*
 * Prepare to analyze block `blockno` of `scan`. The scan has been started
 * with table_beginscan_analyze().  See also
 * table_scan_analyze_next_block().
 *
 * The callback may acquire resources like locks that are held until
 * table_scan_analyze_next_tuple() returns false. It e.g. can make sense
 * to hold a lock until all tuples on a block have been analyzed by
 * scan_analyze_next_tuple.
 *
 * The callback can return false if the block is not suitable for
 * sampling, e.g. because it's a metapage that could never contain tuples.
 *
 * XXX: This obviously is primarily suited for block-based AMs. It's not
 * clear what a good interface for non block based AMs would be, so there
 * isn't one yet.
 */
bool vertex_scan_analyze_next_block(TableScanDesc scan, BlockNumber blockno,
                                    BufferAccessStrategy bstrategy) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    
    return false;
}

/*
 * See table_scan_analyze_next_tuple().
 *
 * Not every AM might have a meaningful concept of dead rows, in which
 * case it's OK to not increment *deadrows - but note that that may
 * influence autovacuum scheduling (see comment for relation_vacuum
 * callback).
 */
bool vertex_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
                                    double *liverows, double *deadrows, TupleTableSlot *slot) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    


    return false;
}

/* see table_index_build_range_scan for reference about parameters */
double vertex_index_build_range_scan(Relation table_rel, Relation index_rel,
                                     struct IndexInfo *index_info, bool allow_sync,
                                     bool anyvisible, bool progress, BlockNumber start_blockno,
                                     BlockNumber numblocks, IndexBuildCallback callback,
                                     void *callback_state, TableScanDesc scan) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    return 0;
}

/* see table_index_validate_scan for reference about parameters */
void vertex_index_validate_scan(Relation table_rel, Relation index_rel,
                                struct IndexInfo *index_info, Snapshot snapshot,
                                struct ValidateIndexState *state) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
}


/* ------------------------------------------------------------------------
 * Miscellaneous functions.
 * ------------------------------------------------------------------------
 */

/*
 * See table_relation_size().
 *
 * Note that currently a few callers use the MAIN_FORKNUM size to figure
 * out the range of potentially interesting blocks (brin, analyze). It's
 * probable that we'll need to revise the interface for those at some
 * point.
 */
uint64 vertex_relation_size(Relation rel, ForkNumber forkNumber) {
  //  if (forkNumber == INIT_FORKNUM)
   //     return 0;

    uint64 nblocks = 0;

    /* InvalidForkNumber indicates returning the size for all forks */
    if (forkNumber == InvalidForkNumber) {
        for (int i = 0; i < MAX_FORKNUM; i++)
             nblocks += smgrnblocks(RelationGetSmgr(rel), i);
        }
    else
        nblocks = smgrnblocks(RelationGetSmgr(rel), forkNumber);

    return nblocks * BLCKSZ;
}


/*
 * This callback should return true if the relation requires a TOAST table
 * and false if it does not.  It may wish to examine the relation's tuple
 * descriptor before making a decision, but if it uses some other method
 * of storing large values (or if it does not support them) it can simply
 * return false.
 */
bool vertex_relation_needs_toast_table(Relation rel) {
    // XXX: for now, no toast table, this will absolutely change, but we will
    // do that later 
    return false;
}

/*
 * This callback should return the OID of the table AM that implements
 * TOAST tables for this AM.  If the relation_needs_toast_table callback
 * always returns false, this callback is not required.
 */
Oid vertex_relation_toast_am(Relation rel) {
    return InvalidOid;
}

/*
 * This callback is invoked when detoasting a value stored in a toast
 * table implemented by this AM.  See table_relation_fetch_toast_slice()
 * for more details.
 */
void vertex_relation_fetch_toast_slice(Relation toastrel, Oid valueid,
                                       int32 attrsize, int32 sliceoffset,
                                       int32 slicelength, struct varlena *result) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_relation_fetch_toat_slice not implemented")));
}

/* ------------------------------------------------------------------------
 * Planner related functions.
 * ------------------------------------------------------------------------
 */

/*
 * See table_relation_estimate_size().
 *
 * While block oriented, it shouldn't be too hard for an AM that doesn't
 * internally use blocks to convert into a usable representation.
 *
 * This differs from the relation_size callback by returning size
 * estimates (both relation size and tuple count) for planning purposes,
 * rather than returning a currently correct estimate.
 */
void vertex_relation_estimate_size(Relation rel, int32 *attr_widths,
                                   BlockNumber *pages, double *tuples,
                                   double *allvisfrac) {
    
}



/* ------------------------------------------------------------------------
 * Executor related functions.
 * ------------------------------------------------------------------------
 */

/*
 * Prepare to fetch / check / return tuples from `tbmres->blockno` as part
 * of a bitmap table scan. `scan` was started via table_beginscan_bm().
 * Return false if there are no tuples to be found on the page, true
 * otherwise.
 *
 * This will typically read and pin the target block, and do the necessary
 * work to allow scan_bitmap_next_tuple() to return tuples (e.g. it might
 * make sense to perform tuple visibility checks at this time). For some
 * AMs it will make more sense to do all the work referencing `tbmres`
 * contents here, for others it might be better to defer more work to
 * scan_bitmap_next_tuple.
 *
 * If `tbmres->blockno` is -1, this is a lossy scan and all visible tuples
 * on the page have to be returned, otherwise the tuples at offsets in
 * `tbmres->offsets` need to be returned.
 *
 * XXX: Currently this may only be implemented if the AM uses md.c as its
 * storage manager, and uses ItemPointer->ip_blkid in a manner that maps
 * blockids directly to the underlying storage. nodeBitmapHeapscan.c
 * performs prefetching directly using that interface.  This probably
 * needs to be rectified at a later point.
 *
 * XXX: Currently this may only be implemented if the AM uses the
 * visibilitymap, as nodeBitmapHeapscan.c unconditionally accesses it to
 * perform prefetching.  This probably needs to be rectified at a later
 * point.
 *
 * Optional callback, but either both scan_bitmap_next_block and
 * scan_bitmap_next_tuple need to exist, or neither.
 */
bool vertex_scan_bitmap_next_block(TableScanDesc scan, struct TBMIterateResult *tbmres) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));

    return false;
}

/*
 * Fetch the next tuple of a bitmap table scan into `slot` and return true
 * if a visible tuple was found, false otherwise.
 *
 * For some AMs it will make more sense to do all the work referencing
 * `tbmres` contents in scan_bitmap_next_block, for others it might be
 * better to defer more work to this callback.
 *
 * Optional callback, but either both scan_bitmap_next_block and
 * scan_bitmap_next_tuple need to exist, or neither.
 */
bool vertex_scan_bitmap_next_tuple(TableScanDesc scan, struct TBMIterateResult *tbmres,
                   TupleTableSlot *slot) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));

    return false;
}

/*
 * Prepare to fetch tuples from the next block in a sample scan. Return
 * false if the sample scan is finished, true otherwise. `scan` was
 * started via table_beginscan_sampling().
 *
 * Typically this will first determine the target block by calling the
 * TsmRoutine's NextSampleBlock() callback if not NULL, or alternatively
 * perform a sequential scan over all blocks.  The determined block is
 * then typically read and pinned.
 *
 * As the TsmRoutine interface is block based, a block needs to be passed
 * to NextSampleBlock(). If that's not appropriate for an AM, it
 * internally needs to perform mapping between the internal and a block
 * based representation.
 *
 * Note that it's not acceptable to hold deadlock prone resources such as
 * lwlocks until scan_sample_next_tuple() has exhausted the tuples on the
 * block - the tuple is likely to be returned to an upper query node, and
 * the next call could be off a long while. Holding buffer pins and such
 * is obviously OK.
 *
 * Currently it is required to implement this interface, as there's no
 * alternative way (contrary e.g. to bitmap scans) to implement sample
 * scans. If infeasible to implement, the AM may raise an error.
 */
bool vertex_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));

    return false;
}

/*
 * This callback, only called after scan_sample_next_block has returned
 * true, should determine the next tuple to be returned from the selected
 * block using the TsmRoutine's NextSampleTuple() callback.
 *
 * The callback needs to perform visibility checks, and only return
 * visible tuples. That obviously can mean calling NextSampleTuple()
 * multiple times.
 *
 * The TsmRoutine interface assumes that there's a maximum offset on a
 * given page, so if that doesn't apply to an AM, it needs to emulate that
 * assumption somehow.
 */
bool vertex_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate,
                       TupleTableSlot *slot) {
    
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));

return false;    
}


/* ------------------------------------------------------------------------
 * Definition of the heap table access method.
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine vertex_heapam_methods = {
    .type = T_TableAmRoutine,

    .slot_callbacks = vertex_heapam_slot_callbacks,

    .scan_begin = vertex_scan_begin,
    .scan_end = vertex_scan_end,
    .scan_rescan = vertex_scan_rescan,
    .scan_getnextslot = vertex_scan_getnextslot,

    .scan_set_tidrange = NULL,
    .scan_getnextslot_tidrange = NULL,

    .parallelscan_estimate = vertex_parallelscan_estimate,
    .parallelscan_initialize = vertex_parallelscan_initialize,
    .parallelscan_reinitialize = vertex_parallelscan_reinitialize,

    .index_fetch_begin = vertex_index_fetch_begin,
    .index_fetch_reset = vertex_index_fetch_reset,
    .index_fetch_end = vertex_index_fetch_end,
    .index_fetch_tuple = vertex_index_fetch_tuple,

    .tuple_insert = vertex_tuple_insert,
    .tuple_insert_speculative = vertex_tuple_insert_speculative,
    .tuple_complete_speculative = vertex_tuple_complete_speculative,
    .multi_insert = vertex_multi_insert,
    .tuple_delete = vertex_tuple_delete,
    .tuple_update = vertex_tuple_update,
    .tuple_lock = vertex_tuple_lock,

    .tuple_fetch_row_version = vertex_tuple_fetch_row_version,
    .tuple_get_latest_tid = vertex_tuple_get_latest_tid,
    .tuple_tid_valid = vertex_tuple_tid_valid,
    .tuple_satisfies_snapshot = vertex_tuple_satisfies_snapshot,
    .index_delete_tuples = vertex_index_delete_tuples,

    .relation_set_new_filenode = vertex_relation_set_new_filenode,
    .relation_nontransactional_truncate = vertex_relation_nontransactional_truncate,
    .relation_copy_data = vertex_relation_copy_data,
    .relation_copy_for_cluster = vertex_relation_copy_for_cluster,
    .relation_vacuum = vertex_relation_vacuum,
    .scan_analyze_next_block = vertex_scan_analyze_next_block,
    .scan_analyze_next_tuple = vertex_scan_analyze_next_tuple,
    .index_build_range_scan = vertex_index_build_range_scan,
    .index_validate_scan = vertex_index_validate_scan,

    .relation_size = vertex_relation_size,
    .relation_needs_toast_table = vertex_relation_needs_toast_table,
    .relation_toast_am = NULL,
    .relation_fetch_toast_slice = NULL,

    .relation_estimate_size = vertex_relation_estimate_size,

    .scan_bitmap_next_block = vertex_scan_bitmap_next_block,
    .scan_bitmap_next_tuple = vertex_scan_bitmap_next_tuple,
    .scan_sample_next_block = vertex_scan_sample_next_block,
    .scan_sample_next_tuple = vertex_scan_sample_next_tuple
};


const TableAmRoutine *
GetVertexHeapamTableAmRoutine(void)
{
    return &vertex_heapam_methods;
}

PG_FUNCTION_INFO_V1(vertex_adjlist_tableam_handler);
Datum
vertex_adjlist_tableam_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&vertex_heapam_methods);
}