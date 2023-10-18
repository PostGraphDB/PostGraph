/*
 * Copyright (C) 2023 PostGraphDB
 *  
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef AG_GRAPHID_H
#define AG_GRAPHID_H

#include "postgres.h"

#include "fmgr.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

#include "catalog/ag_namespace.h"
#include "catalog/pg_type.h"

typedef int64 graphid;
#define F_GRAPHIDEQ F_INT8EQ

#define LABEL_ID_MIN 1
#define LABEL_ID_MAX PG_UINT16_MAX
#define INVALID_LABEL_ID 0

#define label_id_is_valid(id) (id >= LABEL_ID_MIN && id <= LABEL_ID_MAX)

#define ENTRY_ID_MIN INT64CONST(1)
#define ENTRY_ID_MAX INT64CONST(281474976710655) // 0x0000ffffffffffff
#define INVALID_ENTRY_ID INT64CONST(0)

#define entry_id_is_valid(id) (id >= ENTRY_ID_MIN && id <= ENTRY_ID_MAX)

#define ENTRY_ID_BITS (32 + 16)
#define ENTRY_ID_MASK INT64CONST(0x0000ffffffffffff)

#define DATUM_GET_GRAPHID(d) DatumGetInt64(d)
#define GRAPHID_GET_DATUM(x) Int64GetDatum(x)

#define AG_GETARG_GRAPHID(a) DATUM_GET_GRAPHID(PG_GETARG_DATUM(a))
#define AG_RETURN_GRAPHID(x) return GRAPHID_GET_DATUM(x)

// Oid accessors for GRAPHID 
#define GRAPHIDOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("graphid"), ObjectIdGetDatum(postgraph_namespace_id())))
#define GRAPHIDARRAYOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("_graphid"), ObjectIdGetDatum(postgraph_namespace_id())))


#define GET_LABEL_ID(id) \
       (((uint64)id) >> ENTRY_ID_BITS)

graphid make_graphid(const int32 label_id, const int64 entry_id);
int32 get_graphid_label_id(const graphid gid);
int64 get_graphid_entry_id(const graphid gid);

#endif
