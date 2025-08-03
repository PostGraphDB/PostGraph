#ifndef ACCESS_VERTEX_H
#define ACCESS_VERTEX_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "catalog/pg_am_d.h"
#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"
#include "access/relscan.h"
#include "access/hash.h"
/*
 * Descriptor for heap table scans.
 */
typedef struct VertexScanDescData
{

	TableScanDescData rs_base;	/* AM independent part of the descriptor */

	int ndesc;
	TableScanDesc **desc;
	

	/* Hash value of the scan key, ie, the hash key we seek */
	uint32		hashso_sk_hash;

	/* remember the buffer associated with primary bucket */
	Buffer		hashso_bucket_buf;

	/*
	 * remember the buffer associated with primary bucket page of bucket being
	 * split.  it is required during the scan of the bucket which is being
	 * populated during split operation.
	 */
	Buffer		hashso_split_bucket_buf;

	/* Whether scan starts on bucket being populated due to split */
	bool		hashso_buc_populated;

	/*
	 * Whether scanning bucket being split?  The value of this parameter is
	 * referred only when hashso_buc_populated is true.
	 */
	bool		hashso_buc_split;
	/* info about killed items if any (killedItems is NULL if never used) */
	int		   *killedItems;	/* currPos.items indexes of killed items */
	int			numKilled;		/* number of currently stored items */

	/*
	 * Identify all the matching items on a page and save them in
	 * HashScanPosData
	 */
	HashScanPosData currPos;	/* current position data */


} VertexScanDescData;

typedef struct VertexScanDescData *VertexHeapScanDesc;
bool
vertex_hash_next(TableScanDesc scan, ScanDirection dir);
bool
vertex_hash_first(TableScanDesc scan, ScanDirection dir);

void unregister_seq_scan_hook(void);
void register_seq_scan_hook(void);

#endif							/* ACCESS_VERTEX_H */
