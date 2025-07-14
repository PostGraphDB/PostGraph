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
SELECT * FROM cypher_create._ag_label_vertex;

MATCH () RETURN 1;

CREATE VLABEL test;
SELECT * FROM postgraph.ag_label;

CREATE (:test);
SELECT * FROM postgraph.ag_label;
SELECT * FROM cypher_create.test;


CREATE (:test2);
SELECT * FROM postgraph.ag_label;
SELECT * FROM cypher_create.test2;


MATCH () RETURN 1;

CREATE ()-[]->();

CYPHER WITH 1 as a
CREATE ();

CREATE () RETURN 1 as a;

--
-- Clean up
--
DROP GRAPH cypher_create CASCADE;

--
-- End
--
