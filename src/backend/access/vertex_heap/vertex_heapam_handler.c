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
 *  * MultiXactIdGetUpdateXid
 *   *
 *    * Given a multixact Xmax and corresponding infomask, which does not have the
 *     * HEAP_XMAX_LOCK_ONLY bit set, obtain and return the Xid of the updating
 *      * transaction.
 *       *
 *        * Caller is expected to check the status of the updating transaction, if
 *         * necessary.
 *          */
static TransactionId
MultiXactIdGetUpdateXid(TransactionId xmax, uint16 t_infomask)
{
		TransactionId update_xact = InvalidTransactionId;
			MultiXactMember *members;
				int			nmembers;

					Assert(!(t_infomask & HEAP_XMAX_LOCK_ONLY));
						Assert(t_infomask & HEAP_XMAX_IS_MULTI);

							/*
							 * 	 * Since we know the LOCK_ONLY bit is not set, this cannot be a multi from
							 * 	 	 * pre-pg_upgrade.
							 * 	 	 	 */
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
																															 * 			 * in an assert-enabled build, walk the whole array to ensure
																															 * 			 			 * there's no other updater.
																															 * 			 			 			 */
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



/* ----------------
 *        heapgettup - fetch next heap tuple
 *
 *        Initialize the scan if not already done; then advance to the next
 *        tuple as indicated by "dir"; return the next tuple in scan->rs_ctup,
 *        or set scan->rs_ctup.t_data = NULL if no more tuples.
 *
 * dir == NoMovementScanDirection means "re-fetch the tuple indicated
 * by scan->rs_ctup".
 *
 * Note: the reason nkeys/key are passed separately, even though they are
 * kept in the scan descriptor, is that the caller may not want us to check
 * the scankeys.
 *
 * Note: when we fall off the end of the scan in either direction, we
 * reset rs_inited.  This means that a further request with the same
 * scan direction will restart the scan, which is a bit odd, but a
 * request with the opposite scan direction will start a fresh scan
 * in the proper direction.  The latter is required behavior for cursors,
 * while the former case is generally undefined behavior in Postgres
 * so we don't care too much.
 * ----------------
 */
static void
heapgettup(HeapScanDesc scan, ScanDirection dir, int nkeys, ScanKey key)
{
    HeapTuple    tuple = &(scan->rs_ctup);
    Snapshot    snapshot = scan->rs_base.rs_snapshot;
    bool        backward = ScanDirectionIsBackward(dir);
    BlockNumber page;
    bool        finished;
    Page        dp;
    int            lines;
    OffsetNumber lineoff;
    int            linesleft;
    ItemId        lpp;

    /*
     * calculate next starting lineoff, given scan direction
     */
    if (ScanDirectionIsForward(dir))
    {
        if (!scan->rs_inited)
        {
            /*
             * return null immediately if relation is empty
             */
            if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
            {
                Assert(!BufferIsValid(scan->rs_cbuf));
                tuple->t_data = NULL;
                return;
            }
            if (scan->rs_base.rs_parallel != NULL)
            {
                ParallelBlockTableScanDesc pbscan =
                      (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
                ParallelBlockTableScanWorker pbscanwork =
                       scan->rs_parallelworkerdata;
                                                                                                                                        table_block_parallelscan_startblock_init(scan->rs_base.rs_rd, pbscanwork, pbscan);

                                                                                                                                        page = table_block_parallelscan_nextpage(scan->rs_base.rs_rd, pbscanwork, pbscan);

                                                                                                                                        /* Other processes might have already finished the scan. */
                if (page == InvalidBlockNumber) {
                    Assert(!BufferIsValid(scan->rs_cbuf));
                    tuple->t_data = NULL;
                    return;
                }
            }
            else
                 page = scan->rs_startblock; /* first page */
            
        heapgetpage((TableScanDesc) scan, page);
            
        lineoff = FirstOffsetNumber;    /* first offnum */
            scan->rs_inited = true;
       }
       else
       {
           /* continue from previously returned page/tuple */
           page = scan->rs_cblock; /* current page */
           lineoff =            /* next offnum */
                OffsetNumberNext(ItemPointerGetOffsetNumber(&(tuple->t_self)));
       }

       LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

       dp = BufferGetPage(scan->rs_cbuf);
       TestForOldSnapshot(snapshot, scan->rs_base.rs_rd, dp);
       lines = PageGetMaxOffsetNumber(dp);
       /* page and lineoff now reference the physically next tid */

       linesleft = lines - lineoff + 1;
   }
   else if (backward)
   {
        /* backward parallel scan not supported */
        Assert(scan->rs_base.rs_parallel == NULL);

        if (!scan->rs_inited)
        {
            /*
             * return null immediately if relation is empty
             */
             if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
             {
                 Assert(!BufferIsValid(scan->rs_cbuf));
                 tuple->t_data = NULL;
                 return;
             }

             /*
              * Disable reporting to syncscan logic in a backwards scan; it's
              * not very likely anyone else is doing the same thing at the same
              * time, and much more likely that we'll just bollix things for
              * forward scanners.
              */
             scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

             /*
              * Start from last page of the scan.  Ensure we take into account
              * rs_numblocks if it's been adjusted by heap_setscanlimits().
              */
             if (scan->rs_numblocks != InvalidBlockNumber)
                  page = (scan->rs_startblock + scan->rs_numblocks - 1) % scan->rs_nblocks;
             else if (scan->rs_startblock > 0)
                  page = scan->rs_startblock - 1;
             else
                  page = scan->rs_nblocks - 1;
             heapgetpage((TableScanDesc) scan, page);
        }
        else
        {
             /* continue from previously returned page/tuple */
             page = scan->rs_cblock; /* current page */
        }

        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

        dp = BufferGetPage(scan->rs_cbuf);
        TestForOldSnapshot(snapshot, scan->rs_base.rs_rd, dp);
        lines = PageGetMaxOffsetNumber(dp);

        if (!scan->rs_inited)
        {
            lineoff = lines;    /* final offnum */
            scan->rs_inited = true;
        }
        else
        {
             /*
              * The previous returned tuple may have been vacuumed since the
              * previous scan when we use a non-MVCC snapshot, so we must
              * re-establish the lineoff <= PageGetMaxOffsetNumber(dp)
              * invariant
              */
             lineoff =            /* previous offnum */
                   Min(lines,
                       OffsetNumberPrev(ItemPointerGetOffsetNumber(&(tuple->t_self))));
         }
         /* page and lineoff now reference the physically previous tid */

         linesleft = lineoff;
     }
     else
     {
         /*
          * ``no movement'' scan direction: refetch prior tuple
          */
         if (!scan->rs_inited)
         {
              Assert(!BufferIsValid(scan->rs_cbuf));
              tuple->t_data = NULL;
              return;
         }

         page = ItemPointerGetBlockNumber(&(tuple->t_self));
         if (page != scan->rs_cblock)
              heapgetpage((TableScanDesc) scan, page);

         /* Since the tuple was previously fetched, needn't lock page here */
         dp = BufferGetPage(scan->rs_cbuf);
         TestForOldSnapshot(snapshot, scan->rs_base.rs_rd, dp);
         lineoff = ItemPointerGetOffsetNumber(&(tuple->t_self));
         lpp = PageGetItemId(dp, lineoff);
         Assert(ItemIdIsNormal(lpp));

         tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
         tuple->t_len = ItemIdGetLength(lpp);

         return;
     }

     /*
      * advance the scan until we find a qualifying tuple or run out of stuff
      * to scan
      */
     lpp = PageGetItemId(dp, lineoff);
     for (;;)
     {
         /*
          * Only continue scanning the page while we have lines left.
          *
          * Note that this protects us from accessing line pointers past
          * PageGetMaxOffsetNumber(); both for forward scans when we resume the
          * table scan, and for when we start scanning a new page.
          */
          while (linesleft > 0)
          {
               if (ItemIdIsNormal(lpp))
               {
                   bool valid;

                   tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
                   tuple->t_len = ItemIdGetLength(lpp);
                   ItemPointerSet(&(tuple->t_self), page, lineoff);

                   /*
                    * if current tuple qualifies, return it.
                    */
                   valid = HeapTupleSatisfiesVisibility(tuple,
                         snapshot,
                         scan->rs_cbuf);

                   HeapCheckForSerializableConflictOut(valid, scan->rs_base.rs_rd,
                                                 tuple, scan->rs_cbuf,
                                                 snapshot);

                   if (valid && key != NULL)
                       HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
                                                        nkeys, key, valid);

                   if (valid)
                   {
                        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
                        return;
                    }
              }

              /*
               * otherwise move to the next item on the page
               */
               --linesleft;
               if (backward)
               {
                    --lpp;            /* move back in this page's ItemId array */
                    --lineoff;
               }
              else
               {
                    ++lpp;            /* move forward in this page's ItemId array */
                    ++lineoff;
               }
          }

          /*
           * if we get here, it means we've exhausted the items on this page and
           * it's time to move to the next.
           */
          LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

          /*
           * advance to next/prior page and detect end of scan
           */
          if (backward)
          {
               finished = (page == scan->rs_startblock) ||
                           (scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);
                if (page == 0)
                     page = scan->rs_nblocks;
                page--;
          }
          else if (scan->rs_base.rs_parallel != NULL)
          {
               ParallelBlockTableScanDesc pbscan =
                       (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
               ParallelBlockTableScanWorker pbscanwork =
                       scan->rs_parallelworkerdata;

               page = table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
                                             pbscanwork, pbscan);
                finished = (page == InvalidBlockNumber);
         }
         else
         {
              page++;
              if (page >= scan->rs_nblocks)
                  page = 0;
               finished = (page == scan->rs_startblock) ||
                               (scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);

               /*
                * Report our new scan position for synchronization purposes. We
                * don't do that when moving backwards, however. That would just
                * mess up any other forward-moving scanners.
                *
                * Note: we do this before checking for end of scan so that the
                * final state of the position hint is back at the start of the
                * rel.  That's not strictly necessary, but otherwise when you run
                * the same query multiple times the starting position would shift
                * a little bit backwards on every invocation, which is confusing.
                * We don't guarantee any specific ordering in general, though.
                */
               if (scan->rs_base.rs_flags & SO_ALLOW_SYNC)
                     ss_report_location(scan->rs_base.rs_rd, page);
          }

          /*
           * return NULL if we've exhausted all the pages
           */
          if (finished)
          {
               if (BufferIsValid(scan->rs_cbuf))
                    ReleaseBuffer(scan->rs_cbuf);
               scan->rs_cbuf = InvalidBuffer;
               scan->rs_cblock = InvalidBlockNumber;
               tuple->t_data = NULL;
               scan->rs_inited = false;
               return;
          }

          heapgetpage((TableScanDesc) scan, page);

          LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

          dp = BufferGetPage(scan->rs_cbuf);
          TestForOldSnapshot(snapshot, scan->rs_base.rs_rd, dp);
          lines = PageGetMaxOffsetNumber((Page) dp);
          linesleft = lines;
          if (backward)
          {
               lineoff = lines;
               lpp = PageGetItemId(dp, lines);
          }
          else
          {
               lineoff = FirstOffsetNumber;
               lpp = PageGetItemId(dp, FirstOffsetNumber);
          }
     }
}

/* ----------------
 *        heapgettup_pagemode - fetch next heap tuple in page-at-a-time mode
 *
 *        Same API as heapgettup, but used in page-at-a-time mode
 *
 * The internal logic is much the same as heapgettup's too, but there are some
 * differences: we do not take the buffer content lock (that only needs to
 * happen inside heapgetpage), and we iterate through just the tuples listed
 * in rs_vistuples[] rather than all tuples on the page.  Notice that
 * lineindex is 0-based, where the corresponding loop variable lineoff in
 * heapgettup is 1-based.
 * ----------------
 */
static void
heapgettup_pagemode(HeapScanDesc scan, ScanDirection dir, int nkeys, ScanKey key)
{
    HeapTuple    tuple = &(scan->rs_ctup);
    bool        backward = ScanDirectionIsBackward(dir);
    BlockNumber page;
    bool        finished;
    Page        dp;
    int            lines;
    int            lineindex;
    OffsetNumber lineoff;
    int            linesleft;
    ItemId        lpp;

    /*
     * calculate next starting lineindex, given scan direction
     */
    if (ScanDirectionIsForward(dir))
    {
         if (!scan->rs_inited)
         {
             /*
              * return null immediately if relation is empty
              */
             if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
             {
                 Assert(!BufferIsValid(scan->rs_cbuf));
                 tuple->t_data = NULL;
                 return;
             }
             if (scan->rs_base.rs_parallel != NULL)
             {
                 ParallelBlockTableScanDesc pbscan =
                                   (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
                 ParallelBlockTableScanWorker pbscanwork =
                                   scan->rs_parallelworkerdata;
                 table_block_parallelscan_startblock_init(scan->rs_base.rs_rd, pbscanwork, pbscan);

                 page = table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
                                 pbscanwork, pbscan);

                 /* Other processes might have already finished the scan. */
                 if (page == InvalidBlockNumber)
                 {
                      Assert(!BufferIsValid(scan->rs_cbuf));
                      tuple->t_data = NULL;
                      return;
                 }
            }
             else
                  page = scan->rs_startblock; /* first page */
            heapgetpage((TableScanDesc) scan, page);
            lineindex = 0;
            scan->rs_inited = true;
       }
       else
       {
             /* continue from previously returned page/tuple */
             page = scan->rs_cblock; /* current page */
             lineindex = scan->rs_cindex + 1;
       }

       dp = BufferGetPage(scan->rs_cbuf);
       TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, dp);
       lines = scan->rs_ntuples;
       /* page and lineindex now reference the next visible tid */

       linesleft = lines - lineindex;
   }
   else if (backward)
   {
         /* backward parallel scan not supported */
         Assert(scan->rs_base.rs_parallel == NULL);

         if (!scan->rs_inited)
         {
               /*
                * return null immediately if relation is empty
                */
                if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
                {
                     Assert(!BufferIsValid(scan->rs_cbuf));
                     tuple->t_data = NULL;
                     return;
               }

               /*
                * Disable reporting to syncscan logic in a backwards scan; it's
                * not very likely anyone else is doing the same thing at the same
                * time, and much more likely that we'll just bollix things for
                * forward scanners.
                */
               scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

               /*
                * Start from last page of the scan.  Ensure we take into account
                * rs_numblocks if it's been adjusted by heap_setscanlimits().
                */
               if (scan->rs_numblocks != InvalidBlockNumber)
                   page = (scan->rs_startblock + scan->rs_numblocks - 1) % scan->rs_nblocks;
               else if (scan->rs_startblock > 0)
                   page = scan->rs_startblock - 1;
               else
                   page = scan->rs_nblocks - 1;
               heapgetpage((TableScanDesc) scan, page);
          }
          else
          {
                /* continue from previously returned page/tuple */
                page = scan->rs_cblock; /* current page */
          }

          dp = BufferGetPage(scan->rs_cbuf);
          TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, dp);
          lines = scan->rs_ntuples;

          if (!scan->rs_inited)
          {
               lineindex = lines - 1;
               scan->rs_inited = true;
          }
          else
          {
               lineindex = scan->rs_cindex - 1;
          }
          /* page and lineindex now reference the previous visible tid */

          linesleft = lineindex + 1;
    }
    else
    {
         /*
          * ``no movement'' scan direction: refetch prior tuple
          */
          if (!scan->rs_inited)
          {
                Assert(!BufferIsValid(scan->rs_cbuf));
                tuple->t_data = NULL;
                return;
          }

          page = ItemPointerGetBlockNumber(&(tuple->t_self));
          if (page != scan->rs_cblock)
               heapgetpage((TableScanDesc) scan, page);

          /* Since the tuple was previously fetched, needn't lock page here */
          dp = BufferGetPage(scan->rs_cbuf);
          TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, dp);
          lineoff = ItemPointerGetOffsetNumber(&(tuple->t_self));
          lpp = PageGetItemId(dp, lineoff);
          Assert(ItemIdIsNormal(lpp));

          tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
                                                                                                                                  tuple->t_len = ItemIdGetLength(lpp);
                                                                                                                                  /* check that rs_cindex is in sync */
                                                                                                                                  Assert(scan->rs_cindex < scan->rs_ntuples);
                                                                                                                                  Assert(lineoff == scan->rs_vistuples[scan->rs_cindex]);

                                                                                                                                  return;
                                                                                                                                  }

          /*
           * advance the scan until we find a qualifying tuple or run out of stuff
           * to scan
           */
           for (;;)
           {
                while (linesleft > 0)
                {
                     lineoff = scan->rs_vistuples[lineindex];
                     lpp = PageGetItemId(dp, lineoff);
                     Assert(ItemIdIsNormal(lpp));

                     tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
                     tuple->t_len = ItemIdGetLength(lpp);
                     ItemPointerSet(&(tuple->t_self), page, lineoff);

                     /*
                      * if current tuple qualifies, return it.
                      */
                     if (key != NULL)
                     {
                           bool        valid;

                           HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
                                             nkeys, key, valid);
                           if (valid)
                           {
                                scan->rs_cindex = lineindex;
                                return;
                           }
                    }
                     else
                     {
                          scan->rs_cindex = lineindex;
                          return;
                     }

                     /*
                      * otherwise move to the next item on the page
                      */
                     --linesleft;
                     if (backward)
                          --lineindex;
                     else
                          ++lineindex;
                                                                                                                                                                                                                                            }

                     /*
                      * if we get here, it means we've exhausted the items on this page and
                      * it's time to move to the next.
                      */
                     if (backward)
                     {
                          finished = (page == scan->rs_startblock) ||
                                   (scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);
                          if (page == 0)
                                page = scan->rs_nblocks;
                          page--;
                     }
                     else if (scan->rs_base.rs_parallel != NULL)
                     {
                           ParallelBlockTableScanDesc pbscan =
                                            (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
                           ParallelBlockTableScanWorker pbscanwork =
                                     scan->rs_parallelworkerdata;

                            page = table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
                                                  pbscanwork, pbscan);
                            finished = (page == InvalidBlockNumber);
                     }
                     else
                     {
                           page++;
                           if (page >= scan->rs_nblocks)
                                 page = 0;
                           finished = (page == scan->rs_startblock) ||
                                      (scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);

                           /*
                            * Report our new scan position for synchronization purposes. We
                            * don't do that when moving backwards, however. That would just
                            * mess up any other forward-moving scanners.
                            *
                            * Note: we do this before checking for end of scan so that the
                            * final state of the position hint is back at the start of the
                            * rel.  That's not strictly necessary, but otherwise when you run
                            * the same query multiple times the starting position would shift
                            * a little bit backwards on every invocation, which is confusing.
                            * We don't guarantee any specific ordering in general, though.
                            */
                            if (scan->rs_base.rs_flags & SO_ALLOW_SYNC)
                                  ss_report_location(scan->rs_base.rs_rd, page);
                      }

                      /*
                       * return NULL if we've exhausted all the pages
                       */
                      if (finished)
                      {
                           if (BufferIsValid(scan->rs_cbuf))
                                 ReleaseBuffer(scan->rs_cbuf);
                           scan->rs_cbuf = InvalidBuffer;
                           scan->rs_cblock = InvalidBlockNumber;
                           tuple->t_data = NULL;
                           scan->rs_inited = false;
                           return;
                      }

                      heapgetpage((TableScanDesc) scan, page);

                      dp = BufferGetPage(scan->rs_cbuf);
                      TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, dp);
                      lines = scan->rs_ntuples;
                      linesleft = lines;
                      if (backward)
                           lineindex = lines - 1;
                      else
                           lineindex = 0;
              }
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
TableScanDesc vertex_scan_begin(Relation relation, Snapshot snapshot, int nkeys,
                struct ScanKeyData *key, 
                ParallelTableScanDesc parallel_scan, uint32 flags) {
    HeapScanDesc scan;

    /*
     * increment relation ref count while scanning relation
     *
     * This is just to make really sure the relcache entry won't go away while
     * the scan has a pointer to it.  Caller should be holding the rel open
     * anyway, so this is redundant in all normal scenarios...
     */
    RelationIncrementReferenceCount(relation);

    /*
     * allocate and initialize scan descriptor
     */
    scan = (HeapScanDesc) palloc(sizeof(HeapScanDescData));

    scan->rs_base.rs_rd = relation;
    scan->rs_base.rs_snapshot = snapshot;
    scan->rs_base.rs_nkeys = nkeys;
    scan->rs_base.rs_flags = flags;
    scan->rs_base.rs_parallel = parallel_scan;
    scan->rs_strategy = NULL;    /* set in initscan */

    /*
     * Disable page-at-a-time mode if it's not a MVCC-safe snapshot.
     * TODO: Research Non MVCC Snapshots and Page-at-a-time mode
     */
    if (!(snapshot && IsMVCCSnapshot(snapshot)))
        scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;

    /*
     * For seqscan and sample scans in a serializable transaction, acquire a
     * predicate lock on the entire relation. This is required not only to
     * lock all the matching tuples, but also to conflict with new insertions
     * into the table. In an indexscan, we take page locks on the index pages
     * covering the range specified in the scan qual, but in a heap scan there
     * is nothing more fine-grained to lock. A bitmap scan is a different
     * story, there we have already scanned the index and locked the index
     * pages covering the predicate. But in that case we still have to lock
     * any matching heap tuples. For sample scan we could optimize the locking
     * to be at least page-level granularity, but we'd need to add per-tuple
     * locking for that.
     */
    if (scan->rs_base.rs_flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN))
    {
            /*
             * Ensure a missing snapshot is noticed reliably, even if the
             * isolation mode means predicate locking isn't performed (and
             * therefore the snapshot isn't used here).
             */
            Assert(snapshot);
            PredicateLockRelation(relation, snapshot);
        }

    /* we only need to set this up once */
    scan->rs_ctup.t_tableOid = RelationGetRelid(relation);

    /*
     * Allocate memory to keep track of page allocation for parallel workers
     * when doing a parallel scan.
     */
    if (parallel_scan != NULL)
        scan->rs_parallelworkerdata = palloc(sizeof(ParallelBlockTableScanWorkerData));
    else
        scan->rs_parallelworkerdata = NULL;

    /*
     * we do this here instead of in initscan() because heap_rescan also calls
     * initscan() and we don't want to allocate memory again
     */
    if (nkeys > 0)
        scan->rs_base.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
    else
        scan->rs_base.rs_key = NULL;

    initscan(scan, key, false);

    return (TableScanDesc) scan;
}

void vertex_scan_end(TableScanDesc sscan) {
    HeapScanDesc scan = (HeapScanDesc) sscan;
    
    /* Note: no locking manipulations needed */

    /*
     * unpin scan buffers
     */
    if (BufferIsValid(scan->rs_cbuf))
            ReleaseBuffer(scan->rs_cbuf);

    /*
     * decrement relation reference count and free scan descriptor storage
     */
    RelationDecrementReferenceCount(scan->rs_base.rs_rd);

    if (scan->rs_base.rs_key)
            pfree(scan->rs_base.rs_key);

    if (scan->rs_strategy != NULL)
            FreeAccessStrategy(scan->rs_strategy);

    if (scan->rs_parallelworkerdata != NULL)
            pfree(scan->rs_parallelworkerdata);

    if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
            UnregisterSnapshot(scan->rs_base.rs_snapshot);

    pfree(scan);
}

/*
 * Restart relation scan.  If set_params is set to true, allow_{strat,
 * sync, pagemode} (see scan_begin) changes should be taken into account.
 */
void vertex_scan_rescan(TableScanDesc scan, struct ScanKeyData *key,
            bool set_params, bool allow_strat,
            bool allow_sync, bool allow_pagemode) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_rescan not implemented")));
}

/*
 * Return next tuple from `scan`, store in slot.
 */
bool vertex_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                TupleTableSlot *slot) {
    HeapScanDesc scan = (HeapScanDesc) sscan;

    /* Note: no locking manipulations needed */
    if (sscan->rs_flags & SO_ALLOW_PAGEMODE)
        heapgettup_pagemode(scan, direction, sscan->rs_nkeys, sscan->rs_key);
    else
        heapgettup(scan, direction, sscan->rs_nkeys, sscan->rs_key);

    if (scan->rs_ctup.t_data == NULL) {
        ExecClearTuple(slot);
        return false;
    }

    /*
     * if we get here it means we have a new current scan tuple, so point to
     * the proper return buffer and return the tuple.
     */

    pgstat_count_heap_getnext(scan->rs_base.rs_rd);

    ExecStoreBufferHeapTuple(&scan->rs_ctup, slot, scan->rs_cbuf);
    return true;
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
    
    return NULL;
}

/*
 * Initialize ParallelTableScanDesc for a parallel scan of this relation.
 * `pscan` will be sized according to parallelscan_estimate() for the same
 * relation.
 */
Size vertex_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_initialize not implemented")));
    
    return NULL;
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
 * that tuple. Index AMs can use that to avoid returning that tid in
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


/* ------------------------------------------------------------------------
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
    

    return NULL;
}

/* ------------------------------------------------------------------------
 * Manipulations of physical tuples.
 * ------------------------------------------------------------------------
 */

/* see table_tuple_insert() for reference about parameters */
void vertex_tuple_insert(Relation relation, TupleTableSlot *slot,
                         CommandId cid, int options,
                         struct BulkInsertStateData *bistate) {
    // TODO, this is not implemented, merely the framework of whats to come
    bool shouldFree = true;
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

    /* Update the tuple with table oid */
    slot->tts_tableOid = RelationGetRelid(relation);
    tuple->t_tableOid = slot->tts_tableOid;

    /* Perform the insertion, and copy the resulting ItemPointer */
    heap_insert(relation, tuple, cid, options, bistate);
    ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

    if (shouldFree)
            pfree(tuple);
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

    TM_Result    result;
        TransactionId xid = GetCurrentTransactionId();
    ItemId        lp;
    HeapTupleData tp;
    Page        page;
    BlockNumber block;
    Buffer        buffer;
    Buffer        vmbuffer = InvalidBuffer;
    TransactionId new_xmax;
    uint16        new_infomask,
                        new_infomask2;
    bool        have_tuple_lock = false;
    bool        iscombo;
    bool        all_visible_cleared = false;
    HeapTuple    old_key_tuple = NULL;    /* replica identity of the tuple */
    bool        old_key_copied = false;

    Assert(ItemPointerIsValid(tid));

    /*
     *      * Forbid this during a parallel operation, lest it allocate a combo CID.
     *           * Other workers might need that combo CID for visibility checks, and we
     *                * have no provision for broadcasting it to them.
     *                     */
    if (IsInParallelMode())
            ereport(ERROR,
                                (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
                                              errmsg("cannot delete tuples during a parallel operation")));

    block = ItemPointerGetBlockNumber(tid);
    buffer = ReadBuffer(relation, block);
    page = BufferGetPage(buffer);

    /*
     *      * Before locking the buffer, pin the visibility map page if it appears to
     *           * be necessary.  Since we haven't got the lock yet, someone else might be
     *                * in the middle of changing this, so we'll need to recheck after we have
     *                     * the lock.
     *                          */
    if (PageIsAllVisible(page))
            visibilitymap_pin(relation, block, &vmbuffer);

    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    Assert(ItemIdIsNormal(lp));

    tp.t_tableOid = RelationGetRelid(relation);
    tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    tp.t_len = ItemIdGetLength(lp);
    tp.t_self = *tid;

l1:
    /*
     *      * If we didn't pin the visibility map page and the page has become all
     *           * visible while we were busy locking the buffer, we'll have to unlock and
     *                * re-lock, to avoid holding the buffer lock across an I/O.  That's a bit
     *                     * unfortunate, but hopefully shouldn't happen often.
     *                          */
    if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
        {
                LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
                    visibilitymap_pin(relation, block, &vmbuffer);
                        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
                        }

    result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);

    if (result == TM_Invisible)
        {
                UnlockReleaseBuffer(buffer);
                    ereport(ERROR,
                                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                                      errmsg("attempted to delete invisible tuple")));
                       }
    else if (result == TM_BeingModified && wait)
        {
                TransactionId xwait;
                    uint16        infomask;

                        /* must copy state data before unlocking buffer */
                        xwait = HeapTupleHeaderGetRawXmax(tp.t_data);
                            infomask = tp.t_data->t_infomask;

                                /*
                                 *          * Sleep until concurrent transaction ends -- except when there's a
                                 *                   * single locker and it's our own transaction.  Note we don't care
                                 *                            * which lock mode the locker has, because we need the strongest one.
                                 *                                     *
                                 *                                              * Before sleeping, we need to acquire tuple lock to establish our
                                 *                                                       * priority for the tuple (see heap_lock_tuple).  LockTuple will
                                 *                                                                * release us when we are next-in-line for the tuple.
                                 *                                                                         *
                                 *                                                                                  * If we are forced to "start over" below, we keep the tuple lock;
                                 *                                                                                           * this arranges that we stay at the head of the line while rechecking
                                 *                                                                                                    * tuple state.
                                 *                                                                                                             */
                                if (infomask & HEAP_XMAX_IS_MULTI)
                                        {
                                                    bool        current_is_member = false;

                                                            if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
                                                                                                            LockTupleExclusive, &current_is_member))
                                                                        {
                                                                                        LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

                                                                                                    /*
                                                                                                     *                  * Acquire the lock, if necessary (but skip it when we're
                                                                                                     *                                   * requesting a lock and already have one; avoids deadlock).
                                                                                                     *                                                    */
                                                                                                    if (!current_is_member)
                                                                                                                        heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
                                                                                                                                                                     LockWaitBlock, &have_tuple_lock);

                                                                                                                /* wait for multixact */
                                                                                                                MultiXactIdWait((MultiXactId) xwait, MultiXactStatusUpdate, infomask,
                                                                                                                                                    relation, &(tp.t_self), XLTW_Delete,
                                                                                                                                                                                NULL);
                                                                                                                            LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

                                                                                                                                        /*
                                                                                                                                         *                  * If xwait had just locked the tuple then some other xact
                                                                                                                                         *                                   * could update this tuple before we get to this point.  Check
                                                                                                                                         *                                                    * for xmax change, and start over if so.
                                                                                                                                         *                                                                     *
                                                                                                                                         *                                                                                      * We also must start over if we didn't pin the VM page, and
                                                                                                                                         *                                                                                                       * the page has become all visible.
                                                                                                                                         *                                                                                                                        */
                                                                                                                                        if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
                                                                                                                                                                xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
                                                                                                                                                                                !TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
                                                                                                                                                                                                                         xwait))
                                                                                                                                                            goto l1;
                                                                                                                                                }

                                                                    /*
                                                                     *              * You might think the multixact is necessarily done here, but not
                                                                     *                           * so: it could have surviving members, namely our own xact or
                                                                     *                                        * other subxacts of this backend.  It is legal for us to delete
                                                                     *                                                     * the tuple in either case, however (the latter case is
                                                                     *                                                                  * essentially a situation of upgrading our former shared lock to
                                                                     *                                                                               * exclusive).  We don't bother changing the on-disk hint bits
                                                                     *                                                                                            * since we are about to overwrite the xmax altogether.
                                                                     *                                                                                                         */
                                                                }
                                    else if (!TransactionIdIsCurrentTransactionId(xwait))
                                            {
                                                        /*
                                                         *              * Wait for regular transaction to end; but first, acquire tuple
                                                         *                           * lock.
                                                         *                                        */
                                                        LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
                                                                heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
                                                                                                     LockWaitBlock, &have_tuple_lock);
                                                                        XactLockTableWait(xwait, relation, &(tp.t_self), XLTW_Delete);
                                                                                LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

                                                                                        /*
                                                                                         *              * xwait is done, but if xwait had just locked the tuple then some
                                                                                         *                           * other xact could update this tuple before we get to this point.
                                                                                         *                                        * Check for xmax change, and start over if so.
                                                                                         *                                                     *
                                                                                         *                                                                  * We also must start over if we didn't pin the VM page, and the
                                                                                         *                                                                               * page has become all visible.
                                                                                         *                                                                                            */
                                                                                        if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
                                                                                                            xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
                                                                                                                        !TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
                                                                                                                                                             xwait))
                                                                                                        goto l1;

                                                                                                /* Otherwise check if it committed or aborted */
                                                                                                UpdateXmaxHintBits(tp.t_data, buffer, xwait);
                                                                                                    }

                                        /*
                                         *          * We may overwrite if previous xmax aborted, or if it committed but
                                         *                   * only locked the tuple without updating it.
                                         *                            */
                                        if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
                                                        HEAP_XMAX_IS_LOCKED_ONLY(tp.t_data->t_infomask) ||
                                                                HeapTupleHeaderIsOnlyLocked(tp.t_data))
                                                    result = TM_Ok;
                                            else if (!ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
                                                        result = TM_Updated;
                                                else
                                                            result = TM_Deleted;
                                                }

    /* sanity check the result HeapTupleSatisfiesUpdate() and the logic above */
    if (result != TM_Ok)
        {
                Assert(result == TM_SelfModified ||
                                   result == TM_Updated ||
                                              result == TM_Deleted ||
                                                   result == TM_BeingModified);
                     Assert(!(tp.t_data->t_infomask & HEAP_XMAX_INVALID));
                Assert(result != TM_Updated ||
                               !ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid));
            }

    if (crosscheck != InvalidSnapshot && result == TM_Ok)
        {
                /* Perform additional check for transaction-snapshot mode RI updates */
                if (!HeapTupleSatisfiesVisibility(&tp, crosscheck, buffer))
                            result = TM_Updated;
                }

    if (result != TM_Ok)
        {
                tmfd->ctid = tp.t_data->t_ctid;
                    tmfd->xmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
                        if (result == TM_SelfModified)
                                    tmfd->cmax = HeapTupleHeaderGetCmax(tp.t_data);
                            else
                                        tmfd->cmax = InvalidCommandId;
                                UnlockReleaseBuffer(buffer);
                                    if (have_tuple_lock)
                                                UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);
                                        if (vmbuffer != InvalidBuffer)
                                                    ReleaseBuffer(vmbuffer);
                                            return result;
                                            }

    /*
     *      * We're about to do the actual delete -- check for conflict first, to
     *           * avoid possibly having to roll back work we've just done.
     *                *
     *                     * This is safe without a recheck as long as there is no possibility of
     *                          * another process scanning the page between this check and the delete
     *                               * being visible to the scan (i.e., an exclusive buffer content lock is
     *                                    * continuously held from this point until the tuple delete is visible).
     *                                         */
    CheckForSerializableConflictIn(relation, tid, BufferGetBlockNumber(buffer));

    /* replace cid with a combo CID if necessary */
    HeapTupleHeaderAdjustCmax(tp.t_data, &cid, &iscombo);

    /*
     *      * Compute replica identity tuple before entering the critical section so
     *           * we don't PANIC upon a memory allocation failure.
     *                */
    old_key_tuple = ExtractReplicaIdentity(relation, &tp, true, &old_key_copied);

    /*
     *      * If this is the first possibly-multixact-able operation in the current
     *           * transaction, set my per-backend OldestMemberMXactId setting. We can be
     *                * certain that the transaction will never become a member of any older
     *                     * MultiXactIds than that.  (We have to do this even if we end up just
     *                          * using our own TransactionId below, since some other backend could
     *                               * incorporate our XID into a MultiXact immediately afterwards.)
     *                                    */
    MultiXactIdSetOldestMember();

    compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(tp.t_data),
                                      tp.t_data->t_infomask, tp.t_data->t_infomask2,
                                                                xid, LockTupleExclusive, true,
                                                                                      &new_xmax, &new_infomask, &new_infomask2);

    START_CRIT_SECTION();

    /*
     *      * If this transaction commits, the tuple will become DEAD sooner or
     *           * later.  Set flag that this page is a candidate for pruning once our xid
     *                * falls below the OldestXmin horizon.  If the transaction finally aborts,
     *                     * the subsequent page pruning will be a no-op and the hint will be
     *                          * cleared.
     *                               */
    PageSetPrunable(page, xid);

    if (PageIsAllVisible(page))
        {
                all_visible_cleared = true;
                    PageClearAllVisible(page);
                        visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
                                                        vmbuffer, VISIBILITYMAP_VALID_BITS);
                        }

    /* store transaction information of xact deleting the tuple */
    tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
    tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
    tp.t_data->t_infomask |= new_infomask;
    tp.t_data->t_infomask2 |= new_infomask2;
    HeapTupleHeaderClearHotUpdated(tp.t_data);
    HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
    HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
    /* Make sure there is no forward chain link in t_ctid */
    tp.t_data->t_ctid = tp.t_self;

    /* Signal that this is actually a move into another partition */
    if (changingPart)
            HeapTupleHeaderSetMovedPartitions(tp.t_data);

    MarkBufferDirty(buffer);

    /*
     *      * XLOG stuff
     *           *
     *                * NB: heap_abort_speculative() uses the same xlog record and replay
     *                     * routines.
     *                          */
    if (RelationNeedsWAL(relation))
        {
                xl_heap_delete xlrec;
                    xl_heap_header xlhdr;
                        XLogRecPtr    recptr;

                            /*
                             *          * For logical decode we need combo CIDs to properly decode the
                             *                   * catalog
                             *                            */
                            if (RelationIsAccessibleInLogicalDecoding(relation))
                                        log_heap_new_cid(relation, &tp);

                                xlrec.flags = 0;
                                    if (all_visible_cleared)
                                                xlrec.flags |= XLH_DELETE_ALL_VISIBLE_CLEARED;
                                        if (changingPart)
                                                    xlrec.flags |= XLH_DELETE_IS_PARTITION_MOVE;
                                            xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
                                                                                              tp.t_data->t_infomask2);
                                                xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
                                                    xlrec.xmax = new_xmax;

                                                        if (old_key_tuple != NULL)
                                                                {
                                                                            if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
                                                                                            xlrec.flags |= XLH_DELETE_CONTAINS_OLD_TUPLE;
                                                                                    else
                                                                                                    xlrec.flags |= XLH_DELETE_CONTAINS_OLD_KEY;
                                                                                        }

                                                            XLogBeginInsert();
                                                                XLogRegisterData((char *) &xlrec, SizeOfHeapDelete);

                                                                    XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

                                                                        /*
                                                                         *          * Log replica identity of the deleted tuple if there is one
                                                                         *                   */
                                                                        if (old_key_tuple != NULL)
                                                                                {
                                                                                            xlhdr.t_infomask2 = old_key_tuple->t_data->t_infomask2;
                                                                                                    xlhdr.t_infomask = old_key_tuple->t_data->t_infomask;
                                                                                                            xlhdr.t_hoff = old_key_tuple->t_data->t_hoff;

                                                                                                                    XLogRegisterData((char *) &xlhdr, SizeOfHeapHeader);
                                                                                                                            XLogRegisterData((char *) old_key_tuple->t_data
                                                                                                                                                             + SizeofHeapTupleHeader,
                                                                                                                                                                                      old_key_tuple->t_len
                                                                                                                                                                                                             - SizeofHeapTupleHeader);
                                                                                                                                   }

                                                                        /* filtering by origin on a row level is much more efficient */
                                                                        XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

                                                                        recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

                                                                        PageSetLSN(page, recptr);
                                                                    }

    END_CRIT_SECTION();

    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

    if (vmbuffer != InvalidBuffer)
            ReleaseBuffer(vmbuffer);

    /*
     *      * If the tuple has toasted out-of-line attributes, we need to delete
     *           * those items too.  We have to do this before releasing the buffer
     *                * because we need to look at the contents of the tuple, but it's OK to
     *                     * release the content lock on the buffer first.
     *                          */
    if (relation->rd_rel->relkind != RELKIND_RELATION &&
                relation->rd_rel->relkind != RELKIND_MATVIEW)
        {
                /* toast table entries should never be recursively toasted */
                Assert(!HeapTupleHasExternal(&tp));
                }
    else if (HeapTupleHasExternal(&tp))
            heap_toast_delete(relation, &tp, false);

    /*
     *      * Mark tuple for invalidation from system caches at next command
     *           * boundary. We have to do this before releasing the buffer because we
     *                * need to look at the contents of the tuple.
     *                     */
    CacheInvalidateHeapTuple(relation, &tp, NULL);

    /* Now we can release the buffer */
    ReleaseBuffer(buffer);

    /*
     *      * Release the lmgr tuple lock, if we had it.
     *           */
    if (have_tuple_lock)
            UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);

    pgstat_count_heap_delete(relation);

    if (old_key_tuple != NULL && old_key_copied)
            heap_freetuple(old_key_tuple);

    return TM_Ok;    


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

    SMgrRelation srel;

    if(persistence != RELPERSISTENCE_PERMANENT || rel->rd_rel->relkind != RELKIND_RELATION)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg_internal("vertex am can only work on permenant tables")));   

    /*
     * Initialize to the minimum XID that could put tuples in the table. We
     * know that no xacts older than RecentXmin are still running, so that
     * will do.
     */
    *freezeXid = RecentXmin;

     /*
      * Similarly, initialize the minimum Multixact to the first value that
      * could possibly be stored in tuples in the table.  Running transactions
      * could reuse values from their local cache, so we are careful to
      * consider all currently running multis.
      *
      * XXX this could be refined further, but is it worth the hassle?
      */
    *minmulti = GetOldestMultiXactId();

    srel = RelationCreateStorage(*newrnode, persistence);

    smgrclose(srel);
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
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg_internal("vertex_parallelscan_reinitialize not implemented")));
    


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
    .relation_copy_for_cluster = vertex_relation_nontransactional_truncate,
    .relation_vacuum = vertex_relation_vacuum,
    .scan_analyze_next_block = vertex_scan_analyze_next_block,
    .scan_analyze_next_tuple = vertex_scan_analyze_next_tuple,
    .index_build_range_scan = vertex_index_validate_scan,
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

PG_FUNCTION_INFO_V1(vertex_tableam_handler);
Datum
vertex_tableam_handler(PG_FUNCTION_ARGS)
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

