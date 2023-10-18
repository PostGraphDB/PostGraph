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

/*
 * Declarations for gtype data type support.
 */

#ifndef AG_VERTEX_H
#define AG_VERTEX_H

#include "access/htup_details.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/array.h"
#include "utils/numeric.h"
#include "utils/syscache.h"

#include "catalog/ag_namespace.h"
#include "catalog/pg_type.h"
#include "utils/graphid.h"
#include "utils/gtype.h"

/* Convenience macros */
#define DATUM_GET_VERTEX(d) ((vertex *)PG_DETOAST_DATUM(d))
#define VERTEX_GET_DATUM(p) PointerGetDatum(p)
#define AG_GET_ARG_VERTEX(x) DATUM_GET_VERTEX(PG_GETARG_DATUM(x))
#define AG_RETURN_VERTEX(x) PG_RETURN_POINTER(x)


typedef uint32 ventry;

#define EXTRACT_VERTEX_ID(v) \
    (*((int64 *)(&((vertex *)v)->children[0])))

#define EXTRACT_VERTEX_GRAPH_OID(v) \
    (*((Oid *)(&((vertex *)v)->children[2])))

/*
 * An vertex, within an gtype Datum.
 *
 * An array has one child for each element, stored in array order.
 */
typedef struct
{
    int32 vl_len_; // varlena header (do not touch directly!)
    ventry children[FLEXIBLE_ARRAY_MEMBER];
} vertex;

void append_vertex_to_string(StringInfoData *buffer, vertex *v);
char *extract_vertex_label(vertex *v);
gtype *extract_vertex_properties(vertex *v);
Datum build_vertex(PG_FUNCTION_ARGS);
vertex *create_vertex(graphid id, Oid graph_oid, gtype *properties);	
int extract_vertex_label_length(vertex *v);

#define VERTEXOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("vertex"), ObjectIdGetDatum(postgraph_namespace_id())))

#define VERTEXARRAYOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("_vertex"), ObjectIdGetDatum(postgraph_namespace_id())))

#endif
