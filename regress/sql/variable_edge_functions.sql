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

CREATE GRAPH variable_edge_functions;
USE GRAPH variable_edge_functions;
--
-- Create table to hold the start and end vertices to test the SRF
--

-- Create a graph to test
CREATE (b:begin)-[:edge]->(u1:middle)-[:edge]->(u2:middle)-[:edge]->(u3:middle)-[:edge]->(e:end),
	(u1)-[:self_loop]->(u1),
	(e)-[:self_loop]->(e),
	(b)-[:alternate_edge]->(u1),
	(u2)-[:alternate_edge]->(u3),
	(u3)-[:alternate_edge]->(e),
	(u2)-[:bypass_edge]->(e),
	(e)-[:alternate_edge]->(u3), 
	(u3)-[:alternate_edge]->(u2),
	(u2)-[:bypass_edge]->(b);


MATCH (:begin)-[e*3..3]->(:end) RETURN nodes(e);
MATCH (:begin)-[e*3..3]->(:end) WITH nodes(e) as nodes RETURN nodes[1];
MATCH (:begin)-[e*3..3]->(:end) WITH nodes(e) as nodes RETURN nodes[size(nodes) - 1];


MATCH (:begin)-[e*3..3]->(:end) RETURN relationships(e);
MATCH (:begin)-[e*3..3]->(:end) WITH relationships(e) as edges RETURN edges[1];
MATCH (:begin)-[e*3..3]->(:end) WITH relationships(e) as edges RETURN edges[size(edges) - 1];


MATCH (:begin)-[ve*3..3]->(:end) 
MATCH ()-[e]->()
RETURN e @> ve, id(e), edges(ve);

MATCH (:begin)-[ve*3..3]->(:end)
MATCH ()-[e]->()
RETURN ve <@ e, id(e), edges(ve);

MATCH (:begin)-[e*]->(:end) RETURN DISTINCT @-@ e;

--
-- Clean up
--
DROP GRAPH variable_edge_functions CASCADE;

--
-- End
--
