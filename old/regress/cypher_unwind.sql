/*
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
 */

LOAD 'postgraph';
SET search_path TO postgraph;

SELECT create_graph('cypher_unwind');

SELECT * FROM cypher('cypher_unwind', $$
    UNWIND [1, 2, 3] AS i RETURN i
$$) as (i gtype);

SELECT * FROM cypher('cypher_unwind', $$
    CREATE ({a: [1, 2, 3]}), ({a: [4, 5, 6]})
$$) as (i gtype);

SELECT * FROM cypher('cypher_unwind', $$
    MATCH (n) WITH n.a AS a UNWIND a AS i RETURN *
$$) as (i gtype, j gtype);

SELECT * FROM cypher('cypher_unwind', $$
    WITH [[1, 2], [3, 4], 5] AS nested
    UNWIND nested AS x
    UNWIND x AS y
    RETURN y
$$) as (i gtype);

SELECT * FROM cypher('cypher_unwind', $$
    MATCH (n_1)
    WITH collect(n_1) as n
    UNWIND n as a
    SET a.i = 1
    RETURN a
$$) as (i vertex);


SELECT * FROM cypher('cypher_unwind', $$
    MATCH (n_1)
    WITH collect(n_1) as n
    UNWIND n as a
    RETURN a
$$) as (i vertex);

SELECT * FROM cypher('cypher_unwind', $$
    MATCH (n_1)
    WITH collect(n_1) as n
    UNWIND n as a
    RETURN a
$$) as (i gtype);

SELECT * FROM cypher('cypher_unwind', $$
    MATCH (n_1)
    WITH collect(n_1) as n
    UNWIND n as a
    SET a.i = 1
    RETURN a
$$) as (i vertex);

SELECT * FROM cypher('cypher_unwind', $$
    MATCH (n_1)
    WITH collect(n_1) as n
    UNWIND n as a
    WITH a
    SET a.i = 1
    RETURN a
$$) as (i vertex);

SELECT * FROM cypher('cypher_unwind', $$
    MATCH (n_1)
    WITH collect(n_1) as n
    UNWIND n as a
    CREATE ({i: a.i})
    RETURN a
$$) as (i vertex);

SELECT * FROM cypher('cypher_unwind', $$
    CREATE ()-[:e {a: [1, 2, 3]}]->()
$$) as (i gtype);

SELECT * FROM cypher('cypher_unwind', $$
    CREATE ()-[:e {a: [1, 2, 3]}]->()
$$) as (i gtype);

SELECT * FROM cypher('cypher_unwind', $$
    MATCH t=()-[:e]->()
    UNWIND relationships(t) as rel
    RETURN rel
$$) as (i edge);


SELECT drop_graph('cypher_unwind', true);
