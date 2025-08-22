/*
 * Copyright (C) 2025 PostGraphDB
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


#ifndef POSTGRAPH_HASHSET_H
#define POSTGRAPH_HASHSET_H

#include "utils/graphid.h"

#define DATUM_GET_HASHSET_P(d) ((hashset *)PG_DETOAST_DATUM(d))
#define HASHSET_P_GET_DATUM(p) PointerGetDatum(p)
#define AG_GET_ARG_HASHSET_P(x) DATUM_GET_HASHSET_P(PG_GETARG_DATUM(x))
#define AG_RETURN_HASHSET_P(x) PG_RETURN_POINTER(x)

typedef struct
{
    int32 vl_len_;
    size_t data_size;
    char data[FLEXIBLE_ARRAY_MEMBER];
} hashset;

// New struct for the serialized bucket metadata
typedef struct SerializedBucketInfo {
    uint32_t offset; // Byte offset from the start of the key data block
    uint32_t count;  // Number of keys in this bucket
} SerializedBucketInfo;

typedef struct HashSetNode {
    graphid key;
    struct HashSetNode* next;
} HashSetNode;

typedef struct HashSetValue {
    HashSetNode** buckets;
    int capacity; 
    int size; 
} HashSetValue;



HashSetNode* createHashSetNode(graphid key);
HashSetValue* createHashSet(int capacity);
int hashFunction(graphid key, int capacity);
void insert(HashSetValue* set, graphid key);
bool contains(HashSetValue* set, graphid key);
bool removeElement(HashSetValue* set, graphid key);
void destroyHashSet(HashSetValue* set);
void printHashSet(HashSetValue* set);
void serializeHashSetToBuffer(const HashSetValue* set, char* buffer, size_t buffer_size);
HashSetValue* deserializeHashSetFromBuffer(const char* buffer, size_t buffer_size);
bool searchSerializedHashSetInBuffer(const char* buffer, size_t buffer_size, graphid key);

// Oid accessors for HASHSET
#define HASHSETOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("hashset"), ObjectIdGetDatum(postgraph_namespace_id())))
#define HASHSETARRAYOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("_hashset"), ObjectIdGetDatum(postgraph_namespace_id())))

#endif
