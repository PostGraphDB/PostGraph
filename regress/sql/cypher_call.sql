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

LOAD 'postgraph';
SET search_path TO postgraph;

CREATE GRAPH cypher_call;
USE GRAPH cypher_call;

--
-- Create table to hold the start and end vertices to test the SRF
--
CYPHER CALL sin(1) as a RETURN a;

CYPHER CALL sin(1) as b RETURN b;

CYPHER WITH [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] as lst CALL unnest(lst) as b RETURN b;

CYPHER WITH [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] as lst CALL unnest(lst) as b WHERE b % 2 = 0 RETURN b;

CYPHER CALL unnest([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]) as b RETURN b;

CYPHER CALL pg_catalog.generate_series(1, 2, 3);

MATCH (n) CALL this_isnt_a_real_function(1, 2, 3);

CYPHER CALL { RETURN 1 } RETURN 2;

CYPHER CALL { RETURN 1 as a } RETURN a;

CYPHER CALL { CREATE (n) } RETURN n;

CYPHER CALL { CREATE (n) RETURN n as n } RETURN n;


--
-- Clean up
--
DROP GRAPH cypher_call CASCADE;

--
-- End
--
