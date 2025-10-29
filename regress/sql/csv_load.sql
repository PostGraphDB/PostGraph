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

\! cp -r regress/load/data regress/instance/data/load

LOAD 'postgraph';

CREATE GRAPH load_csv;
USE GRAPH load_csv;

CYPHER LOAD CSV 'load/test.csv' AS n
RETURN n;

CYPHER LOAD CSV 'load/test.csv' AS n
RETURN n[0];

CYPHER WITH  '["booker12", "9012", "Rachel", "Booker"]' AS n
RETURN n[2];

CYPHER LOAD CSV 'load/test.csv' AS n;

CYPHER LOAD CSV 'load/test.csv' AS n
CREATE (a:Person {name: n});

MATCH (a:Person) RETURN a;

MATCH (a:Person) 
LOAD CSV 'load/test.csv' AS n
CREATE (a)-[:elabel]->(b:Person {name: n})
RETURN a, b;



/* Crashes
MATCH (a:Person) 
LOAD CSV 'load/test.csv' AS n
MERGE (b:Person {name: n})
CREATE (a)-[:elabel2]->(b);


MATCH (a:Person) 
LOAD CSV 'load/test.csv' AS n
MERGE (b:Person {name: n})
CREATE (a)-[:elabel2]->(b)
RETURN a, b;*/

--
-- Clean up
--
DROP GRAPH load_csv CASCADE;

--
-- End
--
