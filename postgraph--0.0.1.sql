/*
 * PostGraph
 * Copyright (C) 2023 by PostGraph
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * Portions Copyright (c) 2020-2023, Apache Software Foundation
 * Portions Copyright (c) 2019-2020, Bitnine Global
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION age" to load this file. \quit

--
-- catalog tables
--
CREATE TABLE ag_graph (graphid oid NOT NULL, name name NOT NULL, namespace regnamespace NOT NULL);

CREATE UNIQUE INDEX ag_graph_graphid_index ON ag_graph USING btree (graphid);
CREATE UNIQUE INDEX ag_graph_name_index ON ag_graph USING btree (name);
CREATE UNIQUE INDEX ag_graph_namespace_index ON ag_graph USING btree (namespace);

-- 0 is an invalid label ID
CREATE DOMAIN label_id AS int NOT NULL CHECK (VALUE > 0 AND VALUE <= 65535);
CREATE DOMAIN label_kind AS "char" NOT NULL CHECK (VALUE = 'v' OR VALUE = 'e');

CREATE TABLE ag_label (name name NOT NULL, graph oid NOT NULL, id label_id, kind label_kind, relation regclass NOT NULL, CONSTRAINT fk_graph_oid FOREIGN KEY(graph) REFERENCES ag_graph(graphid));

CREATE UNIQUE INDEX ag_label_name_graph_index ON ag_label USING btree (name, graph);
CREATE UNIQUE INDEX ag_label_graph_oid_index ON ag_label USING btree (graph, id);
CREATE UNIQUE INDEX ag_label_relation_index ON ag_label USING btree (relation);

--
-- catalog lookup functions
--
CREATE FUNCTION _label_id(graph_name name, label_name name) RETURNS label_id LANGUAGE c STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- utility functions
--
CREATE FUNCTION create_graph(graph_name name) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION create_graph_if_not_exists(graph_name name) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION drop_graph(graph_name name, cascade boolean = false) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION create_vlabel(graph_name name, label_name name) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION create_elabel(graph_name name, label_name name) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION alter_graph(graph_name name, operation cstring, new_value name) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION drop_label(graph_name name, label_name name, force boolean = false) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION load_labels_from_file(graph_name name, label_name name, file_path text, id_field_exists bool default true) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION load_edges_from_file(graph_name name, label_name name, file_path text) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';

--
-- graphid type
--
CREATE TYPE graphid;

CREATE FUNCTION graphid_in(cstring) RETURNS graphid LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION graphid_out(graphid) RETURNS cstring LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
-- binary I/O functions
CREATE FUNCTION graphid_send(graphid) RETURNS bytea LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION graphid_recv(internal) RETURNS graphid LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';

CREATE TYPE graphid (INPUT = graphid_in, OUTPUT = graphid_out, SEND = graphid_send, RECEIVE = graphid_recv, INTERNALLENGTH = 8, PASSEDBYVALUE, ALIGNMENT = float8, STORAGE = plain);

--
-- graphid - comparison operators (=, <>, <, >, <=, >=)
--
CREATE FUNCTION graphid_eq(graphid, graphid) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR = (FUNCTION = graphid_eq, LEFTARG = graphid, RIGHTARG = graphid, COMMUTATOR = =, NEGATOR = <>, RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES);
CREATE FUNCTION graphid_ne(graphid, graphid) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR <> (FUNCTION = graphid_ne, LEFTARG = graphid, RIGHTARG = graphid, COMMUTATOR = <>, NEGATOR = =, RESTRICT = neqsel, JOIN = neqjoinsel);
CREATE FUNCTION graphid_lt(graphid, graphid) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR < (FUNCTION = graphid_lt, LEFTARG = graphid, RIGHTARG = graphid, COMMUTATOR = >, NEGATOR = >=, RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
CREATE FUNCTION graphid_gt(graphid, graphid) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR > (FUNCTION = graphid_gt, LEFTARG = graphid, RIGHTARG = graphid, COMMUTATOR = <, NEGATOR = <=, RESTRICT = scalargtsel, JOIN = scalargtjoinsel);
CREATE FUNCTION graphid_le(graphid, graphid) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR <= (FUNCTION = graphid_le, LEFTARG = graphid, RIGHTARG = graphid, COMMUTATOR = >=, NEGATOR = >, RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
CREATE FUNCTION graphid_ge(graphid, graphid) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR >= (FUNCTION = graphid_ge, LEFTARG = graphid, RIGHTARG = graphid, COMMUTATOR = <=, NEGATOR = <, RESTRICT = scalargesel, JOIN = scalargejoinsel);

--
-- graphid - B-tree support functions
--
-- comparison support
CREATE FUNCTION graphid_btree_cmp(graphid, graphid) RETURNS int LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
-- sort support
CREATE FUNCTION graphid_btree_sort(internal) RETURNS void LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- define operator classes for graphid
--
CREATE OPERATOR CLASS graphid_ops DEFAULT FOR TYPE graphid USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
FUNCTION 1 graphid_btree_cmp (graphid, graphid), FUNCTION 2 graphid_btree_sort (internal);

--
-- graphid functions
--
CREATE FUNCTION _graphid(label_id int, entry_id bigint) RETURNS graphid LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION _label_name(graph_oid oid, graphid) RETURNS cstring LANGUAGE c STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION _extract_label_id(graphid) RETURNS label_id LANGUAGE c STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- agtype type and its support functions
--

-- define agtype as a shell type first
CREATE TYPE agtype;

CREATE FUNCTION agtype_in(cstring) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_out(agtype) RETURNS cstring LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
-- binary I/O functions
CREATE FUNCTION agtype_send(agtype) RETURNS bytea LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_recv(internal) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE TYPE agtype (INPUT = agtype_in, OUTPUT = agtype_out, SEND = agtype_send, RECEIVE = agtype_recv, LIKE = jsonb);

--
-- vertex
--
CREATE TYPE vertex;

CREATE FUNCTION vertex_in(cstring) RETURNS vertex LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION vertex_out(vertex) RETURNS cstring LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION build_vertex(graphid, cstring, agtype) RETURNS vertex LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';

CREATE TYPE vertex (INPUT = vertex_in, OUTPUT = vertex_out, LIKE = jsonb);

--
-- vertex - access operators (->, ->> )
--
CREATE FUNCTION vertex_property_access(vertex, text) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR -> (LEFTARG = vertex, RIGHTARG = text, FUNCTION = vertex_property_access);
CREATE FUNCTION vertex_property_access_text(vertex, text) RETURNS text LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ->> (LEFTARG = vertex, RIGHTARG = text, FUNCTION = vertex_property_access_text);

--
-- vertex - contains operators (@>, <@)
--
CREATE FUNCTION vertex_contains(vertex, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR @> (LEFTARG = vertex, RIGHTARG = agtype, FUNCTION = vertex_contains, COMMUTATOR = '<@', RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION vertex_contained_by(agtype, vertex) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR <@ (LEFTARG = agtype, RIGHTARG = vertex, FUNCTION = vertex_contained_by, COMMUTATOR = '@>', RESTRICT = contsel, JOIN = contjoinsel);

--
-- vertex - key existence operators (?, ?|, ?&)
--
CREATE FUNCTION vertex_exists(vertex, text) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ? (LEFTARG = vertex, RIGHTARG = text, FUNCTION = vertex_exists, COMMUTATOR = '?', RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION vertex_exists_any(vertex, text[]) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ?| (LEFTARG = vertex, RIGHTARG = text[], FUNCTION = vertex_exists_any, RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION vertex_exists_all(vertex, text[]) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ?& (LEFTARG = vertex, RIGHTARG = text[], FUNCTION = vertex_exists_all, RESTRICT = contsel, JOIN = contjoinsel);

--
-- vertex functions
--
CREATE FUNCTION id(vertex) RETURNS graphid LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME', 'vertex_id';
CREATE FUNCTION label(vertex) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME', 'vertex_label';

--
-- edge
--
CREATE TYPE edge;

CREATE FUNCTION edge_in(cstring) RETURNS edge LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION edge_out(edge) RETURNS cstring LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION build_edge(graphid, graphid, graphid, cstring, agtype) RETURNS edge LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';

CREATE TYPE edge (INPUT = edge_in, OUTPUT = edge_out, LIKE = jsonb);


--
-- agtype - mathematical operators (+, -, *, /, %, ^)
--
CREATE FUNCTION agtype_add(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR + (FUNCTION = agtype_add, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = +);
CREATE FUNCTION agtype_sub(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR - (FUNCTION = agtype_sub, LEFTARG = agtype, RIGHTARG = agtype);
CREATE FUNCTION agtype_neg(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR - (FUNCTION = agtype_neg, RIGHTARG = agtype);
CREATE FUNCTION agtype_mul(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR * (FUNCTION = agtype_mul, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = *);
CREATE FUNCTION agtype_div(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR / (FUNCTION = agtype_div, LEFTARG = agtype, RIGHTARG = agtype);
CREATE FUNCTION agtype_mod(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR % (FUNCTION = agtype_mod, LEFTARG = agtype, RIGHTARG = agtype);
CREATE FUNCTION agtype_pow(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ^ (FUNCTION = agtype_pow, LEFTARG = agtype, RIGHTARG = agtype);


--
-- graphid - hash operator class
--
CREATE FUNCTION graphid_hash_cmp(graphid) RETURNS INTEGER LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR CLASS graphid_ops_hash DEFAULT FOR TYPE graphid USING hash AS OPERATOR 1 =, FUNCTION 1 graphid_hash_cmp(graphid);

--
-- agtype - comparison operators (=, <>, <, >, <=, >=)
--
CREATE FUNCTION agtype_eq(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR = (FUNCTION = agtype_eq, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = =, NEGATOR = <>, RESTRICT = eqsel, JOIN = eqjoinsel, HASHES);
CREATE FUNCTION agtype_ne(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR <> (FUNCTION = agtype_ne, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = <>, NEGATOR = =, RESTRICT = neqsel, JOIN = neqjoinsel);
CREATE FUNCTION agtype_lt(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR < (FUNCTION = agtype_lt, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = >, NEGATOR = >=, RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
CREATE FUNCTION agtype_gt(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR > (FUNCTION = agtype_gt, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = <, NEGATOR = <=, RESTRICT = scalargtsel, JOIN = scalargtjoinsel);
CREATE FUNCTION agtype_le(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR <= (FUNCTION = agtype_le, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = >=, NEGATOR = >, RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
CREATE FUNCTION agtype_ge(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR >= (FUNCTION = agtype_ge, LEFTARG = agtype, RIGHTARG = agtype, COMMUTATOR = <=, NEGATOR = <, RESTRICT = scalargesel, JOIN = scalargejoinsel);
CREATE FUNCTION agtype_btree_cmp(agtype, agtype) RETURNS INTEGER LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- agtype - btree operator class
--
CREATE OPERATOR CLASS agtype_ops_btree DEFAULT FOR TYPE agtype USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >, OPERATOR 5 >=, FUNCTION 1 agtype_btree_cmp(agtype, agtype);

--
-- agtype - hash operator class
--
CREATE FUNCTION agtype_hash_cmp(agtype) RETURNS INTEGER LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR CLASS agtype_ops_hash DEFAULT FOR TYPE agtype USING hash AS OPERATOR 1 =, FUNCTION 1 agtype_hash_cmp(agtype);

--
-- agtype - access operators (->, ->> )
--
CREATE FUNCTION agtype_object_field(agtype, text) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR -> (LEFTARG = agtype, RIGHTARG = text, FUNCTION = agtype_object_field);
CREATE FUNCTION agtype_object_field_text(agtype, text) RETURNS text LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ->> (LEFTARG = agtype, RIGHTARG = text, FUNCTION = agtype_object_field_text);
CREATE FUNCTION agtype_array_element(agtype, int4) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR -> (LEFTARG = agtype, RIGHTARG = int4, FUNCTION = agtype_array_element);
CREATE FUNCTION agtype_array_element_text(agtype, int4) RETURNS text LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ->> (LEFTARG = agtype, RIGHTARG = int4, FUNCTION = agtype_array_element_text);

--
-- agtype - contains operators (@> <@)
--
CREATE FUNCTION agtype_contains(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR @> (LEFTARG = agtype, RIGHTARG = agtype, FUNCTION = agtype_contains, COMMUTATOR = '<@', RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION agtype_contained_by(agtype, agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR <@ (LEFTARG = agtype, RIGHTARG = agtype, FUNCTION = agtype_contained_by, COMMUTATOR = '@>', RESTRICT = contsel, JOIN = contjoinsel);

--
-- Key Existence Operators ? ?| ?&
--
CREATE FUNCTION agtype_exists(agtype, text) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ? (LEFTARG = agtype, RIGHTARG = text, FUNCTION = agtype_exists, COMMUTATOR = '?', RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION agtype_exists_any(agtype, text[]) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ?| (LEFTARG = agtype, RIGHTARG = text[], FUNCTION = agtype_exists_any, RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION agtype_exists_all(agtype, text[]) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR ?& (LEFTARG = agtype, RIGHTARG = text[], FUNCTION = agtype_exists_all, RESTRICT = contsel, JOIN = contjoinsel);

--
-- agtype GIN support
--
CREATE FUNCTION gin_compare_agtype(text, text) RETURNS int AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION gin_extract_agtype(agtype, internal) RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION gin_extract_agtype_query(agtype, internal, int2, internal, internal) RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION gin_consistent_agtype(internal, int2, agtype, int4, internal, internal) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION gin_triconsistent_agtype(internal, int2, agtype, int4, internal, internal, internal) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS gin_agtype_ops DEFAULT FOR TYPE agtype USING gin AS
OPERATOR 7 @>, OPERATOR 9 ?(agtype, text), OPERATOR 10 ?|(agtype, text[]), OPERATOR 11 ?&(agtype, text[]),
FUNCTION 1 gin_compare_agtype, FUNCTION 2 gin_extract_agtype, FUNCTION 3 gin_extract_agtype_query, FUNCTION 4 gin_consistent_agtype, FUNCTION 6 gin_triconsistent_agtype,
STORAGE text;

--
-- graphid typecasting
--
CREATE FUNCTION graphid_to_agtype(graphid) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (graphid AS agtype) WITH FUNCTION graphid_to_agtype(graphid);
CREATE FUNCTION agtype_to_graphid(agtype) RETURNS graphid LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS graphid) WITH FUNCTION agtype_to_graphid(agtype) AS IMPLICIT;

--
-- agtype - entity creation
--
CREATE FUNCTION _agtype_build_path(VARIADIC "any") RETURNS agtype LANGUAGE c STABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION _agtype_build_vertex(graphid, cstring, agtype) RETURNS agtype LANGUAGE c IMMUTABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION _agtype_build_edge(graphid, graphid, graphid, cstring, agtype) RETURNS agtype LANGUAGE c IMMUTABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- MATCH edge uniqueness
--
CREATE FUNCTION _ag_enforce_edge_uniqueness(VARIADIC "any") RETURNS bool LANGUAGE c IMMUTABLE PARALLEL SAFE as 'MODULE_PATHNAME';

--
-- agtype - map literal (`{key: expr, ...}`)
--
CREATE FUNCTION agtype_build_map(VARIADIC "any") RETURNS agtype LANGUAGE c IMMUTABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_build_map() RETURNS agtype LANGUAGE c IMMUTABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME', 'agtype_build_map_noargs';

--
-- There are times when the optimizer might eliminate
-- functions we need. Wrap the function with this to
-- prevent that from happening
--
CREATE FUNCTION agtype_volatile_wrapper(agt agtype) RETURNS agtype AS $return_value$
BEGIN RETURN agt; END;
$return_value$ LANGUAGE plpgsql
VOLATILE
CALLED ON NULL INPUT
PARALLEL SAFE;

--
-- agtype - list literal (`[expr, ...]`)
--

CREATE FUNCTION agtype_build_list(VARIADIC "any") RETURNS agtype LANGUAGE c IMMUTABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_build_list() RETURNS agtype LANGUAGE c IMMUTABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME', 'agtype_build_list_noargs';

--
-- agtype - typecasting
--
-- agtype -> text (explicit)
CREATE FUNCTION agtype_to_text(agtype) RETURNS text LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS text) WITH FUNCTION agtype_to_text(agtype);
-- agtype -> boolean (implicit)
CREATE FUNCTION agtype_to_bool(agtype) RETURNS boolean LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS boolean) WITH FUNCTION agtype_to_bool(agtype) AS IMPLICIT;
-- boolean -> agtype (explicit)
CREATE FUNCTION bool_to_agtype(boolean) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (boolean AS agtype) WITH FUNCTION bool_to_agtype(boolean);
-- float8 -> agtype (explicit)
CREATE FUNCTION float8_to_agtype(float8) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (float8 AS agtype) WITH FUNCTION float8_to_agtype(float8);
-- agtype -> float8 (implicit)
CREATE FUNCTION agtype_to_float8(agtype) RETURNS float8 LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS float8) WITH FUNCTION agtype_to_float8(agtype);
-- int8 -> agtype (explicit)
CREATE FUNCTION int8_to_agtype(int8) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (int8 AS agtype) WITH FUNCTION int8_to_agtype(int8);
-- agtype -> int8
CREATE FUNCTION agtype_to_int8(variadic "any") RETURNS bigint LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS bigint) WITH FUNCTION agtype_to_int8(variadic "any") AS ASSIGNMENT;
-- agtype -> int4
CREATE FUNCTION agtype_to_int4(variadic "any") RETURNS int LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS int) WITH FUNCTION agtype_to_int4(variadic "any");
-- agtype -> int2
CREATE FUNCTION agtype_to_int2(variadic "any") RETURNS smallint LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS smallint) WITH FUNCTION agtype_to_int2(variadic "any");
-- agtype -> int4[]
CREATE FUNCTION agtype_to_int4_array(variadic "any") RETURNS int[] LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE CAST (agtype AS int[]) WITH FUNCTION agtype_to_int4_array(variadic "any");

--
-- agtype - access operators
--
-- for series of `map.key` and `container[expr]`
CREATE FUNCTION agtype_field_access(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR -> (LEFTARG = agtype, RIGHTARG = agtype, FUNCTION = agtype_field_access);
CREATE FUNCTION agtype_access_slice(agtype, agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_in_operator(agtype, agtype) RETURNS bool LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE OPERATOR @= (FUNCTION = agtype_in_operator, LEFTARG = agtype, RIGHTARG = agtype, NEGATOR = !@=, RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES);

--
-- agtype - string matching (`STARTS WITH`, `ENDS WITH`, `CONTAINS`, & =~)
--
CREATE FUNCTION agtype_string_match_starts_with(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_string_match_ends_with(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_string_match_contains(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_eq_tilde(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- functions for updating clauses
--
-- This function is defined as a VOLATILE function to prevent the optimizer
-- from pulling up Query's for CREATE clauses.
CREATE FUNCTION _cypher_create_clause(internal) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION _cypher_set_clause(internal) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION _cypher_delete_clause(internal) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION _cypher_merge_clause(internal) RETURNS void LANGUAGE c AS 'MODULE_PATHNAME';

--
-- query functions
--
CREATE FUNCTION cypher(graph_name name, query_string cstring, params agtype = NULL) RETURNS SETOF record LANGUAGE c AS 'MODULE_PATHNAME';
CREATE FUNCTION get_cypher_keywords(OUT word text, OUT catcode "char", OUT catdesc text) RETURNS SETOF record LANGUAGE c STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE COST 10 ROWS 60 AS 'MODULE_PATHNAME';

--
-- Scalar Functions
--
CREATE FUNCTION age_id(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_start_id(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_end_id(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_head(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_last(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_properties(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_startnode(agtype, agtype) RETURNS agtype LANGUAGE c STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_endnode(agtype, agtype) RETURNS agtype LANGUAGE c STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_length(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_size(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_type(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME','age_label';
CREATE FUNCTION age_label(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';

-- XXX: Typecasting and typecasting need to be merged
CREATE FUNCTION age_toboolean(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_tofloat(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_tointeger(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_tostring(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION agtype_typecast_numeric(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';

--
-- String functions
--
CREATE FUNCTION age_reverse(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_toupper(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_tolower(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_ltrim(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_rtrim(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_trim(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_right(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_left(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_substring(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_split(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_replace(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- Trig functions - radian input
--
CREATE FUNCTION age_sin(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_cos(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_tan(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_cot(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_asin(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_acos(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_atan(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_atan2(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_degrees(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_radians(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_round(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_ceil(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_floor(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_abs(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT AS 'MODULE_PATHNAME';
CREATE FUNCTION age_sign(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_log(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_log10(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_e() RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_exp(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_sqrt(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_pi() RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_rand() RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_timestamp() RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- Agreggation
--
-- accumlates floats into an array for aggregation
CREATE FUNCTION age_agtype_float8_accum(_float8, agtype) RETURNS _float8 LANGUAGE c IMMUTABLE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME';

-- count(agtype)
CREATE AGGREGATE age_count(*) (stype = int8, sfunc = int8inc, finalfunc = int8_to_agtype, combinefunc = int8pl, finalfunc_modify = READ_WRITE, initcond = 0, parallel = safe);
CREATE AGGREGATE age_count(agtype) (stype = int8, sfunc = int8inc_any, finalfunc = int8_to_agtype, combinefunc = int8pl, finalfunc_modify = READ_WRITE, initcond = 0, parallel = safe);
-- stdev(internal, agtype)
CREATE FUNCTION age_float8_stddev_samp_aggfinalfn(_float8) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE AGGREGATE age_stdev(agtype) (stype = _float8, sfunc = age_agtype_float8_accum, finalfunc = age_float8_stddev_samp_aggfinalfn, combinefunc = float8_combine, finalfunc_modify = read_only, initcond = '{0,0,0}', parallel = safe);
-- stdevp(internal, agtype)
CREATE FUNCTION age_float8_stddev_pop_aggfinalfn(_float8) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE AGGREGATE age_stdevp(agtype) (stype = _float8, sfunc = age_agtype_float8_accum, finalfunc = age_float8_stddev_pop_aggfinalfn, combinefunc = float8_combine, finalfunc_modify = read_only, initcond = '{0,0,0}', parallel = safe);
-- avg(agytpe)
CREATE AGGREGATE age_avg(agtype) (stype = _float8, sfunc = age_agtype_float8_accum, finalfunc = float8_avg, combinefunc = float8_combine, finalfunc_modify = read_only, initcond = '{0,0,0}', parallel = safe);
CREATE FUNCTION age_agtype_sum(agtype, agtype) RETURNS agtype LANGUAGE c IMMUTABLE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME';
-- sum(agtype)
CREATE AGGREGATE age_sum(agtype) (stype = agtype, sfunc = age_agtype_sum, combinefunc = age_agtype_sum, finalfunc_modify = read_only, parallel = safe);
-- max(variadic "any")
CREATE FUNCTION age_agtype_larger_aggtransfn(agtype, variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE AGGREGATE age_max(variadic "any") (stype = agtype, sfunc = age_agtype_larger_aggtransfn, combinefunc = age_agtype_larger_aggtransfn, finalfunc_modify = read_only, parallel = safe);
-- min(agtype)
CREATE FUNCTION age_agtype_smaller_aggtransfn(agtype, variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE AGGREGATE age_min(variadic "any") (stype = agtype, sfunc = age_agtype_smaller_aggtransfn, combinefunc = age_agtype_smaller_aggtransfn, finalfunc_modify = read_only, parallel = safe);
-- percentileCont(internal, agtype) and percentileDisc(internal, agtype)
CREATE FUNCTION age_percentile_aggtransfn(internal, agtype, agtype) RETURNS internal LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_percentile_cont_aggfinalfn(internal) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_percentile_disc_aggfinalfn(internal) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE AGGREGATE age_percentilecont(agtype, agtype) (stype = internal, sfunc = age_percentile_aggtransfn, finalfunc = age_percentile_cont_aggfinalfn, parallel = safe);
CREATE AGGREGATE age_percentiledisc(agtype, agtype) (stype = internal, sfunc = age_percentile_aggtransfn, finalfunc = age_percentile_disc_aggfinalfn, parallel = safe);
-- collect(variadic "any")
CREATE FUNCTION age_collect_aggtransfn(internal, variadic "any") RETURNS internal LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_collect_aggfinalfn(internal) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE AGGREGATE age_collect(variadic "any") (stype = internal, sfunc = age_collect_aggtransfn, finalfunc = age_collect_aggfinalfn, parallel = safe);

--
-- John's crap
--
-- original VLE function definition
CREATE FUNCTION age_vle(IN agtype, IN agtype, IN agtype, IN agtype, IN agtype, IN agtype, IN agtype, OUT edges agtype)
RETURNS SETOF agtype LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL UNSAFE AS 'MODULE_PATHNAME';

-- This is an overloaded function definition to allow for the VLE local context
-- caching mechanism to coexist with the previous VLE version.
CREATE FUNCTION age_vle(IN agtype, IN agtype, IN agtype, IN agtype, IN agtype, IN agtype, IN agtype, IN agtype, OUT edges agtype)
RETURNS SETOF agtype LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL UNSAFE AS 'MODULE_PATHNAME';
-- function to build an edge for a VLE match

-- TODO: remove
CREATE FUNCTION age_build_vle_match_edge(agtype, agtype) RETURNS agtype LANGUAGE C STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_match_vle_terminal_edge(variadic "any") RETURNS boolean LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_materialize_vle_path(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_materialize_vle_edges(agtype) RETURNS agtype LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_match_vle_edge_to_id_qual(variadic "any") RETURNS boolean LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_match_two_vle_edges(agtype, agtype) RETURNS boolean LANGUAGE C STABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_vertex_stats(agtype, agtype) RETURNS agtype LANGUAGE c STABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_delete_global_graphs(agtype) RETURNS boolean LANGUAGE c VOLATILE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- list functions
--
CREATE FUNCTION age_keys(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_labels(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_nodes(agtype) RETURNS agtype LANGUAGE c IMMUTABLE RETURNS NULL ON NULL INPUT PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_relationships(agtype) RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_range(variadic "any") RETURNS agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';
CREATE FUNCTION age_unnest(agtype, block_types boolean = false) RETURNS SETOF agtype LANGUAGE c IMMUTABLE PARALLEL SAFE AS 'MODULE_PATHNAME';

--
-- End
--
