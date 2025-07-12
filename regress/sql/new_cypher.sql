/*
 * Copyright (C) 2024 PostGraphDB
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
-- Regression tests don't preload extensions, gotta load it first

LOAD 'postgraph';

SET search_path TO 'postgraph';
SET search_path TO DEFAULT;
SET search_path TO 'postgraph';

CREATE EXTENSION postgraph;
CREATE EXTENSION hstore CASCADE SCHEMA public;
CREATE EXTENSION IF NOT EXISTS hstore;
CREATE EXTENSION IF NOT EXISTS pg_trgm VERSION '1.3';

-- Basic Graph creation
CREATE GRAPH new_cypher;

-- Assign Graph to use
USE GRAPH new_cypher;

-- Reuse Name, should throw error
CREATE GRAPH new_cypher;

-- Graph Does not exist, should throw error
USE GRAPH new_cypher;

CREATE GRAPH new_cypher_2;
USE GRAPH new_cypher_2;
USE GRAPH new_cypher;

SELECT * FROM postgraph.ag_label WHERE name = '_ag_label_vertex';
\d+ new_cypher._adj__ag_label_vertex

SELECT c.relname AS table_name,am.amname AS access_method
FROM pg_class c
JOIN pg_am am ON c.relam = am.oid
WHERE c.relkind = 'r' AND c.relname = '_adj__ag_label_vertex';

DROP GRAPH new_cypher CASCADE;
DROP GRAPH new_cypher_2 CASCADE;