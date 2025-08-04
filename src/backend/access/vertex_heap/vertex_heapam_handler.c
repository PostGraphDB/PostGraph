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

static XLogRecPtr log_heap_new_cid(Relation relation, HeapTuple tup);
static MultiXactStatus
get_mxact_status_for_lock(LockTupleMode mode, bool is_update);
static bool heap_acquire_tuplock(Relation relation, ItemPointer tid,
								 LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool *have_tuple_lock);
uint32
vertex_hash_init(Relation rel, double num_tuples, ForkNumber forkNum);
void
vertex_hash_doinsert(Relation rel, HeapTuple itup, Relation heapRel);
void
vertex_exec_index_build_ScanKeys(PlanState *planstate, Relation index,
					   List *quals,
					   ScanKey scanKeys, int *numScanKeys);
typedef struct vertex_hash_struct
{
    graphid id; // hash key
    ItemPointerData itemPointer;
    gtype *properties;
    Page page;
    ItemId itemId;
    Buffer buffer;
} vertex_hash_struct;


static vertex_hash_struct *iterator = NULL;
static HTAB *vertex_hash = NULL; 
static HASH_SEQ_STATUS scanStatus;

#include "access/parallel.h"
/*
 * Subroutine for heap_insert(). Prepares a tuple for insertion. This sets the
 * tuple header fields and toasts the tuple if necessary.  Returns a toasted
 * version of the tuple if it was toasted, or the original tuple if not. Note
 * that in any case, the header fields are also set in the original tuple.
 */
static HeapTuple
heap_prepare_insert(Relation relation, HeapTuple tup, TransactionId xid,
					CommandId cid, int options)
{
	/*
	 * To allow parallel inserts, we need to ensure that they are safe to be
	 * performed in workers. We have the infrastructure to allow parallel
	 * inserts in general except for the cases where inserts generate a new
	 * CommandId (eg. inserts into a table having a foreign key column).
	 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot insert tuples in a parallel worker")));

	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, xid);
	if (options & HEAP_INSERT_FROZEN)
		HeapTupleHeaderSetXminFrozen(tup->t_data);

	HeapTupleHeaderSetCmin(tup->t_data, cid);
	HeapTupleHeaderSetXmax(tup->t_data, 0); /* for cleanliness */
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(tup));
		return tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
		return heap_toast_insert_or_update(relation, tup, NULL, options);
	else
		return tup;
}














//BlackPink
/*
 *  * Given infomask/infomask2, compute the bits that must be saved in the
 *   * "infobits" field of xl_heap_delete, xl_heap_update, xl_heap_lock,
 *    * xl_heap_lock_updated WAL records.
 *     *
 *      * See fix_infomask_from_infobits.
 *       */
static uint8
compute_infobits(uint16 infomask, uint16 infomask2)
{
		return
			((infomask & HEAP_XMAX_IS_MULTI) != 0 ? XLHL_XMAX_IS_MULTI : 0) |
				((infomask & HEAP_XMAX_LOCK_ONLY) != 0 ? XLHL_XMAX_LOCK_ONLY : 0) |
					((infomask & HEAP_XMAX_EXCL_LOCK) != 0 ? XLHL_XMAX_EXCL_LOCK : 0) |
					/* note we ignore HEAP_XMAX_SHR_LOCK here */
					((infomask & HEAP_XMAX_KEYSHR_LOCK) != 0 ? XLHL_XMAX_KEYSHR_LOCK : 0) |
				((infomask2 & HEAP_KEYS_UPDATED) != 0 ?
			XLHL_KEYS_UPDATED : 0);
}

/*
 * MultiXactIdGetUpdateXid
 *
 * Given a multixact Xmax and corresponding infomask, which does not have the
 * HEAP_XMAX_LOCK_ONLY bit set, obtain and return the Xid of the updating
 * transaction.
 *
 * Caller is expected to check the status of the updating transaction, if
 * necessary.
 */
static TransactionId
MultiXactIdGetUpdateXid(TransactionId xmax, uint16 t_infomask)
{
	TransactionId update_xact = InvalidTransactionId;
	MultiXactMember *members;
	int			nmembers;

	Assert(!(t_infomask & HEAP_XMAX_LOCK_ONLY));
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	/*
	 * Since we know the LOCK_ONLY bit is not set, this cannot be a multi from
	 * pre-pg_upgrade.
	 */
	nmembers = GetMultiXactIdMembers(xmax, &members, false, false);

	if (nmembers > 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			/* Ignore lockers */
			if (!ISUPDATE_from_mxstatus(members[i].status))
		    	continue;

			/* there can be at most one updater */
			Assert(update_xact == InvalidTransactionId);
            update_xact = members[i].xid;
#ifndef USE_ASSERT_CHECKING
			/*
			 * in an assert-enabled build, walk the whole array to ensure
			 * there's no other updater.
			 */
			break;
#endif
		}

	    pfree(members);
	}

	return update_xact;
}



/*
 *  * Given two versions of the same t_infomask for a tuple, compare them and
 *   * return whether the relevant status for a tuple Xmax has changed.  This is
 *    * used after a buffer lock has been released and reacquired: we want to ensure
 *     * that the tuple state continues to be the same it was when we previously
 *      * examined it.
 *       *
 *        * Note the Xmax field itself must be compared separately.
 *         */
static inline bool
xmax_infomask_changed(uint16 new_infomask, uint16 old_infomask)
{
		const uint16 interesting =
				HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY | HEAP_LOCK_MASK;

			if ((new_infomask & interesting) != (old_infomask & interesting))
						return true;

				return false;
}

/*
 *  * This table maps tuple lock strength values for each particular
 *   * MultiXactStatus value.
 *    */
static const int MultiXactStatusLock[MaxMultiXactStatus + 1] =
{
		LockTupleKeyShare,			/* ForKeyShare */
			LockTupleShare,				/* ForShare */
				LockTupleNoKeyExclusive,	/* ForNoKeyUpdate */
					LockTupleExclusive,			/* ForUpdate */
						LockTupleNoKeyExclusive,	/* NoKeyUpdate */
							LockTupleExclusive			/* Update */
};


/* Get the LockTupleMode for a given MultiXactStatus */
#define TUPLOCK_from_mxstatus(status) \
				(MultiXactStatusLock[(status)])
/* ----------------------------------------------------------------
 *                         heap support routines
 * ----------------------------------------------------------------
 */


/*
 *  * Given an original set of Xmax and infomask, and a transaction (identified by
 *   * add_to_xmax) acquiring a new lock of some mode, compute the new Xmax and
 *    * corresponding infomasks to use on the tuple.
 *     *
 *      * Note that this might have side effects such as creating a new MultiXactId.
 *       *
 *        * Most callers will have called HeapTupleSatisfiesUpdate before this function;
 *         * that will have set the HEAP_XMAX_INVALID bit if the xmax was a MultiXactId
 *          * but it was not running anymore. There is a race condition, which is that the
 *           * MultiXactId may have finished since then, but that uncommon case is handled
 *            * either here, or within MultiXactIdExpand.
 *             *
 *              * There is a similar race condition possible when the old xmax was a regular
 *               * TransactionId.  We test TransactionIdIsInProgress again just to narrow the
 *                * window, but it's still possible to end up creating an unnecessary
 *                 * MultiXactId.  Fortunately this is harmless.
 *                  */
static void
compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
								  uint16 old_infomask2, TransactionId add_to_xmax,
								  						  LockTupleMode mode, bool is_update,
														  						  TransactionId *result_xmax, uint16 *result_infomask,
																				  						  uint16 *result_infomask2)
{
		TransactionId new_xmax;
			uint16		new_infomask,
									new_infomask2;

				Assert(TransactionIdIsCurrentTransactionId(add_to_xmax));

l5:
					new_infomask = 0;
						new_infomask2 = 0;
							if (old_infomask & HEAP_XMAX_INVALID)
									{
												/*
												 * 		 * No previous locker; we just insert our own TransactionId.
												 * 		 		 *
												 * 		 		 		 * Note that it's critical that this case be the first one checked,
												 * 		 		 		 		 * because there are several blocks below that come back to this one
												 * 		 		 		 		 		 * to implement certain optimizations; old_infomask might contain
												 * 		 		 		 		 		 		 * other dirty bits in those cases, but we don't really care.
												 * 		 		 		 		 		 		 		 */
												if (is_update)
															{
																			new_xmax = add_to_xmax;
																						if (mode == LockTupleExclusive)
																											new_infomask2 |= HEAP_KEYS_UPDATED;
																								}
														else
																	{
																					new_infomask |= HEAP_XMAX_LOCK_ONLY;
																								switch (mode)
																												{
																																	case LockTupleKeyShare:
																																							new_xmax = add_to_xmax;
																																												new_infomask |= HEAP_XMAX_KEYSHR_LOCK;
																																																	break;
																																																					case LockTupleShare:
																																																						new_xmax = add_to_xmax;
																																																											new_infomask |= HEAP_XMAX_SHR_LOCK;
																																																																break;
																																																																				case LockTupleNoKeyExclusive:
																																																																					new_xmax = add_to_xmax;
																																																																										new_infomask |= HEAP_XMAX_EXCL_LOCK;
																																																																															break;
																																																																																			case LockTupleExclusive:
																																																																																				new_xmax = add_to_xmax;
																																																																																									new_infomask |= HEAP_XMAX_EXCL_LOCK;
																																																																																														new_infomask2 |= HEAP_KEYS_UPDATED;
																																																																																																			break;
																																																																																																							default:
																																																																																																								new_xmax = InvalidTransactionId;	/* silence compiler */
																																																																																																													elog(ERROR, "invalid lock mode");
																																																																																																																}
																										}
															}
								else if (old_infomask & HEAP_XMAX_IS_MULTI)
										{
													MultiXactStatus new_status;

															/*
															 * 		 * Currently we don't allow XMAX_COMMITTED to be set for multis, so
															 * 		 		 * cross-check.
															 * 		 		 		 */
															Assert(!(old_infomask & HEAP_XMAX_COMMITTED));

																	/*
																	 * 		 * A multixact together with LOCK_ONLY set but neither lock bit set
																	 * 		 		 * (i.e. a pg_upgraded share locked tuple) cannot possibly be running
																	 * 		 		 		 * anymore.  This check is critical for databases upgraded by
																	 * 		 		 		 		 * pg_upgrade; both MultiXactIdIsRunning and MultiXactIdExpand assume
																	 * 		 		 		 		 		 * that such multis are never passed.
																	 * 		 		 		 		 		 		 */
																	if (HEAP_LOCKED_UPGRADED(old_infomask))
																				{
																								old_infomask &= ~HEAP_XMAX_IS_MULTI;
																											old_infomask |= HEAP_XMAX_INVALID;
																														goto l5;
																																}

																			/*
																			 * 		 * If the XMAX is already a MultiXactId, then we need to expand it to
																			 * 		 		 * include add_to_xmax; but if all the members were lockers and are
																			 * 		 		 		 * all gone, we can do away with the IS_MULTI bit and just set
																			 * 		 		 		 		 * add_to_xmax as the only locker/updater.  If all lockers are gone
																			 * 		 		 		 		 		 * and we have an updater that aborted, we can also do without a
																			 * 		 		 		 		 		 		 * multi.
																			 * 		 		 		 		 		 		 		 *
																			 * 		 		 		 		 		 		 		 		 * The cost of doing GetMultiXactIdMembers would be paid by
																			 * 		 		 		 		 		 		 		 		 		 * MultiXactIdExpand if we weren't to do this, so this check is not
																			 * 		 		 		 		 		 		 		 		 		 		 * incurring extra work anyhow.
																			 * 		 		 		 		 		 		 		 		 		 		 		 */
																			if (!MultiXactIdIsRunning(xmax, HEAP_XMAX_IS_LOCKED_ONLY(old_infomask)))
																						{
																										if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) ||
																																!TransactionIdDidCommit(MultiXactIdGetUpdateXid(xmax,
																																																		old_infomask)))
																														{
																																			/*
																																			 * 				 * Reset these bits and restart; otherwise fall through to
																																			 * 				 				 * create a new multi below.
																																			 * 				 				 				 */
																																			old_infomask &= ~HEAP_XMAX_IS_MULTI;
																																							old_infomask |= HEAP_XMAX_INVALID;
																																											goto l5;
																																														}
																												}

																					new_status = get_mxact_status_for_lock(mode, is_update);

																							new_xmax = MultiXactIdExpand((MultiXactId) xmax, add_to_xmax,
																																		 new_status);
																									GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
																										}
									else if (old_infomask & HEAP_XMAX_COMMITTED)
											{
														/*
														 * 		 * It's a committed update, so we need to preserve him as updater of
														 * 		 		 * the tuple.
														 * 		 		 		 */
														MultiXactStatus status;
																MultiXactStatus new_status;

																		if (old_infomask2 & HEAP_KEYS_UPDATED)
																						status = MultiXactStatusUpdate;
																				else
																								status = MultiXactStatusNoKeyUpdate;

																						new_status = get_mxact_status_for_lock(mode, is_update);

																								/*
																								 * 		 * since it's not running, it's obviously impossible for the old
																								 * 		 		 * updater to be identical to the current one, so we need not check
																								 * 		 		 		 * for that case as we do in the block above.
																								 * 		 		 		 		 */
																								new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
																										GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
																											}
										else if (TransactionIdIsInProgress(xmax))
												{
															/*
															 * 		 * If the XMAX is a valid, in-progress TransactionId, then we need to
															 * 		 		 * create a new MultiXactId that includes both the old locker or
															 * 		 		 		 * updater and our own TransactionId.
															 * 		 		 		 		 */
															MultiXactStatus new_status;
																	MultiXactStatus old_status;
																			LockTupleMode old_mode;

																					if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
																								{
																												if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
																																	old_status = MultiXactStatusForKeyShare;
																															else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
																																				old_status = MultiXactStatusForShare;
																																		else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
																																						{
																																											if (old_infomask2 & HEAP_KEYS_UPDATED)
																																																	old_status = MultiXactStatusForUpdate;
																																															else
																																																					old_status = MultiXactStatusForNoKeyUpdate;
																																																		}
																																					else
																																									{
																																														/*
																																														 * 				 * LOCK_ONLY can be present alone only when a page has been
																																														 * 				 				 * upgraded by pg_upgrade.  But in that case,
																																														 * 				 				 				 * TransactionIdIsInProgress() should have returned false.  We
																																														 * 				 				 				 				 * assume it's no longer locked in this case.
																																														 * 				 				 				 				 				 */
																																														elog(WARNING, "LOCK_ONLY found for Xid in progress %u", xmax);
																																																		old_infomask |= HEAP_XMAX_INVALID;
																																																						old_infomask &= ~HEAP_XMAX_LOCK_ONLY;
																																																										goto l5;
																																																													}
																																							}
																							else
																										{
																														/* it's an update, but which kind? */
																														if (old_infomask2 & HEAP_KEYS_UPDATED)
																																			old_status = MultiXactStatusUpdate;
																																	else
																																						old_status = MultiXactStatusNoKeyUpdate;
																																			}

																									old_mode = TUPLOCK_from_mxstatus(old_status);

																											/*
																											 * 		 * If the lock to be acquired is for the same TransactionId as the
																											 * 		 		 * existing lock, there's an optimization possible: consider only the
																											 * 		 		 		 * strongest of both locks as the only one present, and restart.
																											 * 		 		 		 		 */
																											if (xmax == add_to_xmax)
																														{
																																		/*
																																		 * 			 * Note that it's not possible for the original tuple to be
																																		 * 			 			 * updated: we wouldn't be here because the tuple would have been
																																		 * 			 			 			 * invisible and we wouldn't try to update it.  As a subtlety,
																																		 * 			 			 			 			 * this code can also run when traversing an update chain to lock
																																		 * 			 			 			 			 			 * future versions of a tuple.  But we wouldn't be here either,
																																		 * 			 			 			 			 			 			 * because the add_to_xmax would be different from the original
																																		 * 			 			 			 			 			 			 			 * updater.
																																		 * 			 			 			 			 			 			 			 			 */
																																		Assert(HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));

																																					/* acquire the strongest of both */
																																					if (mode < old_mode)
																																										mode = old_mode;
																																								/* mustn't touch is_update */

																																								old_infomask |= HEAP_XMAX_INVALID;
																																											goto l5;
																																													}

																													/* otherwise, just fall back to creating a new multixact */
																													new_status = get_mxact_status_for_lock(mode, is_update);
																															new_xmax = MultiXactIdCreate(xmax, old_status,
																																										 add_to_xmax, new_status);
																																	GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
																																		}
											else if (!HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) &&
																 TransactionIdDidCommit(xmax))
													{
																/*
																 * 		 * It's a committed update, so we gotta preserve him as updater of the
																 * 		 		 * tuple.
																 * 		 		 		 */
																MultiXactStatus status;
																		MultiXactStatus new_status;

																				if (old_infomask2 & HEAP_KEYS_UPDATED)
																								status = MultiXactStatusUpdate;
																						else
																										status = MultiXactStatusNoKeyUpdate;

																								new_status = get_mxact_status_for_lock(mode, is_update);

																										/*
																										 * 		 * since it's not running, it's obviously impossible for the old
																										 * 		 		 * updater to be identical to the current one, so we need not check
																										 * 		 		 		 * for that case as we do in the block above.
																										 * 		 		 		 		 */
																										new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
																												GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
																													}
												else
														{
																	/*
																	 * 		 * Can get here iff the locking/updating transaction was running when
																	 * 		 		 * the infomask was extracted from the tuple, but finished before
																	 * 		 		 		 * TransactionIdIsInProgress got to run.  Deal with it as if there was
																	 * 		 		 		 		 * no locker at all in the first place.
																	 * 		 		 		 		 		 */
																	old_infomask |= HEAP_XMAX_INVALID;
																			goto l5;
																				}

													*result_infomask = new_infomask;
														*result_infomask2 = new_infomask2;
															*result_xmax = new_xmax;
}



/*
 *  * For a given MultiXactId, return the hint bits that should be set in the
 *   * tuple's infomask.
 *    *
 *     * Normally this should be called for a multixact that was just created, and
 *      * so is on our local cache, so the GetMembers call is fast.
 *       */
static void
GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
							   uint16 *new_infomask2)
{
		int			nmembers;
			MultiXactMember *members;
				int			i;
					uint16		bits = HEAP_XMAX_IS_MULTI;
						uint16		bits2 = 0;
							bool		has_update = false;
								LockTupleMode strongest = LockTupleKeyShare;

									/*
									 * 	 * We only use this in multis we just created, so they cannot be values
									 * 	 	 * pre-pg_upgrade.
									 * 	 	 	 */
									nmembers = GetMultiXactIdMembers(multi, &members, false, false);

										for (i = 0; i < nmembers; i++)
												{
															LockTupleMode mode;

																	/*
																	 * 		 * Remember the strongest lock mode held by any member of the
																	 * 		 		 * multixact.
																	 * 		 		 		 */
																	mode = TUPLOCK_from_mxstatus(members[i].status);
																			if (mode > strongest)
																							strongest = mode;

																					/* See what other bits we need */
																					switch (members[i].status)
																								{
																												case MultiXactStatusForKeyShare:
																																case MultiXactStatusForShare:
																																case MultiXactStatusForNoKeyUpdate:
																																	break;

																																				case MultiXactStatusForUpdate:
																																					bits2 |= HEAP_KEYS_UPDATED;
																																									break;

																																												case MultiXactStatusNoKeyUpdate:
																																													has_update = true;
																																																	break;

																																																				case MultiXactStatusUpdate:
																																																					bits2 |= HEAP_KEYS_UPDATED;
																																																									has_update = true;
																																																													break;
																																																															}
																						}

											if (strongest == LockTupleExclusive ||
															strongest == LockTupleNoKeyExclusive)
														bits |= HEAP_XMAX_EXCL_LOCK;
												else if (strongest == LockTupleShare)
															bits |= HEAP_XMAX_SHR_LOCK;
													else if (strongest == LockTupleKeyShare)
																bits |= HEAP_XMAX_KEYSHR_LOCK;

														if (!has_update)
																	bits |= HEAP_XMAX_LOCK_ONLY;

															if (nmembers > 0)
																		pfree(members);

																*new_infomask = bits;
																	*new_infomask2 = bits2;
}


/*
 *  * Build a heap tuple representing the configured REPLICA IDENTITY to represent
 *   * the old tuple in a UPDATE or DELETE.
 *    *
 *     * Returns NULL if there's no need to log an identity or if there's no suitable
 *      * key defined.
 *       *
 *        * Pass key_required true if any replica identity columns changed value, or if
 *         * any of them have any external data.  Delete must always pass true.
 *          *
 *           * *copy is set to true if the returned tuple is a modified copy rather than
 *            * the same tuple that was passed in.
 *             */
static HeapTuple
ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_required,
							   bool *copy)
{
		TupleDesc	desc = RelationGetDescr(relation);
			char		replident = relation->rd_rel->relreplident;
				Bitmapset  *idattrs;
					HeapTuple	key_tuple;
						bool		nulls[MaxHeapAttributeNumber];
							Datum		values[MaxHeapAttributeNumber];

								*copy = false;

									if (!RelationIsLogicallyLogged(relation))
												return NULL;

										if (replident == REPLICA_IDENTITY_NOTHING)
													return NULL;

											if (replident == REPLICA_IDENTITY_FULL)
													{
																/*
																 * 		 * When logging the entire old tuple, it very well could contain
																 * 		 		 * toasted columns. If so, force them to be inlined.
																 * 		 		 		 */
																if (HeapTupleHasExternal(tp))
																			{
																							*copy = true;
																										tp = toast_flatten_tuple(tp, desc);
																												}
																		return tp;
																			}

												/* if the key isn't required and we're only logging the key, we're done */
												if (!key_required)
															return NULL;

													/* find out the replica identity columns */
													idattrs = RelationGetIndexAttrBitmap(relation,
																									 INDEX_ATTR_BITMAP_IDENTITY_KEY);

														/*
														 * 	 * If there's no defined replica identity columns, treat as !key_required.
														 * 	 	 * (This case should not be reachable from heap_update, since that should
														 * 	 	 	 * calculate key_required accurately.  But heap_delete just passes
														 * 	 	 	 	 * constant true for key_required, so we can hit this case in deletes.)
														 * 	 	 	 	 	 */
														if (bms_is_empty(idattrs))
																	return NULL;

															/*
															 * 	 * Construct a new tuple containing only the replica identity columns,
															 * 	 	 * with nulls elsewhere.  While we're at it, assert that the replica
															 * 	 	 	 * identity columns aren't null.
															 * 	 	 	 	 */
															heap_deform_tuple(tp, desc, values, nulls);

																for (int i = 0; i < desc->natts; i++)
																		{
																					if (bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber,
																														  idattrs))
																									Assert(!nulls[i]);
																							else
																											nulls[i] = true;
																								}

																	key_tuple = heap_form_tuple(desc, values, nulls);
																		*copy = true;

																			bms_free(idattrs);

																				/*
																				 * 	 * If the tuple, which by here only contains indexed columns, still has
																				 * 	 	 * toasted columns, force them to be inlined. This is somewhat unlikely
																				 * 	 	 	 * since there's limits on the size of indexed columns, so we don't
																				 * 	 	 	 	 * duplicate toast_flatten_tuple()s functionality in the above loop over
																				 * 	 	 	 	 	 * the indexed columns, even if it would be more efficient.
																				 * 	 	 	 	 	 	 */
																				if (HeapTupleHasExternal(key_tuple))
																						{
																									HeapTuple	oldtup = key_tuple;

																											key_tuple = toast_flatten_tuple(oldtup, desc);
																													heap_freetuple(oldtup);
																														}

																					return key_tuple;
}

/*
 *  * Each tuple lock mode has a corresponding heavyweight lock, and one or two
 *   * corresponding MultiXactStatuses (one to merely lock tuples, another one to
 *    * update them).  This table (and the macros below) helps us determine the
 *     * heavyweight lock mode and MultiXactStatus values to use for any particular
 *      * tuple lock strength.
 *       *
 *        * Don't look at lockstatus/updstatus directly!  Use get_mxact_status_for_lock
 *         * instead.
 *          */
static const struct
{
		LOCKMODE	hwlock;
			int			lockstatus;
				int			updstatus;
}

			tupleLockExtraInfo[MaxLockTupleMode + 1] =
{
		{							/* LockTupleKeyShare */
					AccessShareLock,
							MultiXactStatusForKeyShare,
									-1						/* KeyShare does not allow updating tuples */
											},
			{							/* LockTupleShare */
						RowShareLock,
								MultiXactStatusForShare,
										-1						/* Share does not allow updating tuples */
												},
				{							/* LockTupleNoKeyExclusive */
							ExclusiveLock,
									MultiXactStatusForNoKeyUpdate,
											MultiXactStatusNoKeyUpdate
													},
					{							/* LockTupleExclusive */
								AccessExclusiveLock,
										MultiXactStatusForUpdate,
												MultiXactStatusUpdate
														}
};



/* Get the LOCKMODE for a given MultiXactStatus */
#define LOCKMODE_from_mxstatus(status) \
				(tupleLockExtraInfo[TUPLOCK_from_mxstatus((status))].hwlock)

/*
 *  * Acquire heavyweight locks on tuples, using a LockTupleMode strength value.
 *   * This is more readable than having every caller translate it to lock.h's
 *    * LOCKMODE.
 *     */
#define LockTupleTuplock(rel, tup, mode) \
		LockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define UnlockTupleTuplock(rel, tup, mode) \
		UnlockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define ConditionalLockTupleTuplock(rel, tup, mode) \
		ConditionalLockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)

#ifdef USE_PREFETCH
/*
 *  * heap_index_delete_tuples and index_delete_prefetch_buffer use this
 *   * structure to coordinate prefetching activity
 *    */
typedef struct
{
		BlockNumber cur_hblkno;
			int			next_item;
				int			ndeltids;
					TM_IndexDelete *deltids;
} IndexDeletePrefetchState;
#endif

/* heap_index_delete_tuples bottom-up index deletion costing constants */
#define BOTTOMUP_MAX_NBLOCKS			6
#define BOTTOMUP_TOLERANCE_NBLOCKS		3

/*
 *  * heap_index_delete_tuples uses this when determining which heap blocks it
 *   * must visit to help its bottom-up index deletion caller
 *    */
typedef struct IndexDeleteCounts
{
		int16		npromisingtids; /* Number of "promising" TIDs in group */
			int16		ntids;			/* Number of TIDs in group */
				int16		ifirsttid;		/* Offset to group's first deltid */
} IndexDeleteCounts;

/*
 *  * UpdateXmaxHintBits - update tuple hint bits after xmax transaction ends
 *   *
 *    * This is called after we have waited for the XMAX transaction to terminate.
 *     * If the transaction aborted, we guarantee the XMAX_INVALID hint bit will
 *      * be set on exit.  If the transaction committed, we set the XMAX_COMMITTED
 *       * hint bit if possible --- but beware that that may not yet be possible,
 *        * if the transaction committed asynchronously.
 *         *
 *          * Note that if the transaction was a locker only, we set HEAP_XMAX_INVALID
 *           * even if it commits.
 *            *
 *             * Hence callers should look only at XMAX_INVALID.
 *              *
 *               * Note this is not allowed for tuples whose xmax is a multixact.
 *                */
static void
UpdateXmaxHintBits(HeapTupleHeader tuple, Buffer buffer, TransactionId xid)
{
		Assert(TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple), xid));
			Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

				if (!(tuple->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)))
						{
									if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask) &&
														TransactionIdDidCommit(xid))
													HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
																							 xid);
											else
															HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
																									 InvalidTransactionId);
												}
}

/*
 *  * Does the given multixact conflict with the current transaction grabbing a
 *   * tuple lock of the given strength?
 *    *
 *     * The passed infomask pairs up with the given multixact in the tuple header.
 *      *
 *       * If current_is_member is not NULL, it is set to 'true' if the current
 *        * transaction is a member of the given multixact.
 *         */
static bool
DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
								LockTupleMode lockmode, bool *current_is_member)
{
		int			nmembers;
			MultiXactMember *members;
				bool		result = false;
					LOCKMODE	wanted = tupleLockExtraInfo[lockmode].hwlock;

						if (HEAP_LOCKED_UPGRADED(infomask))
									return false;

							nmembers = GetMultiXactIdMembers(multi, &members, false,
																		 HEAP_XMAX_IS_LOCKED_ONLY(infomask));
								if (nmembers >= 0)
										{
													int			i;

															for (i = 0; i < nmembers; i++)
																		{
																						TransactionId memxid;
																									LOCKMODE	memlockmode;

																												if (result && (current_is_member == NULL || *current_is_member))
																																	break;

																															memlockmode = LOCKMODE_from_mxstatus(members[i].status);

																																		/* ignore members from current xact (but track their presence) */
																																		memxid = members[i].xid;
																																					if (TransactionIdIsCurrentTransactionId(memxid))
																																									{
																																														if (current_is_member != NULL)
																																																				*current_is_member = true;
																																																		continue;
																																																					}
																																								else if (result)
																																													continue;

																																											/* ignore members that don't conflict with the lock we want */
																																											if (!DoLockModesConflict(memlockmode, wanted))
																																																continue;

																																														if (ISUPDATE_from_mxstatus(members[i].status))
																																																		{
																																																							/* ignore aborted updaters */
																																																							if (TransactionIdDidAbort(memxid))
																																																													continue;
																																																										}
																																																	else
																																																					{
																																																										/* ignore lockers-only that are no longer in progress */
																																																										if (!TransactionIdIsInProgress(memxid))
																																																																continue;
																																																													}

																																																				/*
																																																				 * 			 * Whatever remains are either live lockers that conflict with our
																																																				 * 			 			 * wanted lock, and updaters that are not aborted.  Those conflict
																																																				 * 			 			 			 * with what we want.  Set up to return true, but keep going to
																																																				 * 			 			 			 			 * look for the current transaction among the multixact members,
																																																				 * 			 			 			 			 			 * if needed.
																																																				 * 			 			 			 			 			 			 */
																																																				result = true;
																																																						}
																	pfree(members);
																		}

									return result;
}

/*
 *  * Do_MultiXactIdWait
 *   *		Actual implementation for the two functions below.
 *    *
 *     * 'multi', 'status' and 'infomask' indicate what to sleep on (the status is
 *      * needed to ensure we only sleep on conflicting members, and the infomask is
 *       * used to optimize multixact access in case it's a lock-only multi); 'nowait'
 *        * indicates whether to use conditional lock acquisition, to allow callers to
 *         * fail if lock is unavailable.  'rel', 'ctid' and 'oper' are used to set up
 *          * context information for error messages.  'remaining', if not NULL, receives
 *           * the number of members that are still running, including any (non-aborted)
 *            * subtransactions of our own transaction.
 *             *
 *              * We do this by sleeping on each member using XactLockTableWait.  Any
 *               * members that belong to the current backend are *not* waited for, however;
 *                * this would not merely be useless but would lead to Assert failure inside
 *                 * XactLockTableWait.  By the time this returns, it is certain that all
 *                  * transactions *of other backends* that were members of the MultiXactId
 *                   * that conflict with the requested status are dead (and no new ones can have
 *                    * been added, since it is not legal to add members to an existing
 *                     * MultiXactId).
 *                      *
 *                       * But by the time we finish sleeping, someone else may have changed the Xmax
 *                        * of the containing tuple, so the caller needs to iterate on us somehow.
 *                         *
 *                          * Note that in case we return false, the number of remaining members is
 *                           * not to be trusted.
 *                            */
static bool
Do_MultiXactIdWait(MultiXactId multi, MultiXactStatus status,
						   uint16 infomask, bool nowait,
						   				   Relation rel, ItemPointer ctid, XLTW_Oper oper,
										   				   int *remaining)
{
		bool		result = true;
			MultiXactMember *members;
				int			nmembers;
					int			remain = 0;

						/* for pre-pg_upgrade tuples, no need to sleep at all */
						nmembers = HEAP_LOCKED_UPGRADED(infomask) ? -1 :
									GetMultiXactIdMembers(multi, &members, false,
																		  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

							if (nmembers >= 0)
									{
												int			i;

														for (i = 0; i < nmembers; i++)
																	{
																					TransactionId memxid = members[i].xid;
																								MultiXactStatus memstatus = members[i].status;

																											if (TransactionIdIsCurrentTransactionId(memxid))
																															{
																																				remain++;
																																								continue;
																																											}

																														if (!DoLockModesConflict(LOCKMODE_from_mxstatus(memstatus),
																																										 LOCKMODE_from_mxstatus(status)))
																																		{
																																							if (remaining && TransactionIdIsInProgress(memxid))
																																													remain++;
																																											continue;
																																														}

																																	/*
																																	 * 			 * This member conflicts with our multi, so we have to sleep (or
																																	 * 			 			 * return failure, if asked to avoid waiting.)
																																	 * 			 			 			 *
																																	 * 			 			 			 			 * Note that we don't set up an error context callback ourselves,
																																	 * 			 			 			 			 			 * but instead we pass the info down to XactLockTableWait.  This
																																	 * 			 			 			 			 			 			 * might seem a bit wasteful because the context is set up and
																																	 * 			 			 			 			 			 			 			 * tore down for each member of the multixact, but in reality it
																																	 * 			 			 			 			 			 			 			 			 * should be barely noticeable, and it avoids duplicate code.
																																	 * 			 			 			 			 			 			 			 			 			 */
																																	if (nowait)
																																					{
																																										result = ConditionalXactLockTableWait(memxid);
																																														if (!result)
																																																				break;
																																																	}
																																				else
																																									XactLockTableWait(memxid, rel, ctid, oper);
																																						}

																pfree(members);
																	}

								if (remaining)
											*remaining = remain;

									return result;
}

/*
 *  * MultiXactIdWait
 *   *		Sleep on a MultiXactId.
 *    *
 *     * By the time we finish sleeping, someone else may have changed the Xmax
 *      * of the containing tuple, so the caller needs to iterate on us somehow.
 *       *
 *        * We return (in *remaining, if not NULL) the number of members that are still
 *         * running, including any (non-aborted) subtransactions of our own transaction.
 *          */
static void
MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
						Relation rel, ItemPointer ctid, XLTW_Oper oper,
										int *remaining)
{
		(void) Do_MultiXactIdWait(multi, status, infomask, false,
											  rel, ctid, oper, remaining);
}

/*
 *  * ConditionalMultiXactIdWait
 *   *		As above, but only lock if we can get the lock without blocking.
 *    *
 *     * By the time we finish sleeping, someone else may have changed the Xmax
 *      * of the containing tuple, so the caller needs to iterate on us somehow.
 *       *
 *        * If the multixact is now all gone, return true.  Returns false if some
 *         * transactions might still be running.
 *          *
 *           * We return (in *remaining, if not NULL) the number of members that are still
 *            * running, including any (non-aborted) subtransactions of our own transaction.
 *             */
static bool
ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
								   uint16 infomask, Relation rel, int *remaining)
{
		return Do_MultiXactIdWait(multi, status, infomask, true,
											  rel, NULL, XLTW_None, remaining);
}



/* ----------------
 *        initscan - scan code common to heap_beginscan and heap_rescan
 * ----------------
 */
static void
initscan(HeapScanDesc scan, ScanKey key, bool keep_startblock)
{
    ParallelBlockTableScanDesc bpscan = NULL;
    bool        allow_strat;
    bool        allow_sync;

    /*
     * Determine the number of blocks we have to scan.
     *
     * It is sufficient to do this once at scan start, since any tuples added
     * while the scan is in progress will be invisible to my snapshot anyway.
     * (That is not true when using a non-MVCC snapshot.  However, we couldn't
     * guarantee to return tuples added after scan start anyway, since they
     * might go into pages we already scanned.  To guarantee consistent
     * results for a non-MVCC snapshot, the caller must hold some higher-level
     * lock that ensures the interesting tuple(s) won't change.)
     */
    if (scan->rs_base.rs_parallel != NULL)
    {
         bpscan = (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
         scan->rs_nblocks = bpscan->phs_nblocks;
    }
    else
         scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);

    /*
     * If the table is large relative to NBuffers, use a bulk-read access
     * strategy and enable synchronized scanning (see syncscan.c).  Although
     * the thresholds for these features could be different, we make them the
     * same so that there are only two behaviors to tune rather than four.
     * (However, some callers need to be able to disable one or both of these
     * behaviors, independently of the size of the table; also there is a GUC
     * variable that can disable synchronized scanning.)
     *
     * Note that table_block_parallelscan_initialize has a very similar test;
     * if you change this, consider changing that one, too.
     */
    if (!RelationUsesLocalBuffers(scan->rs_base.rs_rd) && scan->rs_nblocks > NBuffers / 4) {
        allow_strat = (scan->rs_base.rs_flags & SO_ALLOW_STRAT) != 0;
        allow_sync = (scan->rs_base.rs_flags & SO_ALLOW_SYNC) != 0;
    }
    else
        allow_strat = allow_sync = false;

    if (allow_strat)
    {
        /* During a rescan, keep the previous strategy object. */
        if (scan->rs_strategy == NULL)
            scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
        }
        else
        {
            if (scan->rs_strategy != NULL)
                FreeAccessStrategy(scan->rs_strategy);
            scan->rs_strategy = NULL;
        }

        if (scan->rs_base.rs_parallel != NULL)
        {
        /* For parallel scan, believe whatever ParallelTableScanDesc says. */
            if (scan->rs_base.rs_parallel->phs_syncscan)
                scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
            else
                scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
         }
         else if (keep_startblock)
         {
             /*
              * When rescanning, we want to keep the previous startblock setting,
              * so that rewinding a cursor doesn't generate surprising results.
              * Reset the active syncscan setting, though.
              */
              if (allow_sync && synchronize_seqscans)
                   scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
              else
                   scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
        }
        else if (allow_sync && synchronize_seqscans)
        {
             scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
             scan->rs_startblock = ss_get_location(scan->rs_base.rs_rd, scan->rs_nblocks);
        }
        else
        {
             scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
             scan->rs_startblock = 0;
         }

         scan->rs_numblocks = InvalidBlockNumber;
         scan->rs_inited = false;
         scan->rs_ctup.t_data = NULL;
         ItemPointerSetInvalid(&scan->rs_ctup.t_self);
         scan->rs_cbuf = InvalidBuffer;
         scan->rs_cblock = InvalidBlockNumber;

         /* page-at-a-time fields are always invalid when not rs_inited */

         /*
          * copy the scan key, if appropriate
          */
         if (key != NULL && scan->rs_base.rs_nkeys > 0)
              memcpy(scan->rs_base.rs_key, key, scan->rs_base.rs_nkeys * sizeof(ScanKeyData));

         /*
          * Currently, we only have a stats counter for sequential heap scans (but
          * e.g for bitmap scans the underlying bitmap index scans will be counted,
          * and for sample scans we update stats for tuple fetches).
          */
         if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN)
             pgstat_count_heap_scan(scan->rs_base.rs_rd);
}


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
	//Relation rel = table_open(RelnameGetRelid(make_vertex_adjlist_alias(RelationGetRelationName(relation))), AccessShareLock);
	TableAmRoutine *tableam = GetHeapamTableAmRoutine();
	Oid oid = get_relname_relid(make_vertex_adjlist_alias(get_rel_name(RelationGetRelid(relation))), get_rel_namespace(RelationGetRelid(relation)));
	Relation rel = RelationIdGetRelation(oid);

	TableScanDesc *desc = tableam->scan_begin(rel,
		  									 snapshot, 0, NULL, parallel_scan, flags);
	vertex_desc->desc = palloc(sizeof (TableScanDesc *) * vertex_desc->ndesc);
	vertex_desc->desc[0] = desc;
	return vertex_desc;
}

void vertex_scan_end(TableScanDesc sscan) {
	VertexScanDescData *vertex_desc = sscan;
	
	//Relation rel = table_open(RelnameGetRelid(make_vertex_adjlist_alias(RelationGetRelationName(relation))), AccessShareLock);
	TableAmRoutine *tableam = GetHeapamTableAmRoutine();
	//Oid oid = get_relname_relid(make_vertex_adjlist_alias(get_rel_name(vertex_desc->rs_base.rs_rd->rd_id)), get_rel_namespace(vertex_desc->rs_base.rs_rd->rd_id));
	//table_close((*vertex_desc->desc[0]->rs_rd, AccessShareLock);

	tableam->scan_end(vertex_desc->desc[0]);

	return ;
}

/*
 * Restart relation scan.  If set_params is set to true, allow_{strat,
 * sync, pagemode} (see scan_begin) changes should be taken into account.
 */
void vertex_scan_rescan(TableScanDesc scan, struct ScanKeyData *key,
            bool set_params, bool allow_strat,
            bool allow_sync, bool allow_pagemode) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_estimate not implemented")));
    
}

/*
 * Return next tuple from `scan`, store in slot.
 */
bool vertex_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                TupleTableSlot *slot) {
	VertexScanDescData *vertex_desc = sscan;
	TableAmRoutine *tableam = GetHeapamTableAmRoutine();

	ereport(WARNING,errmsg_internal("here"));
	return tableam->scan_getnextslot(vertex_desc->desc[0], direction, slot);

    VertexHeapScanDesc so = (VertexHeapScanDesc) sscan;
	bool		res;

	/*
	 * If we've already initialized this scan, we can just advance it in the
	 * appropriate direction.  If we haven't done so yet, we call a routine to
	 * get the first item in the scan.
	 */
	if (!HashScanPosIsValid(so->currPos))
		return vertex_hash_first(sscan, direction);

	return vertex_hash_next(sscan, direction);
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

Page
vertex_RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple,
					 bool token);

Page
vertex_RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple,
					 bool token)
{
	Page		pageHeader;
	OffsetNumber offnum;

	/*
	 * A tuple that's being inserted speculatively should already have its
	 * token set.
	 */
	Assert(!token || HeapTupleHeaderIsSpeculative(tuple->t_data));

	/*
	 * Do not allow tuples with invalid combinations of hint bits to be placed
	 * on a page.  This combination is detected as corruption by the
	 * contrib/amcheck logic, so if you disable this assertion, make
	 * corresponding changes there.
	 */
	Assert(!((tuple->t_data->t_infomask & HEAP_XMAX_COMMITTED) &&
			 (tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI)));

	/* Add the tuple to the page */
	pageHeader = BufferGetPage(buffer);

	offnum = PageAddItem(pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, false, true);

	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "failed to add tuple to page");

	/* Update tuple->t_self to the actual position where it was stored */
	ItemPointerSet(&(tuple->t_self), BufferGetBlockNumber(buffer), offnum);

	/*
	 * Insert the correct position into CTID of the stored tuple, too (unless
	 * this is a speculative insertion, in which case the token is held in
	 * CTID field instead)
	 */
	if (!token)
	{
		ItemId		itemId = PageGetItemId(pageHeader, offnum);
		HeapTupleHeader item = (HeapTupleHeader) PageGetItem(pageHeader, itemId);

		item->t_ctid = tuple->t_self;
	}
    return pageHeader;
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

	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	TM_Result	result;

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	result = heap_update(relation, otid, tuple, cid, crosscheck, wait,
						 tmfd, lockmode);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	/*
	 * Decide whether new index entries are needed for the tuple
	 *
	 * Note: heap_update returns the tid (location) of the new tuple in the
	 * t_self field.
	 *
	 * If it's a HOT update, we mustn't insert new index entries.
	 */
	*update_indexes = result == TM_Ok && !HeapTupleIsHeapOnly(tuple);

	if (shouldFree)
		pfree(tuple);

	return result;
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


    
    SMgrRelation srel;
    *minmulti = GetOldestMultiXactId();

    srel = RelationCreateStorage(*newrnode, persistence);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrnode, INIT_FORKNUM);
		smgrimmedsync(srel, INIT_FORKNUM);

    smgrclose(srel);
    HASHCTL hash_ctl;

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(graphid);
    hash_ctl.entrysize = sizeof(vertex_hash_struct);

    HTAB *rel_hash_table = ShmemInitHash(RelationGetRelationName(rel),16, 1000, &hash_ctl,
			     HASH_ELEM | HASH_BLOBS | HASH_COMPARE);
    vertex_hash_init(rel, 0, INIT_FORKNUM);
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
    BlockNumber curpages;
    BlockNumber relpages;
    double reltuples;
    BlockNumber relallvisible;
    double density;
    double overhead_bytes_per_tuple = HEAP_OVERHEAD_BYTES_PER_TUPLE;
    double usable_bytes_per_page = HEAP_USABLE_BYTES_PER_PAGE;
    
    /* it should have storage, so we can call the smgr */
    curpages = RelationGetNumberOfBlocks(rel);

    /* coerce values in pg_class to more desirable types */
    relpages = (BlockNumber) rel->rd_rel->relpages;
    reltuples = (double) rel->rd_rel->reltuples;
    relallvisible = (BlockNumber) rel->rd_rel->relallvisible;

    /*
     * HACK: if the relation has never yet been vacuumed, use a minimum size
     * estimate of 10 pages.  The idea here is to avoid assuming a
     * newly-created table is really small, even if it currently is, because
     * that may not be true once some data gets loaded into it.  Once a vacuum
     * or analyze cycle has been done on it, it's more reasonable to believe
     * the size is somewhat stable.
     *
     * (Note that this is only an issue if the plan gets cached and used again
     * after the table has been filled.  What we're trying to avoid is using a
     * nestloop-type plan on a table that has grown substantially since the
     * plan was made.  Normally, autovacuum/autoanalyze will occur once enough
     * inserts have happened and cause cached-plan invalidation; but that
     * doesn't happen instantaneously, and it won't happen at all for cases
     * such as temporary tables.)
     *
     * We test "never vacuumed" by seeing whether reltuples < 0.
     *
     * If the table has inheritance children, we don't apply this heuristic.
     * Totally empty parent tables are quite common, so we should be willing
     * to believe that they are empty.
     */
    if (curpages < 10 && reltuples < 0 && !rel->rd_rel->relhassubclass)
        curpages = 10;

    /* report estimated # pages */
    *pages = curpages;
    /* quick exit if rel is clearly empty */
    if (curpages == 0)
    {
        *tuples = 0;
        *allvisfrac = 0;
        return;
    }

    /* estimate number of tuples from previous tuple density */
    if (reltuples >= 0 && relpages > 0)
        density = reltuples / (double) relpages;
    else {
        /*
         * When we have no data because the relation was never yet vacuumed,
         * estimate tuple width from attribute datatypes.  We assume here that
         * the pages are completely full, which is OK for tables but is
         * probably an overestimate for indexes.  Fortunately
         * get_relation_info() can clamp the overestimate to the parent
         * table's size.
         *
         * Note: this code intentionally disregards alignment considerations,
         * because (a) that would be gilding the lily considering how crude
         * the estimate is, (b) it creates platform dependencies in the
         * default plans which are kind of a headache for regression testing,
         * and (c) different table AMs might use different padding schemes.
         */
        int32 tuple_width;

        tuple_width = get_rel_data_width(rel, attr_widths);
        tuple_width += overhead_bytes_per_tuple;
        /* note: integer division is intentional here */
        density = usable_bytes_per_page / tuple_width;
    }
    *tuples = rint(density * (double) curpages);

    /*
     * We use relallvisible as-is, rather than scaling it up like we do for
     * the pages and tuples counts, on the theory that any pages added since
     * the last VACUUM are most likely not marked all-visible.  But costsize.c
     * wants it converted to a fraction.
     */
    if (relallvisible == 0 || curpages <= 0)
        *allvisfrac = 0;
    else if ((double) relallvisible >= curpages)
        *allvisfrac = 1;
    else
        *allvisfrac = (double) relallvisible / curpages;
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



/*
 *  * Perform XLogInsert of an XLOG_HEAP2_NEW_CID record
 *   *
 *    * This is only used in wal_level >= WAL_LEVEL_LOGICAL, and only for catalog
 *     * tuples.
 *      */
static XLogRecPtr
log_heap_new_cid(Relation relation, HeapTuple tup)
{
		xl_heap_new_cid xlrec;

			XLogRecPtr	recptr;
				HeapTupleHeader hdr = tup->t_data;

					Assert(ItemPointerIsValid(&tup->t_self));
						Assert(tup->t_tableOid != InvalidOid);

							xlrec.top_xid = GetTopTransactionId();
								xlrec.target_node = relation->rd_node;
									xlrec.target_tid = tup->t_self;

										/*
										 * 	 * If the tuple got inserted & deleted in the same TX we definitely have a
										 * 	 	 * combo CID, set cmin and cmax.
										 * 	 	 	 */
										if (hdr->t_infomask & HEAP_COMBOCID)
												{
															Assert(!(hdr->t_infomask & HEAP_XMAX_INVALID));
																	Assert(!HeapTupleHeaderXminInvalid(hdr));
																			xlrec.cmin = HeapTupleHeaderGetCmin(hdr);
																					xlrec.cmax = HeapTupleHeaderGetCmax(hdr);
																							xlrec.combocid = HeapTupleHeaderGetRawCommandId(hdr);
																								}
											/* No combo CID, so only cmin or cmax can be set by this TX */
											else
													{
																/*
																 * 		 * Tuple inserted.
																 * 		 		 *
																 * 		 		 		 * We need to check for LOCK ONLY because multixacts might be
																 * 		 		 		 		 * transferred to the new tuple in case of FOR KEY SHARE updates in
																 * 		 		 		 		 		 * which case there will be an xmax, although the tuple just got
																 * 		 		 		 		 		 		 * inserted.
																 * 		 		 		 		 		 		 		 */
																if (hdr->t_infomask & HEAP_XMAX_INVALID ||
																					HEAP_XMAX_IS_LOCKED_ONLY(hdr->t_infomask))
																			{
																							xlrec.cmin = HeapTupleHeaderGetRawCommandId(hdr);
																										xlrec.cmax = InvalidCommandId;
																												}
																		/* Tuple from a different tx updated or deleted. */
																		else
																					{
																									xlrec.cmin = InvalidCommandId;
																												xlrec.cmax = HeapTupleHeaderGetRawCommandId(hdr);

																														}
																				xlrec.combocid = InvalidCommandId;
																					}

												/*
												 * 	 * Note that we don't need to register the buffer here, because this
												 * 	 	 * operation does not modify the page. The insert/update/delete that
												 * 	 	 	 * called us certainly did, but that's WAL-logged separately.
												 * 	 	 	 	 */
												XLogBeginInsert();
													XLogRegisterData((char *) &xlrec, SizeOfHeapNewCid);

														/* will be looked at irrespective of origin */

														recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_NEW_CID);

															return recptr;
}



/*
 *  * Return the MultiXactStatus corresponding to the given tuple lock mode.
 *   */
static MultiXactStatus
get_mxact_status_for_lock(LockTupleMode mode, bool is_update)
{
		int			retval;

			if (is_update)
						retval = tupleLockExtraInfo[mode].updstatus;
				else
							retval = tupleLockExtraInfo[mode].lockstatus;

					if (retval == -1)
								elog(ERROR, "invalid lock tuple mode %d/%s", mode,
													 is_update ? "true" : "false");

						return (MultiXactStatus) retval;
}


/*
 *  * Acquire heavyweight lock on the given tuple, in preparation for acquiring
 *   * its normal, Xmax-based tuple lock.
 *    *
 *     * have_tuple_lock is an input and output parameter: on input, it indicates
 *      * whether the lock has previously been acquired (and this function does
 *       * nothing in that case).  If this function returns success, have_tuple_lock
 *        * has been flipped to true.
 *         *
 *          * Returns false if it was unable to obtain the lock; this can only happen if
 *           * wait_policy is Skip.
 *            */
static bool
heap_acquire_tuplock(Relation relation, ItemPointer tid, LockTupleMode mode,
							 LockWaitPolicy wait_policy, bool *have_tuple_lock)
{
		if (*have_tuple_lock)
					return true;

			switch (wait_policy)
					{
								case LockWaitBlock:
												LockTupleTuplock(relation, tid, mode);
															break;

																	case LockWaitSkip:
																		if (!ConditionalLockTupleTuplock(relation, tid, mode))
																							return false;
																					break;

																							case LockWaitError:
																								if (!ConditionalLockTupleTuplock(relation, tid, mode))
																													ereport(ERROR,
																																					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
																																					 						 errmsg("could not obtain lock on row in relation \"%s\"",
																																												 								RelationGetRelationName(relation))));
																											break;
																												}
				*have_tuple_lock = true;

					return true;
}


#define VertexHashGetFillFactor(relation) \
	 (relation)->rd_options ? \
	 ((HashOptions *) (relation)->rd_options)->fillfactor :	\
	 HASH_DEFAULT_FILLFACTOR
#define VertexHashGetTargetPageUsage(relation) \
	(BLCKSZ * VertexHashGetFillFactor(relation) / 100)


#include "access/hash_xlog.h"

/*
 *	_hash_init() -- Initialize the metadata page of a hash index,
 *				the initial buckets, and the initial bitmap page.
 *
 * The initial number of buckets is dependent on num_tuples, an estimate
 * of the number of tuples to be loaded into the index initially.  The
 * chosen number of buckets is returned.
 *
 * We are fairly cavalier about locking here, since we know that no one else
 * could be accessing this index.  In particular the rule about not holding
 * multiple buffer locks is ignored.
 */
uint32
vertex_hash_init(Relation rel, double num_tuples, ForkNumber forkNum)
{
	Buffer		metabuf;
	Buffer		buf;
	Buffer		bitmapbuf;
	Page		pg;
	HashMetaPage metap;
	RegProcedure procid;
	int32		data_width;
	int32		item_width;
	int32		ffactor;
	uint32		num_buckets;
	uint32		i;
	bool		use_wal;
    zero_damaged_pages = true;
	/* safety check */
	if (RelationGetNumberOfBlocksInFork(rel, forkNum) != 0)
		elog(ERROR, "cannot initialize non-empty hash index \"%s\"",
			 RelationGetRelationName(rel));

	/*
	 * WAL log creation of pages if the relation is persistent, or this is the
	 * init fork.  Init forks for unlogged relations always need to be WAL
	 * logged.
	 */
	use_wal = RelationNeedsWAL(rel) || forkNum == INIT_FORKNUM;

	/*
	 * Determine the target fill factor (in tuples per bucket) for this index.
	 * The idea is to make the fill factor correspond to pages about as full
	 * as the user-settable fillfactor parameter says.  We can compute it
	 * exactly since the index datatype (i.e. uint32 hash key) is fixed-width.
	 */
	data_width = sizeof(uint32);
	item_width = MAXALIGN(sizeof(IndexTupleData)) + MAXALIGN(data_width) +
		sizeof(ItemIdData);		/* include the line pointer */
    //TODO FillFactor is a configuration setting, don't hard coded this forever
	ffactor = 75 / item_width;
	/* keep to a sane range */
	if (ffactor < 10)
		ffactor = 10;


	/*
	 * We initialize the metapage, the first N bucket pages, and the first
	 * bitmap page in sequence, using _hash_getnewbuf to cause smgrextend()
	 * calls to occur.  This ensures that the smgr level has the right idea of
	 * the physical index length.
	 *
	 * Critical section not required, because on error the creation of the
	 * whole relation will be rolled back.
	 */
	metabuf = _hash_getnewbuf(rel, HASH_METAPAGE, MAIN_FORKNUM);
	_hash_init_metabuffer(metabuf, num_tuples, 0, ffactor, false);
	MarkBufferDirty(metabuf);

	pg = BufferGetPage(metabuf);
	metap = HashPageGetMeta(pg);

	/* XLOG stuff */
	if (use_wal)
	{
		xl_hash_init_meta_page xlrec;
		XLogRecPtr	recptr;

		xlrec.num_tuples = num_tuples;
		xlrec.procid = metap->hashm_procid;
		xlrec.ffactor = metap->hashm_ffactor;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashInitMetaPage);
		XLogRegisterBuffer(0, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INIT_META_PAGE);

		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	num_buckets = metap->hashm_maxbucket + 1;

	/*
	 * Release buffer lock on the metapage while we initialize buckets.
	 * Otherwise, we'll be in interrupt holdoff and the CHECK_FOR_INTERRUPTS
	 * won't accomplish anything.  It's a bad idea to hold buffer locks for
	 * long intervals in any case, since that can block the bgwriter.
	 */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/*
	 * Initialize and WAL Log the first N buckets
	 */
	for (i = 0; i < num_buckets; i++)
	{
		BlockNumber blkno;

		/* Allow interrupts, in case N is huge */
		CHECK_FOR_INTERRUPTS();

		blkno = BUCKET_TO_BLKNO(metap, i);
		buf = _hash_getnewbuf(rel, blkno, MAIN_FORKNUM);
		_hash_initbuf(buf, metap->hashm_maxbucket, i, LH_BUCKET_PAGE, false);
		MarkBufferDirty(buf);

		if (use_wal)
			log_newpage(&rel->rd_node,
						forkNum,
						blkno,
						BufferGetPage(buf),
						true);
		_hash_relbuf(rel, buf);
	}

	/* Now reacquire buffer lock on metapage */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Initialize bitmap page
	 */
	bitmapbuf = _hash_getnewbuf(rel, num_buckets + 1, MAIN_FORKNUM);
	_hash_initbitmapbuffer(bitmapbuf, metap->hashm_bmsize, false);
	MarkBufferDirty(bitmapbuf);

	/* add the new bitmap page to the metapage's list of bitmaps */
	/* metapage already has a write lock */
	if (metap->hashm_nmaps >= HASH_MAX_BITMAPS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of overflow pages in hash index \"%s\"",
						RelationGetRelationName(rel))));

	metap->hashm_mapp[metap->hashm_nmaps] = num_buckets + 1;

	metap->hashm_nmaps++;
	MarkBufferDirty(metabuf);

	/* XLOG stuff */
	if (use_wal)
	{
		xl_hash_init_bitmap_page xlrec;
		XLogRecPtr	recptr;

		xlrec.bmsize = metap->hashm_bmsize;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashInitBitmapPage);
		XLogRegisterBuffer(0, bitmapbuf, REGBUF_WILL_INIT);

		/*
		 * This is safe only because nobody else can be modifying the index at
		 * this stage; it's only visible to the transaction that is creating
		 * it.
		 */
		XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INIT_BITMAP_PAGE);

		PageSetLSN(BufferGetPage(bitmapbuf), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	/* all done */
	_hash_relbuf(rel, bitmapbuf);
	_hash_relbuf(rel, metabuf);

	return num_buckets;
}



/*
 *	_hash_doinsert() -- Handle insertion of a single index tuple.
 *
 *		This routine is called by the public interface routines, hashbuild
 *		and hashinsert.  By here, itup is completely filled in.
 */
void
vertex_hash_doinsert(Relation rel, HeapTuple itup, Relation heapRel)
{
    HeapTuple tuple = itup;
	Buffer		buf = InvalidBuffer;
	Buffer		bucket_buf;
	Buffer		metabuf;
	HashMetaPage metap;
	HashMetaPage usedmetap = NULL;
	Page		metapage;
	Page		page;
	HashPageOpaque pageopaque;
	Size		itemsz;
	bool		do_expand;
	uint32		hashkey;
	Bucket		bucket;
	OffsetNumber itup_off;

	/*
	 * Get the hash key for the item (it's stored in the index tuple itself).
	 */
	bool isnull;
	hashkey = heap_getattr(itup, 1, RelationGetDescr(rel), &isnull);
	//hashkey = vertex_hash_get_indextuple_hashkey(itup); //TODO

	/* compute item size too */
	itemsz = (itup)->t_len;
	itemsz = MAXALIGN(itemsz);	/* be safe, PageAddItem will do this but we
								 * need to be consistent */

restart_insert:

	/*
	 * Read the metapage.  We don't lock it yet; HashMaxItemSize() will
	 * examine pd_pagesize_version, but that can't change so we can examine it
	 * without a lock.
	 */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK, LH_META_PAGE);
	metapage = BufferGetPage(metabuf);

	/*
	 * Check whether the item can fit on a hash page at all. (Eventually, we
	 * ought to try to apply TOAST methods if not.)  Note that at this point,
	 * itemsz doesn't include the ItemId.
	 *
	 * XXX this is useless code if we are only storing hash keys.
	 */
	if (itemsz > HashMaxItemSize(metapage))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds hash maximum %zu",
						itemsz, HashMaxItemSize(metapage)),
				 errhint("Values larger than a buffer page cannot be indexed.")));

	/* Lock the primary bucket page for the target bucket. */
	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_WRITE,
										  &usedmetap);
	Assert(usedmetap != NULL);

	CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(buf));

	/* remember the primary bucket buffer to release the pin on it at end. */
	bucket_buf = buf;

	page = BufferGetPage(buf);
	pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
	bucket = pageopaque->hasho_bucket;

	/*
	 * If this bucket is in the process of being split, try to finish the
	 * split before inserting, because that might create room for the
	 * insertion to proceed without allocating an additional overflow page.
	 * It's only interesting to finish the split if we're trying to insert
	 * into the bucket from which we're removing tuples (the "old" bucket),
	 * not if we're trying to insert into the bucket into which tuples are
	 * being moved (the "new" bucket).
	 */
	if (H_BUCKET_BEING_SPLIT(pageopaque) && IsBufferCleanupOK(buf))
	{
		/* release the lock on bucket buffer, before completing the split. */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		_hash_finish_split(rel, metabuf, buf, bucket,
						   usedmetap->hashm_maxbucket,
						   usedmetap->hashm_highmask,
						   usedmetap->hashm_lowmask);

		/* release the pin on old and meta buffer.  retry for insert. */
		_hash_dropbuf(rel, buf);
		_hash_dropbuf(rel, metabuf);
		goto restart_insert;
	}

	/* Do the insertion */
	while (PageGetFreeSpace(page) < itemsz)
	{
		BlockNumber nextblkno;

		/*
		 * Check if current page has any DEAD tuples. If yes, delete these
		 * tuples and see if we can get a space for the new item to be
		 * inserted before moving to the next page in the bucket chain.
		 */
		if (H_HAS_DEAD_TUPLES(pageopaque))
		{

			/*if (IsBufferCleanupOK(buf))
			{
				_hash_vacuum_one_page(rel, heapRel, metabuf, buf);

				if (PageGetFreeSpace(page) >= itemsz)
					break;		// OK, now we have enough space 
			}*/
		}

		/*
		 * no space on this page; check for an overflow page
		 */
		nextblkno = pageopaque->hasho_nextblkno;

		if (BlockNumberIsValid(nextblkno))
		{
			/*
			 * ovfl page exists; go get it.  if it doesn't have room, we'll
			 * find out next pass through the loop test above.  we always
			 * release both the lock and pin if this is an overflow page, but
			 * only the lock if this is the primary bucket page, since the pin
			 * on the primary bucket must be retained throughout the scan.
			 */
			if (buf != bucket_buf)
				_hash_relbuf(rel, buf);
			else
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			buf = _hash_getbuf(rel, nextblkno, HASH_WRITE, LH_OVERFLOW_PAGE);
			page = BufferGetPage(buf);
		}
		else
		{
			/*
			 * we're at the end of the bucket chain and we haven't found a
			 * page with enough room.  allocate a new overflow page.
			 */

			/* release our write lock without modifying buffer */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/* chain to a new overflow page */
			buf = _hash_addovflpage(rel, metabuf, buf, (buf == bucket_buf) ? true : false);
			page = BufferGetPage(buf);

			/* should fit now, given test above */
			Assert(PageGetFreeSpace(page) >= itemsz);
		}
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
		Assert((pageopaque->hasho_flag & LH_PAGE_TYPE) == LH_OVERFLOW_PAGE);
		Assert(pageopaque->hasho_bucket == bucket);
	}

	/*
	 * Write-lock the metapage so we can increment the tuple count. After
	 * incrementing it, check to see if it's time for a split.
	 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/* found page with enough space, so add the item here */
	itup_off = _hash_pgaddtup(rel, buf, itemsz, (IndexTuple)itup);
	MarkBufferDirty(buf);

	/* metapage operations */
	metap = HashPageGetMeta(metapage);
	metap->hashm_ntuples += 1;

	/* Make sure this stays in sync with _hash_expandtable() */
	do_expand = metap->hashm_ntuples >
		(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1);

	MarkBufferDirty(metabuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_insert xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = itup_off;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashInsert);

		XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);

		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) itup, itemsz);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INSERT);

		PageSetLSN(BufferGetPage(buf), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	/* drop lock on metapage, but keep pin */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/*
	 * Release the modified page and ensure to release the pin on primary
	 * page.
	 */
	_hash_relbuf(rel, buf);
	if (buf != bucket_buf)
		_hash_dropbuf(rel, bucket_buf);

	/* Attempt to split if a split is needed */
	if (do_expand)
		_hash_expandtable(rel, metabuf);

	/* Finally drop our pin on the metapage */
	_hash_dropbuf(rel, metabuf);
}

#include "executor/nodeSeqscan.h"

static exec_seq_scan_scan_key_hook_type prev_exec_seq_scan_scan_key_hook = NULL;

void postgraph_seq_scan_key_hook (SeqScanState *node,
					   				int *numScanKeys, ScanKey scanKeys) {

	ereport(WARNING, errmsg("In hook"));

vertex_exec_index_build_ScanKeys(node, node->ss.ss_currentRelation,
					   node->ss.ps.plan->qual,
					   scanKeys, numScanKeys);

	return;
}

void register_seq_scan_hook(void){
	prev_exec_seq_scan_scan_key_hook = exec_seq_scan_scan_key_hook;
	exec_seq_scan_scan_key_hook = postgraph_seq_scan_key_hook;
}

void unregister_seq_scan_hook(void){
	exec_seq_scan_scan_key_hook = prev_exec_seq_scan_scan_key_hook;
}
	



/*
 * ExecIndexBuildScanKeys
 *		Build the index scan keys from the index qualification expressions
 *
 * The index quals are passed to the index AM in the form of a ScanKey array.
 * This routine sets up the ScanKeys, fills in all constant fields of the
 * ScanKeys, and prepares information about the keys that have non-constant
 * comparison values.  We divide index qual expressions into five types:
 *
 * 1. Simple operator with constant comparison value ("indexkey op constant").
 * For these, we just fill in a ScanKey containing the constant value.
 *
 * 2. Simple operator with non-constant value ("indexkey op expression").
 * For these, we create a ScanKey with everything filled in except the
 * expression value, and set up an IndexRuntimeKeyInfo struct to drive
 * evaluation of the expression at the right times.
 *
 * 3. RowCompareExpr ("(indexkey, indexkey, ...) op (expr, expr, ...)").
 * For these, we create a header ScanKey plus a subsidiary ScanKey array,
 * as specified in access/skey.h.  The elements of the row comparison
 * can have either constant or non-constant comparison values.
 *
 * 4. ScalarArrayOpExpr ("indexkey op ANY (array-expression)").  If the index
 * supports amsearcharray, we handle these the same as simple operators,
 * setting the SK_SEARCHARRAY flag to tell the AM to handle them.  Otherwise,
 * we create a ScanKey with everything filled in except the comparison value,
 * and set up an IndexArrayKeyInfo struct to drive processing of the qual.
 * (Note that if we use an IndexArrayKeyInfo struct, the array expression is
 * always treated as requiring runtime evaluation, even if it's a constant.)
 *
 * 5. NullTest ("indexkey IS NULL/IS NOT NULL").  We just fill in the
 * ScanKey properly.
 *
 * This code is also used to prepare ORDER BY expressions for amcanorderbyop
 * indexes.  The behavior is exactly the same, except that we have to look up
 * the operator differently.  Note that only cases 1 and 2 are currently
 * possible for ORDER BY.
 *
 * Input params are:
 *
 * planstate: executor state node we are working for
 * index: the index we are building scan keys for
 * quals: indexquals (or indexorderbys) expressions
 * isorderby: true if processing ORDER BY exprs, false if processing quals
 * *runtimeKeys: ptr to pre-existing IndexRuntimeKeyInfos, or NULL if none
 * *numRuntimeKeys: number of pre-existing runtime keys
 *
 * Output params are:
 *
 * *scanKeys: receives ptr to array of ScanKeys
 * *numScanKeys: receives number of scankeys
 * *runtimeKeys: receives ptr to array of IndexRuntimeKeyInfos, or NULL if none
 * *numRuntimeKeys: receives number of runtime keys
 * *arrayKeys: receives ptr to array of IndexArrayKeyInfos, or NULL if none
 * *numArrayKeys: receives number of array keys
 *
 * Caller may pass NULL for arrayKeys and numArrayKeys to indicate that
 * IndexArrayKeyInfos are not supported.
 */
void
vertex_exec_index_build_ScanKeys(PlanState *planstate, Relation index,
					   List *quals,
					   ScanKey scanKeys, int *numScanKeys)
{
	ListCell   *qual_cell;

	IndexRuntimeKeyInfo *runtime_keys;
	int			n_runtime_keys;
	int			max_runtime_keys;
	int			j;

	/* Allocate array for ScanKey */
	scanKeys = palloc(sizeof(IndexRuntimeKeyInfo));

	/*
	 * runtime_keys array is dynamically resized as needed.  We handle it this
	 * way so that the same runtime keys array can be shared between
	 * indexquals and indexorderbys, which will be processed in separate calls
	 * of this function.  Caller must be sure to pass in NULL/0 for first
	 * call.
	 */
	numScanKeys = 0;



	/*
	 * for each opclause in the given qual, convert the opclause into a single
	 * scan key
	 */

	j = 0;
	foreach(qual_cell, quals)
	{
		Expr	   *clause = (Expr *) lfirst(qual_cell);

		Oid			opno;		/* operator's OID */
		RegProcedure opfuncid;	/* operator proc id used in scan */
		Oid			opfamily;	/* opfamily of index column */
		int			op_strategy;	/* operator's strategy number */
		Oid			op_lefttype;	/* operator's declared input types */
		Oid			op_righttype;
		Expr	   *leftop;		/* expr on lhs of operator */
		Expr	   *rightop;	/* expr on rhs ... */
		AttrNumber	varattno;	/* att number used in scan */
		int			indnkeyatts;

		indnkeyatts = 4;// id, startid, endid, properties
		if (IsA(clause, OpExpr))
		{		
			ScanKey		this_scan_key = palloc(sizeof(ScanKey));


			IndexRuntimeKeyInfo * runtimeKey = (IndexRuntimeKeyInfo *)scanKeys;
			/* indexkey op const or indexkey op expression */
			int			flags = 0;
			Datum		scanvalue;

			opno = ((OpExpr *) clause)->opno;
			opfuncid = ((OpExpr *) clause)->opfuncid;

			/*
			 * leftop should be the index key Var, possibly relabeled
			 */
			leftop = (Expr *) get_leftop(clause);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == INDEX_VAR))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > indnkeyatts)
				elog(ERROR, "bogus index qualification");

			if (varattno != 1)
				continue;
			/*
			 * We have to look up the operator's strategy number.  This
			 * provides a cross-check that the operator does match the index.
			 */
			opfamily = index->rd_opfamily[varattno - 1];

			bool *isorderby;
			get_op_opfamily_properties(opno, opfamily, isorderby,
									   &op_strategy,
									   &op_lefttype,
									   &op_righttype);

			if (isorderby)
				flags |= SK_ORDER_BY;

			/*
			 * rightop is the constant or variable comparison value
			 */
			rightop = (Expr *) get_rightop(clause);

			if (rightop && IsA(rightop, RelabelType))
				rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			


	
			/*
			 * initialize the scan key's fields appropriately
			 */
			ScanKeyEntryInitialize(this_scan_key,
								   flags,
								   varattno,	/* attribute number to scan */
								   op_strategy, /* op's strategy */
								   op_righttype,	/* strategy subtype */
								   ((OpExpr *) clause)->inputcollid,	/* collation */
								   opfuncid,	/* reg proc to use */
								   scanvalue);	/* constant */	


			runtimeKey->scan_key = this_scan_key;
			runtimeKey->key_expr = ExecInitExpr(rightop, planstate);
			runtimeKey->key_toastable = TypeIsToastable(op_righttype);

			scanvalue = (Datum) 0;
			numScanKeys++;

			break;
		}
	}

}