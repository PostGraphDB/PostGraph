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


void
vertex_exec_index_build_ScanKeys(PlanState *planstate, Relation index,
					   List *quals,
					   ScanKey *scanKeys, int *numScanKeys);
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
	TableAmRoutine *tableam = GetHeapamTableAmRoutine();
	Oid oid = get_relname_relid(make_vertex_adjlist_alias(get_rel_name(RelationGetRelid(relation))), get_rel_namespace(RelationGetRelid(relation)));
				ereport(WARNING, errmsg("begin_scan runtimeKey %p",&key));
	Relation rel = RelationIdGetRelation(oid);

	TableScanDesc *desc = tableam->scan_begin(rel, snapshot, nkeys, key, parallel_scan, flags);
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
            errmsg_internal("vertex_scan_rescan not implemented")));
    
}

/*
 * Return next tuple from `scan`, store in slot.
 */
bool vertex_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                TupleTableSlot *slot) {
	VertexScanDescData *vertex_desc = sscan;

	/*if(vertex_desc->rs_base.rs_nkeys != 1)
		ereport(ERROR,errmsg_internal("edges are a key value store, provide the key"));
	
	ereport(WARNING, errmsg("scankey in get slot %p %p", vertex_desc->rs_base.rs_key, &vertex_desc->rs_base.rs_key));
	if(vertex_desc->rs_base.rs_key == 0)
		ereport(ERROR,errmsg_internal("edges are a key value store, provide the key"));
			ereport(WARNING, errmsg("runtimeKey %p", vertex_desc->rs_base.rs_key));
	TableAmRoutine *tableam = GetHeapamTableAmRoutine(); 
 	IndexRuntimeKeyInfo *indexRuntimeKeyInfo = (IndexRuntimeKeyInfo *)vertex_desc->rs_base.rs_key;
	*/
	TableAmRoutine *tableam = GetHeapamTableAmRoutine();
	//ereport(WARNING, errmsg("Datum %d graphid %ld", ((Datum)((IndexRuntimeKeyInfo *)vertex_desc->rs_base.rs_key)->scan_key->sk_argument)), DATUM_GET_GRAPHID((Datum)((IndexRuntimeKeyInfo *)vertex_desc->rs_base.rs_key)->scan_key->sk_argument));
	
	bool result =true;
	return tableam->scan_getnextslot(vertex_desc->desc[0], direction, slot);
/*	while (result) {
	result =  tableam->scan_getnextslot(vertex_desc->desc[0], direction, slot);

	if (!result) {
		ereport(WARNING, errmsg("result"));
		return result;
	}
	SeqScanState *node= ((IndexRuntimeKeyInfo *)vertex_desc->rs_base.rs_key)->key_expr->parent;
	ExprState *qual = node->ss.ps.qual;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	ResetExprContext(econtext);

		econtext->ecxt_scantuple = slot;

		if (qual == NULL || ExecQual(qual, econtext)) {
			ereport(WARNING, errmsg("qual passed"));
			return true;
		} else
			ereport(WARNING, errmsg("qual failed"));
	}
	return result;*/
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