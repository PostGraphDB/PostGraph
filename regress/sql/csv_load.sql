
\! cp regress/test.csv regress/instance/data/

LOAD 'postgraph';

CREATE GRAPH cypher_create;
USE GRAPH cypher_create;

CYPHER LOAD CSV './test.csv' AS n
RETURN n;

--
-- Clean up
--
DROP GRAPH cypher_create CASCADE;

--
-- End
--
