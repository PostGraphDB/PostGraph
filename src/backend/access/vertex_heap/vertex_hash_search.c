/*-------------------------------------------------------------------------
 *
 * hashsearch.c
 *	  search code for postgres hash tables
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashsearch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/rel.h"
#include "storage/procarray.h"

#include "access/vertex.h"

static bool vertex_hash_readpage(TableScanDesc scan, Buffer *bufP,
						   ScanDirection dir);
static int	vertex_hash_load_qualified_items(TableScanDesc scan, Page page,
									   OffsetNumber offnum, ScanDirection dir);
static inline void vertex_hash_saveitem(VertexHeapScanDesc so, int itemIndex,
								  OffsetNumber offnum, IndexTuple itup);
static void vertex_hash_readnext(TableScanDesc scan, Buffer *bufp,
						   Page *pagep, HashPageOpaque *opaquep);
static void vertex_hash_kill_items(TableScanDesc scan);

/*
 *	vertex_hash_next() -- Get the next item in a scan.
 *
 *		On entry, so->currPos describes the current page, which may
 *		be pinned but not locked, and so->currPos.itemIndex identifies
 *		which item was previously returned.
 *
 *		On successful exit, scan->xs_ctup.t_self is set to the TID
 *		of the next heap tuple. so->currPos is updated as needed.
 *
 *		On failure exit (no more tuples), we return false with pin
 *		held on bucket page but no pins or locks held on overflow
 *		page.
 */
bool
vertex_hash_next(TableScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->rs_rd;
	VertexHeapScanDesc so = (VertexHeapScanDesc) scan;
	HashScanPosItem *currItem;
	BlockNumber blkno;
	Buffer		buf;
	bool		end_of_scan = false;

	/*
	 * Advance to the next tuple on the current page; or if done, try to read
	 * data from the next or previous page based on the scan direction. Before
	 * moving to the next or previous page make sure that we deal with all the
	 * killed items.
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (++so->currPos.itemIndex > so->currPos.lastItem)
		{
			if (so->numKilled > 0)
				vertex_hash_kill_items(scan);

			blkno = so->currPos.nextPage;
			if (BlockNumberIsValid(blkno))
			{
				buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
				TestForOldSnapshot(scan->rs_snapshot, rel, BufferGetPage(buf));
				if (!vertex_hash_readpage(scan, &buf, dir))
					end_of_scan = true;
			}
			else
				end_of_scan = true;
		}
	}
	else
	{
		if (--so->currPos.itemIndex < so->currPos.firstItem)
		{
			if (so->numKilled > 0)
				vertex_hash_kill_items(scan);

			blkno = so->currPos.prevPage;
			if (BlockNumberIsValid(blkno))
			{
				buf = _hash_getbuf(rel, blkno, HASH_READ,
								   LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
				TestForOldSnapshot(scan->rs_snapshot, rel, BufferGetPage(buf));

				/*
				 * We always maintain the pin on bucket page for whole scan
				 * operation, so releasing the additional pin we have acquired
				 * here.
				 */
				if (buf == so->hashso_bucket_buf ||
					buf == so->hashso_split_bucket_buf)
					_hash_dropbuf(rel, buf);

				if (!vertex_hash_readpage(scan, &buf, dir))
					end_of_scan = true;
			}
			else
				end_of_scan = true;
		}
	}

	if (end_of_scan)
	{
		//_hash_dropscanbuf(rel,( TableScanDesc)so); TODO
		HashScanPosInvalidate(so->currPos);
		return false;
	}

	/* OK, itemIndex says what to return */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	//scan->xs_heaptid = currItem->heapTid;

	return true;
}

/*
 * Advance to next page in a bucket, if any.  If we are scanning the bucket
 * being populated during split operation then this function advances to the
 * bucket being split after the last bucket page of bucket being populated.
 */
static void
vertex_hash_readnext(TableScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->rs_rd;
	VertexHeapScanDesc so = (VertexHeapScanDesc) scan;
	bool		block_found = false;

	blkno = (*opaquep)->hasho_nextblkno;

	/*
	 * Retain the pin on primary bucket page till the end of scan.  Refer the
	 * comments in vertex_hash_first to know the reason of retaining pin.
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, *bufp);

	*bufp = InvalidBuffer;
	/* check for interrupts while we're not holding any buffer lock */
	CHECK_FOR_INTERRUPTS();
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
		block_found = true;
	}
	else if (so->hashso_buc_populated && !so->hashso_buc_split)
	{
		/*
		 * end of bucket, scan bucket being split if there was a split in
		 * progress at the start of scan.
		 */
		*bufp = so->hashso_split_bucket_buf;

		/*
		 * buffer for bucket being split must be valid as we acquire the pin
		 * on it before the start of scan and retain it till end of scan.
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);
		PredicateLockPage(rel, BufferGetBlockNumber(*bufp), scan->rs_snapshot);

		/*
		 * setting hashso_buc_split to true indicates that we are scanning
		 * bucket being split.
		 */
		so->hashso_buc_split = true;

		block_found = true;
	}

	if (block_found)
	{
		*pagep = BufferGetPage(*bufp);
		TestForOldSnapshot(scan->rs_snapshot, rel, *pagep);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);
	}
}

/*
 * Advance to previous page in a bucket, if any.  If the current scan has
 * started during split operation then this function advances to bucket
 * being populated after the first bucket page of bucket being split.
 */
static void
vertex__hash_readprev(TableScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->rs_rd;
	VertexHeapScanDesc so = (VertexHeapScanDesc) scan;
	bool		haveprevblk;

	blkno = (*opaquep)->hasho_prevblkno;

	/*
	 * Retain the pin on primary bucket page till the end of scan.  Refer the
	 * comments in vertex_hash_first to know the reason of retaining pin.
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
	{
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
		haveprevblk = false;
	}
	else
	{
		_hash_relbuf(rel, *bufp);
		haveprevblk = true;
	}

	*bufp = InvalidBuffer;
	/* check for interrupts while we're not holding any buffer lock */
	CHECK_FOR_INTERRUPTS();

	if (haveprevblk)
	{
		Assert(BlockNumberIsValid(blkno));
		*bufp = _hash_getbuf(rel, blkno, HASH_READ,
							 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		*pagep = BufferGetPage(*bufp);
		TestForOldSnapshot(scan->rs_snapshot, rel, *pagep);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);

		/*
		 * We always maintain the pin on bucket page for whole scan operation,
		 * so releasing the additional pin we have acquired here.
		 */
		if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
			_hash_dropbuf(rel, *bufp);
	}
	else if (so->hashso_buc_populated && so->hashso_buc_split)
	{
		/*
		 * end of bucket, scan bucket being populated if there was a split in
		 * progress at the start of scan.
		 */
		*bufp = so->hashso_bucket_buf;

		/*
		 * buffer for bucket being populated must be valid as we acquire the
		 * pin on it before the start of scan and retain it till end of scan.
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = (HashPageOpaque) PageGetSpecialPointer(*pagep);

		/* move to the end of bucket chain */
		while (BlockNumberIsValid((*opaquep)->hasho_nextblkno))
			vertex_hash_readnext(scan, bufp, pagep, opaquep);

		/*
		 * setting hashso_buc_split to false indicates that we are scanning
		 * bucket being populated.
		 */
		so->hashso_buc_split = false;
	}
}

/*
 *	vertex_hash_first() -- Find the first item in a scan.
 *
 *		We find the first item (or, if backward scan, the last item) in the
 *		index that satisfies the qualification associated with the scan
 *		descriptor.
 *
 *		On successful exit, if the page containing current index tuple is an
 *		overflow page, both pin and lock are released whereas if it is a bucket
 *		page then it is pinned but not locked and data about the matching
 *		tuple(s) on the page has been loaded into so->currPos,
 *		scan->xs_ctup.t_self is set to the heap TID of the current tuple.
 *
 *		On failure exit (no more tuples), we return false, with pin held on
 *		bucket page but no pins or locks held on overflow page.
 */
bool
vertex_hash_first(TableScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->rs_rd;
	VertexHeapScanDesc so = (VertexHeapScanDesc) scan;
	ScanKey		cur;
	uint32		hashkey;
	Bucket		bucket;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	HashScanPosItem *currItem;

	pgstat_count_index_scan(rel);

	/*
	 * We do not support hash scans with no index qualification, because we
	 * would have to read the whole index rather than just one bucket. That
	 * creates a whole raft of problems, since we haven't got a practical way
	 * to lock all the buckets against splits or compactions.
	 */
	 // XXX: This is a major problem and must be solved
	 if (scan->rs_nkeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("hash indexes do not support whole-index scans")));

	/* There may be more than one index qual, but we hash only the first */
	cur = &scan->rs_key[0];

	/* We support only single-column hash indexes */
	//Assert(cur->sk_attno == 1);
	/* And there's only one operator strategy, too */
	//Assert(cur->sk_strategy == HTEqualStrategyNumber);

	/*
	 * If the constant in the index qual is NULL, assume it cannot match any
	 * items in the index.
	 */
	if (cur->sk_flags & SK_ISNULL)
		return false;

	/*
	 * Okay to compute the hash key.  We want to do this before acquiring any
	 * locks, in case a user-defined hash function happens to be slow.
	 *
	 * If scankey operator is not a cross-type comparison, we can use the
	 * cached hash function; otherwise gotta look it up in the catalogs.
	 *
	 * We support the convention that sk_subtype == InvalidOid means the
	 * opclass input type; this is a hack to simplify life for ScanKeyInit().
	 */
	if (cur->sk_subtype == rel->rd_opcintype[0] ||
		cur->sk_subtype == InvalidOid)
		hashkey = _hash_datum2hashkey(rel, cur->sk_argument);
	else
		hashkey = _hash_datum2hashkey_type(rel, cur->sk_argument, cur->sk_subtype);

	so->hashso_sk_hash = hashkey;

	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_READ, NULL);
	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->rs_snapshot);
	page = BufferGetPage(buf);
	//TestForOldSnapshot(scan->rs_snapshot, rel, page);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);
	bucket = opaque->hasho_bucket;

	so->hashso_bucket_buf = buf;

	/*
	 * If a bucket split is in progress, then while scanning the bucket being
	 * populated, we need to skip tuples that were copied from bucket being
	 * split.  We also need to maintain a pin on the bucket being split to
	 * ensure that split-cleanup work done by vacuum doesn't remove tuples
	 * from it till this scan is done.  We need to maintain a pin on the
	 * bucket being populated to ensure that vacuum doesn't squeeze that
	 * bucket till this scan is complete; otherwise, the ordering of tuples
	 * can't be maintained during forward and backward scans.  Here, we have
	 * to be cautious about locking order: first, acquire the lock on bucket
	 * being split; then, release the lock on it but not the pin; then,
	 * acquire a lock on bucket being populated and again re-verify whether
	 * the bucket split is still in progress.  Acquiring the lock on bucket
	 * being split first ensures that the vacuum waits for this scan to
	 * finish.
	 */
	if (H_BUCKET_BEING_POPULATED(opaque))
	{
		BlockNumber old_blkno;
		Buffer		old_buf;

		old_blkno = _hash_get_oldblock_from_newbucket(rel, bucket);

		/*
		 * release the lock on new bucket and re-acquire it after acquiring
		 * the lock on old bucket.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		old_buf = _hash_getbuf(rel, old_blkno, HASH_READ, LH_BUCKET_PAGE);
		TestForOldSnapshot(scan->rs_snapshot, rel, BufferGetPage(old_buf));

		/*
		 * remember the split bucket buffer so as to use it later for
		 * scanning.
		 */
		so->hashso_split_bucket_buf = old_buf;
		LockBuffer(old_buf, BUFFER_LOCK_UNLOCK);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = (HashPageOpaque) PageGetSpecialPointer(page);
		Assert(opaque->hasho_bucket == bucket);

		if (H_BUCKET_BEING_POPULATED(opaque))
			so->hashso_buc_populated = true;
		else
		{
			_hash_dropbuf(rel, so->hashso_split_bucket_buf);
			so->hashso_split_bucket_buf = InvalidBuffer;
		}
	}

	/* If a backwards scan is requested, move to the end of the chain */
	if (ScanDirectionIsBackward(dir))
	{
		/*
		 * Backward scans that start during split needs to start from end of
		 * bucket being split.
		 */
		while (BlockNumberIsValid(opaque->hasho_nextblkno) ||
			   (so->hashso_buc_populated && !so->hashso_buc_split))
			vertex_hash_readnext(scan, &buf, &page, &opaque);
	}

	/* remember which buffer we have pinned, if any */
	Assert(BufferIsInvalid(so->currPos.buf));
	so->currPos.buf = buf;

	/* Now find all the tuples satisfying the qualification from a page */
	if (!vertex_hash_readpage(scan, &buf, dir))
		return false;

	/* OK, itemIndex says what to return */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	//scan->xs_heaptid = currItem->heapTid;

	/* if we're here, vertex_hash_readpage found a valid tuples */
	return true;
}

void
vertex_hash_kill_items(TableScanDesc scan)
{
	HashScanOpaque so = (HashScanOpaque) scan;
	Relation	rel = scan->rs_rd;
	BlockNumber blkno;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum,
				maxoff;
	int			numKilled = so->numKilled;
	int			i;
	bool		killedsomething = false;
	bool		havePin = false;

	Assert(so->numKilled > 0);
	Assert(so->killedItems != NULL);
	Assert(HashScanPosIsValid(so->currPos));

	/*
	 * Always reset the scan state, so we don't look for same items on other
	 * pages.
	 */
	so->numKilled = 0;

	blkno = so->currPos.currPage;
	if (HashScanPosIsPinned(so->currPos))
	{
		/*
		 * We already have pin on this buffer, so, all we need to do is
		 * acquire lock on it.
		 */
		havePin = true;
		buf = so->currPos.buf;
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	}
	else
		buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);

	page = BufferGetPage(buf);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	for (i = 0; i < numKilled; i++)
	{
		int			itemIndex = so->killedItems[i];
		HashScanPosItem *currItem = &so->currPos.items[itemIndex];

		offnum = currItem->indexOffset;

		Assert(itemIndex >= so->currPos.firstItem &&
			   itemIndex <= so->currPos.lastItem);

		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	ituple = (IndexTuple) PageGetItem(page, iid);

			if (ItemPointerEquals(&ituple->t_tid, &currItem->heapTid))
			{
				/* found the item */
				ItemIdMarkDead(iid);
				killedsomething = true;
				break;			/* out of inner search loop */
			}
			offnum = OffsetNumberNext(offnum);
		}
	}

	/*
	 * Since this can be redone later if needed, mark as dirty hint. Whenever
	 * we mark anything LP_DEAD, we also set the page's
	 * LH_PAGE_HAS_DEAD_TUPLES flag, which is likewise just a hint.
	 */
	if (killedsomething)
	{
		opaque->hasho_flag |= LH_PAGE_HAS_DEAD_TUPLES;
		MarkBufferDirtyHint(buf, true);
	}

	if (so->hashso_bucket_buf == so->currPos.buf ||
		havePin)
		LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, buf);
}


/*
 *	vertex_hash_readpage() -- Load data from current index page into so->currPos
 *
 *	We scan all the items in the current index page and save them into
 *	so->currPos if it satisfies the qualification. If no matching items
 *	are found in the current page, we move to the next or previous page
 *	in a bucket chain as indicated by the direction.
 *
 *	Return true if any matching items are found else return false.
 */
static bool
vertex_hash_readpage(TableScanDesc scan, Buffer *bufP, ScanDirection dir)
{
	Relation	rel = scan->rs_rd;
	VertexHeapScanDesc so = (VertexHeapScanDesc) scan;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum;
	uint16		itemIndex;

	buf = *bufP;
	Assert(BufferIsValid(buf));
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);

	so->currPos.buf = buf;
	so->currPos.currPage = BufferGetBlockNumber(buf);

	if (ScanDirectionIsForward(dir))
	{
		BlockNumber prev_blkno = InvalidBlockNumber;

		for (;;)
		{
			/* new page, locate starting position by binary search */
			offnum = _hash_binsearch(page, so->hashso_sk_hash);

			itemIndex = vertex_hash_load_qualified_items(scan, page, offnum, dir);

			if (itemIndex != 0)
				break;

			/*
			 * Could not find any matching tuples in the current page, move to
			 * the next page. Before leaving the current page, deal with any
			 * killed items.
			 */
			if (so->numKilled > 0)
				vertex_hash_kill_items(scan);

			/*
			 * If this is a primary bucket page, hasho_prevblkno is not a real
			 * block number.
			 */
			if (so->currPos.buf == so->hashso_bucket_buf ||
				so->currPos.buf == so->hashso_split_bucket_buf)
				prev_blkno = InvalidBlockNumber;
			else
				prev_blkno = opaque->hasho_prevblkno;

			vertex_hash_readnext(scan, &buf, &page, &opaque);
			if (BufferIsValid(buf))
			{
				so->currPos.buf = buf;
				so->currPos.currPage = BufferGetBlockNumber(buf);
			}
			else
			{
				/*
				 * Remember next and previous block numbers for scrollable
				 * cursors to know the start position and return false
				 * indicating that no more matching tuples were found. Also,
				 * don't reset currPage or lsn, because we expect
				 * vertex_hash_kill_items to be called for the old page after this
				 * function returns.
				 */
				so->currPos.prevPage = prev_blkno;
				so->currPos.nextPage = InvalidBlockNumber;
				so->currPos.buf = buf;
				return false;
			}
		}

		so->currPos.firstItem = 0;
		so->currPos.lastItem = itemIndex - 1;
		so->currPos.itemIndex = 0;
	}
	else
	{
		BlockNumber next_blkno = InvalidBlockNumber;

		for (;;)
		{
			/* new page, locate starting position by binary search */
			offnum = _hash_binsearch_last(page, so->hashso_sk_hash);

			itemIndex = vertex_hash_load_qualified_items(scan, page, offnum, dir);

			if (itemIndex != MaxIndexTuplesPerPage)
				break;

			/*
			 * Could not find any matching tuples in the current page, move to
			 * the previous page. Before leaving the current page, deal with
			 * any killed items.
			 */
			if (so->numKilled > 0)
				vertex_hash_kill_items(scan);

			if (so->currPos.buf == so->hashso_bucket_buf ||
				so->currPos.buf == so->hashso_split_bucket_buf)
				next_blkno = opaque->hasho_nextblkno;

			vertex__hash_readprev(scan, &buf, &page, &opaque);
			if (BufferIsValid(buf))
			{
				so->currPos.buf = buf;
				so->currPos.currPage = BufferGetBlockNumber(buf);
			}
			else
			{
				/*
				 * Remember next and previous block numbers for scrollable
				 * cursors to know the start position and return false
				 * indicating that no more matching tuples were found. Also,
				 * don't reset currPage or lsn, because we expect
				 * vertex_hash_kill_items to be called for the old page after this
				 * function returns.
				 */
				so->currPos.prevPage = InvalidBlockNumber;
				so->currPos.nextPage = next_blkno;
				so->currPos.buf = buf;
				return false;
			}
		}

		so->currPos.firstItem = itemIndex;
		so->currPos.lastItem = MaxIndexTuplesPerPage - 1;
		so->currPos.itemIndex = MaxIndexTuplesPerPage - 1;
	}

	if (so->currPos.buf == so->hashso_bucket_buf ||
		so->currPos.buf == so->hashso_split_bucket_buf)
	{
		so->currPos.prevPage = InvalidBlockNumber;
		so->currPos.nextPage = opaque->hasho_nextblkno;
		LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		so->currPos.prevPage = opaque->hasho_prevblkno;
		so->currPos.nextPage = opaque->hasho_nextblkno;
		_hash_relbuf(rel, so->currPos.buf);
		so->currPos.buf = InvalidBuffer;
	}

	Assert(so->currPos.firstItem <= so->currPos.lastItem);
	return true;
}

/*
 * vertex_hash_checkqual -- does the index tuple satisfy the scan conditions?
 */
bool
vertex_hash_checkqual(TableScanDesc scan, IndexTuple itup)
{
	/*
	 * Currently, we can't check any of the scan conditions since we do not
	 * have the original index entry value to supply to the sk_func. Always
	 * return true; we expect that hashgettuple already set the recheck flag
	 * to make the main indexscan code do it.
	 */
#ifdef NOT_USED
	TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);
	ScanKey		key = scan->keyData;
	int			scanKeySize = scan->numberOfKeys;

	while (scanKeySize > 0)
	{
		Datum		datum;
		bool		isNull;
		Datum		test;

		datum = index_getattr(itup,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		/* assume sk_func is strict */
		if (isNull)
			return false;
		if (key->sk_flags & SK_ISNULL)
			return false;

		test = FunctionCall2Coll(&key->sk_func, key->sk_collation,
								 datum, key->sk_argument);

		if (!DatumGetBool(test))
			return false;

		key++;
		scanKeySize--;
	}
#endif

	return true;
}

/*
 * Load all the qualified items from a current index page
 * into so->currPos. Helper function for vertex_hash_readpage.
 */
static int
vertex_hash_load_qualified_items(TableScanDesc scan, Page page,
						   OffsetNumber offnum, ScanDirection dir)
{
	VertexHeapScanDesc so = (VertexHeapScanDesc)scan;
	IndexTuple	itup;
	int			itemIndex;
	OffsetNumber maxoff;

	maxoff = PageGetMaxOffsetNumber(page);

	if (ScanDirectionIsForward(dir))
	{
		/* load items[] in ascending order */
		itemIndex = 0;

		while (offnum <= maxoff)
		{
			Assert(offnum >= FirstOffsetNumber);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				((ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberNext(offnum);	/* move forward */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				vertex_hash_checkqual(scan, itup))
			{
				/* tuple is qualified, so remember it */
				vertex_hash_saveitem(so, itemIndex, offnum, itup);
				itemIndex++;
			}
			else
			{
				/*
				 * No more matching tuples exist in this page. so, exit while
				 * loop.
				 */
				break;
			}

			offnum = OffsetNumberNext(offnum);
		}

		Assert(itemIndex <= MaxIndexTuplesPerPage);
		return itemIndex;
	}
	else
	{
		/* load items[] in descending order */
		itemIndex = MaxIndexTuplesPerPage;

		while (offnum >= FirstOffsetNumber)
		{
			Assert(offnum <= maxoff);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(true &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberPrev(offnum);	/* move back */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				vertex_hash_checkqual(scan, itup))
			{
				itemIndex--;
				/* tuple is qualified, so remember it */
				vertex_hash_saveitem(so, itemIndex, offnum, itup);
			}
			else
			{
				/*
				 * No more matching tuples exist in this page. so, exit while
				 * loop.
				 */
				break;
			}

			offnum = OffsetNumberPrev(offnum);
		}

		Assert(itemIndex >= 0);
		return itemIndex;
	}
}

/* Save an index item into so->currPos.items[itemIndex] */
static inline void
vertex_hash_saveitem(VertexHeapScanDesc so, int itemIndex,
			   OffsetNumber offnum, IndexTuple itup)
{
	HashScanPosItem *currItem = &so->currPos.items[itemIndex];

	currItem->heapTid = itup->t_tid;
	currItem->indexOffset = offnum;
}
