/*
 * Copyright (C) 2023-2024 PostGraphDB
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
 *
 * Portions Copyright (c) 2020-2023, Apache Software Foundation
 * Portions Copyright (c) 2019-2020, Bitnine Global
 */ 


LOAD 'postgraph';

CREATE GRAPH cypher_create;
USE GRAPH cypher_create;

CREATE ();

CREATE (:v);

CREATE (:v {});

CREATE (:v {prop_key: 'value'});

MATCH (n:v) RETURN n;

-- Left relationship
CREATE (:v {id:'right rel, initial node'})-[:e {id:'right rel'}]->(:v {id:'right rel, end node'});

-- Right relationship
CREATE (:v {id:'left rel, initial node'})<-[:e {id:'left rel'}]-(:v {id:'left rel, end node'});

-- Pattern creates a path from the initial node to the last node
CREATE (:v {id: 'path, initial node'})-[:e {id: 'path, edge one'}]->(:v {id:'path, middle node'})-[:e {id:'path, edge two'}]->(:v {id:'path, last node'});

-- middle vertex points to the initial and last vertex
CREATE (:v {id: 'divergent, initial node'})<-[:e {id: 'divergent, edge one'}]-(:v {id: 'divergent middle node'})-[:e {id: 'divergent, edge two'}]->(:v {id: 'divergent, end node'});

-- initial and last vertex point to the middle vertex
CREATE (:v {id: 'convergent, initial node'})-[:e {id: 'convergent, edge one'}]->(:v {id: 'convergent middle node'})<-[:e {id: 'convergent, edge two'}]-(:v {id: 'convergent, end node'});

-- Validate Paths work correctly
CREATE (:v {id: 'paths, vertex one'})-[:e {id: 'paths, edge one'}]->(:v {id: 'paths, vertex two'}),
       (:v {id: 'paths, vertex three'})-[:e {id: 'paths, edge two'}]->(:v {id: 'paths, vertex four'});

--edge with double relationship will throw an error
CREATE (:v)<-[:e]->();

--edge with no relationship will throw an error
CREATE (:v)-[:e]-();

--edges without labels are not supported
CREATE (:v)-[]->(:v);

MATCH (n) RETURN n;
MATCH ()-[e]-() RETURN e;

CREATE (:n_var {var_name: 'Node A'});
CREATE (:n_var {var_name: 'Node B'});
CREATE (:n_var {var_name: 'Node C'});

MATCH (a:n_var), (b:n_var) WHERE a.var_name <> b.var_name CREATE (a)-[:e_var {var_name: a.name + ' -> ' + b.name}]->(b);

MATCH (a:n_var) CREATE (a)-[:e_var {var_name: a.var_name + ' -> ' + a.var_name}]->(a);

MATCH (a:n_var) CREATE (a)-[:e_var {var_name: a.var_name + ' -> new node'}]->(:n_other_node);

MATCH (a:n_var) WHERE a.var_name = 'Node A' CREATE (a)-[b:e_var]->();

CREATE (a)-[:b_var]->() RETURN a, id(a);

CREATE ()-[b:e_var]->() RETURN b, id(b);

CREATE (a)-[b:e_var {id: 0}]->() RETURN a, b, b.id, b.id + 1;

MATCH (a:n_var) CREATE (a)-[b:e_var]->(a) RETURN a, b;

MATCH (a:n_var) CREATE (a)-[b:e_var]->(c) RETURN a, b, c;

CREATE (a)-[:e_var]->() RETURN a;

CREATE ()-[b:e_var]->() RETURN b;

CREATE p=()-[:e_var]->() RETURN p;

CREATE p=(a {id:0})-[:e_var]->(a) RETURN p;

MATCH (a:n_var) CREATE p=(a)-[:e_var]->(a) RETURN p;

CREATE p=(a)-[:e_var]->(), (a)-[b:e_var]->(a) RETURN p, b;

MATCH (a:n_var) WHERE a.var_name = 'Node Z' CREATE (a)-[:e_var {var_name: a.var_name + ' -> doesnt exist'}]->(:n_other_node) RETURN a;

MATCH (n:n_var) RETURN n;
MATCH ()-[e:e_var]->() RETURN e;

--Validate every vertex has the correct label
MATCH (n) RETURN n;

-- prepared statements
/*
PREPARE p_1 AS SELECT * FROM cypher('cypher_create', $$CREATE (v:new_vertex {key: 'value'}) RETURN v$$) AS (a vertex);
EXECUTE p_1;
EXECUTE p_1;

PREPARE p_2 AS SELECT * FROM cypher('cypher_create', $$CREATE (v:new_vertex {key: $var_name}) RETURN v$$, $1) AS (a vertex);
EXECUTE p_2('{'var_name': 'Hello Prepared Statements'}');
EXECUTE p_2('{'var_name': 'Hello Prepared Statements 2'}');

-- pl/pgsql
CREATE FUNCTION create_test()
RETURNS TABLE(v vertex)
LANGUAGE plpgsql
VOLATILE
AS $BODY$
BEGIN
	RETURN QUERY SELECT * FROM cypher('cypher_create', $$CREATE (v:new_vertex {key: 'value'}) RETURN v$$) AS (a vertex);
END
$BODY$;

SELECT create_test();
SELECT create_test();
*/

--
-- check the cypher CREATE clause inside an INSERT INTO
--
/*
CREATE TABLE simple_path (u vertex, e edge, v vertex);

INSERT INTO simple_path(SELECT * FROM cypher('cypher_create',
    $$CREATE (u)-[e:knows]->(v) return u, e, v
    $$) AS (u vertex, e edge, v vertex));

SELECT count(*) FROM simple_path;
*/
--
-- check the cypher CREATE clause inside of a BEGIN/END/COMMIT block
--
/*
BEGIN;
SELECT * FROM cypher('cypher_create', $$ CREATE (a:Part {part_num: '670'}) $$) as (a gtype);
SELECT * FROM cypher('cypher_create', $$ MATCH (a:Part) RETURN a $$) as (a vertex);

SELECT * FROM cypher('cypher_create', $$ CREATE (a:Part {part_num: '671'}) $$) as (a gtype);
SELECT * FROM cypher('cypher_create', $$ CREATE (a:Part {part_num: '672'}) $$) as (a gtype);
SELECT * FROM cypher('cypher_create', $$ MATCH (a:Part) RETURN a $$) as (a vertex);

SELECT * FROM cypher('cypher_create', $$ CREATE (a:Part {part_num: '673'}) $$) as (a gtype);
SELECT * FROM cypher('cypher_create', $$ MATCH (a:Part) RETURN a $$) as (a vertex);
END;
*/

--
-- Errors
--
-- Var 'a' cannot have properties in the create clause
MATCH (a:n_var) WHERE a.var_name = 'Node A' CREATE (a {test:1})-[:e_var]->();

-- Var 'a' cannot change labels
MATCH (a:n_var) WHERE a.var_name = 'Node A' CREATE (a:new_label)-[:e_var]->();

MATCH (a:n_var)-[b]-() WHERE a.var_name = 'Node A' CREATE (a)-[b:e_var]->();

--CREATE with joins
/*
SELECT *
FROM (CREATE (a) RETURN a) as q(a vertex),
     (CREATE (b) RETURN b) as t(b vertex);
*/

--
-- Clean up
--
DROP GRAPH cypher_create CASCADE;

--
-- End
--
