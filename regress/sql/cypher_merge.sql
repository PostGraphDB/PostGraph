
/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * 'License'); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * 'AS IS' BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

LOAD 'postgraph';
SET search_path TO postgraph;

CREATE GRAPH cypher_merge;
USE GRAPH cypher_merge;

/*
 * Section 1: MERGE with single vertex
 */
/*
 * test 1: Single MERGE Clause, path doesn't exist
 */
--test query
MERGE ();
MATCH (n) RETURN n;

EXPLAIN MERGE ({i: 1});

MERGE ({i: 1});
MATCH (n) RETURN n;

MATCH (n {i: 1}) RETURN n;

MERGE ({i: 1});
MATCH (n) RETURN n;

MERGE (n {i: 'Hello Merge'});
MATCH (n) RETURN n;

MERGE (n {i: 'Hello Merge'}) RETURN n;

/*
 * Clean up graph
 */
DROP GRAPH cypher_merge CASCADE;
