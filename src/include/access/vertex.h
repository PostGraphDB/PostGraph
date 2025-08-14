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
} VertexScanDescData;

typedef struct VertexScanDescData *VertexHeapScanDesc;

#endif							/* ACCESS_VERTEX_H */
