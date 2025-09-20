/*
 * Copyright (C) 2023-2025 PostGraphDB
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
SELECT * FROM cypher_create._ag_label_vertex;

MATCH () RETURN 1;

SELECT '{}'::postgraph.gtype;
CREATE ({msg: 'Hello World'});
MATCH (a) RETURN a;
SELECT * FROM cypher_create._ag_label_vertex;


CREATE VLABEL test;
CREATE (:test);

CREATE (:test2);

MATCH () RETURN 1;

MATCH (a) RETURN a;

MATCH (a:test) RETURN a;

CREATE ()-[]->();

CREATE (a)-[]->();

CREATE (a)-[]->() RETURN a;

CREATE (a)-[]->(b) RETURN a;

CREATE (a {msg: 'hello vertex out'}) RETURN a;

CREATE (a {msg: 'hello vertex out'}) RETURN a.msg;

CREATE (a {msg: 'hello vertex out'})-[]->(b) RETURN a;


CREATE (a)-[e]->(b) RETURN e;

CREATE (a)-[e {msg: 'hello edge'}]->(b) RETURN e;


CREATE ()-[{msg: 'hello edge'}]->();
SELECT * FROM cypher_create._adj__ag_label_vertex;

CREATE ()-[:elabel]->();

MATCH ()-[]->() RETURN 1;

MATCH ()<-[]-() RETURN 1;

MATCH ()<-[q]-() RETURN 1;


MATCH (a)-[]->() RETURN a;

MATCH (a)<-[]-() RETURN a;


MATCH ()-[]->(a) RETURN a;

MATCH ()<-[]-(a) RETURN a;

CREATE ()<-[{msg: 'Hello edge'}]-();
CREATE ()-[{msg: 'Hello edge'}]->();

MATCH ()<-[a]-() RETURN a;

CYPHER WITH 1 as a
CREATE ();

CREATE () RETURN 1 as a;

CREATE ()-[]->()-[]->();
CREATE ()-[]->()-[]->();
MATCH  ()<-[]-()<-[]-() RETURN 1;
MATCH  ()-[]->()-[]->() RETURN 1;


EXPLAIN ANALYZE MATCH  (q)-[]->(q)-[]->() RETURN 1;
MATCH  (q)-[]->(q)-[]->() RETURN 1;


EXPLAIN ANALYZE
MATCH (q)
MATCH (q)-[]->()
RETURN q;

MATCH (q)
MATCH (q)-[]->()
RETURN q;

EXPLAIN ANALYZE
MATCH (q)
MATCH ()-[]->(q)
RETURN q;

MATCH (q)
MATCH ()-[]->(q)
RETURN q;

EXPLAIN ANALYZE
MATCH (q)
MATCH (b)-[]->()
RETURN q;

MATCH (q)
MATCH (b)-[]->()
RETURN q;

EXPLAIN MATCH  (q)-[]->()<-[]-(q) RETURN 1;
MATCH  (q)-[]->()<-[]-(q) RETURN 1;

EXPLAIN ANALYZE MATCH  (q)<-[]-(q) RETURN 1;
MATCH  (q)<-[]-(q) RETURN 1;

EXPLAIN MATCH  ()-[]->()<-[]-() RETURN 1;
EXPLAIN ANALYZE MATCH  ()-[]->()<-[]-() RETURN 1;
MATCH  ()-[]->()<-[]-() RETURN 1;

EXPLAIN MATCH  (:test)-[]->()<-[]-(:test) RETURN 1;
EXPLAIN ANALYZE MATCH  (:test)-[]->()<-[]-(:test) RETURN 1;
MATCH  (:test)-[]->()<-[]-(:test) RETURN 1;


MATCH  ()-[*1]->()-[]->() RETURN 1;
EXPLAIN ANALYZE MATCH  ()-[*1]->()-[]->() RETURN 1;

SELECT * FROM cypher_create._adj__ag_label_vertex;
MATCH (a) RETURN a;
--SELECT * FROM cypher_create.idx_hash__adj__ag_label_vertex;

MATCH ()-[*1]->() RETURN 1;
MATCH ()-[]->() RETURN 1;
MATCH ()-[*2]->() RETURN 1;

MATCH ()-[*1..2]->() RETURN 1;

MATCH ()-[*..2]->() RETURN 1;

MATCH ()-[*2..]->() RETURN 1;


MATCH ()-[*1..]->() RETURN 1;

MATCH ()-[:elabel]->() RETURN 1;

MATCH ()-[:elabel*1]->() RETURN 1;

MATCH ()-[:elabel*1..]->() RETURN 1;

MATCH ()-[:elabel*1..2]->() RETURN 1;

MATCH ()-[:elabel*2]->() RETURN 1;

MATCH ()-[:elabel*..2]->() RETURN 1;

MATCH (n:test)
CREATE ()-[:elabel]->(:test2)
RETURN 1;

MATCH (n:test)
CREATE (n)-[:elabel]->(:test2)
RETURN 1;
MATCH (n)
CREATE (n)-[:elabel2]->(:test3)
RETURN 1;

MATCH (n)
CREATE (n)<-[:elabel2]-(:test3)
RETURN 1;

--
-- Clean up
--
DROP GRAPH cypher_create CASCADE;

--
-- End
--
