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

SELECT create_graph('drop');

DROP EXTENSION postgraph;

SELECT nspname FROM pg_catalog.pg_namespace WHERE nspname = 'drop';

SELECT tablename FROM pg_catalog.pg_tables WHERE schemaname = 'postgraph';

-- Recreate the extension and validate we can recreate a graph
CREATE EXTENSION postgraph;

SELECT create_graph('drop');

-- Create a schema that uses the gtype, so we can't just drop postgraph.
CREATE SCHEMA other_schema;

CREATE TABLE other_schema.tbl (id gtype);

-- Should Fail because gtype can't be dropped
DROP EXTENSION postgraph;

-- Check the graph still exist, because the DROP command failed
SELECT nspname FROM pg_catalog.pg_namespace WHERE nspname = 'drop';

-- Should succeed, delete the 'drop' schema and leave 'other_schema'
DROP EXTENSION postgraph CASCADE;

-- 'other_schema' should exist, 'drop' should be deleted
SELECT nspname FROM pg_catalog.pg_namespace WHERE nspname IN ('other_schema', 'drop');
